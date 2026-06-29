/*
 * examples/voice_chat.cpp
 *
 * Push-to-talk voice conversation with the AI agent via WebSocket.
 *
 * Audio capture/playback uses the on-board SpacemitAudio driver
 * (SpacemitAudio::AudioCapture / AudioPlayer, PortAudio-based) instead of
 * sox, so it runs directly against the device microphone/speaker.
 *
 * The K3 snd-es8326 card typically only supports 48 kHz capture/playback,
 * while the voice protocol exchanges 16 kHz mono PCM. Capture is therefore
 * opened at the hardware rate and resampled down to 16 kHz before being sent;
 * TTS audio (16 kHz) is resampled up to the playback rate before being played.
 *
 * Requirements:
 *   - agent-gateway running with voice channel enabled.
 *   - Build with -DDA_BUILD_VOICE_CHAT_AUDIO=ON (pulls in the audio/ driver).
 *
 * Controls:
 *   [Enter]  Start / stop recording
 *   [q]      Quit
 *
 * Environment variables:
 *   VOICE_WS_URL          WebSocket URL        (default: ws://127.0.0.1:3001/ws/voice)
 *   DEVICE_ID             Device ID            (default: product-f60mo4-device-da15ec6584ef)
 *   PRODUCT_ID            Product ID           (default: product-f60mo4)
 *   VOICE_INPUT_DEVICE    Mic device index     (default: -1, auto)
 *   VOICE_OUTPUT_DEVICE   Speaker device index (default: -1, auto)
 *   VOICE_CAPTURE_RATE    Capture sample rate  (default: 48000)
 *   VOICE_CAPTURE_CHANNELS Capture channels    (default: 1)
 *   VOICE_TTS_RATE        Incoming TTS rate    (default: 16000)
 *   VOICE_PLAY_RATE       Playback sample rate (default: 48000)
 *   VOICE_PLAY_CHANNELS   Playback channels    (default: 1)
 *   TTS_SAMPLE_RATE       Alias for VOICE_TTS_RATE (back-compat)
 *   VOICE_TLS_CA_FILE / MQTT_TLS_CA_FILE
 *   VOICE_TLS_CERT_FILE / MQTT_TLS_CERT_FILE
 *   VOICE_TLS_KEY_FILE / MQTT_TLS_KEY_FILE
 *   VOICE_TLS_SERVER_NAME                (optional SNI / hostname verify target)
 *   VOICE_TLS_INSECURE                   (0/1, default 0)
 */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include <pthread.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "device_agent/voice_client.h"

#include "audio_base.hpp"
#include "audio_resampler.hpp"

using SpacemitAudio::AudioCapture;
using SpacemitAudio::AudioPlayer;

/* --- Config ---------------------------------------------------------------- */

static const char *g_ws_url      = "ws://127.0.0.1:3001/ws/voice";
static const char *g_device_id   = "product-f60mo4-device-da15ec6584ef";
static const char *g_product_id  = "product-f60mo4";

static int g_input_device      = -1;
static int g_output_device     = -1;
static int g_capture_rate      = 48000;
static int g_capture_channels  = 1;
static int g_tts_rate          = 16000;   /* rate of PCM arriving from server */
static int g_play_rate         = 48000;
static int g_play_channels     = 1;

/* Voice protocol is 16 kHz mono -- this is what we send / declare to server. */
static const int kVoiceRate     = 16000;
static const int kVoiceChannels = 1;

/* --- Global state ---------------------------------------------------------- */

static da_voice_client_t *g_voice    = NULL;
static volatile int       g_stop     = 0;
static volatile int       g_recording = 0;

static AudioCapture *g_capture = nullptr;
static Resampler    *g_cap_resampler = nullptr;  /* capture_rate -> 16k, mono */
static pthread_mutex_t g_cap_mutex = PTHREAD_MUTEX_INITIALIZER;

/* TTS audio buffer (16 kHz mono int16) */
static int16_t  *g_tts_buf    = NULL;
static size_t    g_tts_len    = 0;
static size_t    g_tts_cap    = 0;
static pthread_mutex_t g_tts_mutex = PTHREAD_MUTEX_INITIALIZER;

/* --- Terminal raw mode ----------------------------------------------------- */

static struct termios g_old_termios;

static void enable_raw_mode(void) {
  struct termios raw;
  tcgetattr(STDIN_FILENO, &g_old_termios);
  raw = g_old_termios;
  raw.c_lflag &= ~(ECHO | ICANON);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
}

static void disable_raw_mode(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_old_termios);
}

/* --- TTS buffer ----------------------------------------------------------- */

static void tts_buf_append(const int16_t *pcm, size_t n) {
  pthread_mutex_lock(&g_tts_mutex);
  if (g_tts_len + n > g_tts_cap) {
    size_t new_cap = (g_tts_cap == 0) ? 16384 : g_tts_cap * 2;
    while (new_cap < g_tts_len + n) new_cap *= 2;
    int16_t *nb = (int16_t *)realloc(g_tts_buf, new_cap * sizeof(int16_t));
    if (nb) { g_tts_buf = nb; g_tts_cap = new_cap; }
  }
  if (g_tts_len + n <= g_tts_cap) {
    memcpy(g_tts_buf + g_tts_len, pcm, n * sizeof(int16_t));
    g_tts_len += n;
  }
  pthread_mutex_unlock(&g_tts_mutex);
}

/* --- PCM helpers ---------------------------------------------------------- */

/* Downmix interleaved PCM16 to mono by averaging channels. */
static std::vector<int16_t> downmix_to_mono(const int16_t *in, size_t frames,
                                            int channels) {
  if (channels == 1)
    return std::vector<int16_t>(in, in + frames);
  std::vector<int16_t> out(frames);
  for (size_t f = 0; f < frames; ++f) {
    int sum = 0;
    for (int c = 0; c < channels; ++c) sum += in[f * channels + c];
    out[f] = (int16_t)(sum / channels);
  }
  return out;
}

/* Resample mono PCM16 from input_rate to output_rate (linear). */
static std::vector<int16_t> resample_pcm16(const std::vector<int16_t> &input,
                                           int input_rate, int output_rate) {
  if (input_rate == output_rate || input.empty()) return input;

  std::vector<float> fin(input.size());
  for (size_t i = 0; i < input.size(); ++i)
    fin[i] = static_cast<float>(input[i]) / 32768.0f;

  Resampler::Config cfg;
  cfg.input_sample_rate  = input_rate;
  cfg.output_sample_rate = output_rate;
  cfg.channels = 1;
  cfg.method = (output_rate > input_rate) ? ResampleMethod::LINEAR_UPSAMPLE
                                          : ResampleMethod::LINEAR_DOWNSAMPLE;

  Resampler rs(cfg);
  if (!rs.initialize()) return {};

  std::vector<float> fout = rs.process(fin);
  std::vector<int16_t> out(fout.size());
  for (size_t i = 0; i < fout.size(); ++i) {
    float s = std::clamp(fout[i], -1.0f, 1.0f);
    out[i] = (int16_t)(s * 32767.0f);
  }
  return out;
}

/* Convert mono PCM16 to interleaved multi-channel by duplication. */
static std::vector<int16_t> expand_channels(const std::vector<int16_t> &mono,
                                            int out_channels) {
  if (out_channels <= 1 || mono.empty()) return mono;
  std::vector<int16_t> out(mono.size() * out_channels);
  for (size_t i = 0; i < mono.size(); ++i)
    for (int c = 0; c < out_channels; ++c)
      out[i * out_channels + c] = mono[i];
  return out;
}

/* --- Capture -> voice client ---------------------------------------------- */

static void send_capture_chunk(const uint8_t *data, size_t size) {
  if (!g_recording || !g_voice) return;

  size_t n_samples = size / sizeof(int16_t);
  if (n_samples == 0) return;

  const int16_t *in = reinterpret_cast<const int16_t *>(data);
  size_t frames = n_samples / g_capture_channels;
  if (frames == 0) return;

  std::vector<int16_t> mono = downmix_to_mono(in, frames, g_capture_channels);

  std::vector<int16_t> out16;
  if (g_capture_rate == kVoiceRate) {
    out16 = std::move(mono);
  } else {
    std::vector<float> fin(frames);
    for (size_t i = 0; i < frames; ++i)
      fin[i] = static_cast<float>(mono[i]) / 32768.0f;

    pthread_mutex_lock(&g_cap_mutex);
    std::vector<float> fout = g_cap_resampler->process(fin);
    pthread_mutex_unlock(&g_cap_mutex);

    out16.resize(fout.size());
    for (size_t i = 0; i < fout.size(); ++i) {
      float s = std::clamp(fout[i], -1.0f, 1.0f);
      out16[i] = (int16_t)(s * 32767.0f);
    }
  }

  if (!out16.empty())
    da_voice_client_send_audio(g_voice, out16.data(), out16.size());
}

/* --- Recording rounds ----------------------------------------------------- */

static void start_recording_round(void) {
  g_recording = 1;
  printf("\n  [REC] Recording... (press Enter to stop)\n");

  pthread_mutex_lock(&g_cap_mutex);
  if (g_cap_resampler) g_cap_resampler->reset();
  pthread_mutex_unlock(&g_cap_mutex);

  da_voice_client_start_listening(g_voice, "manual");

  if (!g_capture->Start(g_capture_rate, g_capture_channels)) {
    fprintf(stderr, "[voice] failed to start capture\n");
    g_recording = 0;
    da_voice_client_stop_listening(g_voice);
  }
}

static void stop_recording_round(void) {
  g_recording = 0;
  if (g_capture) g_capture->Stop();
  printf("  [STOP] Processing...\n");
  da_voice_client_stop_listening(g_voice);
}

/* --- TTS playback --------------------------------------------------------- */

static void play_tts_buffer(void) {
  pthread_mutex_lock(&g_tts_mutex);
  size_t n = g_tts_len;
  std::vector<int16_t> audio(g_tts_buf ? g_tts_buf : nullptr,
                             g_tts_buf ? g_tts_buf + n : nullptr);
  g_tts_len = 0;
  pthread_mutex_unlock(&g_tts_mutex);

  if (audio.empty()) return;

  /* Resample TTS rate -> playback rate (mono). */
  std::vector<int16_t> play = resample_pcm16(audio, g_tts_rate, g_play_rate);
  if (play.empty() && !audio.empty()) {
    fprintf(stderr, "[voice] failed to resample TTS audio\n");
    return;
  }

  /* Expand to playback channels. */
  std::vector<int16_t> out = expand_channels(play, g_play_channels);

  AudioPlayer player(g_output_device);
  if (!player.Start(g_play_rate, g_play_channels)) {
    fprintf(stderr, "[voice] failed to start player\n");
    return;
  }

  const size_t chunk_samples = 4096 * static_cast<size_t>(g_play_channels);
  for (size_t off = 0; off < out.size(); off += chunk_samples) {
    size_t samples = std::min(chunk_samples, out.size() - off);
    const uint8_t *data = reinterpret_cast<const uint8_t *>(out.data() + off);
    if (!player.Write(data, samples * sizeof(int16_t))) {
      fprintf(stderr, "[voice] failed to play audio\n");
      break;
    }
  }

  player.Stop();
  player.Close();
}

/* --- Voice client callbacks ----------------------------------------------- */

static void on_connected(const char *session_id, void *ud) {
  (void)ud;
  printf("[voice] connected  session: %s\n", session_id);
  printf("\n[Enter] to speak, [q] to quit > ");
  fflush(stdout);
}

static void on_disconnected(const char *reason, void *ud) {
  (void)ud;
  printf("\n[voice] disconnected: %s\n", reason);
  g_stop = 1;
}

static void on_state(da_voice_state_t state, void *ud) {
  (void)ud;
  static const char *names[] = {
    "disconnected", "connecting", "connected",
    "listening", "processing", "playing"
  };
  int idx = (int)state;
  if (idx >= 0 && idx < 6)
    printf("  state -> %s\n", names[idx]);
}

static void on_asr(const char *text, bool definite, const char *task_id, void *ud) {
  (void)ud; (void)task_id;
  printf("\r\033[K  ASR %s %s", definite ? "ok" : "...", text);
  if (definite) printf("\n");
  fflush(stdout);
}

static void on_agent_reply(const char *text, bool streaming,
                            const char *task_id, void *ud) {
  (void)ud; (void)task_id;
  if (!streaming) printf("  Agent: %s\n", text);
}

static void on_tts_audio(const int16_t *pcm, size_t n_samples,
                          const char *task_id, void *ud) {
  (void)ud; (void)task_id;
  tts_buf_append(pcm, n_samples);
}

static void on_tts_complete(const char *task_id, void *ud) {
  (void)ud; (void)task_id;
  play_tts_buffer();
  printf("  (playback done)\n");
  printf("\n[Enter] to speak, [q] to quit > ");
  fflush(stdout);
}

static void on_error(const char *msg, void *ud) {
  (void)ud;
  fprintf(stderr, "\n[voice] error: %s\n", msg);
}

/* --- Cleanup --------------------------------------------------------------- */

static void cleanup(void) {
  disable_raw_mode();
  if (g_recording) {
    g_recording = 0;
    if (g_capture) g_capture->Stop();
  }
  if (g_capture) {
    g_capture->Close();
    delete g_capture;
    g_capture = nullptr;
  }
  if (g_cap_resampler) {
    delete g_cap_resampler;
    g_cap_resampler = nullptr;
  }
  if (g_voice) {
    da_voice_client_disconnect(g_voice);
    da_voice_client_free(g_voice);
    g_voice = NULL;
  }
  free(g_tts_buf);
  g_tts_buf = NULL;
}

static void handle_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

/* --- Env helpers ---------------------------------------------------------- */

static int env_int(const char *name, int def) {
  const char *v = getenv(name);
  return (v && v[0]) ? atoi(v) : def;
}

static void list_devices(void) {
  printf("Input devices:\n");
  for (auto &p : AudioCapture::ListDevices())
    printf("  [%d] %s\n", p.first, p.second.c_str());
  printf("Output devices:\n");
  for (auto &p : AudioPlayer::ListDevices())
    printf("  [%d] %s\n", p.first, p.second.c_str());
}

/* --- Main ------------------------------------------------------------------ */

int main(void) {
  /* Read config from environment */
  const char *ws_url  = getenv("VOICE_WS_URL")    ? getenv("VOICE_WS_URL")    : g_ws_url;
  const char *device  = getenv("DEVICE_ID")        ? getenv("DEVICE_ID")       : g_device_id;
  const char *product = getenv("PRODUCT_ID")       ? getenv("PRODUCT_ID")      : g_product_id;
  const char *voice_tls_ca = getenv("VOICE_TLS_CA_FILE") ? getenv("VOICE_TLS_CA_FILE") : getenv("MQTT_TLS_CA_FILE");
  const char *voice_tls_cert = getenv("VOICE_TLS_CERT_FILE") ? getenv("VOICE_TLS_CERT_FILE") : getenv("MQTT_TLS_CERT_FILE");
  const char *voice_tls_key = getenv("VOICE_TLS_KEY_FILE") ? getenv("VOICE_TLS_KEY_FILE") : getenv("MQTT_TLS_KEY_FILE");
  const char *voice_tls_sni = getenv("VOICE_TLS_SERVER_NAME");
  const char *voice_tls_insecure = getenv("VOICE_TLS_INSECURE");

  g_input_device     = env_int("VOICE_INPUT_DEVICE",    g_input_device);
  g_output_device    = env_int("VOICE_OUTPUT_DEVICE",   g_output_device);
  g_capture_rate     = env_int("VOICE_CAPTURE_RATE",    g_capture_rate);
  g_capture_channels = env_int("VOICE_CAPTURE_CHANNELS", g_capture_channels);
  g_play_rate        = env_int("VOICE_PLAY_RATE",       g_play_rate);
  g_play_channels    = env_int("VOICE_PLAY_CHANNELS",   g_play_channels);
  /* TTS_SAMPLE_RATE is the legacy alias for the incoming TTS rate. */
  g_tts_rate         = env_int("VOICE_TTS_RATE",
                               env_int("TTS_SAMPLE_RATE", g_tts_rate));

  printf("Voice Chat Example\n");
  printf("==================\n");
  printf("Server  : %s\n", ws_url);
  printf("Product : %s\n", product);
  printf("Device  : %s\n", device);
  printf("Capture : %d Hz, %d ch, device=%d\n",
         g_capture_rate, g_capture_channels, g_input_device);
  printf("Play    : %d Hz, %d ch, device=%d (TTS in %d Hz)\n",
         g_play_rate, g_play_channels, g_output_device, g_tts_rate);
  if (voice_tls_ca && voice_tls_ca[0])
    printf("TLS CA  : %s\n", voice_tls_ca);
  if (voice_tls_sni && voice_tls_sni[0])
    printf("TLS SNI : %s\n", voice_tls_sni);
  printf("\n");
  list_devices();
  printf("\n");

  da_voice_options_t opts = {
    .ws_url     = ws_url,
    .device_id  = device,
    .product_id = product,
    .voice_type = NULL,
    .provider   = NULL,
    .tls_ca_file = voice_tls_ca,
    .tls_cert_file = voice_tls_cert,
    .tls_key_file = voice_tls_key,
    .tls_server_name = voice_tls_sni,
    .tls_insecure = voice_tls_insecure ? atoi(voice_tls_insecure) : 0,
    .sample_rate = kVoiceRate,
    .channels    = kVoiceChannels,
  };

  da_voice_callbacks_t cbs = {
    .on_connected    = on_connected,
    .on_disconnected = on_disconnected,
    .on_state        = on_state,
    .on_asr          = on_asr,
    .on_agent_reply  = on_agent_reply,
    .on_tts_audio    = on_tts_audio,
    .on_tts_complete = on_tts_complete,
    .on_error        = on_error,
    .user_data       = NULL,
  };

  g_voice = da_voice_client_new(&opts, &cbs);
  if (!g_voice) {
    fprintf(stderr, "Failed to create voice client\n");
    return 1;
  }

  /* Set up capture (not started yet) and the capture-rate -> 16k resampler. */
  g_capture = new AudioCapture(g_input_device);
  g_capture->SetCallback([](const uint8_t *data, size_t size) {
    send_capture_chunk(data, size);
  });

  if (g_capture_rate != kVoiceRate) {
    Resampler::Config cfg;
    cfg.input_sample_rate  = g_capture_rate;
    cfg.output_sample_rate = kVoiceRate;
    cfg.channels = kVoiceChannels;
    cfg.method = (kVoiceRate > g_capture_rate) ? ResampleMethod::LINEAR_UPSAMPLE
                                               : ResampleMethod::LINEAR_DOWNSAMPLE;
    g_cap_resampler = new Resampler(cfg);
    if (!g_cap_resampler->initialize()) {
      fprintf(stderr, "Failed to initialize capture resampler\n");
      cleanup();
      return 1;
    }
  }

  signal(SIGINT,  handle_signal);
  signal(SIGTERM, handle_signal);

  printf("Connecting to %s...\n", ws_url);
  if (da_voice_client_connect(g_voice) != 0) {
    fprintf(stderr, "Failed to connect\n");
    cleanup();
    return 1;
  }

  /* Keyboard input loop */
  enable_raw_mode();
  while (!g_stop) {
    char c = 0;
    ssize_t nr = read(STDIN_FILENO, &c, 1);
    if (nr <= 0) break;

    /* q or Ctrl-C */
    if (c == 'q' || c == '\x03') {
      printf("\nGoodbye!\n");
      break;
    }

    /* Enter -> toggle recording */
    if (c == '\r' || c == '\n') {
      if (g_recording)
        stop_recording_round();
      else
        start_recording_round();
    }
  }

  cleanup();
  return 0;
}

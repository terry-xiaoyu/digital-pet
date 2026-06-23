/*
 * examples/voice_chat.c
 *
 * Push-to-talk voice conversation with the AI agent via WebSocket.
 *
 * Uses the system `sox` commands `rec` and `play` for audio I/O,
 * matching the approach in the TypeScript voice-chat example.
 *
 * Requirements:
 *   - agent-gateway running with voice channel enabled.
 *   - sox installed:
 *       macOS : brew install sox
 *       Ubuntu: sudo apt install sox
 *   - libwebsockets and libmosquitto available (build with -DA_BUILD_VOICE=ON).
 *
 * Controls:
 *   [Enter]  Start / stop recording
 *   [q]      Quit
 *
 * Environment variables:
 *   VOICE_WS_URL    WebSocket URL          (default: ws://127.0.0.1:3001/ws/voice)
 *   DEVICE_ID       Device ID              (default: product-f60mo4-device-da15ec6584ef)
 *   PRODUCT_ID      Product ID             (default: product-f60mo4)
 *   TTS_SAMPLE_RATE Playback sample rate   (default: 16000)
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
#include <sys/wait.h>

#include "device_agent/voice_client.h"

/* --- Config ---------------------------------------------------------------- */

static const char *g_ws_url         = "ws://127.0.0.1:3001/ws/voice";
static const char *g_device_id      = "product-f60mo4-device-da15ec6584ef";
static const char *g_product_id     = "product-f60mo4";
static int         g_tts_sample_rate = 16000;

/* --- Global state ---------------------------------------------------------- */

static da_voice_client_t *g_voice    = NULL;
static volatile int       g_stop     = 0;
static volatile int       g_recording = 0;
static FILE              *g_rec_pipe  = NULL;

/* TTS audio buffer */
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
    int16_t *nb = realloc(g_tts_buf, new_cap * sizeof(int16_t));
    if (nb) { g_tts_buf = nb; g_tts_cap = new_cap; }
  }
  if (g_tts_len + n <= g_tts_cap) {
    memcpy(g_tts_buf + g_tts_len, pcm, n * sizeof(int16_t));
    g_tts_len += n;
  }
  pthread_mutex_unlock(&g_tts_mutex);
}

static void tts_buf_clear(void) {
  pthread_mutex_lock(&g_tts_mutex);
  g_tts_len = 0;
  pthread_mutex_unlock(&g_tts_mutex);
}

/* --- Audio helpers using sox ----------------------------------------------- */

static void start_recording(void) {
  /* rec: raw PCM Int16LE, 16 kHz, mono */
  g_rec_pipe = popen(
    "rec -r 16000 -b 16 -c 1 -e signed-integer -t raw -q -",
    "r");
  if (!g_rec_pipe) {
    fprintf(stderr, "[voice] popen(rec) failed. Is sox installed?\n");
    return;
  }
}

static void stop_recording_pipe(void) {
  if (g_rec_pipe) {
    pclose(g_rec_pipe);
    g_rec_pipe = NULL;
  }
}

static void play_tts_buffer(void) {
  pthread_mutex_lock(&g_tts_mutex);
  size_t  n      = g_tts_len;
  int16_t *audio = malloc(n * sizeof(int16_t));
  if (audio) memcpy(audio, g_tts_buf, n * sizeof(int16_t));
  pthread_mutex_unlock(&g_tts_mutex);

  if (!audio || n == 0) {
    free(audio);
    return;
  }

  /* play: raw PCM from stdin */
  char cmd[256];
  snprintf(cmd, sizeof(cmd),
           "play -r %d -b 16 -c 1 -e signed-integer -t raw -q -",
           g_tts_sample_rate);
  FILE *play_pipe = popen(cmd, "w");
  if (!play_pipe) {
    fprintf(stderr, "[voice] popen(play) failed. Is sox installed?\n");
  } else {
    fwrite(audio, sizeof(int16_t), n, play_pipe);
    pclose(play_pipe);
  }
  free(audio);
}

/* --- Recording thread ----------------------------------------------------- */

static void *record_thread_fn(void *arg) {
  (void)arg;
  uint8_t chunk[4096];

  while (g_recording && g_rec_pipe) {
    size_t n = fread(chunk, 1, sizeof(chunk), g_rec_pipe);
    if (n == 0) break;
    /* Send Int16LE samples to voice client */
    size_t n_samples = n / 2;
    if (n_samples > 0)
      da_voice_client_send_audio(g_voice, (const int16_t *)chunk, n_samples);
  }
  return NULL;
}

static pthread_t g_rec_thread;

static void start_recording_round(void) {
  g_recording = 1;
  printf("\n  [REC] Recording... (press Enter to stop)\n");

  da_voice_client_start_listening(g_voice, "manual");
  start_recording();

  if (pthread_create(&g_rec_thread, NULL, record_thread_fn, NULL) != 0)
    fprintf(stderr, "[voice] failed to start recording thread\n");
}

static void stop_recording_round(void) {
  g_recording = 0;
  stop_recording_pipe();
  pthread_join(g_rec_thread, NULL);
  printf("  [STOP] Processing...\n");
  da_voice_client_stop_listening(g_voice);
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
  /* Overwrite line with streaming ASR text */
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
  tts_buf_clear();
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
    stop_recording_pipe();
  }
  if (g_voice) {
    da_voice_client_disconnect(g_voice);
    da_voice_client_free(g_voice);
    g_voice = NULL;
  }
  free(g_tts_buf);
}

static void handle_signal(int sig) {
  (void)sig;
  g_stop = 1;
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
  const char *tts_sr  = getenv("TTS_SAMPLE_RATE");
  if (tts_sr) g_tts_sample_rate = atoi(tts_sr);

  printf("Voice Chat Example\n");
  printf("==================\n");
  printf("Server  : %s\n", ws_url);
  printf("Product : %s\n", product);
  printf("Device  : %s\n", device);
  printf("TTS Hz  : %d\n\n", g_tts_sample_rate);
  if (voice_tls_ca && voice_tls_ca[0])
    printf("TLS CA  : %s\n", voice_tls_ca);
  if (voice_tls_sni && voice_tls_sni[0])
    printf("TLS SNI : %s\n", voice_tls_sni);

  da_voice_options_t opts = {
    .ws_url     = ws_url,
    .device_id  = device,
    .product_id = product,
    .tls_ca_file = voice_tls_ca,
    .tls_cert_file = voice_tls_cert,
    .tls_key_file = voice_tls_key,
    .tls_server_name = voice_tls_sni,
    .tls_insecure = voice_tls_insecure ? atoi(voice_tls_insecure) : 0,
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

  signal(SIGINT,  handle_signal);
  signal(SIGTERM, handle_signal);

  printf("Connecting to %s...\n", ws_url);
  if (da_voice_client_connect(g_voice) != 0) {
    fprintf(stderr, "Failed to connect\n");
    da_voice_client_free(g_voice);
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

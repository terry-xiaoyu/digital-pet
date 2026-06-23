/**
 * device_agent/voice_client.h
 *
 * WebSocket voice client for the Device Agent C SDK.
 * Implements the xiaozhi-esp32 binary protocol v3 over libwebsockets.
 *
 * Binary frame layout (4-byte header, big-endian):
 *   [0]   type: 0=audio, 1=json
 *   [1]   reserved
 *   [2-3] payload length (uint16, big-endian)
 *   [4..] payload bytes
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* --- State machine --------------------------------------------------------- */

typedef enum da_voice_state {
  DA_VOICE_DISCONNECTED = 0,
  DA_VOICE_CONNECTING,
  DA_VOICE_CONNECTED,
  DA_VOICE_LISTENING,
  DA_VOICE_PROCESSING,
  DA_VOICE_PLAYING,
} da_voice_state_t;

/* --- Options --------------------------------------------------------------- */

typedef struct da_voice_options {
  /** WebSocket URL, e.g. "ws://127.0.0.1:3001/ws/voice" */
  const char *ws_url;
  /** Device ID for schema context, or NULL */
  const char *device_id;
  /** Product ID for schema context, or NULL */
  const char *product_id;
  /** TTS voice ID (provider-specific), or NULL -> server default */
  const char *voice_type;
  /** ASR/TTS provider: "volcengine" | "aliyun" | "aws" | "elevenlabs", or NULL */
  const char *provider;
  /** CA certificate bundle path for WSS server verification, or NULL */
  const char *tls_ca_file;
  /** Client certificate path for WSS mTLS, or NULL */
  const char *tls_cert_file;
  /** Client private key path for WSS mTLS, or NULL */
  const char *tls_key_file;
  /** Optional TLS server name (SNI + hostname verify target), NULL -> ws host */
  const char *tls_server_name;
  /** Disable WSS certificate verification (0 = secure default, non-zero = insecure) */
  int tls_insecure;
  /** Audio input sample rate.  0 -> default (16000 Hz) */
  int sample_rate;
  /** Audio input channels.  0 -> default (1 = mono) */
  int channels;
} da_voice_options_t;

/* --- Callbacks ------------------------------------------------------------- */

/** Called once the hello handshake completes successfully. */
typedef void (*da_voice_on_connected_fn)(const char *session_id,
                                          void       *user_data);

/** Called when the WebSocket connection closes. */
typedef void (*da_voice_on_disconnected_fn)(const char *reason,
                                             void       *user_data);

/** Called on every state machine transition. */
typedef void (*da_voice_on_state_fn)(da_voice_state_t state,
                                      void            *user_data);

/**
 * Called for each ASR (speech-to-text) result chunk.
 *
 * @param text     Recognised text (partial or final).
 * @param definite true = final result for this utterance.
 * @param task_id  Task correlation ID, may be NULL.
 */
typedef void (*da_voice_on_asr_fn)(const char *text,
                                    bool        definite,
                                    const char *task_id,
                                    void       *user_data);

/**
 * Called when the AI agent produces a reply text chunk.
 *
 * @param text      Reply text fragment.
 * @param streaming true = more fragments follow; false = reply is complete.
 * @param task_id   Task correlation ID, may be NULL.
 */
typedef void (*da_voice_on_agent_reply_fn)(const char *text,
                                            bool        streaming,
                                            const char *task_id,
                                            void       *user_data);

/**
 * Called with each TTS audio chunk (PCM Int16LE samples).
 *
 * @param pcm       Pointer to Int16LE audio samples.
 * @param n_samples Number of int16_t values in pcm.
 * @param task_id   Task correlation ID, may be NULL.
 */
typedef void (*da_voice_on_tts_audio_fn)(const int16_t *pcm,
                                          size_t         n_samples,
                                          const char    *task_id,
                                          void          *user_data);

/**
 * Called when TTS playback for a task is complete.
 *
 * @param task_id Task correlation ID, may be NULL.
 */
typedef void (*da_voice_on_tts_complete_fn)(const char *task_id,
                                             void       *user_data);

/** Called on any error. */
typedef void (*da_voice_on_error_fn)(const char *message, void *user_data);

typedef struct da_voice_callbacks {
  da_voice_on_connected_fn    on_connected;
  da_voice_on_disconnected_fn on_disconnected;
  da_voice_on_state_fn        on_state;
  da_voice_on_asr_fn          on_asr;
  da_voice_on_agent_reply_fn  on_agent_reply;
  da_voice_on_tts_audio_fn    on_tts_audio;
  da_voice_on_tts_complete_fn on_tts_complete;
  da_voice_on_error_fn        on_error;
  void                       *user_data;
} da_voice_callbacks_t;

/* --- Client handle --------------------------------------------------------- */

typedef struct da_voice_client da_voice_client_t;

/**
 * Create a new voice client (does not connect yet).
 *
 * @param options   Configuration options (deep-copied).
 * @param callbacks Event callbacks (copied by value; user_data pointer stored).
 * @return Heap-allocated client, or NULL on allocation failure.
 */
da_voice_client_t *da_voice_client_new(const da_voice_options_t  *options,
                                        const da_voice_callbacks_t *callbacks);

/** Free all resources.  Calls da_voice_client_disconnect() if connected. */
void da_voice_client_free(da_voice_client_t *client);

/**
 * Open the WebSocket connection and perform the hello handshake.
 * Blocks until the handshake completes or fails.
 * Starts an internal service thread; do NOT call da_voice_client_service() manually.
 *
 * @return 0 on success, -1 on failure.
 */
int da_voice_client_connect(da_voice_client_t *client);

/**
 * Send a goodbye frame and close the WebSocket.
 * Blocks until the service thread exits.
 */
void da_voice_client_disconnect(da_voice_client_t *client);

/**
 * Begin a listening round (tell the server to start receiving audio).
 *
 * @param mode  "manual" - caller calls da_voice_client_stop_listening() to end;
 *              "auto"   - server detects end of speech automatically.
 * @return A task ID string (valid until the next call - copy if you need it).
 */
const char *da_voice_client_start_listening(da_voice_client_t *client,
                                             const char        *mode);

/**
 * Send a chunk of PCM audio to the server.
 * Must be called while in LISTENING state.
 *
 * @param pcm       Pointer to Int16LE audio samples (16 kHz mono by default).
 * @param n_samples Number of int16_t values to send.
 * @return 0 on success, -1 on failure.
 */
int da_voice_client_send_audio(da_voice_client_t *client,
                                const int16_t     *pcm,
                                size_t             n_samples);

/**
 * Signal the end of the audio input; triggers ASR finalization and agent
 * processing.
 */
void da_voice_client_stop_listening(da_voice_client_t *client);

/**
 * Abort the current agent reply / TTS playback.
 *
 * @param start_new_round If true, immediately starts a new listening round and
 *                        returns the new task ID string (valid until next call).
 *                        If false, returns NULL.
 */
const char *da_voice_client_abort(da_voice_client_t *client,
                                   bool               start_new_round);

/** Return the current client state. */
da_voice_state_t da_voice_client_get_state(const da_voice_client_t *client);

/** Return the session ID assigned by the server (NULL if not yet connected). */
const char *da_voice_client_get_session_id(const da_voice_client_t *client);

#ifdef __cplusplus
}
#endif

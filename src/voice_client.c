/*
 * device_agent/voice_client.c
 *
 * WebSocket voice client implementation using libwebsockets.
 *
 * Threading model:
 *   A single background thread runs lws_service() continuously.
 *   All outgoing messages are placed in a mutex-protected ring queue.
 *   After enqueueing, lws_cancel_service() is called to wake the service
 *   thread, which then calls lws_callback_on_writable() and drains the queue
 *   inside LWS_CALLBACK_CLIENT_WRITEABLE.
 *
 * da_voice_client_connect() blocks until the server hello handshake
 * completes or the connection fails, using a mutex+condvar.
 */

#include "device_agent/voice_client.h"

#if defined(__has_include)
#  if __has_include(<cjson/cJSON.h>)
#    include <cjson/cJSON.h>
#  elif __has_include(<cJSON.h>)
#    include <cJSON.h>
#  else
#    error "cJSON header not found"
#  endif
#else
#  include <cJSON.h>
#endif
#include <libwebsockets.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* --- Binary protocol v3 ---------------------------------------------------- */

#define PROTO_HEADER_SIZE 4
#define PROTO_TYPE_AUDIO  0
#define PROTO_TYPE_JSON   1

static void encode_header(uint8_t *hdr, uint8_t type, uint16_t payload_len) {
  hdr[0] = type;
  hdr[1] = 0; /* reserved */
  hdr[2] = (uint8_t)(payload_len >> 8);
  hdr[3] = (uint8_t)(payload_len & 0xFF);
}

/* --- Send queue ------------------------------------------------------------ */

#define SEND_QUEUE_CAPACITY 64

typedef struct {
  uint8_t *buf;     /* LWS_PRE bytes + header/payload */
  size_t   len;     /* payload length (after LWS_PRE) */
  int      lws_flags; /* LWS_WRITE_BINARY or LWS_WRITE_TEXT */
} send_item_t;

typedef struct {
  send_item_t     items[SEND_QUEUE_CAPACITY];
  int             head, tail, count;
  pthread_mutex_t mutex;
} send_queue_t;

static int sq_push(send_queue_t *sq, const uint8_t *payload, size_t len, int flags) {
  pthread_mutex_lock(&sq->mutex);
  if (sq->count >= SEND_QUEUE_CAPACITY) {
    pthread_mutex_unlock(&sq->mutex);
    return -1;
  }
  send_item_t *it = &sq->items[sq->tail];
  it->buf = malloc(LWS_PRE + len);
  if (!it->buf) {
    pthread_mutex_unlock(&sq->mutex);
    return -1;
  }
  memcpy(it->buf + LWS_PRE, payload, len);
  it->len       = len;
  it->lws_flags = flags;
  sq->tail      = (sq->tail + 1) % SEND_QUEUE_CAPACITY;
  sq->count++;
  pthread_mutex_unlock(&sq->mutex);
  return 0;
}

static int sq_pop(send_queue_t *sq, send_item_t *out) {
  pthread_mutex_lock(&sq->mutex);
  if (sq->count == 0) {
    pthread_mutex_unlock(&sq->mutex);
    return -1;
  }
  *out = sq->items[sq->head];
  sq->head  = (sq->head + 1) % SEND_QUEUE_CAPACITY;
  sq->count--;
  pthread_mutex_unlock(&sq->mutex);
  return 0;
}

static int sq_peek_count(send_queue_t *sq) {
  pthread_mutex_lock(&sq->mutex);
  int n = sq->count;
  pthread_mutex_unlock(&sq->mutex);
  return n;
}

/* --- Client struct --------------------------------------------------------- */

struct da_voice_client {
  /* Options (deep-copied strings) */
  char ws_host[256];
  int  ws_port;
  char ws_path[512];
  bool ws_tls;
  char device_id[256];
  char product_id[256];
  char voice_type[128];
  char provider[64];
  char tls_ca_file[512];
  char tls_cert_file[512];
  char tls_key_file[512];
  char tls_server_name[256];
  bool tls_insecure;
  int  sample_rate;
  int  channels;

  /* Callbacks */
  da_voice_callbacks_t cbs;

  /* libwebsockets */
  struct lws_context *ctx;
  struct lws         *wsi;   /* set in CLIENT_ESTABLISHED, cleared in CLOSED */

  /* Service thread */
  pthread_t   svc_thread;
  atomic_bool running;

  /* State */
  da_voice_state_t state;
  char             session_id[128];
  char             current_task_id[64];
  uint64_t         task_counter;

  /* Handshake synchronization */
  bool            hello_settled;
  int             hello_rc;
  char            hello_error[256];
  pthread_mutex_t hello_mutex;
  pthread_cond_t  hello_cond;

  /* Send queue */
  send_queue_t sq;

  /* Receive reassembly buffer */
  uint8_t *rx_buf;
  size_t   rx_len;
  size_t   rx_cap;
};

/* --- Helpers --------------------------------------------------------------- */

static void set_state(struct da_voice_client *c, da_voice_state_t next) {
  if (c->state == next) return;
  c->state = next;
  if (c->cbs.on_state) c->cbs.on_state(next, c->cbs.user_data);
}

static void send_json_str(struct da_voice_client *c, const char *str) {
  size_t len = strlen(str);
  sq_push(&c->sq, (const uint8_t *)str, len, LWS_WRITE_TEXT);
  if (c->ctx) lws_cancel_service(c->ctx);
}

static void send_json(struct da_voice_client *c, cJSON *root) {
  char *str = cJSON_PrintUnformatted(root);
  if (str) {
    send_json_str(c, str);
    free(str);
  }
}

static const char *new_task_id(struct da_voice_client *c) {
  c->task_counter++;
  snprintf(c->current_task_id, sizeof(c->current_task_id),
           "task-%llu", (unsigned long long)c->task_counter);
  return c->current_task_id;
}

/* --- Receive buffer helpers ----------------------------------------------- */

static int rx_append(struct da_voice_client *c, const uint8_t *data, size_t len) {
  if (c->rx_len + len > c->rx_cap) {
    size_t new_cap = (c->rx_cap == 0) ? 8192 : c->rx_cap * 2;
    while (new_cap < c->rx_len + len) new_cap *= 2;
    uint8_t *nb = realloc(c->rx_buf, new_cap);
    if (!nb) return -1;
    c->rx_buf = nb;
    c->rx_cap = new_cap;
  }
  memcpy(c->rx_buf + c->rx_len, data, len);
  c->rx_len += len;
  return 0;
}

static void rx_clear(struct da_voice_client *c) { c->rx_len = 0; }

/* --- Message processing --------------------------------------------------- */

static void handle_json_message(struct da_voice_client *c,
                                  const char *raw, size_t len) {
  cJSON *root = cJSON_ParseWithLength(raw, len);
  if (!root) return;

  const cJSON *j_type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(j_type)) { cJSON_Delete(root); return; }
  const char *type = j_type->valuestring;

  if (strcmp(type, "asr") == 0) {
    const cJSON *j_text    = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *j_def     = cJSON_GetObjectItemCaseSensitive(root, "definite");
    const cJSON *j_task_id = cJSON_GetObjectItemCaseSensitive(root, "taskId");
    if (c->cbs.on_asr) {
      c->cbs.on_asr(
        cJSON_IsString(j_text) ? j_text->valuestring : "",
        cJSON_IsTrue(j_def),
        cJSON_IsString(j_task_id) ? j_task_id->valuestring : NULL,
        c->cbs.user_data);
    }
  } else if (strcmp(type, "agent_reply") == 0) {
    const cJSON *j_text      = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *j_streaming = cJSON_GetObjectItemCaseSensitive(root, "streaming");
    const cJSON *j_task_id   = cJSON_GetObjectItemCaseSensitive(root, "taskId");
    bool streaming = cJSON_IsTrue(j_streaming);
    if (c->cbs.on_agent_reply) {
      c->cbs.on_agent_reply(
        cJSON_IsString(j_text) ? j_text->valuestring : "",
        streaming,
        cJSON_IsString(j_task_id) ? j_task_id->valuestring : NULL,
        c->cbs.user_data);
    }
    if (!streaming) set_state(c, DA_VOICE_PLAYING);
  } else if (strcmp(type, "tts_complete") == 0) {
    const cJSON *j_task_id = cJSON_GetObjectItemCaseSensitive(root, "taskId");
    if (c->cbs.on_tts_complete) {
      c->cbs.on_tts_complete(
        cJSON_IsString(j_task_id) ? j_task_id->valuestring : NULL,
        c->cbs.user_data);
    }
    set_state(c, DA_VOICE_CONNECTED);
  }

  cJSON_Delete(root);
}

static void handle_binary_message(struct da_voice_client *c,
                                    const uint8_t *data, size_t len) {
  if (len < PROTO_HEADER_SIZE) return;
  uint8_t  frame_type  = data[0];
  uint16_t payload_len = (uint16_t)((data[2] << 8) | data[3]);
  if (len < (size_t)(PROTO_HEADER_SIZE + payload_len)) return;

  const uint8_t *payload = data + PROTO_HEADER_SIZE;

  if (frame_type == PROTO_TYPE_AUDIO) {
    /* PCM Int16LE */
    size_t n_samples = payload_len / 2;
    if (c->cbs.on_tts_audio && n_samples > 0) {
      /* Ensure aligned read */
      int16_t *pcm = malloc(payload_len);
      if (pcm) {
        memcpy(pcm, payload, payload_len);
        c->cbs.on_tts_audio(pcm, n_samples,
                             c->current_task_id[0] ? c->current_task_id : NULL,
                             c->cbs.user_data);
        free(pcm);
      }
    }
  } else if (frame_type == PROTO_TYPE_JSON) {
    handle_json_message(c, (const char *)payload, payload_len);
  }
}

/* --- libwebsockets protocol callback -------------------------------------- */

static int lws_protocol_cb(struct lws *wsi,
                             enum lws_callback_reasons reason,
                             void *user, void *in, size_t len) {
  struct da_voice_client *c =
    (struct da_voice_client *)lws_context_user(lws_get_context(wsi));
  (void)user;

  switch (reason) {

  case LWS_CALLBACK_CLIENT_ESTABLISHED: {
    c->wsi = wsi;
    /* Send client hello */
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "type",    "hello");
    cJSON_AddNumberToObject(hello, "version", 3);
    cJSON *audio = cJSON_CreateObject();
    cJSON_AddStringToObject(audio, "format",      "pcm");
    cJSON_AddNumberToObject(audio, "sample_rate",  c->sample_rate);
    cJSON_AddNumberToObject(audio, "channels",     c->channels);
    cJSON_AddItemToObject  (hello, "audio_params", audio);
    if (c->voice_type[0]) cJSON_AddStringToObject(hello, "voice_type", c->voice_type);
    if (c->provider[0])   cJSON_AddStringToObject(hello, "provider",   c->provider);
    if (c->device_id[0])  cJSON_AddStringToObject(hello, "deviceId",   c->device_id);
    if (c->product_id[0]) cJSON_AddStringToObject(hello, "productId",  c->product_id);
    send_json(c, hello);
    cJSON_Delete(hello);
    lws_callback_on_writable(wsi);
    break;
  }

  case LWS_CALLBACK_CLIENT_RECEIVE: {
    bool is_binary = lws_frame_is_binary(wsi);

    /* Accumulate fragments */
    rx_append(c, (const uint8_t *)in, len);

    if (!lws_is_final_fragment(wsi)) break; /* wait for more */

    /* Process complete message */
    if (is_binary) {
      /* First check if it is a hello reply wrapping JSON in binary */
      handle_binary_message(c, c->rx_buf, c->rx_len);
    } else {
      /* Text frame */
      const char *raw = (const char *)c->rx_buf;
      size_t      raw_len = c->rx_len;

      /* First text frame after connect may be the server hello */
      if (!c->hello_settled) {
        cJSON *root = cJSON_ParseWithLength(raw, raw_len);
        if (root) {
          const cJSON *j_type = cJSON_GetObjectItemCaseSensitive(root, "type");
          const cJSON *j_sid  = cJSON_GetObjectItemCaseSensitive(root, "session_id");
          if (cJSON_IsString(j_type) && strcmp(j_type->valuestring, "hello") == 0 &&
              cJSON_IsString(j_sid)) {
            strncpy(c->session_id, j_sid->valuestring, sizeof(c->session_id) - 1);
            set_state(c, DA_VOICE_CONNECTED);
            pthread_mutex_lock(&c->hello_mutex);
            c->hello_settled = true;
            c->hello_rc      = 0;
            pthread_cond_signal(&c->hello_cond);
            pthread_mutex_unlock(&c->hello_mutex);
            if (c->cbs.on_connected)
              c->cbs.on_connected(c->session_id, c->cbs.user_data);
            cJSON_Delete(root);
            rx_clear(c);
            break;
          }
          cJSON_Delete(root);
        }
      }
      handle_json_message(c, raw, raw_len);
    }
    rx_clear(c);
    break;
  }

  case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
    /* Woken by lws_cancel_service() -- request writable if queue non-empty */
    if (c->wsi && sq_peek_count(&c->sq) > 0)
      lws_callback_on_writable(c->wsi);
    break;

  case LWS_CALLBACK_CLIENT_WRITEABLE: {
    send_item_t it;
    while (sq_pop(&c->sq, &it) == 0) {
      lws_write(wsi, it.buf + LWS_PRE, it.len, (enum lws_write_protocol)it.lws_flags);
      free(it.buf);
      if (sq_peek_count(&c->sq) > 0)
        lws_callback_on_writable(wsi);
      break; /* send one frame per writeable callback */
    }
    break;
  }

  case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: {
    const char *err = in ? (const char *)in : "connection error";
    if (!c->hello_settled) {
      pthread_mutex_lock(&c->hello_mutex);
      c->hello_settled = true;
      c->hello_rc      = -1;
      strncpy(c->hello_error, err, sizeof(c->hello_error) - 1);
      pthread_cond_signal(&c->hello_cond);
      pthread_mutex_unlock(&c->hello_mutex);
    }
    if (c->cbs.on_error) c->cbs.on_error(err, c->cbs.user_data);
    c->wsi = NULL;
    break;
  }

  case LWS_CALLBACK_CLIENT_CLOSED: {
    c->wsi = NULL;
    set_state(c, DA_VOICE_DISCONNECTED);
    if (!c->hello_settled) {
      pthread_mutex_lock(&c->hello_mutex);
      c->hello_settled = true;
      c->hello_rc      = -1;
      strncpy(c->hello_error, "closed before hello", sizeof(c->hello_error) - 1);
      pthread_cond_signal(&c->hello_cond);
      pthread_mutex_unlock(&c->hello_mutex);
    }
    if (c->cbs.on_disconnected)
      c->cbs.on_disconnected("closed", c->cbs.user_data);
    break;
  }

  default:
    break;
  }
  return 0;
}

static struct lws_protocols g_protocols[] = {
  { "voice-protocol-v3", lws_protocol_cb, 0, 65536, 0, NULL, 0 },
  LWS_PROTOCOL_LIST_TERM
};

/* --- Service thread -------------------------------------------------------- */

static void *svc_thread_fn(void *arg) {
  struct da_voice_client *c = (struct da_voice_client *)arg;
  while (atomic_load(&c->running)) {
    lws_service(c->ctx, 50 /* timeout_ms */);
  }
  return NULL;
}

/* --- Public API ------------------------------------------------------------ */

da_voice_client_t *da_voice_client_new(const da_voice_options_t  *opts,
                                        const da_voice_callbacks_t *cbs) {
  if (!opts || !opts->ws_url) return NULL;

  struct da_voice_client *c = calloc(1, sizeof(*c));
  if (!c) return NULL;

  /* Parse ws_url -> host, port, path, tls */
  const char *url = opts->ws_url;
  c->ws_tls = false;
  c->ws_port = 80;
  const char *after_scheme = url;
  if (strncmp(url, "wss://", 6) == 0) {
    c->ws_tls  = true;
    c->ws_port = 443;
    after_scheme = url + 6;
  } else if (strncmp(url, "ws://", 5) == 0) {
    after_scheme = url + 5;
  }
  const char *slash = strchr(after_scheme, '/');
  const char *colon = strchr(after_scheme, ':');
  if (colon && (!slash || colon < slash)) {
    int hlen = (int)(colon - after_scheme);
    if (hlen > 0 && hlen < (int)sizeof(c->ws_host) - 1) {
      strncpy(c->ws_host, after_scheme, (size_t)hlen);
      c->ws_host[hlen] = '\0';
    }
    c->ws_port = atoi(colon + 1);
  } else if (slash) {
    int hlen = (int)(slash - after_scheme);
    if (hlen > 0 && hlen < (int)sizeof(c->ws_host) - 1) {
      strncpy(c->ws_host, after_scheme, (size_t)hlen);
      c->ws_host[hlen] = '\0';
    }
  } else {
    strncpy(c->ws_host, after_scheme, sizeof(c->ws_host) - 1);
  }
  if (slash)
    strncpy(c->ws_path, slash, sizeof(c->ws_path) - 1);
  else
    strncpy(c->ws_path, "/", sizeof(c->ws_path) - 1);

  if (opts->device_id)  strncpy(c->device_id,  opts->device_id,  sizeof(c->device_id)  - 1);
  if (opts->product_id) strncpy(c->product_id, opts->product_id, sizeof(c->product_id) - 1);
  if (opts->voice_type) strncpy(c->voice_type, opts->voice_type, sizeof(c->voice_type) - 1);
  if (opts->provider)   strncpy(c->provider,   opts->provider,   sizeof(c->provider)   - 1);
  if (opts->tls_ca_file) strncpy(c->tls_ca_file, opts->tls_ca_file, sizeof(c->tls_ca_file) - 1);
  if (opts->tls_cert_file) strncpy(c->tls_cert_file, opts->tls_cert_file, sizeof(c->tls_cert_file) - 1);
  if (opts->tls_key_file) strncpy(c->tls_key_file, opts->tls_key_file, sizeof(c->tls_key_file) - 1);
  if (opts->tls_server_name) strncpy(c->tls_server_name, opts->tls_server_name, sizeof(c->tls_server_name) - 1);
  c->tls_insecure = (opts->tls_insecure != 0);

  c->sample_rate = (opts->sample_rate > 0) ? opts->sample_rate : 16000;
  c->channels    = (opts->channels    > 0) ? opts->channels    : 1;

  if (cbs) c->cbs = *cbs;

  pthread_mutex_init(&c->hello_mutex, NULL);
  pthread_cond_init (&c->hello_cond,  NULL);
  pthread_mutex_init(&c->sq.mutex,    NULL);

  atomic_init(&c->running, false);
  c->state = DA_VOICE_DISCONNECTED;
  return c;
}

void da_voice_client_free(da_voice_client_t *c) {
  if (!c) return;
  da_voice_client_disconnect(c);
  free(c->rx_buf);
  pthread_mutex_destroy(&c->hello_mutex);
  pthread_cond_destroy (&c->hello_cond);
  pthread_mutex_destroy(&c->sq.mutex);
  free(c);
}

int da_voice_client_connect(da_voice_client_t *c) {
  if (atomic_load(&c->running)) return -1; /* already connected */

  /* Reset handshake state */
  c->hello_settled = false;
  c->hello_rc      = 0;
  c->hello_error[0]= '\0';
  c->session_id[0] = '\0';
  c->wsi           = NULL;
  set_state(c, DA_VOICE_CONNECTING);

  /* Create LWS context */
  struct lws_context_creation_info info;
  memset(&info, 0, sizeof(info));
  info.options   = LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
  info.port      = CONTEXT_PORT_NO_LISTEN;
  info.protocols = g_protocols;
  info.user      = c; /* accessible via lws_context_user() */
  info.client_ssl_ca_filepath = c->tls_ca_file[0] ? c->tls_ca_file : NULL;
  info.client_ssl_cert_filepath = c->tls_cert_file[0] ? c->tls_cert_file : NULL;
  info.client_ssl_private_key_filepath = c->tls_key_file[0] ? c->tls_key_file : NULL;

  c->ctx = lws_create_context(&info);
  if (!c->ctx) {
    fprintf(stderr, "[voice] lws_create_context failed\n");
    return -1;
  }

  /* Start service thread */
  atomic_store(&c->running, true);
  if (pthread_create(&c->svc_thread, NULL, svc_thread_fn, c) != 0) {
    lws_context_destroy(c->ctx);
    c->ctx = NULL;
    atomic_store(&c->running, false);
    return -1;
  }

  /* Initiate connection */
  struct lws_client_connect_info ci;
  memset(&ci, 0, sizeof(ci));
  ci.context        = c->ctx;
  ci.address        = c->ws_host;
  ci.port           = c->ws_port;
  ci.path           = c->ws_path;
  ci.host           = c->tls_server_name[0] ? c->tls_server_name : c->ws_host;
  ci.origin         = c->ws_host;
  ci.protocol       = g_protocols[0].name;
  ci.ssl_connection = c->ws_tls ? LCCSCF_USE_SSL : 0;
#ifdef LCCSCF_USE_SSL_CLIENT_CERTS
  if (c->tls_cert_file[0] && c->tls_key_file[0])
    ci.ssl_connection |= LCCSCF_USE_SSL_CLIENT_CERTS;
#endif
#ifdef LCCSCF_ALLOW_INSECURE
  if (c->tls_insecure)
    ci.ssl_connection |= LCCSCF_ALLOW_INSECURE;
#endif

  if (!lws_client_connect_via_info(&ci)) {
    fprintf(stderr,
            "[voice] lws_client_connect_via_info failed (host=%s port=%d path=%s tls=%d sni=%s ca=%s)\n",
            c->ws_host,
            c->ws_port,
            c->ws_path,
            c->ws_tls ? 1 : 0,
            c->tls_server_name[0] ? c->tls_server_name : c->ws_host,
            c->tls_ca_file[0] ? c->tls_ca_file : "<system>");
    atomic_store(&c->running, false);
    pthread_join(c->svc_thread, NULL);
    lws_context_destroy(c->ctx);
    c->ctx = NULL;
    return -1;
  }

  /* Block until hello handshake */
  pthread_mutex_lock(&c->hello_mutex);
  while (!c->hello_settled)
    pthread_cond_wait(&c->hello_cond, &c->hello_mutex);
  int rc = c->hello_rc;
  if (rc != 0)
    fprintf(stderr, "[voice] handshake failed: %s\n", c->hello_error);
  pthread_mutex_unlock(&c->hello_mutex);

  if (rc != 0) {
    atomic_store(&c->running, false);
    pthread_join(c->svc_thread, NULL);
    lws_context_destroy(c->ctx);
    c->ctx = NULL;
  }
  return rc;
}

void da_voice_client_disconnect(da_voice_client_t *c) {
  if (!atomic_load(&c->running)) return;

  /* Send goodbye before closing */
  if (c->wsi && c->session_id[0]) {
    cJSON *bye = cJSON_CreateObject();
    cJSON_AddStringToObject(bye, "type",       "goodbye");
    cJSON_AddStringToObject(bye, "session_id", c->session_id);
    send_json(c, bye);
    cJSON_Delete(bye);
    /* small pause to let the frame queue drain */
    struct timespec t = { .tv_sec = 0, .tv_nsec = 100000000L };
    nanosleep(&t, NULL);
  }

  atomic_store(&c->running, false);
  if (c->ctx) lws_cancel_service(c->ctx);
  pthread_join(c->svc_thread, NULL);

  if (c->ctx) {
    lws_context_destroy(c->ctx);
    c->ctx = NULL;
  }
  c->wsi = NULL;
  set_state(c, DA_VOICE_DISCONNECTED);
}

const char *da_voice_client_start_listening(da_voice_client_t *c,
                                              const char        *mode) {
  const char *tid = new_task_id(c);
  set_state(c, DA_VOICE_LISTENING);
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type",   "listen");
  cJSON_AddStringToObject(msg, "mode",   mode ? mode : "manual");
  cJSON_AddStringToObject(msg, "taskId", tid);
  send_json(c, msg);
  cJSON_Delete(msg);
  return tid;
}

int da_voice_client_send_audio(da_voice_client_t *client,
                                const int16_t     *pcm,
                                size_t             n_samples) {
  if (!pcm || n_samples == 0) return -1;
  size_t   payload_len = PROTO_HEADER_SIZE + n_samples * sizeof(int16_t);
  uint8_t *buf         = malloc(payload_len);
  if (!buf) return -1;
  encode_header(buf, PROTO_TYPE_AUDIO, (uint16_t)(n_samples * sizeof(int16_t)));
  memcpy(buf + PROTO_HEADER_SIZE, pcm, n_samples * sizeof(int16_t));
  int rc = sq_push(&client->sq, buf, payload_len, LWS_WRITE_BINARY);
  free(buf);
  if (rc == 0 && client->ctx) lws_cancel_service(client->ctx);
  return rc;
}

void da_voice_client_stop_listening(da_voice_client_t *c) {
  set_state(c, DA_VOICE_PROCESSING);
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type",   "stop");
  if (c->current_task_id[0])
    cJSON_AddStringToObject(msg, "taskId", c->current_task_id);
  send_json(c, msg);
  cJSON_Delete(msg);
}

const char *da_voice_client_abort(da_voice_client_t *c, bool start_new_round) {
  cJSON *msg = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "type",   "abort");
  if (c->current_task_id[0])
    cJSON_AddStringToObject(msg, "taskId", c->current_task_id);
  send_json(c, msg);
  cJSON_Delete(msg);

  if (start_new_round) return da_voice_client_start_listening(c, "manual");

  c->current_task_id[0] = '\0';
  set_state(c, DA_VOICE_CONNECTED);
  return NULL;
}

da_voice_state_t da_voice_client_get_state(const da_voice_client_t *c) {
  return c->state;
}

const char *da_voice_client_get_session_id(const da_voice_client_t *c) {
  return c->session_id[0] ? c->session_id : NULL;
}

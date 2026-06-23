/*
 * device_agent/client.c
 *
 * MQTT device client implementation using Eclipse Mosquitto.
 *
 * Topic conventions:
 *   commands  : device-agent/{productId}/device/{deviceId}/commands
 *   telemetry : v1/{productId}/{deviceId}/telemetry
 *   events    : v1/{productId}/{deviceId}/event
 *   responses : device-agent/{productId}/device/{deviceId}/responses
 */

#include "device_agent/client.h"

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
#include <mosquitto.h>

#include <stdatomic.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

/* --- Constants ------------------------------------------------------------- */

#define DEFAULT_TOPIC_PREFIX      "device-agent"
#define DEFAULT_DATA_PREFIX       "v1"
#define DEFAULT_NAMESPACE         "default"
#define UNKNOWN_PRODUCT_SEGMENT   "_"
#define DEFAULT_HEARTBEAT_MS      30000
#define MAX_TOPIC_LEN             512
#define MAX_SMALL_STR             256
#define MAX_CLIENT_ID_LEN         128
#define MAX_PATH_STR              512
#define DEFAULT_COMMAND_TOPIC_TEMPLATE   "device-agent/{productId}/device/{deviceId}/commands"
#define DEFAULT_RESPONSE_TOPIC_TEMPLATE  "device-agent/{productId}/device/{deviceId}/responses"
#define DEFAULT_TELEMETRY_TOPIC_TEMPLATE "v1/{productId}/{deviceId}/telemetry"
#define DEFAULT_EVENT_TOPIC_TEMPLATE     "v1/{productId}/{deviceId}/event"

/* --- Internal struct ------------------------------------------------------- */

struct da_client {
  /* Connection config */
  char broker_host[MAX_SMALL_STR];
  int  broker_port;
  bool broker_tls;
  char namespace[MAX_SMALL_STR];
  char device_id[MAX_SMALL_STR];
  char product_id[MAX_SMALL_STR];
  char model_id[MAX_SMALL_STR];
  char topic_prefix[MAX_SMALL_STR];
  char data_prefix[MAX_SMALL_STR];
  char username[MAX_SMALL_STR];
  char password[MAX_SMALL_STR];
  char tls_ca_file[MAX_PATH_STR];
  char tls_cert_file[MAX_PATH_STR];
  char tls_key_file[MAX_PATH_STR];
  bool tls_insecure;
  char client_id[MAX_CLIENT_ID_LEN];
  int  heartbeat_interval_ms;

  /* Derived MQTT topics */
  char commands_topic[MAX_TOPIC_LEN];
  char telemetry_topic[MAX_TOPIC_LEN];
  char events_topic[MAX_TOPIC_LEN];
  char responses_topic[MAX_TOPIC_LEN];

  /* Mosquitto handle */
  struct mosquitto *mosq;

  /* Application callbacks */
  da_command_handler_fn     command_handler;
  void                     *command_handler_data;
  da_connection_callbacks_t cbs;

  /* Heartbeat background thread */
  pthread_t    heartbeat_thread;
  atomic_bool  heartbeat_running;
  bool         heartbeat_thread_started;
};

/* --- Helpers --------------------------------------------------------------- */

static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static bool env_truthy(const char *name) {
  const char *v = getenv(name);
  if (!v || !v[0]) return false;
  if (strcmp(v, "1") == 0) return true;
  if (strcmp(v, "true") == 0) return true;
  if (strcmp(v, "TRUE") == 0) return true;
  if (strcmp(v, "yes") == 0) return true;
  if (strcmp(v, "on") == 0) return true;
  return false;
}

static void parse_broker_url(const char *url,
                              char *host_out, int host_cap,
                              int  *port_out,
                              bool *tls_out) {
  *tls_out  = false;
  *port_out = 1883;

  const char *p = url;
  if (strncmp(p, "mqtts://", 8) == 0) {
    *tls_out  = true;
    *port_out = 8883;
    p += 8;
  } else if (strncmp(p, "mqtt://", 7) == 0) {
    p += 7;
  }

  /* host:port  or  host */
  const char *colon = strrchr(p, ':');
  if (colon) {
    int len = (int)(colon - p);
    if (len >= host_cap) len = host_cap - 1;
    strncpy(host_out, p, (size_t)len);
    host_out[len] = '\0';
    *port_out = atoi(colon + 1);
  } else {
    strncpy(host_out, p, (size_t)(host_cap - 1));
    host_out[host_cap - 1] = '\0';
  }
}

static int append_topic_segment(char *out, int cap, int *pos, const char *value) {
  if (!value) value = "";
  while (*value) {
    if (*pos >= cap - 1) {
      if (cap > 0) out[cap - 1] = '\0';
      return -1;
    }
    out[*pos] = *value;
    (*pos)++;
    value++;
  }
  out[*pos] = '\0';
  return 0;
}

static int render_topic_template(const char *template_str,
                                  const char *pid,
                                  const char *did,
                                  char *out,
                                  int cap) {
  int pos = 0;
  if (cap <= 0) return -1;
  out[0] = '\0';

  for (const char *p = template_str; *p;) {
    if (strncmp(p, "{productId}", 11) == 0) {
      if (append_topic_segment(out, cap, &pos, pid) != 0) return -1;
      p += 11;
      continue;
    }
    if (strncmp(p, "{deviceId}", 10) == 0) {
      if (append_topic_segment(out, cap, &pos, did) != 0) return -1;
      p += 10;
      continue;
    }

    if (pos >= cap - 1) {
      out[cap - 1] = '\0';
      return -1;
    }
    out[pos++] = *p++;
    out[pos] = '\0';
  }

  return 0;
}

static const char *select_topic_template(const char *explicit_template,
                                          bool use_prefixed_template,
                                          const char *prefix_template,
                                          const char *default_template) {
  if (explicit_template) return explicit_template;
  if (use_prefixed_template) return prefix_template;
  return default_template;
}

static int publish_json(struct da_client *c, const char *topic, cJSON *root) {
  char *str = cJSON_PrintUnformatted(root);
  if (!str) return -1;
  int rc = mosquitto_publish(c->mosq, NULL, topic,
                              (int)strlen(str), str, 0, false);
  free(str);
  return (rc == MOSQ_ERR_SUCCESS) ? 0 : -1;
}

static void add_default_metadata(struct da_client *c, cJSON *root) {
  cJSON *metadata = cJSON_CreateObject();
  if (!metadata) return;

  if (c->product_id[0] != '\0' && strcmp(c->product_id, UNKNOWN_PRODUCT_SEGMENT) != 0) {
    cJSON_AddStringToObject(metadata, "productId", c->product_id);
  }
  if (c->model_id[0] != '\0') {
    cJSON_AddStringToObject(metadata, "modelId", c->model_id);
  }

  if (metadata->child) {
    cJSON_AddItemToObject(root, "metadata", metadata);
    return;
  }

  cJSON_Delete(metadata);
}

/* --- Mosquitto callbacks --------------------------------------------------- */

static void on_connect_cb(struct mosquitto *mosq, void *ud, int rc) {
  struct da_client *c = (struct da_client *)ud;
  (void)mosq;

  if (rc != 0) {
    if (c->cbs.on_error) {
      char buf[128];
      snprintf(buf, sizeof(buf), "MQTT connect error: %s",
               mosquitto_connack_string(rc));
      c->cbs.on_error(buf, c->cbs.user_data);
    }
    return;
  }

  int sub_rc = mosquitto_subscribe(mosq, NULL, c->commands_topic, 1);
  if (sub_rc != MOSQ_ERR_SUCCESS) {
    if (c->cbs.on_error) {
      char buf[192];
      snprintf(buf, sizeof(buf),
               "MQTT subscribe failed (rc=%d): %s",
               sub_rc, mosquitto_strerror(sub_rc));
      c->cbs.on_error(buf, c->cbs.user_data);
    }
    return;
  }
  da_client_send_status(c, "online", NULL);

  if (c->cbs.on_connect) c->cbs.on_connect(c->cbs.user_data);
}

static void on_disconnect_cb(struct mosquitto *mosq, void *ud, int rc) {
  struct da_client *c = (struct da_client *)ud;
  (void)mosq;
  atomic_store(&c->heartbeat_running, false);
  if (c->cbs.on_disconnect) {
    char reason[160];
    if (rc == 0) {
      snprintf(reason, sizeof(reason), "graceful (rc=0)");
    } else {
      snprintf(reason, sizeof(reason), "rc=%d (%s)", rc, mosquitto_strerror(rc));
    }
    c->cbs.on_disconnect(reason, c->cbs.user_data);
  }
}

static void on_log_cb(struct mosquitto *mosq, void *ud, int level, const char *str) {
  struct da_client *c = (struct da_client *)ud;
  (void)mosq;
  fprintf(stderr, "[da_client][mosq][L%d] %s\n", level, str ? str : "");
  if (c && c->cbs.on_error && str) {
    char msg[512];
    snprintf(msg, sizeof(msg), "mosquitto log L%d: %s", level, str);
    c->cbs.on_error(msg, c->cbs.user_data);
  }
}

static void on_message_cb(struct mosquitto *mosq, void *ud,
                           const struct mosquitto_message *msg) {
  struct da_client *c = (struct da_client *)ud;
  (void)mosq;

  if (!msg->payload || msg->payloadlen == 0) return;
  if (strcmp(msg->topic, c->commands_topic) != 0) return;

  cJSON *root = cJSON_ParseWithLength((const char *)msg->payload,
                                       (size_t)msg->payloadlen);
  if (!root) {
    if (c->cbs.on_error) c->cbs.on_error("Invalid command JSON", c->cbs.user_data);
    return;
  }

  const cJSON *j_rid    = cJSON_GetObjectItemCaseSensitive(root, "requestId");
  const cJSON *j_cmd    = cJSON_GetObjectItemCaseSensitive(root, "cmd");
  const cJSON *j_params = cJSON_GetObjectItemCaseSensitive(root, "params");

  const char *request_id = (cJSON_IsString(j_rid) && j_rid->valuestring)
                             ? j_rid->valuestring : "";
  const char *cmd_name   = (cJSON_IsString(j_cmd) && j_cmd->valuestring)
                             ? j_cmd->valuestring : "";

  char *params_json = j_params ? cJSON_PrintUnformatted(j_params) : NULL;

  da_command_t cmd_obj = {
    .request_id  = request_id,
    .cmd         = cmd_name,
    .params_json = params_json,
  };

  da_response_t resp = { .code = 500, .msg = "no handler", .data_json = NULL };

  if (c->command_handler)
    c->command_handler(&cmd_obj, &resp, c->command_handler_data);

  /* Publish response */
  cJSON *rroot = cJSON_CreateObject();
  cJSON_AddNumberToObject(rroot, "code", resp.code);
  cJSON_AddStringToObject(rroot, "msg",  resp.msg ? resp.msg : "ok");
  cJSON_AddStringToObject(rroot, "requestId", request_id);
  cJSON_AddNumberToObject(rroot, "ts", (double)now_ms());
  add_default_metadata(c, rroot);
  if (resp.data_json) {
    cJSON *data = cJSON_Parse(resp.data_json);
    if (data) cJSON_AddItemToObject(rroot, "data", data);
  }
  publish_json(c, c->responses_topic, rroot);
  cJSON_Delete(rroot);

  free(params_json);
  cJSON_Delete(root);
}

/* --- Heartbeat thread ------------------------------------------------------ */

static void *heartbeat_fn(void *arg) {
  struct da_client *c = (struct da_client *)arg;
  const int tick_ms = 100;
  struct timespec tick = {
    .tv_sec = 0,
    .tv_nsec = tick_ms * 1000000L,
  };
  int elapsed_ms = 0;

  while (atomic_load(&c->heartbeat_running)) {
    nanosleep(&tick, NULL);
    if (!atomic_load(&c->heartbeat_running)) {
      break;
    }

    elapsed_ms += tick_ms;
    if (elapsed_ms >= c->heartbeat_interval_ms) {
      da_client_send_status(c, "online", NULL);
      elapsed_ms = 0;
    }
  }
  return NULL;
}

/* --- Public API ------------------------------------------------------------ */

da_client_t *da_client_new(const da_client_options_t *opts) {
  if (!opts || !opts->broker_url || !opts->device_id)
    return NULL;

  struct da_client *c = calloc(1, sizeof(*c));
  if (!c) return NULL;

  mosquitto_lib_init();

  const char *prefix  = opts->topic_prefix             ? opts->topic_prefix             : DEFAULT_TOPIC_PREFIX;
  const char *dpfx    = opts->device_data_topic_prefix ? opts->device_data_topic_prefix : DEFAULT_DATA_PREFIX;
  const char *product = opts->product_id               ? opts->product_id               : UNKNOWN_PRODUCT_SEGMENT;
  const char *model   = opts->model_id                 ? opts->model_id                 : "";
  const char *ns      = opts->namespace                ? opts->namespace                : DEFAULT_NAMESPACE;
  char prefixed_command_template[MAX_TOPIC_LEN];
  char prefixed_response_template[MAX_TOPIC_LEN];
  char prefixed_telemetry_template[MAX_TOPIC_LEN];
  char prefixed_event_template[MAX_TOPIC_LEN];

  snprintf(prefixed_command_template, sizeof(prefixed_command_template),
           "%s/{productId}/device/{deviceId}/commands", prefix);
  snprintf(prefixed_response_template, sizeof(prefixed_response_template),
           "%s/{productId}/device/{deviceId}/responses", prefix);
  snprintf(prefixed_telemetry_template, sizeof(prefixed_telemetry_template),
           "%s/{productId}/{deviceId}/telemetry", dpfx);
  snprintf(prefixed_event_template, sizeof(prefixed_event_template),
           "%s/{productId}/{deviceId}/event", dpfx);

  const char *commands_template = select_topic_template(
    opts->commands_topic_template,
    opts->topic_prefix != NULL,
    prefixed_command_template,
    DEFAULT_COMMAND_TOPIC_TEMPLATE);
  const char *responses_template = select_topic_template(
    opts->responses_topic_template,
    opts->topic_prefix != NULL,
    prefixed_response_template,
    DEFAULT_RESPONSE_TOPIC_TEMPLATE);
  const char *telemetry_template = select_topic_template(
    opts->telemetry_topic_template,
    opts->device_data_topic_prefix != NULL,
    prefixed_telemetry_template,
    DEFAULT_TELEMETRY_TOPIC_TEMPLATE);
  const char *event_template = select_topic_template(
    opts->event_topic_template,
    opts->device_data_topic_prefix != NULL,
    prefixed_event_template,
    DEFAULT_EVENT_TOPIC_TEMPLATE);

  strncpy(c->namespace,    ns,        sizeof(c->namespace)        - 1);
  strncpy(c->device_id,    opts->device_id, sizeof(c->device_id)  - 1);
  strncpy(c->product_id,   product,    sizeof(c->product_id)       - 1);
  strncpy(c->model_id,     model,      sizeof(c->model_id)         - 1);
  strncpy(c->topic_prefix, prefix,     sizeof(c->topic_prefix)     - 1);
  strncpy(c->data_prefix,  dpfx,       sizeof(c->data_prefix)      - 1);
  if (opts->username) strncpy(c->username, opts->username, sizeof(c->username) - 1);
  if (opts->password) strncpy(c->password, opts->password, sizeof(c->password) - 1);
  if (opts->tls_ca_file) strncpy(c->tls_ca_file, opts->tls_ca_file, sizeof(c->tls_ca_file) - 1);
  if (opts->tls_cert_file) strncpy(c->tls_cert_file, opts->tls_cert_file, sizeof(c->tls_cert_file) - 1);
  if (opts->tls_key_file) strncpy(c->tls_key_file, opts->tls_key_file, sizeof(c->tls_key_file) - 1);
  c->tls_insecure = (opts->tls_insecure != 0);

  c->heartbeat_interval_ms = (opts->heartbeat_interval_ms > 0)
                               ? opts->heartbeat_interval_ms
                               : (opts->heartbeat_interval_ms == -1 ? -1 : DEFAULT_HEARTBEAT_MS);

  /* Build topics */
  if (render_topic_template(commands_template, c->product_id, c->device_id,
                            c->commands_topic, sizeof(c->commands_topic)) != 0 ||
      render_topic_template(telemetry_template, c->product_id, c->device_id,
                            c->telemetry_topic, sizeof(c->telemetry_topic)) != 0 ||
      render_topic_template(event_template, c->product_id, c->device_id,
                            c->events_topic, sizeof(c->events_topic)) != 0 ||
      render_topic_template(responses_template, c->product_id, c->device_id,
                            c->responses_topic, sizeof(c->responses_topic)) != 0) {
    fprintf(stderr, "[da_client] topic template rendered longer than %d bytes\n", MAX_TOPIC_LEN);
    free(c);
    return NULL;
  }

  /* Client ID */
  if (opts->client_id) {
    strncpy(c->client_id, opts->client_id, sizeof(c->client_id) - 1);
  } else {
    snprintf(c->client_id, sizeof(c->client_id),
             "%.48s/dev/%.64s", c->namespace, c->device_id);
  }

  /* Parse broker URL */
  parse_broker_url(opts->broker_url,
                   c->broker_host, sizeof(c->broker_host),
                   &c->broker_port, &c->broker_tls);

  /* Create mosquitto instance */
  c->mosq = mosquitto_new(c->client_id, /*clean_session=*/true, c);
  if (!c->mosq) { free(c); return NULL; }

  mosquitto_connect_callback_set   (c->mosq, on_connect_cb);
  mosquitto_disconnect_callback_set(c->mosq, on_disconnect_cb);
  mosquitto_message_callback_set   (c->mosq, on_message_cb);

  if (c->username[0])
    mosquitto_username_pw_set(c->mosq, c->username, c->password);

  if (env_truthy("MQTT_DEBUG")) {
    mosquitto_log_callback_set(c->mosq, on_log_cb);
  }

  if (c->broker_tls || c->tls_ca_file[0] || c->tls_cert_file[0] || c->tls_key_file[0]) {
    const char *cafile = c->tls_ca_file[0] ? c->tls_ca_file : NULL;
    const char *certfile = c->tls_cert_file[0] ? c->tls_cert_file : NULL;
    const char *keyfile = c->tls_key_file[0] ? c->tls_key_file : NULL;
    int rc;

    if ((certfile && !keyfile) || (!certfile && keyfile)) {
      fprintf(stderr, "[da_client] tls config error: both cert and key are required for mTLS\n");
      mosquitto_destroy(c->mosq);
      free(c);
      return NULL;
    }

    if (!cafile) {
#ifdef MOSQ_OPT_TLS_USE_OS_CERTS
      rc = mosquitto_int_option(c->mosq, MOSQ_OPT_TLS_USE_OS_CERTS, 1);
      if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[da_client] tls os certs option failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(c->mosq);
        free(c);
        return NULL;
      }
#else
      if (!certfile && !keyfile) {
        fprintf(stderr,
                "[da_client] tls config error: this libmosquitto build needs MQTT_TLS_CA_FILE or mTLS files\n");
        mosquitto_destroy(c->mosq);
        free(c);
        return NULL;
      }
#endif
    }

    if (cafile || certfile || keyfile) {
      rc = mosquitto_tls_set(c->mosq, cafile, NULL, certfile, keyfile, NULL);
      if (rc != MOSQ_ERR_SUCCESS) {
        fprintf(stderr, "[da_client] tls_set failed: %s\n", mosquitto_strerror(rc));
        mosquitto_destroy(c->mosq);
        free(c);
        return NULL;
      }
    }

    rc = mosquitto_tls_insecure_set(c->mosq, c->tls_insecure);
    if (rc != MOSQ_ERR_SUCCESS) {
      fprintf(stderr, "[da_client] tls_insecure_set failed: %s\n", mosquitto_strerror(rc));
      mosquitto_destroy(c->mosq);
      free(c);
      return NULL;
    }
  }

  atomic_init(&c->heartbeat_running, false);
  return c;
}

void da_client_free(da_client_t *c) {
  if (!c) return;
  if (c->mosq) {
    da_client_disconnect(c);
    mosquitto_destroy(c->mosq);
  }
  mosquitto_lib_cleanup();
  free(c);
}

void da_client_set_command_handler(da_client_t *c,
                                    da_command_handler_fn handler,
                                    void *user_data) {
  c->command_handler      = handler;
  c->command_handler_data = user_data;
}

void da_client_set_connection_callbacks(da_client_t *c,
                                         const da_connection_callbacks_t *cbs) {
  c->cbs = *cbs;
}

int da_client_connect(da_client_t *c) {
  int rc = mosquitto_connect(c->mosq, c->broker_host, c->broker_port,
                              /*keepalive=*/60);
  if (rc != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "[da_client] connect failed (host=%s port=%d tls=%d rc=%d): %s\n",
            c->broker_host, c->broker_port, c->broker_tls ? 1 : 0,
            rc, mosquitto_strerror(rc));
    return -1;
  }
  rc = mosquitto_loop_start(c->mosq);
  if (rc != MOSQ_ERR_SUCCESS) {
    fprintf(stderr, "[da_client] loop_start failed (rc=%d): %s\n", rc, mosquitto_strerror(rc));
    return -1;
  }

  if (c->heartbeat_interval_ms > 0) {
    atomic_store(&c->heartbeat_running, true);
    if (pthread_create(&c->heartbeat_thread, NULL, heartbeat_fn, c) == 0)
      c->heartbeat_thread_started = true;
  }
  return 0;
}

int da_client_disconnect(da_client_t *c) {
  if (!c->mosq) return 0;

  atomic_store(&c->heartbeat_running, false);
  if (c->heartbeat_thread_started) {
    pthread_join(c->heartbeat_thread, NULL);
    c->heartbeat_thread_started = false;
  }

  /* Best-effort offline status before disconnect */
  da_client_send_status(c, "offline", NULL);
  /* Give mosquitto a moment to flush the publish */
  struct timespec t = { .tv_sec = 0, .tv_nsec = 200000000L };
  nanosleep(&t, NULL);

  mosquitto_disconnect(c->mosq);
  mosquitto_loop_stop(c->mosq, /*force=*/false);
  return 0;
}

int da_client_send_telemetry(da_client_t *c,
                              const char  *type,
                              const char  *data_json) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return -1;
  cJSON_AddStringToObject(root, "type", type);
  cJSON_AddNumberToObject(root, "ts",   (double)now_ms());
  add_default_metadata(c, root);
  cJSON *data = data_json ? cJSON_Parse(data_json) : cJSON_CreateObject();
  if (data) cJSON_AddItemToObject(root, "data", data);
  int rc = publish_json(c, c->telemetry_topic, root);
  cJSON_Delete(root);
  return rc;
}

int da_client_send_event(da_client_t *c,
                          const char  *event_name,
                          const char  *extra_json) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return -1;
  cJSON_AddStringToObject(root, "type", "event");
  cJSON_AddNumberToObject(root, "ts",   (double)now_ms());
  add_default_metadata(c, root);
  cJSON *data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "event", event_name);
  if (extra_json) {
    cJSON *extra = cJSON_Parse(extra_json);
    if (extra && cJSON_IsObject(extra)) {
      cJSON *item = extra->child;
      while (item) {
        cJSON_AddItemToObject(data, item->string, cJSON_Duplicate(item, true));
        item = item->next;
      }
      cJSON_Delete(extra);
    }
  }
  cJSON_AddItemToObject(root, "data", data);
  int rc = publish_json(c, c->events_topic, root);
  cJSON_Delete(root);
  return rc;
}

int da_client_send_status(da_client_t *c,
                           const char  *status,
                           const char  *state_json) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return -1;
  cJSON_AddStringToObject(root, "type", "status");
  cJSON_AddNumberToObject(root, "ts",   (double)now_ms());
  add_default_metadata(c, root);
  cJSON *data = cJSON_CreateObject();
  cJSON_AddStringToObject(data, "status", status);
  if (state_json) {
    cJSON *state = cJSON_Parse(state_json);
    if (state) cJSON_AddItemToObject(data, "state", state);
  }
  cJSON_AddItemToObject(root, "data", data);
  int rc = publish_json(c, c->telemetry_topic, root);
  cJSON_Delete(root);
  return rc;
}

const char *da_client_commands_topic (const da_client_t *c) { return c->commands_topic;  }
const char *da_client_telemetry_topic(const da_client_t *c) { return c->telemetry_topic; }
const char *da_client_events_topic   (const da_client_t *c) { return c->events_topic;    }
const char *da_client_responses_topic(const da_client_t *c) { return c->responses_topic; }

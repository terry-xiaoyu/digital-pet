/*
 * src/main.c -- Generated Device Entry Point
 *
 * This file is generated from your device-spec.json.
 * Fill in the TODO command handler sections with your device logic.
 * See README.md for build and run instructions.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device_agent/client.h"
#include "pet_motors.h"

/* cJSON is always linked into the SDK; use it for robust parameter parsing. */
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

#ifdef DA_VISION_ENABLED
#include "device_agent/vision_client.h"
#endif

/* --- Configuration (overridden by environment variables) ------------------- */

#define DEFAULT_BROKER_URL   "mqtt://127.0.0.1:1883"
#define DEFAULT_NAMESPACE    "default"
#define DEFAULT_PRODUCT_ID   "product-f60mo4"
#define DEFAULT_MODEL_ID     "model_0542c8597ce5559303529ce27f7d57bf"
#define DEFAULT_DEVICE_ID    "product-f60mo4-device-da15ec6584ef"
#define DEFAULT_TOPIC_PREFIX "device-agent"
#define DEFAULT_CLIENT_ID    "default/dev/product-f60mo4-device-da15ec6584ef"
#define DEFAULT_COMMAND_TOPIC_TEMPLATE   "device-agent/{productId}/device/{deviceId}/commands"
#define DEFAULT_RESPONSE_TOPIC_TEMPLATE  "device-agent/{productId}/device/{deviceId}/responses"
#define DEFAULT_TELEMETRY_TOPIC_TEMPLATE "v1/{productId}/{deviceId}/telemetry"
#define DEFAULT_EVENT_TOPIC_TEMPLATE     "v1/{productId}/{deviceId}/event"

/* --- Device state ---------------------------------------------------------- */
/*
 * Mirrors the "properties" block of device-spec.json. The three movable parts
 * (head/body/tail) share the same shape, so they are kept in parallel arrays
 * indexed by pet_part_t; the fan is a single enum string.
 */

static struct {
  bool        active[3];     /* head_tilting / body_turning / tail_swinging  */
  const char *direction[3];  /* "left" | "right"                             */
  const char *amplitude[3];  /* "large" | "medium" | "small"                 */
  const char *fan_speed;     /* "fast" | "medium" | "slow" | "off"           */
} g_state = {
  .active    = { false, false, false },
  .direction = { "left", "left", "left" },
  .amplitude = { "medium", "medium", "medium" },
  .fan_speed = "off",
};

/* Per-part JSON key names, matching device-spec.json properties and events. */
static const struct part_meta {
  const char *event_name; /* events.*                  */
  const char *active_key; /* outputData active flag    */
  const char *dir_key;    /* outputData direction      */
  const char *amp_key;    /* outputData amplitude      */
} PART_META[3] = {
  { "head_state_changed", "tilting",  "tilt_direction",  "tilt_amplitude"  },
  { "body_state_changed", "turning",  "turn_direction",  "turn_amplitude"  },
  { "tail_state_changed", "swinging", "swing_direction", "swing_amplitude" },
};

static da_client_t *g_client = NULL;
static volatile int g_stop   = 0;
static char g_resp_payload[512]; /* command response data (state snapshot) */
static const char *env_or(const char *name, const char *fallback);

#ifdef DA_VISION_ENABLED
static char g_response_payload[16384];
static const char *g_vision_command_names = "";

static char *dup_range(const char *start, size_t len) {
  char *out = calloc(len + 1, 1);
  if (!out) return NULL;
  memcpy(out, start, len);
  return out;
}

static char *dup_cstr(const char *value) {
  return value ? dup_range(value, strlen(value)) : NULL;
}

static int is_vision_command(const char *cmd) {
  if (!cmd) return 0;
  const size_t cmd_len = strlen(cmd);
  const char *cursor = g_vision_command_names;
  while ((cursor = strchr(cursor, '|'))) {
    cursor++;
    if (strncmp(cursor, cmd, cmd_len) == 0 && cursor[cmd_len] == '|') {
      return 1;
    }
  }
  return 0;
}

static int is_supported_image_mime(const char *mime_type) {
  return mime_type &&
         (strcmp(mime_type, "image/jpeg") == 0 ||
          strcmp(mime_type, "image/png") == 0 ||
          strcmp(mime_type, "image/webp") == 0);
}

static int image_from_data_url(const char *data_url,
                               char **mime_type,
                               char **image_base64) {
  if (!data_url || strncmp(data_url, "data:", 5) != 0) return 0;
  const char *marker = strstr(data_url, ";base64,");
  if (!marker) return 0;

  char *mime = dup_range(data_url + 5, (size_t)(marker - (data_url + 5)));
  char *payload = dup_cstr(marker + 8);
  if (!mime || !payload || !is_supported_image_mime(mime) || !payload[0]) {
    free(mime);
    free(payload);
    return 0;
  }

  *mime_type = mime;
  *image_base64 = payload;
  return 1;
}

static char *json_string(cJSON *root, const char *name) {
  cJSON *value = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsString(value) || !value->valuestring || !value->valuestring[0]) {
    return NULL;
  }
  return dup_cstr(value->valuestring);
}

static int capture_local_vision_image(char **mime_type, char **image_base64) {
  (void)mime_type;
  (void)image_base64;
  return 0;
}

static int resolve_vision_image(const char *params_json,
                                char **mime_type,
                                char **image_base64) {
  cJSON *params = params_json ? cJSON_Parse(params_json) : NULL;
  if (params) {
    char *data_url = json_string(params, "imageDataUrl");
    if (image_from_data_url(data_url, mime_type, image_base64)) {
      free(data_url);
      cJSON_Delete(params);
      return 1;
    }
    free(data_url);

    char *base64 = json_string(params, "imageBase64");
    char *mime = json_string(params, "mimeType");
    if (base64 && mime && is_supported_image_mime(mime)) {
      *mime_type = mime;
      *image_base64 = base64;
      cJSON_Delete(params);
      return 1;
    }
    free(base64);
    free(mime);
    cJSON_Delete(params);
  }

  if (image_from_data_url(getenv("VISION_FALLBACK_IMAGE_DATA_URL"), mime_type, image_base64)) {
    return 1;
  }

  return capture_local_vision_image(mime_type, image_base64);
}

static void set_json_error(da_response_t *out, const char *msg, const char *error) {
  out->code = 500;
  out->msg = msg;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", 0);
  cJSON_AddStringToObject(root, "error", error ? error : msg);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  snprintf(g_response_payload, sizeof(g_response_payload), "%s", json ? json : "{\"ok\":false}");
  free(json);
  out->data_json = g_response_payload;
}

static void set_vision_success(da_response_t *out,
                               const char *command,
                               const char *result,
                               const da_vision_frame_ref_t *ref) {
  out->code = 0;
  out->msg = "ok";
  cJSON *root = cJSON_CreateObject();
  cJSON_AddBoolToObject(root, "ok", 1);
  cJSON_AddStringToObject(root, "command", command);
  cJSON_AddStringToObject(root, "result", result ? result : "");
  cJSON *vision_ref = cJSON_AddObjectToObject(root, "visionRef");
  cJSON_AddStringToObject(vision_ref, "frameId", ref->frame_id);
  cJSON_AddStringToObject(vision_ref, "capturedAt", ref->captured_at);
  if (ref->source[0]) cJSON_AddStringToObject(vision_ref, "source", ref->source);
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  snprintf(g_response_payload, sizeof(g_response_payload), "%s", json ? json : "{\"ok\":true}");
  free(json);
  out->data_json = g_response_payload;
}

static int handle_vision_command(const da_command_t *cmd, da_response_t *out) {
  char *mime_type = NULL;
  char *image_base64 = NULL;
  if (!resolve_vision_image(cmd->params_json, &mime_type, &image_base64)) {
    out->code = 400;
    out->msg = "INVALID_IMAGE_INPUT";
    out->data_json = "{\"ok\":false}";
    return 0;
  }

  const char *device_id = getenv("DEVICE_ID") ? getenv("DEVICE_ID") : DEFAULT_DEVICE_ID;
  const char *namespace_value = getenv("NAMESPACE") ? getenv("NAMESPACE") : DEFAULT_NAMESPACE;
  da_vision_client_options_t options = {
    .chat_host = env_or("VOICE_CHAT_HOST", "127.0.0.1:3001"),
    .namespace_value = namespace_value,
    .device_id = device_id,
    .timeout_ms = 120000,
  };

  char session_id[256];
  snprintf(session_id, sizeof(session_id), "%s-%s", device_id,
           cmd->request_id && cmd->request_id[0] ? cmd->request_id : "vision");

  char err[1024] = {0};
  da_vision_frame_ref_t ref = {0};
  int upload_rc = da_vision_upload_frame(&options, session_id, mime_type, image_base64,
                                         &ref, err, sizeof(err));
  free(mime_type);
  free(image_base64);
  if (upload_rc != 0) {
    set_json_error(out, "VISION_UPLOAD_FAILED", err);
    return 0;
  }

  char result[8192] = {0};
  if (da_vision_chat_recognize(&options, session_id,
                               "Identify the key objects in the image and briefly describe their status and location.",
                               cmd->request_id, cmd->cmd, &ref,
                               result, sizeof(result), err, sizeof(err)) != 0) {
    set_json_error(out, "VISION_ROUND_FAILED", err);
    return 0;
  }

  set_vision_success(out, cmd->cmd, result, &ref);
  return 0;
}
#endif

/* --- State snapshot helper ------------------------------------------------ */

#define BOOLSTR(b) ((b) ? "true" : "false")

static void state_to_json(char *buf, size_t cap) {
  snprintf(buf, cap,
           "{"
           "\"head_tilting\":%s,\"head_tilt_direction\":\"%s\",\"head_tilt_amplitude\":\"%s\","
           "\"body_turning\":%s,\"body_turn_direction\":\"%s\",\"body_turn_amplitude\":\"%s\","
           "\"tail_swinging\":%s,\"tail_swing_direction\":\"%s\",\"tail_swing_amplitude\":\"%s\","
           "\"fan_speed\":\"%s\""
           "}",
           BOOLSTR(g_state.active[PET_PART_HEAD]), g_state.direction[PET_PART_HEAD], g_state.amplitude[PET_PART_HEAD],
           BOOLSTR(g_state.active[PET_PART_BODY]), g_state.direction[PET_PART_BODY], g_state.amplitude[PET_PART_BODY],
           BOOLSTR(g_state.active[PET_PART_TAIL]), g_state.direction[PET_PART_TAIL], g_state.amplitude[PET_PART_TAIL],
           g_state.fan_speed);
}

/* Publish the full state snapshot as both a status update and telemetry. */
static void report_state(void) {
  char snap[512];
  state_to_json(snap, sizeof(snap));
  da_client_send_status(g_client, "online", snap);
  da_client_send_telemetry(g_client, "state", snap);
  /* Echo the resulting state back in the command response. */
  snprintf(g_resp_payload, sizeof(g_resp_payload), "%s", snap);
}

/* Emit the per-part "*_state_changed" event reflecting current state. */
static void emit_part_event(pet_part_t part) {
  const struct part_meta *m = &PART_META[part];
  char extra[256];
  snprintf(extra, sizeof(extra),
           "{\"%s\":%s,\"%s\":\"%s\",\"%s\":\"%s\"}",
           m->active_key, BOOLSTR(g_state.active[part]),
           m->dir_key, g_state.direction[part],
           m->amp_key, g_state.amplitude[part]);
  da_client_send_event(g_client, m->event_name, extra);
}

/* --- Parameter parsing ----------------------------------------------------- */

/* Look up a string enum value and map it to an index in @names[0..count).
 * Returns the matched index, or -1 if missing/invalid. */
static int parse_enum(const cJSON *params, const char *key,
                      const char *const *names, int count) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(params, key);
  if (!cJSON_IsString(item) || !item->valuestring) return -1;
  for (int i = 0; i < count; i++) {
    if (strcmp(item->valuestring, names[i]) == 0) return i;
  }
  return -1;
}

static const char *const DIRECTION_NAMES[2] = { "left", "right" };
static const char *const AMPLITUDE_NAMES[3] = { "large", "medium", "small" };
static const char *const SPEED_NAMES[3]     = { "fast", "medium", "slow" };
static const char *const FAN_NAMES[4]       = { "fast", "medium", "slow", "off" };

/* Map a parsed amplitude index ("large"/"medium"/"small") to pet_amplitude_t. */
static pet_amplitude_t to_amp(int idx) {
  switch (idx) {
    case 0:  return PET_AMP_LARGE;
    case 2:  return PET_AMP_SMALL;
    default: return PET_AMP_MEDIUM;
  }
}

/* Map a parsed speed index ("fast"/"medium"/"slow") to pet_speed_t. */
static pet_speed_t to_speed(int idx) {
  switch (idx) {
    case 0:  return PET_SPEED_FAST;
    case 2:  return PET_SPEED_SLOW;
    default: return PET_SPEED_MEDIUM;
  }
}

static void reply_ok(da_response_t *out) {
  out->code = 0;
  out->msg  = "ok";
  out->data_json = g_resp_payload;
}

static void reply_bad_params(da_response_t *out) {
  out->code = 400;
  out->msg  = "invalid params";
  out->data_json = NULL;
}

/* --- Per-command handlers -------------------------------------------------- */

/* head_tilt / body_turn / tail_swing: move to a held off-center pose. */
static void cmd_move(pet_part_t part, const cJSON *p, da_response_t *out) {
  int dir = parse_enum(p, "direction", DIRECTION_NAMES, 2);
  int amp = parse_enum(p, "amplitude", AMPLITUDE_NAMES, 3);
  int spd = parse_enum(p, "speed", SPEED_NAMES, 3);
  if (dir < 0 || amp < 0 || spd < 0) { reply_bad_params(out); return; }

  pet_part_move(part, dir == 0 ? PET_DIR_LEFT : PET_DIR_RIGHT,
                to_amp(amp), to_speed(spd));

  g_state.active[part]    = true;
  g_state.direction[part] = DIRECTION_NAMES[dir];
  g_state.amplitude[part] = AMPLITUDE_NAMES[amp];

  report_state();
  emit_part_event(part);
  reply_ok(out);
}

/* head_shake / tail_wag: oscillate @count times, finishing centered. */
static void cmd_oscillate(pet_part_t part, const cJSON *p, da_response_t *out) {
  int amp = parse_enum(p, "amplitude", AMPLITUDE_NAMES, 3);
  int spd = parse_enum(p, "speed", SPEED_NAMES, 3);
  const cJSON *count_item = cJSON_GetObjectItemCaseSensitive(p, "count");
  if (amp < 0 || spd < 0 || !cJSON_IsNumber(count_item)) {
    reply_bad_params(out);
    return;
  }
  int count = count_item->valueint;
  if (count < 0) count = 0;
  if (count > 3) count = 3;

  pet_part_oscillate(part, to_amp(amp), to_speed(spd), count);

  /* The motion is transient and ends centered. */
  g_state.active[part]    = false;
  g_state.amplitude[part] = AMPLITUDE_NAMES[amp];

  report_state();
  emit_part_event(part);
  reply_ok(out);
}

/* head_reset / body_reset / tail_reset: return to neutral. */
static void cmd_reset(pet_part_t part, da_response_t *out) {
  pet_part_reset(part);
  g_state.active[part] = false;

  report_state();
  emit_part_event(part);
  reply_ok(out);
}

/* set_fan_speed: choose a speed tier ("off" stops the fan). */
static void cmd_set_fan_speed(const cJSON *p, da_response_t *out) {
  int idx = parse_enum(p, "speed", FAN_NAMES, 4);
  if (idx < 0) { reply_bad_params(out); return; }

  static const pet_fan_speed_t MAP[4] = {
    PET_FAN_FAST, PET_FAN_MEDIUM, PET_FAN_SLOW, PET_FAN_OFF,
  };
  pet_fan_set(MAP[idx]);
  g_state.fan_speed = FAN_NAMES[idx];

  report_state();
  char extra[64];
  snprintf(extra, sizeof(extra), "{\"speed\":\"%s\"}", g_state.fan_speed);
  da_client_send_event(g_client, "fan_state_changed", extra);
  reply_ok(out);
}

/* --- Command handler ------------------------------------------------------- */

static int handle_command(const da_command_t *cmd,
                           da_response_t      *out,
                           void               *user_data) {
  (void)user_data;
  printf("[device] command: %s  params: %s\n", cmd->cmd,
         cmd->params_json ? cmd->params_json : "null");

#ifdef DA_VISION_ENABLED
  if (is_vision_command(cmd->cmd)) {
    return handle_vision_command(cmd, out);
  }
#endif

  /* params_json is the compact JSON of the "params" object, or NULL. */
  cJSON *params = cmd->params_json ? cJSON_Parse(cmd->params_json) : NULL;

  if      (strcmp(cmd->cmd, "head_tilt")     == 0) cmd_move(PET_PART_HEAD, params, out);
  else if (strcmp(cmd->cmd, "head_shake")    == 0) cmd_oscillate(PET_PART_HEAD, params, out);
  else if (strcmp(cmd->cmd, "head_reset")    == 0) cmd_reset(PET_PART_HEAD, out);
  else if (strcmp(cmd->cmd, "body_turn")     == 0) cmd_move(PET_PART_BODY, params, out);
  else if (strcmp(cmd->cmd, "body_reset")    == 0) cmd_reset(PET_PART_BODY, out);
  else if (strcmp(cmd->cmd, "tail_swing")    == 0) cmd_move(PET_PART_TAIL, params, out);
  else if (strcmp(cmd->cmd, "tail_wag")      == 0) cmd_oscillate(PET_PART_TAIL, params, out);
  else if (strcmp(cmd->cmd, "tail_reset")    == 0) cmd_reset(PET_PART_TAIL, out);
  else if (strcmp(cmd->cmd, "set_fan_speed") == 0) cmd_set_fan_speed(params, out);
  else {
    out->code = 404;
    out->msg  = "unknown command";
  }

  cJSON_Delete(params);
  return 0;
}

/* --- Connection callbacks -------------------------------------------------- */

static void on_connect(void *ud) {
  (void)ud;
  char snap[512];
  state_to_json(snap, sizeof(snap));
  da_client_send_status(g_client, "online", snap);
  da_client_send_telemetry(g_client, "state", snap);
  printf("[device] connected -- commands: %s\n",
         da_client_commands_topic(g_client));
}

static void on_disconnect(const char *reason, void *ud) {
  (void)ud;
  printf("[device] disconnected: %s\n", reason);
}

static void on_error(const char *msg, void *ud) {
  (void)ud;
  fprintf(stderr, "[device] error: %s\n", msg);
}

static void handle_signal(int sig) { (void)sig; g_stop = 1; }

static const char *env_or(const char *name, const char *fallback) {
  const char *value = getenv(name);
  return value ? value : fallback;
}

/* --- Main ------------------------------------------------------------------ */

int main(void) {
  const char *broker   = env_or("MQTT_BROKER_URL", DEFAULT_BROKER_URL);
  const char *ns       = env_or("NAMESPACE", DEFAULT_NAMESPACE);
  const char *product  = env_or("PRODUCT_ID", DEFAULT_PRODUCT_ID);
  const char *model    = DEFAULT_MODEL_ID;
  const char *device   = env_or("DEVICE_ID", DEFAULT_DEVICE_ID);
  const char *prefix   = env_or("TOPIC_PREFIX", DEFAULT_TOPIC_PREFIX);
  const char *client_id = env_or("MQTT_CLIENT_ID", DEFAULT_CLIENT_ID);
  const char *commands_topic = env_or("MQTT_TOPIC_DEVICE_COMMAND",
                                      DEFAULT_COMMAND_TOPIC_TEMPLATE);
  const char *responses_topic = env_or("MQTT_TOPIC_DEVICE_RESPONSE",
                                       DEFAULT_RESPONSE_TOPIC_TEMPLATE);
  const char *telemetry_topic = env_or("MQTT_TOPIC_TELEMETRY",
                                       DEFAULT_TELEMETRY_TOPIC_TEMPLATE);
  const char *event_topic = env_or("MQTT_TOPIC_EVENT", DEFAULT_EVENT_TOPIC_TEMPLATE);

  printf("[device] broker=%s namespace=%s product=%s model=%s device=%s\n",
         broker, ns, product, model, device);

  da_client_options_t opts = {
    .broker_url = broker,
    .namespace = ns,
    .device_id = device,
    .product_id = product,
    .model_id = model,
    .topic_prefix = prefix,
    .commands_topic_template = commands_topic,
    .responses_topic_template = responses_topic,
    .telemetry_topic_template = telemetry_topic,
    .event_topic_template = event_topic,
    .username = getenv("MQTT_USERNAME"),
    .password = getenv("MQTT_PASSWORD"),
    .tls_ca_file = getenv("MQTT_TLS_CA_FILE"),
    .tls_cert_file = getenv("MQTT_TLS_CERT_FILE"),
    .tls_key_file = getenv("MQTT_TLS_KEY_FILE"),
    .tls_insecure = getenv("MQTT_TLS_INSECURE") ? atoi(getenv("MQTT_TLS_INSECURE")) : 0,
    .client_id = client_id,
  };

  g_client = da_client_new(&opts);
  if (!g_client) { fprintf(stderr, "Failed to create client\n"); return 1; }

  if (pet_motors_init() != 0) {
    fprintf(stderr, "Failed to initialize motors\n");
    da_client_free(g_client);
    return 1;
  }

  da_connection_callbacks_t cbs = {
    .on_connect    = on_connect,
    .on_disconnect = on_disconnect,
    .on_error      = on_error,
  };
  da_client_set_connection_callbacks(g_client, &cbs);
  da_client_set_command_handler(g_client, handle_command, NULL);

  signal(SIGINT, handle_signal);
  signal(SIGTERM, handle_signal);

  if (da_client_connect(g_client) != 0) {
    fprintf(stderr, "Connection failed\n");
    pet_motors_deinit();
    da_client_free(g_client);
    return 1;
  }

  printf("[device] running -- press Ctrl-C to stop\n");
  while (!g_stop) sleep(1);

  printf("[device] shutting down\n");
  da_client_disconnect(g_client);
  pet_motors_deinit();
  da_client_free(g_client);
  return 0;
}

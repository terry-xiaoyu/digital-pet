/**
 * device_agent/client.h
 *
 * MQTT device client for the Device Agent C SDK.
 * Uses Eclipse Mosquitto (libmosquitto) as the MQTT transport.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/* --- Configuration --------------------------------------------------------- */

/**
 * Options passed to da_client_new().
 * All string fields must remain valid for the lifetime of the client
 * (they are deep-copied internally, so it is safe to discard the
 * original struct after da_client_new() returns).
 */
typedef struct da_client_options {
  /** MQTT broker URL, e.g. "mqtt://127.0.0.1:1883" or "mqtts://host:8883" */
  const char *broker_url;
  /** Optional namespace used for the default MQTT client ID */
  const char *namespace;
  /** Unique device identifier */
  const char *device_id;
  /**
   * Product ID (used to build telemetry / event topics).
   * Pass NULL to use the default wildcard segment "_".
   */
  const char *product_id;
  /** Optional product model ID added to outbound metadata. */
  const char *model_id;
  /**
   * MQTT topic prefix for command / response topics.
   * Default: "device-agent"
   */
  const char *topic_prefix;
  /**
   * Prefix for telemetry / event data topics.
   * Default: "v1"
   */
  const char *device_data_topic_prefix;
  /**
   * MQTT topic templates. Supported placeholders: {productId}, {deviceId}.
   * Defaults:
   * - device-agent/{productId}/device/{deviceId}/commands
   * - device-agent/{productId}/device/{deviceId}/responses
   * - v1/{productId}/{deviceId}/telemetry
   * - v1/{productId}/{deviceId}/event
   */
  const char *commands_topic_template;
  const char *responses_topic_template;
  const char *telemetry_topic_template;
  const char *event_topic_template;
  /** MQTT username (NULL = no auth) */
  const char *username;
  /** MQTT password (NULL = no auth) */
  const char *password;
  /** CA certificate bundle path for TLS server verification (NULL = system defaults) */
  const char *tls_ca_file;
  /** Client certificate path for mTLS (NULL = disabled) */
  const char *tls_cert_file;
  /** Client private key path for mTLS (NULL = disabled) */
  const char *tls_key_file;
  /** Disable TLS certificate verification (0 = secure default, non-zero = insecure) */
  int tls_insecure;
  /**
   * Explicit MQTT client ID.  NULL -> auto-generated from device_id + timestamp.
   */
  const char *client_id;
  /**
   * Heartbeat interval in milliseconds.
   * 0 -> default (30 000 ms).  Set to -1 to disable heartbeats.
   */
  int heartbeat_interval_ms;
} da_client_options_t;

/* --- Commands -------------------------------------------------------------- */

/**
 * An inbound command from the platform.
 * All pointers are valid only during the command handler callback.
 */
typedef struct da_command {
  const char *request_id; /**< Opaque correlation ID - must be echoed in response */
  const char *cmd;        /**< Command name, e.g. "power" */
  /**
   * Command parameters as a compact JSON string, e.g. "{\"on\":true}".
   * NULL when the command carries no parameters.
   */
  const char *params_json;
} da_command_t;

/**
 * Response to fill in by the command handler.
 * The handler owns the output strings; they are copied before the callback
 * returns, so it is safe to point to stack buffers.
 */
typedef struct da_response {
  int         code;      /**< 0 = success, non-zero = error */
  const char *msg;       /**< Human-readable status message, e.g. "ok" */
  /**
   * Optional JSON-encoded response payload, e.g. "{\"power\":true}".
   * NULL = no payload.
   */
  const char *data_json;
} da_response_t;

/**
 * Command handler callback.
 *
 * @param cmd       Inbound command (valid only during this call).
 * @param out       Output response to populate.
 * @param user_data Opaque pointer registered with da_client_set_command_handler().
 * @return 0 on success; any non-zero value triggers an error response.
 */
typedef int (*da_command_handler_fn)(const da_command_t *cmd,
                                     da_response_t      *out,
                                     void               *user_data);

/* --- Connection callbacks ------------------------------------------------- */

typedef void (*da_on_connect_fn)(void *user_data);
typedef void (*da_on_disconnect_fn)(const char *reason, void *user_data);
typedef void (*da_on_error_fn)(const char *message, void *user_data);

typedef struct da_connection_callbacks {
  da_on_connect_fn    on_connect;
  da_on_disconnect_fn on_disconnect;
  da_on_error_fn      on_error;
  void               *user_data;
} da_connection_callbacks_t;

/* --- Client handle --------------------------------------------------------- */

typedef struct da_client da_client_t;

/**
 * Create a new device client.
 *
 * @param options Configuration (all strings deep-copied).
 * @return Heap-allocated client, or NULL on allocation failure.
 */
da_client_t *da_client_new(const da_client_options_t *options);

/** Free all resources.  Calls da_client_disconnect() if still connected. */
void da_client_free(da_client_t *client);

/** Register the command handler (replaces any previous handler). */
void da_client_set_command_handler(da_client_t          *client,
                                   da_command_handler_fn handler,
                                   void                 *user_data);

/** Register connection lifecycle callbacks. */
void da_client_set_connection_callbacks(da_client_t                    *client,
                                        const da_connection_callbacks_t *cbs);

/**
 * Connect to the MQTT broker and start the internal event loop thread.
 * Publishes an "online" status message on success.
 *
 * @return 0 on success, -1 on failure.
 */
int da_client_connect(da_client_t *client);

/**
 * Publish an "offline" status, disconnect from the broker, and stop
 * the event loop thread.
 *
 * @return 0 on success, -1 on failure.
 */
int da_client_disconnect(da_client_t *client);

/**
 * Publish a telemetry message.
 *
 * @param type      Telemetry type string, e.g. "state" or "sensors".
 * @param data_json Compact JSON object string, e.g. "{\"temp\":22.5}".
 * @return 0 on success, -1 on failure.
 */
int da_client_send_telemetry(da_client_t *client,
                              const char  *type,
                              const char  *data_json);

/**
 * Publish a structured device event.
 *
 * @param event_name Name defined in the product model, e.g. "button_pressed".
 * @param extra_json Optional JSON object of extra payload fields, or NULL.
 * @return 0 on success, -1 on failure.
 */
int da_client_send_event(da_client_t *client,
                          const char  *event_name,
                          const char  *extra_json);

/**
 * Publish an online/offline status (also embeds the device state snapshot).
 *
 * @param status     "online" or "offline".
 * @param state_json Optional JSON object snapshot of device state, or NULL.
 * @return 0 on success, -1 on failure.
 */
int da_client_send_status(da_client_t *client,
                           const char  *status,
                           const char  *state_json);

/** Return the commands subscription topic (valid after da_client_new). */
const char *da_client_commands_topic(const da_client_t *client);

/** Return the telemetry publish topic (valid after da_client_new). */
const char *da_client_telemetry_topic(const da_client_t *client);

/** Return the events publish topic (valid after da_client_new). */
const char *da_client_events_topic(const da_client_t *client);

/** Return the responses publish topic (valid after da_client_new). */
const char *da_client_responses_topic(const da_client_t *client);

#ifdef __cplusplus
}
#endif

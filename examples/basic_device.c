/*
 * examples/basic_device.c
 *
 * Basic Device Agent SDK example - MQTT device with simulated state.
 *
 * This example implements a simple device that:
 *   - Connects to an MQTT broker
 *   - Handles "power" and "brightness" commands
 *   - Reports telemetry every 10 seconds
 *   - Sends a structured event when brightness changes significantly
 *   - Shuts down cleanly on Ctrl-C / SIGTERM
 *
 * Usage:
 *   export MQTT_BROKER_URL=mqtt://127.0.0.1:1883
 *   export NAMESPACE=default
 *   export PRODUCT_ID=product-f60mo4
 *   export DEVICE_ID=product-f60mo4-device-da15ec6584ef
 *   ./build/examples/basic_device
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "device_agent/client.h"

/* --- Device state ---------------------------------------------------------- */

static struct {
  int  power;       /* 0 = off, 1 = on                   */
  int  brightness;  /* 0 - 100                            */
  int  temperature; /* Celsius (simulated, read-only)      */
} g_state = { .power = 0, .brightness = 50, .temperature = 25 };

static da_client_t *g_client = NULL;
static volatile int g_stop   = 0;

/* --- Build a JSON snapshot of the current state --------------------------- */

static void state_to_json(char *buf, size_t cap) {
  snprintf(buf, cap,
           "{\"power\":%s,\"brightness\":%d,\"temperature\":%d}",
           g_state.power ? "true" : "false",
           g_state.brightness,
           g_state.temperature);
}

/* --- Command handler ------------------------------------------------------- */

static int handle_command(const da_command_t *cmd,
                           da_response_t      *out,
                           void               *user_data) {
  (void)user_data;

  printf("[device] command: %s  params: %s\n",
         cmd->cmd,
         cmd->params_json ? cmd->params_json : "null");

  if (strcmp(cmd->cmd, "power") == 0) {
    /* params: {"on": true|false} */
    int new_power = g_state.power;
    if (cmd->params_json) {
      if (strstr(cmd->params_json, "\"on\":true"))  new_power = 1;
      if (strstr(cmd->params_json, "\"on\":false")) new_power = 0;
    }
    g_state.power = new_power;

    char snap[256];
    state_to_json(snap, sizeof(snap));
    da_client_send_status(g_client, "online", snap);
    da_client_send_telemetry(g_client, "state", snap);

    out->code = 0;
    out->msg  = new_power ? "power ON" : "power OFF";
    return 0;
  }

  if (strcmp(cmd->cmd, "brightness") == 0) {
    /* params: {"value": 0-100} */
    int old_val = g_state.brightness;
    int new_val = g_state.brightness;
    if (cmd->params_json) {
      const char *p = strstr(cmd->params_json, "\"value\":");
      if (p) new_val = atoi(p + 8);
    }
    new_val = new_val < 0 ? 0 : (new_val > 100 ? 100 : new_val);
    g_state.brightness = new_val;

    char snap[256];
    state_to_json(snap, sizeof(snap));
    da_client_send_status(g_client, "online", snap);
    da_client_send_telemetry(g_client, "state", snap);

    /* Emit a structured event if brightness changed significantly */
    if (abs(new_val - old_val) >= 20) {
      char extra[128];
      snprintf(extra, sizeof(extra),
               "{\"old_value\":%d,\"new_value\":%d}", old_val, new_val);
      da_client_send_event(g_client, "brightness_changed", extra);
    }

    out->code = 0;
    out->msg  = "brightness updated";
    return 0;
  }

  out->code = 404;
  out->msg  = "unknown command";
  return 0;
}

/* --- Connection callbacks -------------------------------------------------- */

static void on_connect(void *ud) {
  (void)ud;
  printf("[device] connected to broker\n");

  char snap[256];
  state_to_json(snap, sizeof(snap));
  da_client_send_status(g_client, "online", snap);
  da_client_send_telemetry(g_client, "state", snap);

  printf("[device] commands topic : %s\n", da_client_commands_topic(g_client));
  printf("[device] telemetry topic: %s\n", da_client_telemetry_topic(g_client));
}

static void on_disconnect(const char *reason, void *ud) {
  (void)ud;
  printf("[device] disconnected: %s\n", reason);
}

static void on_error(const char *msg, void *ud) {
  (void)ud;
  fprintf(stderr, "[device] error: %s\n", msg);
}

/* --- Signals --------------------------------------------------------------- */

static void handle_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

/* --- Main ------------------------------------------------------------------ */

int main(void) {
  const char *broker  = getenv("MQTT_BROKER_URL") ? getenv("MQTT_BROKER_URL") : "mqtt://127.0.0.1:1883";
  const char *ns      = getenv("NAMESPACE")       ? getenv("NAMESPACE")       : "default";
  const char *product = getenv("PRODUCT_ID")      ? getenv("PRODUCT_ID")      : "product-f60mo4";
  const char *device  = getenv("DEVICE_ID")       ? getenv("DEVICE_ID")       : "product-f60mo4-device-da15ec6584ef";
  const char *prefix  = getenv("TOPIC_PREFIX")    ? getenv("TOPIC_PREFIX")    : "device-agent";
  const char *client_id = getenv("MQTT_CLIENT_ID");

  printf("Device Agent SDK - Basic Device Example\n");
  printf("========================================\n");
  printf("Broker  : %s\n", broker);
  printf("Namespace: %s\n", ns);
  printf("Product : %s\n", product);
  printf("Device  : %s\n", device);
  printf("\n");

  da_client_options_t opts = {
    .broker_url = broker,
    .namespace = ns,
    .device_id = device,
    .product_id = product,
    .topic_prefix = prefix,
    .commands_topic_template = getenv("MQTT_TOPIC_DEVICE_COMMAND"),
    .responses_topic_template = getenv("MQTT_TOPIC_DEVICE_RESPONSE"),
    .telemetry_topic_template = getenv("MQTT_TOPIC_TELEMETRY"),
    .event_topic_template = getenv("MQTT_TOPIC_EVENT"),
    .username = getenv("MQTT_USERNAME"),
    .password = getenv("MQTT_PASSWORD"),
    .client_id = client_id,
  };

  g_client = da_client_new(&opts);
  if (!g_client) {
    fprintf(stderr, "Failed to create device client\n");
    return 1;
  }

  da_connection_callbacks_t cbs = {
    .on_connect    = on_connect,
    .on_disconnect = on_disconnect,
    .on_error      = on_error,
    .user_data     = NULL,
  };
  da_client_set_connection_callbacks(g_client, &cbs);
  da_client_set_command_handler(g_client, handle_command, NULL);

  signal(SIGINT,  handle_signal);
  signal(SIGTERM, handle_signal);

  printf("Connecting to %s...\n", broker);
  if (da_client_connect(g_client) != 0) {
    fprintf(stderr, "Failed to connect\n");
    da_client_free(g_client);
    return 1;
  }

  printf("Running -- press Ctrl-C to stop.\n\n");

  int tick = 0;
  while (!g_stop) {
    sleep(1);
    tick++;

    /* Publish telemetry every 10 s */
    if (tick % 10 == 0) {
      /* Simulate slowly changing temperature */
      g_state.temperature = 20 + (tick / 10) % 10;
      char snap[256];
      state_to_json(snap, sizeof(snap));
      da_client_send_telemetry(g_client, "state", snap);
      printf("[device] telemetry sent (tick=%d)\n", tick);
    }
  }

  printf("\nShutting down...\n");
  da_client_disconnect(g_client);
  da_client_free(g_client);

  printf("Done.\n");
  return 0;
}

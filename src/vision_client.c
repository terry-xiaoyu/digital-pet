#include "device_agent/vision_client.h"

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

#include <curl/curl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct response_buffer {
  char *data;
  size_t len;
} response_buffer_t;

static size_t write_response(void *ptr, size_t size, size_t nmemb, void *userdata) {
  size_t chunk_len = size * nmemb;
  response_buffer_t *buffer = (response_buffer_t *)userdata;
  char *next = realloc(buffer->data, buffer->len + chunk_len + 1);
  if (!next) return 0;
  buffer->data = next;
  memcpy(buffer->data + buffer->len, ptr, chunk_len);
  buffer->len += chunk_len;
  buffer->data[buffer->len] = '\0';
  return chunk_len;
}

static int starts_with(const char *value, const char *prefix) {
  return strncmp(value, prefix, strlen(prefix)) == 0;
}

static void build_url(const char *chat_host, const char *path, char *out, size_t out_len) {
  const char *scheme = starts_with(chat_host, "http://") || starts_with(chat_host, "https://")
                         ? ""
                         : "http://";
  snprintf(out, out_len, "%s%s%s", scheme, chat_host, path);
}

static int post_json(const da_vision_client_options_t *options,
                     const char *path,
                     const char *body,
                     char **out_body,
                     char *err_buf,
                     size_t err_buf_len) {
  CURL *curl = curl_easy_init();
  if (!curl) {
    snprintf(err_buf, err_buf_len, "curl init failed");
    return -1;
  }

  char url[512];
  build_url(options->chat_host, path, url, sizeof(url));

  response_buffer_t response = {0};
  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_response);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, options->timeout_ms > 0 ? options->timeout_ms : 120000);

  CURLcode rc = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    snprintf(err_buf, err_buf_len, "%s", curl_easy_strerror(rc));
    free(response.data);
    return -1;
  }
  if (status < 200 || status >= 300) {
    snprintf(err_buf, err_buf_len, "HTTP %ld: %s", status, response.data ? response.data : "");
    free(response.data);
    return -1;
  }

  *out_body = response.data ? response.data : calloc(1, 1);
  return *out_body ? 0 : -1;
}

static void copy_json_string(cJSON *object, const char *key, char *out, size_t out_len) {
  cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
  if (cJSON_IsString(value) && value->valuestring) {
    snprintf(out, out_len, "%s", value->valuestring);
  }
}

int da_vision_upload_frame(const da_vision_client_options_t *options,
                           const char *session_id,
                           const char *mime_type,
                           const char *image_base64,
                           da_vision_frame_ref_t *out_ref,
                           char *err_buf,
                           size_t err_buf_len) {
  if (!options || !options->chat_host || !session_id || !mime_type || !image_base64 || !out_ref) {
    snprintf(err_buf, err_buf_len, "invalid upload arguments");
    return -1;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "sessionId", session_id);
  if (options->namespace_value && options->namespace_value[0]) {
    cJSON_AddStringToObject(root, "namespace", options->namespace_value);
  }
  if (options->device_id && options->device_id[0]) {
    cJSON_AddStringToObject(root, "deviceId", options->device_id);
  }
  cJSON_AddStringToObject(root, "mimeType", mime_type);
  cJSON_AddStringToObject(root, "imageBase64", image_base64);
  cJSON_AddStringToObject(root, "source", "sdk-camera");

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) {
    snprintf(err_buf, err_buf_len, "failed to build upload body");
    return -1;
  }

  char *response_body = NULL;
  int rc = post_json(options, "/api/vision/frames", body, &response_body, err_buf, err_buf_len);
  free(body);
  if (rc != 0) return -1;

  cJSON *response = cJSON_Parse(response_body);
  free(response_body);
  if (!response) {
    snprintf(err_buf, err_buf_len, "invalid upload response JSON");
    return -1;
  }

  memset(out_ref, 0, sizeof(*out_ref));
  copy_json_string(response, "frameId", out_ref->frame_id, sizeof(out_ref->frame_id));
  copy_json_string(response, "capturedAt", out_ref->captured_at, sizeof(out_ref->captured_at));
  copy_json_string(response, "source", out_ref->source, sizeof(out_ref->source));
  cJSON_Delete(response);

  if (!out_ref->frame_id[0] || !out_ref->captured_at[0]) {
    snprintf(err_buf, err_buf_len, "upload response missing frameId/capturedAt");
    return -1;
  }
  return 0;
}

static void append_text(char *out, size_t out_len, const char *text) {
  if (!text || !out_len) return;
  size_t used = strlen(out);
  if (used >= out_len - 1) return;
  snprintf(out + used, out_len - used, "%s", text);
}

static void collect_sse_text(const char *body, char *out, size_t out_len) {
  out[0] = '\0';
  const char *line = body;
  while (line && *line) {
    const char *next = strchr(line, '\n');
    size_t len = next ? (size_t)(next - line) : strlen(line);
    if (len > 5 && strncmp(line, "data:", 5) == 0) {
      char *payload = calloc(len - 4, 1);
      if (payload) {
        memcpy(payload, line + 5, len - 5);
        cJSON *json = cJSON_Parse(payload);
        if (json) {
          cJSON *text = cJSON_GetObjectItemCaseSensitive(json, "text");
          cJSON *chunk = cJSON_GetObjectItemCaseSensitive(json, "textChunk");
          if (cJSON_IsString(text) && text->valuestring) {
            snprintf(out, out_len, "%s", text->valuestring);
          } else if (cJSON_IsString(chunk) && chunk->valuestring) {
            append_text(out, out_len, chunk->valuestring);
          }
          cJSON_Delete(json);
        }
        free(payload);
      }
    }
    line = next ? next + 1 : NULL;
  }
}

int da_vision_chat_recognize(const da_vision_client_options_t *options,
                             const char *session_id,
                             const char *prompt,
                             const char *request_id,
                             const char *command_name,
                             const da_vision_frame_ref_t *vision_ref,
                             char *result_buf,
                             size_t result_buf_len,
                             char *err_buf,
                             size_t err_buf_len) {
  if (!options || !session_id || !vision_ref || !result_buf || result_buf_len == 0) {
    snprintf(err_buf, err_buf_len, "invalid chat arguments");
    return -1;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "sessionId", session_id);
  cJSON_AddBoolToObject(root, "stream", 1);
  cJSON_AddStringToObject(root, "message", prompt && prompt[0] ? prompt : "Recognize the image.");
  cJSON *refs = cJSON_AddArrayToObject(root, "visionRefs");
  cJSON *ref = cJSON_CreateObject();
  cJSON_AddStringToObject(ref, "frameId", vision_ref->frame_id);
  cJSON_AddStringToObject(ref, "capturedAt", vision_ref->captured_at);
  if (vision_ref->source[0]) cJSON_AddStringToObject(ref, "source", vision_ref->source);
  cJSON_AddItemToArray(refs, ref);

  cJSON *metadata = cJSON_AddObjectToObject(root, "metadata");
  if (request_id && request_id[0]) cJSON_AddStringToObject(metadata, "requestId", request_id);
  if (command_name && command_name[0]) cJSON_AddStringToObject(metadata, "command", command_name);
  if (options->device_id && options->device_id[0]) {
    cJSON_AddStringToObject(metadata, "deviceId", options->device_id);
  }

  char *body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) {
    snprintf(err_buf, err_buf_len, "failed to build chat body");
    return -1;
  }

  char *response_body = NULL;
  int rc = post_json(options, "/api/chat", body, &response_body, err_buf, err_buf_len);
  free(body);
  if (rc != 0) return -1;

  collect_sse_text(response_body, result_buf, result_buf_len);
  free(response_body);
  return 0;
}

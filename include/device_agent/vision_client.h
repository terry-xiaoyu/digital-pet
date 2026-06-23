#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct da_vision_client_options {
  const char *chat_host;
  const char *namespace_value;
  const char *device_id;
  long timeout_ms;
} da_vision_client_options_t;

typedef struct da_vision_frame_ref {
  char frame_id[128];
  char captured_at[64];
  char source[64];
} da_vision_frame_ref_t;

int da_vision_upload_frame(const da_vision_client_options_t *options,
                           const char *session_id,
                           const char *mime_type,
                           const char *image_base64,
                           da_vision_frame_ref_t *out_ref,
                           char *err_buf,
                           size_t err_buf_len);

int da_vision_chat_recognize(const da_vision_client_options_t *options,
                             const char *session_id,
                             const char *prompt,
                             const char *request_id,
                             const char *command_name,
                             const da_vision_frame_ref_t *vision_ref,
                             char *result_buf,
                             size_t result_buf_len,
                             char *err_buf,
                             size_t err_buf_len);

#ifdef __cplusplus
}
#endif

#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct avtrans_webrtc_handle avtrans_webrtc_handle;

typedef struct avtrans_webrtc_i420_frame {
  int width;
  int height;
  int64_t timestamp_us;
  const uint8_t* data_y;
  const uint8_t* data_u;
  const uint8_t* data_v;
  int stride_y;
  int stride_u;
  int stride_v;
} avtrans_webrtc_i420_frame;

avtrans_webrtc_handle* avtrans_webrtc_create(void);
void avtrans_webrtc_destroy(avtrans_webrtc_handle* handle);

int avtrans_webrtc_init(avtrans_webrtc_handle* handle);
int avtrans_webrtc_push_i420(avtrans_webrtc_handle* handle,
                             const avtrans_webrtc_i420_frame* frame);

char* avtrans_webrtc_create_answer(avtrans_webrtc_handle* handle,
                                   const char* remote_offer_sdp,
                                   int* ok,
                                   const char** content_type);
void avtrans_webrtc_free_string(char* s);

#ifdef __cplusplus
}
#endif


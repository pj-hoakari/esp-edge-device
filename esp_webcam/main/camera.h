#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#define JPEG_MAX (256 * 1024)
#define JPEG_CHUNK 12000
/* HD is a product requirement: do not make the capture size configurable. */
#define CAMERA_WIDTH 1280
#define CAMERA_HEIGHT 720
void camera_start(void);
/* Legacy MJPEG subscriber. Returns zero until a newer frame is available. */
size_t camera_take(uint8_t *out);
/* Independent subscribers can share the latest frame without stealing it. */
size_t camera_read(uint8_t *out, uint32_t *sequence);

typedef struct {
    uint32_t sequence;     /* in: last sequence seen; out: latest sequence */
    size_t size;           /* latest JPEG size (0 when no new frame was copied) */
    int64_t captured_us;   /* esp_timer timestamp when the frame was published */
} camera_frame_info_t;

/* Copy the latest complete JPEG newer than info->sequence.
 * Returns the copied size, or 0 when no newer frame is available. */
size_t camera_read_frame(uint8_t *out, camera_frame_info_t *info);

typedef enum {
    CAMERA_WAITING, CAMERA_OPENING, CAMERA_STREAMING, CAMERA_DISCONNECTED, CAMERA_ERROR
} camera_state_t;
typedef struct {
    camera_state_t state;
    uint32_t uvc_detections, open_attempts, frames_received, valid_jpegs;
    uint32_t invalid_jpegs, oversized_jpegs, buffer_overflows, transfer_errors;
    uint32_t trimmed_jpegs, trimmed_bytes, missing_soi, missing_eoi;
    uint32_t invalid_no_soi, invalid_marker, invalid_truncated, invalid_no_sof;
    uint32_t invalid_dimensions, invalid_no_sos, invalid_no_eoi, copy_drops;
    uint32_t stream_restarts;
    uint32_t last_frame_bytes;
    unsigned width, height;
    esp_err_t last_open_error;
    int64_t last_valid_us;
} camera_status_t;
void camera_get_status(camera_status_t *status);

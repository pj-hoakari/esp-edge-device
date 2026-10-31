#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    JPEG_FRAME_OK = 0,
    JPEG_FRAME_NO_SOI,
    JPEG_FRAME_BAD_MARKER,
    JPEG_FRAME_TRUNCATED,
    JPEG_FRAME_NO_SOF,
    JPEG_FRAME_BAD_DIMENSIONS,
    JPEG_FRAME_NO_SOS,
    JPEG_FRAME_NO_EOI,
} jpeg_frame_error_t;

/* Return the JPEG length including EOI and discard bytes after EOI.
 * UVC frame buffers are sometimes padded to a bus/DMA alignment after the
 * JPEG, so trailing bytes are not an error: an EOI outside APP data is
 * authoritative because 0xFF in entropy-coded data is always stuffed.
 * Missing markers, truncated segments and wrong dimensions are still
 * rejected. Never invent missing data. */
size_t jpeg_frame_length(const uint8_t *data, size_t size, unsigned width, unsigned height);

/* Same as jpeg_frame_length, additionally reporting why a frame was rejected. */
size_t jpeg_frame_length_ex(const uint8_t *data, size_t size, unsigned width, unsigned height,
                            jpeg_frame_error_t *error);

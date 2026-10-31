#include "jpeg_frame.h"

static size_t fail(jpeg_frame_error_t *error, jpeg_frame_error_t reason)
{
    if (error) *error = reason;
    return 0;
}

size_t jpeg_frame_length_ex(const uint8_t *d, size_t n, unsigned width, unsigned height,
                            jpeg_frame_error_t *error)
{
    if (error) *error = JPEG_FRAME_OK;
    if (!d || n < 4) return fail(error, JPEG_FRAME_TRUNCATED);
    if (d[0] != 255 || d[1] != 216) return fail(error, JPEG_FRAME_NO_SOI);
    size_t p = 2;
    int sof = 0, scan = 0, entropy = 0;
    while (p < n) {
        if (entropy) {
            while (p < n && d[p] != 255) p++;
        } else if (d[p] != 255) {
            return fail(error, JPEG_FRAME_BAD_MARKER);
        }
        if (p >= n) return fail(error, entropy ? JPEG_FRAME_NO_EOI : JPEG_FRAME_TRUNCATED);
        while (p < n && d[p] == 255) p++;
        if (p >= n) return fail(error, JPEG_FRAME_TRUNCATED);
        unsigned marker = d[p++];
        if (entropy && (marker == 0 || (marker >= 208 && marker <= 215))) continue;
        entropy = 0;
        if (marker == 217) {
            if (!sof) return fail(error, JPEG_FRAME_NO_SOF);
            if (!scan) return fail(error, JPEG_FRAME_NO_SOS);
            // Trailing bytes are USB/DMA alignment padding. The browser only
            // receives bytes up to and including this EOI.
            return p;
        }
        if (marker == 0 || marker == 216 || marker == 1 || (marker >= 208 && marker <= 215))
            return fail(error, JPEG_FRAME_BAD_MARKER);
        if (n - p < 2) return fail(error, JPEG_FRAME_TRUNCATED);
        size_t len = (size_t)d[p] * 256 + d[p + 1];
        if (len < 2) return fail(error, JPEG_FRAME_BAD_MARKER);
        if (len > n - p) return fail(error, JPEG_FRAME_TRUNCATED);
        if ((marker >= 192 && marker <= 195) || (marker >= 197 && marker <= 199) ||
            (marker >= 201 && marker <= 203) || (marker >= 205 && marker <= 207)) {
            if (len < 8) return fail(error, JPEG_FRAME_BAD_MARKER);
            unsigned h = (unsigned)d[p + 3] * 256 + d[p + 4];
            unsigned w = (unsigned)d[p + 5] * 256 + d[p + 6];
            unsigned components = d[p + 7];
            if (w != width || h != height || components != 3 || len < 8 + components * 3)
                return fail(error, JPEG_FRAME_BAD_DIMENSIONS);
            sof = 1;
        }
        if (marker == 218) {
            if (!sof) return fail(error, JPEG_FRAME_NO_SOF);
            if (len < 6) return fail(error, JPEG_FRAME_BAD_MARKER);
            scan = 1;
            entropy = 1;
        }
        p += len;
    }
    return fail(error, JPEG_FRAME_NO_EOI);
}

size_t jpeg_frame_length(const uint8_t *data, size_t size, unsigned width, unsigned height)
{
    return jpeg_frame_length_ex(data, size, width, height, NULL);
}

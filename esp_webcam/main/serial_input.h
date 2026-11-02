#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct { size_t length; bool invalid, skip_lf; } serial_input_t;
typedef enum { SERIAL_MORE, SERIAL_READY, SERIAL_INVALID, SERIAL_CANCEL } serial_result_t;

/* Parsing only; the caller decides whether to echo or mask accepted bytes.
 * Reject the entire line on overflow/control bytes;
 * never execute a valid prefix or the tail of an overlong command. */
static inline serial_result_t serial_input_feed(serial_input_t *s, char *out, size_t cap, unsigned char c)
{
    if (s->skip_lf && c == '\n') { s->skip_lf = false; return SERIAL_MORE; }
    s->skip_lf = false;
    if (c == '\r' || c == '\n' || c == 3) {
        serial_result_t result = c == 3 ? SERIAL_CANCEL : s->invalid ? SERIAL_INVALID : SERIAL_READY;
        out[s->length] = 0;
        s->length = 0;
        s->invalid = false;
        s->skip_lf = c == '\r';
        return result;
    }
    if (s->invalid) return SERIAL_MORE;
    if (c == 8 || c == 127) { if (s->length) s->length--; return SERIAL_MORE; }
    if (c < 32 || s->length + 1 >= cap) s->invalid = true;
    else out[s->length++] = (char)c;
    return SERIAL_MORE;
}

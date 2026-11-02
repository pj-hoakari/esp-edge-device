#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* This diagnostic intentionally supports plain UDP STUN over IPv4 only. */
static inline bool stun_probe_server(const char *uri, char *host, size_t size, char port[6])
{
    if (strncmp(uri, "stun:", 5)) return false;
    const char *start = uri + 5, *colon = strchr(start, ':');
    size_t length = colon ? (size_t)(colon - start) : strlen(start);
    if (!length || length >= size) return false;
    for (size_t i = 0; i < length; i++) {
        char c = start[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    unsigned value = 3478;
    if (colon) {
        size_t digits = strlen(colon + 1);
        if (!digits || digits > 5) return false;
        value = 0;
        for (size_t i = 1; colon[i]; i++) {
            if (colon[i] < '0' || colon[i] > '9') return false;
            value = value * 10 + (unsigned)(colon[i] - '0');
        }
        if (!value || value > 65535) return false;
    }
    memcpy(host, start, length); host[length] = 0;
    snprintf(port, 6, "%u", value);
    return true;
}

typedef enum { STUN_PROBE_INVALID, STUN_PROBE_SUCCESS, STUN_PROBE_ERROR } stun_probe_result_t;

static inline stun_probe_result_t stun_probe_response(const uint8_t *data, size_t size, const uint8_t request[20])
{
    if (size < 20 || memcmp(data + 4, request + 4, 16)) return STUN_PROBE_INVALID;
    unsigned type = ((unsigned)data[0] << 8) | data[1];
    size_t length = ((size_t)data[2] << 8) | data[3];
    if ((type != 0x0101 && type != 0x0111) || (length & 3) || length + 20 != size) return STUN_PROBE_INVALID;
    bool mapped = false, error = false;
    for (size_t at = 20; at < size;) {
        if (size - at < 4) return STUN_PROBE_INVALID;
        unsigned attribute = ((unsigned)data[at] << 8) | data[at + 1];
        size_t bytes = ((size_t)data[at + 2] << 8) | data[at + 3];
        size_t padded = (bytes + 3) & ~(size_t)3;
        if (padded > size - at - 4) return STUN_PROBE_INVALID;
        const uint8_t *value = data + at + 4;
        if (attribute == 0x0020 || attribute == 0x0001) {
            if (bytes < 2 || value[0] != 0 || !((value[1] == 1 && bytes == 8) ||
                (value[1] == 2 && bytes == 20))) return STUN_PROBE_INVALID;
            mapped = true;
        } else if (attribute == 0x0009) {
            if (bytes < 4 || value[2] < 3 || value[2] > 6 || value[3] > 99) return STUN_PROBE_INVALID;
            error = true;
        }
        at += 4 + padded;
    }
    return type == 0x0101 ? (mapped ? STUN_PROBE_SUCCESS : STUN_PROBE_INVALID) :
        (error ? STUN_PROBE_ERROR : STUN_PROBE_INVALID);
}

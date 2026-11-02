#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    PERF_PEER_LOOP, PERF_SEND, PERF_COPY, PERF_TLS_WRITE, PERF_TLS_READ,
    PERF_UDP_SEND, PERF_UDP_WAIT, PERF_UDP_RECV, PERF_COUNT
} perf_kind_t;

void perf_record(perf_kind_t kind, int64_t us, size_t bytes, int error);
void perf_reset(void);
int perf_json(char *out, size_t size);

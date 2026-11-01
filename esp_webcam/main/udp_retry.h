#pragma once
#include <errno.h>

/* Match esp_peer's private transport retry result. Do not report a datagram
 * as delivered when Wi-Fi has no room; DTLS/SCTP must handle backpressure. */
#define WEBCAM_UDP_RETRY (-200)
static inline int webcam_udp_send_bounded(int (*send_packet)(void *),
                                         void (*pause_ms)(unsigned), void *ctx)
{
    for (unsigned attempt = 0; ; attempt++) {
        int ret = send_packet(ctx);
        if (ret >= 0) return ret;
        if (errno != ENOBUFS && errno != ENOMEM && errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;
        if (attempt == 2) return WEBCAM_UDP_RETRY;
        pause_ms(1);
    }
}

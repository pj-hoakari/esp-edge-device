#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "transport/udp.h"
#include "peer_utils.h"
#include "udp_retry.h"
#include "esp_timer.h"
#include "perf.h"
#include "webrtc.h"

int __real_udp_socket_recvfrom_nowait(udp_socket_t *, esp_peer_addr_t *, uint8_t *, int, bool);

int __wrap_udp_socket_recvfrom_nowait(udp_socket_t *sock, esp_peer_addr_t *addr,
                                    uint8_t *buf, int len, bool nowait)
{
    // The established sender pumps the peer and yields once per iteration.
    // Poll queued ACK/SACK packets without another blocking select timeout.
    // Keep the vendor receive policy during ICE/DTLS handshakes.
    bool poll = nowait || (webrtc_fast_rx() && webrtc_video_active());
    int64_t start = esp_timer_get_time();
    int ret = __real_udp_socket_recvfrom_nowait(sock, addr, buf, len, poll);
    perf_record(PERF_UDP_RECV, esp_timer_get_time() - start, ret > 0 ? ret : 0, ret < 0);
    return ret;
}

typedef struct {
    int fd, len;
    const uint8_t *data;
    const struct sockaddr *addr;
    socklen_t addr_len;
} send_context_t;

static int send_packet(void *ctx)
{
    const send_context_t *s = ctx;
    int64_t start = esp_timer_get_time();
    int ret = sendto(s->fd, s->data, s->len, MSG_DONTWAIT, s->addr, s->addr_len);
    perf_record(PERF_UDP_SEND, esp_timer_get_time() - start, ret > 0 ? ret : 0, ret < 0);
    return ret;
}

static void pause_ms(unsigned ms)
{
    int64_t start = esp_timer_get_time();
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
    perf_record(PERF_UDP_WAIT, esp_timer_get_time() - start, 0, 0);
}

/* esp_peer exports this transport hook as weak. Keep vendor/submodule sources
 * pinned, replacing only its 20 ms + select retry sleeps with bounded 1 ms
 * retries. Keep the -200 backpressure convention and socket user accounting. */
int udp_socket_sendto(udp_socket_t *sock, esp_peer_addr_t *addr, const uint8_t *buf, int len)
{
    if (!sock || !addr || !buf || len <= 0) return -1;
    struct sockaddr_storage storage = {0};
    send_context_t s = {.fd = -1, .data = buf, .len = len,
                        .addr = (const struct sockaddr *)&storage};
    peer_atomic_inc(&sock->user_count);
    if (addr->family == AF_INET) {
        struct sockaddr_in *sin = (struct sockaddr_in *)&storage;
        sin->sin_family = AF_INET;
        sin->sin_port = htons(addr->port);
        memcpy(&sin->sin_addr.s_addr, addr->ipv4, 4);
        s.fd = sock->fd; s.addr_len = sizeof(*sin);
    } else if (addr->family == AF_INET6) {
        struct sockaddr_in6 *sin = (struct sockaddr_in6 *)&storage;
        sin->sin6_family = AF_INET6;
        sin->sin6_port = htons(addr->port);
        sin->sin6_scope_id = sock->sin6_scope_id;
        memcpy(&sin->sin6_addr, addr->ipv6, 16);
        s.fd = sock->ipv6_fd; s.addr_len = sizeof(*sin);
    }
    int ret = s.fd < 0 ? -1 : webcam_udp_send_bounded(send_packet, pause_ms, &s);
    peer_atomic_dec(&sock->user_count);
    return ret;
}

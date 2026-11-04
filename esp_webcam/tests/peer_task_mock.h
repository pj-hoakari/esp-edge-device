#pragma once
#include <assert.h>
#include <inttypes.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "camera.h"
#include "perf.h"
#include "sender_policy.h"
#include "peer_authorization.h"
#include "device_auth.h"
#define CONFIG_WEBCAM_TARGET_FPS 10
#define CONFIG_WEBCAM_STUN_URL ""
#define ESP_PEER_DTLS_CIPHER_CHACHA 1
#define portMUX_INITIALIZER_UNLOCKED 0
typedef int portMUX_TYPE;
#define portENTER_CRITICAL(x) ((void)0)
#define portEXIT_CRITICAL(x) ((void)0)
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
#define ESP_ERROR_CHECK(x) assert((x) == 0)
#define pdMS_TO_TICKS(x) (x)
#define ESP_PEER_MSG_TYPE_SDP 0
#define ESP_PEER_DATA_CHANNEL_DATA 0
#define ESP_PEER_ERR_WOULD_BLOCK -7
typedef void *esp_peer_handle_t;
typedef struct { const char *stun_url; } esp_peer_ice_server_cfg_t;
typedef struct { int type; uint8_t *data; size_t size; } esp_peer_msg_t;
typedef struct { int type; uint16_t stream_id; uint8_t *data; size_t size; } esp_peer_data_frame_t;
static jmp_buf peer_done;
static int peer_sends, peer_closes, peer_iterations;
static void *allocations[2];
static int allocations_count;
static void *heap_caps_malloc(size_t size, int caps) {
    void *p = calloc(1, size); allocations[allocations_count++] = p; return p;
}
static int esp_peer_pre_generate_cert(void) { return 0; }
static int esp_peer_close(void *peer) { peer_closes++; return 0; }
static int esp_peer_new_connection(void *peer) { abort(); }
static int esp_peer_send_msg(void *peer, const esp_peer_msg_t *msg) { abort(); }
static int open_peer(void) { abort(); }
static void esp_peer_main_loop(void *peer) { }
static void put32(uint8_t *p, uint32_t value) { memcpy(p, &value, 4); }
static void enqueue_stale_offer(void);
static int esp_peer_send_data(void *peer, const esp_peer_data_frame_t *data) {
    assert(!peer_authorization_blocked());
    peer_sends++;
    enqueue_stale_offer();
    /* Inject the real refresh failure between chunks of a partially sent JPEG. */
    assert(!device_auth_force_refresh());
    assert(peer_authorization_blocked());
    assert(peer_closes == 0); /* auth never closes peer directly */
    return 0;
}
static size_t camera_frames;
size_t camera_read_frame(uint8_t *out, camera_frame_info_t *info) {
    camera_frames++; info->sequence++; return JPEG_CHUNK * 2;
}
void perf_record(perf_kind_t kind, int64_t us, size_t bytes, int error) { }
void perf_reset(void) { }
static void stats_reset(void);
static void stats_bump(uint32_t *value) { ++*value; }
static void stats_frame_sent(size_t bytes, int64_t us) { abort(); }
static int xSemaphoreTake(void *lock, unsigned ticks) { return pdTRUE; }
static int xSemaphoreGive(void *lock) { return pdTRUE; }
static void vTaskDelay(unsigned ticks) {
    if (++peer_iterations == 3) longjmp(peer_done, 1);
}

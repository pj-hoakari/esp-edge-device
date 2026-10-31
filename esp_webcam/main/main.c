#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "esp_peer_default.h"
#include "camera.h"
#include "webrtc.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

/* All peer APIs are called by peer_task; callbacks execute in that task too. */
static const char *TAG = "webcam";
static esp_peer_handle_t peer;
static QueueHandle_t commands;
static SemaphoreHandle_t signal_lock;
static char *answer;
static uint32_t session;
static uint32_t active_session;
static uint16_t stream_id;
static atomic_bool channel_open;
static bool jpeg_channel_created;
static volatile int peer_state;
static uint32_t frame_id, awaiting_ack, last_ack_id;
static bool ack_pending;
static int64_t ack_since;
static atomic_int client_motion = -1;
static atomic_int effective_fps = CONFIG_WEBCAM_TARGET_FPS;
static int64_t motion_until, last_ack_us;
typedef struct { char *sdp; uint32_t id; } command_t;

#define KEEPALIVE_US ((int64_t)CONFIG_WEBCAM_KEEPALIVE_MS * 1000)
#define MOTION_HOLD_US ((int64_t)CONFIG_WEBCAM_MOTION_HOLD_MS * 1000)

typedef struct {
    uint32_t sent_frames, send_errors, dropped_frames;
    uint32_t skipped_identical, stale_frames, acked_frames, no_ack;
    uint32_t last_ack_rtt_ms;
    uint64_t sent_bytes;
} sender_stats_t;
static portMUX_TYPE stats_mux = portMUX_INITIALIZER_UNLOCKED;
static sender_stats_t stats;

static void stats_bump(uint32_t *field)
{
    portENTER_CRITICAL(&stats_mux);
    (*field)++;
    portEXIT_CRITICAL(&stats_mux);
}

static void stats_frame_sent(size_t bytes)
{
    portENTER_CRITICAL(&stats_mux);
    stats.sent_frames++;
    stats.sent_bytes += bytes;
    portEXIT_CRITICAL(&stats_mux);
}

static void stats_reset(void)
{
    portENTER_CRITICAL(&stats_mux);
    memset(&stats, 0, sizeof(stats));
    portEXIT_CRITICAL(&stats_mux);
}

bool webrtc_video_active(void) { return atomic_load(&channel_open); }

static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        char *copy = strndup((char *)msg->data, msg->size);
        // Only publish an answer. Initial ICE gathering may report an offer.
        if (!copy || strstr(copy, "a=setup:actpass")) { free(copy); return 0; }
        xSemaphoreTake(signal_lock, portMAX_DELAY);
        if (active_session == session) { free(answer); answer = copy; }
        else free(copy);
        xSemaphoreGive(signal_lock);
    }
    return 0;
}

static int on_state(esp_peer_state_t state, void *ctx)
{
    peer_state = state;
    ESP_LOGI(TAG, "Peer state %d", state);
    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED && !jpeg_channel_created) {
        esp_peer_data_channel_cfg_t ch = {
            .type = ESP_PEER_DATA_CHANNEL_PARTIAL_RELIABLE_RETX,
            .ordered = false, .label = "jpeg",
            .max_retransmit_count = CONFIG_WEBCAM_DATACHANNEL_RETRANSMITS,
        };
        jpeg_channel_created = esp_peer_create_data_channel(peer, &ch) == 0;
    }
    if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED ||
        state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED || state == ESP_PEER_STATE_DATA_CHANNEL_CLOSED) {
        channel_open = false; ack_pending = false;
    }
    return 0;
}

static int on_open(esp_peer_data_channel_info_t *ch, void *ctx)
{
    if (ch->label && strcmp(ch->label, "jpeg") == 0) {
        stream_id = ch->stream_id; channel_open = true;
    }
    return 0;
}

static int on_close(esp_peer_data_channel_info_t *ch, void *ctx)
{
    if (ch->stream_id == stream_id) channel_open = false;
    return 0;
}

static int on_data(esp_peer_data_frame_t *f, void *ctx)
{
    if (f->stream_id != stream_id || f->type != ESP_PEER_DATA_CHANNEL_STRING ||
        f->size < 5 || f->size > 31) return 0;
    char text[32]; memcpy(text, f->data, f->size); text[f->size] = 0;
    if (memcmp(text, "ACK:", 4) != 0) return 0;
    if (text[4] < '0' || text[4] > '9') return 0;
    char *end = NULL;
    unsigned long raw = strtoul(text + 4, &end, 10);
    if (end == text + 4) return 0;
    int motion = -1;
    if (*end == ':') {
        char *mend = NULL;
        unsigned long m = strtoul(end + 1, &mend, 10);
        if (mend == end + 1 || *mend || m > 255) return 0;
        motion = (int)m;
    } else if (*end) {
        return 0;
    }
    uint32_t ack = (uint32_t)raw;
    // A wrap-safe signed comparison accepts delayed ACKs from older frames.
    if ((int32_t)(ack - last_ack_id) <= 0 || (int32_t)(ack - frame_id) > 0) return 0;
    last_ack_id = ack;
    int64_t now = esp_timer_get_time();
    last_ack_us = now;
    if (motion >= 0) {
        atomic_store(&client_motion, motion);
        if (motion >= CONFIG_WEBCAM_MOTION_THRESHOLD) motion_until = now + MOTION_HOLD_US;
    }
    portENTER_CRITICAL(&stats_mux);
    stats.acked_frames++;
    if (ack == awaiting_ack) stats.last_ack_rtt_ms = (uint32_t)((now - ack_since) / 1000);
    portEXIT_CRITICAL(&stats_mux);
    // Any in-range ACK proves the browser is alive. ACKs may lag the newest
    // frame by one, so do not require the newest ID to clear the watchdog.
    ack_pending = false;
    return 0;
}

static int open_peer(void)
{
    esp_peer_default_cfg_t extra = {
        .agent_recv_timeout = 2,
        .keep_role = true,
        .data_ch_cfg = {.cache_timeout = 250, .recv_cache_size = 16384, .send_cache_size = 131072},
    };
    esp_peer_cfg_t cfg = {
        .role = ESP_PEER_ROLE_CONTROLLED,
        .enable_data_channel = true, .manual_ch_create = true, .no_auto_reconnect = true,
        .on_msg = on_msg, .on_state = on_state, .on_channel_open = on_open,
        .on_channel_close = on_close, .on_data = on_data,
        .extra_cfg = &extra, .extra_size = sizeof(extra),
    };
    return esp_peer_open(&cfg, esp_peer_get_default_impl(), &peer);
}

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) p[i] = v >> (8 * i);
}

static void peer_task(void *arg)
{
    uint8_t *frame = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *packet = malloc(JPEG_CHUNK + 20);
    assert(frame && packet);
    size_t size = 0, offset = 0, pending_size = 0;
    uint32_t pending_crc = 0, last_crc = 0;
    size_t last_size = 0;
    bool have_last = false;
    camera_frame_info_t info = {0};
    int64_t next_frame = 0, send_since = 0, last_sent_at = 0, last_log = 0;
    int last_fps = CONFIG_WEBCAM_TARGET_FPS;
    ESP_ERROR_CHECK(esp_peer_pre_generate_cert());
    for (;;) {
        command_t cmd;
        if (xQueueReceive(commands, &cmd, 0) == pdTRUE) {
            if (peer) { esp_peer_close(peer); peer = NULL; }
            active_session = cmd.id;
            channel_open = false; jpeg_channel_created = false; ack_pending = false;
            size = 0; offset = 0; have_last = false; info.sequence = 0;
            next_frame = 0; last_ack_id = 0;
            atomic_store(&client_motion, -1);
            atomic_store(&effective_fps, CONFIG_WEBCAM_TARGET_FPS);
            motion_until = 0; last_ack_us = 0;
            stats_reset();
            xSemaphoreTake(signal_lock, portMAX_DELAY);
            if (active_session == session) { free(answer); answer = NULL; }
            xSemaphoreGive(signal_lock);
            if (cmd.sdp && open_peer() == 0) {
                int ret = esp_peer_new_connection(peer);
                if (ret == 0) {
                    esp_peer_msg_t msg = {.type = ESP_PEER_MSG_TYPE_SDP,
                                         .data = (uint8_t *)cmd.sdp, .size = strlen(cmd.sdp)};
                    ret = esp_peer_send_msg(peer, &msg);
                }
                if (ret) ESP_LOGE(TAG, "Remote SDP failed: %d", ret);
            }
            free(cmd.sdp);
        }
        if (peer) esp_peer_main_loop(peer);
        int64_t now = esp_timer_get_time();
        if (ack_pending && now - ack_since > 2000000) {
            ack_pending = false;
            stats_bump(&stats.no_ack);
            ESP_LOGW(TAG, "No frame acknowledgement for 2 s");
        }
        int cm = atomic_load(&client_motion);
        int fps = CONFIG_WEBCAM_TARGET_FPS;
        // The browser reports how much the decoded picture changed. Drop to
        // idle FPS only while that report is fresh and the motion hold expired.
        // An old client sends no score and keeps the original target rate.
        if (cm >= 0 && last_ack_us && now - last_ack_us < 3000000 && now >= motion_until)
            fps = CONFIG_WEBCAM_IDLE_FPS;
        // Motion just returned: do not wait out the idle slot.
        if (fps > last_fps && !size) next_frame = now;
        last_fps = fps;
        atomic_store(&effective_fps, fps);
        const int64_t period_us = 1000000 / fps;
        const int64_t budget_us = period_us * 3 / 2;
        if (!channel_open) { size = 0; next_frame = 0; }
        // Read at most once per target period. The ACK is a freshness hint,
        // not a transmit lock: waiting for it used to cap the video below the
        // 6 fps goal. A slow viewer now loses old frames instead of stopping
        // the capture pipeline.
        if (channel_open && !size && now >= next_frame) {
            camera_frame_info_t got = info;
            size_t n = camera_read_frame(frame, &got);
            info = got;
            bool stale = n && got.captured_us && now - got.captured_us > budget_us;
            bool same = n && have_last && got.crc32 == last_crc && got.size == last_size &&
                (CONFIG_WEBCAM_KEEPALIVE_MS == 0 || now - last_sent_at < KEEPALIVE_US);
            if (!n) {
                // No complete frame yet. Retry quickly without spinning.
                next_frame = now + period_us / 4;
            } else if (stale) {
                stats_bump(&stats.stale_frames);
                next_frame = now + period_us;
            } else if (same) {
                // The viewer already shows this exact JPEG. Do not spend
                // Wi-Fi airtime until it changes or the keepalive expires.
                stats_bump(&stats.skipped_identical);
                next_frame = now + period_us;
            } else {
                offset = 0; send_since = now; next_frame = now + period_us;
                pending_size = n; pending_crc = got.crc32;
                frame_id++;
            }
        }
        if (size && now - send_since > budget_us) {
            // Never keep transmitting an old frame that missed its slot.
            stats_bump(&stats.dropped_frames);
            size = 0; next_frame = now;
        }
        if (size) {
            size_t n = size - offset;
            if (n > JPEG_CHUNK) n = JPEG_CHUNK;
            memcpy(packet, "JPG1", 4);
            put32(packet + 4, frame_id); put32(packet + 8, size);
            put32(packet + 12, offset); put32(packet + 16, (uint32_t)(send_since / 1000));
            memcpy(packet + 20, frame + offset, n);
            esp_peer_data_frame_t data = {.type = ESP_PEER_DATA_CHANNEL_DATA,
                .stream_id = stream_id, .data = packet, .size = n + 20};
            int ret = esp_peer_send_data(peer, &data);
            if (!ret) {
                offset += n;
                if (offset == size) {
                    stats_frame_sent(pending_size);
                    last_crc = pending_crc; last_size = pending_size; have_last = true;
                    last_sent_at = now;
                    awaiting_ack = frame_id; ack_since = now; ack_pending = true;
                    size = 0;
                }
            } else if (ret != ESP_PEER_ERR_WOULD_BLOCK) {
                stats_bump(&stats.send_errors);
                size = 0; next_frame = now;
            }
        }
        if (now - last_log >= 5000000) {
            sender_stats_t s;
            portENTER_CRITICAL(&stats_mux); s = stats; portEXIT_CRITICAL(&stats_mux);
            ESP_LOGI(TAG, "sent %"PRIu32" frames, %"PRIu64" KiB, skip %"PRIu32
                     ", stale %"PRIu32", drop %"PRIu32", err %"PRIu32", ack %"PRIu32
                     ", fps %d, motion %d", s.sent_frames, (s.sent_bytes + 1023) / 1024,
                     s.skipped_identical, s.stale_frames, s.dropped_frames, s.send_errors,
                     s.acked_frames, fps, cm);
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

extern const uint8_t html_start[] asm("_binary_index_html_start");
extern const uint8_t js_start[] asm("_binary_receiver_js_start");
extern const uint8_t cert_start[] asm("_binary_server_crt_start");
extern const uint8_t cert_end[] asm("_binary_server_crt_end");
extern const uint8_t key_start[] asm("_binary_server_key_start");
extern const uint8_t key_end[] asm("_binary_server_key_end");

static esp_err_t page(httpd_req_t *r)
{
    bool js = strcmp(r->uri, "/receiver.js") == 0;
    httpd_resp_set_type(r, js ? "text/javascript" : "text/html; charset=utf-8");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_sendstr(r, (char *)(js ? js_start : html_start));
}

static esp_err_t offer(httpd_req_t *r)
{
    if (r->content_len < 1 || r->content_len > 16384)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "SDP size invalid");
    char *sdp = malloc(r->content_len + 1);
    if (!sdp) return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "No memory");
    int got = 0;
    while (got < r->content_len) {
        int n = httpd_req_recv(r, sdp + got, r->content_len - got);
        if (n <= 0) { free(sdp); return ESP_FAIL; }
        got += n;
    }
    sdp[got] = 0;
    // LAN-only signaling is direct from the browser. Expand obfuscated host
    // candidates to that browser's observed IPv4 address, without changing
    // browser privacy settings or needing multicast DNS through a hotspot.
    struct sockaddr_in remote;
    socklen_t remote_len = sizeof(remote);
    if (getpeername(httpd_req_to_sockfd(r), (struct sockaddr *)&remote, &remote_len) == 0 &&
        remote.sin_family == AF_INET) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &remote.sin_addr, ip, sizeof(ip));
        char *expanded = malloc(32769);
        size_t used = 0;
        bool ok = expanded != NULL;
        for (char *line = sdp; ok && *line;) {
            char *end = strchr(line, '\n');
            size_t len = end ? (size_t)(end + 1 - line) : strlen(line);
            char *address = NULL, *address_end = NULL;
            if (strncmp(line, "a=candidate:", 12) == 0) {
                char *p = line + 12;
                for (int field = 0; field < 4 && p < line + len; field++) {
                    while (p < line + len && *p != ' ') p++;
                    while (p < line + len && *p == ' ') p++;
                }
                char *q = p;
                while (q < line + len && *q != ' ' && *q != '\r' && *q != '\n') q++;
                char *host = strstr(line, " typ host");
                if (host && host < line + len && q - p > 6 &&
                    memcmp(q - 6, ".local", 6) == 0) {
                    address = p; address_end = q;
                }
            }
            size_t needed = address ? len - (address_end - address) + strlen(ip) : len;
            if (used + needed > 32768) { ok = false; break; }
            if (address) {
                size_t prefix = address - line;
                memcpy(expanded + used, line, prefix); used += prefix;
                memcpy(expanded + used, ip, strlen(ip)); used += strlen(ip);
                memcpy(expanded + used, address_end, len - (address_end - line));
                used += len - (address_end - line);
            } else { memcpy(expanded + used, line, len); used += len; }
            line += len;
        }
        if (ok) { expanded[used] = 0; free(sdp); sdp = expanded; }
        else free(expanded);
    }
    xSemaphoreTake(signal_lock, portMAX_DELAY);
    command_t cmd = {.sdp = sdp, .id = session + 1};
    if (xQueueSend(commands, &cmd, 0) != pdTRUE) {
        xSemaphoreGive(signal_lock);
        free(sdp); httpd_resp_set_status(r, "503 Service Unavailable");
        return httpd_resp_sendstr(r, "Busy");
    }
    session = cmd.id; free(answer); answer = NULL;
    xSemaphoreGive(signal_lock);
    char text[16]; snprintf(text, sizeof(text), "%"PRIu32, cmd.id);
    return httpd_resp_sendstr(r, text);
}

static esp_err_t get_answer(httpd_req_t *r)
{
    char query[32], id[16];
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id, sizeof(id)) != ESP_OK)
        return httpd_resp_send_err(r, HTTPD_400_BAD_REQUEST, "Missing session");
    xSemaphoreTake(signal_lock, portMAX_DELAY);
    if (strtoul(id, NULL, 10) != session) {
        xSemaphoreGive(signal_lock); httpd_resp_set_status(r, "409 Conflict");
        return httpd_resp_sendstr(r, "Session replaced");
    }
    char *copy = answer ? strdup(answer) : NULL;
    xSemaphoreGive(signal_lock);
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    if (!copy) httpd_resp_set_status(r, "202 Accepted");
    esp_err_t ret = httpd_resp_sendstr(r, copy ? copy : "");
    free(copy); return ret;
}

static esp_err_t rtc_status(httpd_req_t *r)
{
    sender_stats_t s;
    portENTER_CRITICAL(&stats_mux); s = stats; portEXIT_CRITICAL(&stats_mux);
    char json[768];
    int len = snprintf(json, sizeof(json),
        "{\"peer_state\":%d,\"channel_open\":%s,\"target_fps\":%d,"
        "\"effective_fps\":%d,\"idle_fps\":%d,\"client_motion\":%d,"
        "\"sent_frames\":%"PRIu32",\"sent_kib\":%"PRIu64",\"send_errors\":%"PRIu32","
        "\"dropped_frames\":%"PRIu32",\"skipped_identical\":%"PRIu32","
        "\"stale_frames\":%"PRIu32",\"acked_frames\":%"PRIu32","
        "\"last_ack_rtt_ms\":%"PRIu32",\"no_ack\":%"PRIu32"}",
        peer_state, channel_open ? "true" : "false", CONFIG_WEBCAM_TARGET_FPS,
        atomic_load(&effective_fps), CONFIG_WEBCAM_IDLE_FPS, atomic_load(&client_motion),
        s.sent_frames, (s.sent_bytes + 1023) / 1024, s.send_errors,
        s.dropped_frames, s.skipped_identical, s.stale_frames, s.acked_frames,
        s.last_ack_rtt_ms, s.no_ack);
    if (len < 0 || len >= sizeof(json))
        return httpd_resp_send_err(r, HTTPD_500_INTERNAL_SERVER_ERROR, "Status buffer too small");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    return httpd_resp_send(r, json, len);
}

static void start_server(void)
{
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.httpd.stack_size = 10240;
    cfg.httpd.ctrl_port = 32770;
    cfg.httpd.max_open_sockets = 3;
    cfg.servercert = cert_start; cfg.servercert_len = cert_end - cert_start;
    cfg.prvtkey_pem = key_start; cfg.prvtkey_len = key_end - key_start;
    httpd_handle_t server;
    ESP_ERROR_CHECK(httpd_ssl_start(&server, &cfg));
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page},
        {.uri = "/receiver.js", .method = HTTP_GET, .handler = page},
        {.uri = "/offer", .method = HTTP_POST, .handler = offer},
        {.uri = "/answer", .method = HTTP_GET, .handler = get_answer},
        {.uri = "/rtc-status", .method = HTTP_GET, .handler = rtc_status},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); i++)
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &routes[i]));
}

void webrtc_start(void)
{
    commands = xQueueCreate(1, sizeof(command_t));
    signal_lock = xSemaphoreCreateMutex();
    assert(commands && signal_lock);
    esp_peer_set_dtls_cipher_pref(ESP_PEER_DTLS_CIPHER_AES_GCM);
    assert(xTaskCreatePinnedToCore(peer_task, "webrtc", 16384, NULL, 6, NULL, 1) == pdPASS);
    start_server();
}

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
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_peer_default.h"
#include "camera.h"
#include "webrtc.h"

/* All peer APIs are called by peer_task; callbacks execute in that task too. */
static const char *TAG = "webcam";
static esp_peer_handle_t peer;
static QueueHandle_t commands;
static SemaphoreHandle_t answer_lock;
static char *pending_answer;
static uint32_t pending_answer_id;
static int64_t pending_answer_at;
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
static char stun_url[] = CONFIG_WEBCAM_STUN_URL;
static esp_peer_ice_server_cfg_t ice_server = {.stun_url = stun_url};
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

int webrtc_status_json(char *out, size_t size)
{
    sender_stats_t s;
    portENTER_CRITICAL(&stats_mux);
    s = stats;
    portEXIT_CRITICAL(&stats_mux);
    return snprintf(out, size,
        "{\"peer_state\":%d,\"channel_open\":%s,\"stream_id\":%u,"
        "\"frame_id\":%"PRIu32",\"effective_fps\":%d,\"client_motion\":%d,"
        "\"sent_frames\":%"PRIu32",\"sent_bytes\":%"PRIu64","
        "\"send_errors\":%"PRIu32",\"dropped_frames\":%"PRIu32","
        "\"skipped_identical\":%"PRIu32",\"stale_frames\":%"PRIu32","
        "\"acked_frames\":%"PRIu32",\"no_ack\":%"PRIu32","
        "\"last_ack_rtt_ms\":%"PRIu32",\"ack_pending\":%s}",
        peer_state, atomic_load(&channel_open) ? "true" : "false", stream_id,
        frame_id, atomic_load(&effective_fps), atomic_load(&client_motion),
        s.sent_frames, s.sent_bytes, s.send_errors, s.dropped_frames,
        s.skipped_identical, s.stale_frames, s.acked_frames, s.no_ack,
        s.last_ack_rtt_ms, ack_pending ? "true" : "false");
}

static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        char *copy = strndup((char *)msg->data, msg->size);
        // Only publish an answer. Initial ICE gathering may report an offer.
        // Hold the answer briefly in signaling_task so the last candidate
        // update from gathering wins before it is posted to the Node server.
        if (!copy || strstr(copy, "a=setup:actpass")) { free(copy); return 0; }
        xSemaphoreTake(answer_lock, portMAX_DELAY);
        free(pending_answer);
        pending_answer = copy;
        pending_answer_id = active_session;
        pending_answer_at = esp_timer_get_time() + 300000;
        xSemaphoreGive(answer_lock);
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
    bool use_stun = CONFIG_WEBCAM_STUN_URL[0] != '\0';
    esp_peer_cfg_t cfg = {
        .server_lists = use_stun ? &ice_server : NULL,
        .server_num = use_stun ? 1 : 0,
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
            xSemaphoreTake(answer_lock, portMAX_DELAY);
            free(pending_answer);
            pending_answer = NULL;
            xSemaphoreGive(answer_lock);
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
                size = n; pending_size = n; pending_crc = got.crc32;
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

#define SDP_MAX 32768

static void build_url(char *out, size_t size, const char *path)
{
    size_t len = strlen(CONFIG_WEBCAM_SIGNALING_URL);
    while (len && CONFIG_WEBCAM_SIGNALING_URL[len - 1] == '/') len--;
    snprintf(out, size, "%.*s%s", (int)len, CONFIG_WEBCAM_SIGNALING_URL, path);
}

// Small blocking HTTP request used only by signaling_task. The SDP bodies are
// bounded so the task can return to polling quickly even on a dead server.
static int http_request(const char *url, const char *body, char *out, size_t out_size,
                        int *status)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 5000,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) return -1;
    int body_len = body ? (int)strlen(body) : 0;
    if (body) {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/sdp");
    }
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK && body && esp_http_client_write(client, body, body_len) != body_len)
        err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0)
        err = ESP_FAIL;
    int total = 0;
    if (err == ESP_OK && out && out_size) {
        while (total < (int)out_size - 1) {
            int n = esp_http_client_read(client, out + total, (int)out_size - 1 - total);
            if (n < 0) { err = ESP_FAIL; break; }
            if (n == 0) break;
            total += n;
        }
        out[total] = 0;
    }
    *status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK ? total : -1;
}

static int ret_delay(int status)
{
    return status == 0 ? 1000 : 250;
}

static void submit_offer(const char *sdp, uint32_t id)
{
    char *copy = strdup(sdp);
    if (!copy) return;
    command_t cmd = {.sdp = copy, .id = id};
    if (xQueueSend(commands, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Offer queue full; dropping offer %"PRIu32, id);
        free(copy);
        return;
    }
    xSemaphoreTake(answer_lock, portMAX_DELAY);
    free(pending_answer);
    pending_answer = NULL;
    xSemaphoreGive(answer_lock);
    ESP_LOGI(TAG, "Signaling offer %"PRIu32" (%u bytes)", id, (unsigned)strlen(sdp));
}

static void signaling_task(void *arg)
{
    char offer_url[256], answer_url[256];
    char *sdp = malloc(SDP_MAX);
    char *reply = malloc(512);
    assert(sdp && reply);
    uint32_t last_offer_id = 0;
    for (;;) {
        int status = 0;
        snprintf(offer_url, sizeof(offer_url), "/api/esp/offer?seen=%"PRIu32, last_offer_id);
        char url[320];
        build_url(url, sizeof(url), offer_url);
        int got = http_request(url, NULL, sdp, SDP_MAX, &status);
        if (got > 0 && status == 200) {
            char *line = strchr(sdp, '\n');
            if (line) {
                *line = 0;
                uint32_t id = (uint32_t)strtoul(sdp, NULL, 10);
                if (id && id != last_offer_id) {
                    submit_offer(line + 1, id);
                    last_offer_id = id;
                }
            }
        }

        char *answer = NULL;
        uint32_t answer_id = 0;
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(answer_lock, portMAX_DELAY);
        if (pending_answer && now >= pending_answer_at) {
            answer = pending_answer;
            answer_id = pending_answer_id;
            pending_answer = NULL;
        }
        xSemaphoreGive(answer_lock);
        if (answer) {
            snprintf(answer_url, sizeof(answer_url), "/api/esp/answer?id=%"PRIu32, answer_id);
            build_url(url, sizeof(url), answer_url);
            int post_status = 0;
            int ret = http_request(url, answer, reply, 512, &post_status);
            if (ret < 0) {
                // Restore only if no newer answer arrived while posting.
                xSemaphoreTake(answer_lock, portMAX_DELAY);
                if (!pending_answer) {
                    pending_answer = answer;
                    pending_answer_id = answer_id;
                    pending_answer_at = esp_timer_get_time() + 500000;
                    answer = NULL;
                }
                xSemaphoreGive(answer_lock);
                ESP_LOGW(TAG, "Answer POST failed; will retry");
            } else if (post_status == 200) {
                ESP_LOGI(TAG, "Signaling answer %"PRIu32" sent", answer_id);
            } else {
                ESP_LOGW(TAG, "Answer %"PRIu32" rejected: HTTP %d", answer_id, post_status);
            }
            free(answer);
        }
        vTaskDelay(pdMS_TO_TICKS(ret_delay(status)));
    }
}

void webrtc_start(void)
{
    commands = xQueueCreate(2, sizeof(command_t));
    answer_lock = xSemaphoreCreateMutex();
    assert(commands && answer_lock);
    esp_peer_set_dtls_cipher_pref(ESP_PEER_DTLS_CIPHER_AES_GCM);
    if (stun_url[0]) ESP_LOGI(TAG, "ICE STUN server: %s", stun_url);
    else ESP_LOGW(TAG, "ICE STUN server disabled; using host candidates only");
    assert(xTaskCreatePinnedToCore(peer_task, "webrtc", 16384, NULL, 6, NULL, 1) == pdPASS);
    if (CONFIG_WEBCAM_SIGNALING_URL[0]) {
        ESP_LOGI(TAG, "Signaling server: %s", CONFIG_WEBCAM_SIGNALING_URL);
        assert(xTaskCreatePinnedToCore(signaling_task, "signal", 8192, NULL, 5, NULL, 0) == pdPASS);
    } else {
        ESP_LOGW(TAG, "Signaling server disabled; WebRTC will not start");
    }
}

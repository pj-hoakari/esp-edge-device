#include "peer_authorization.h"
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
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_peer_default.h"
#include "camera.h"
#include "webrtc.h"
#include "sender_policy.h"
#include "perf.h"
#include "nvs.h"
#include "network_settings.h"
#include "cloud_signaling.h"

/* All peer APIs are called by peer_task; callbacks execute in that task too. */
static const char *TAG = "webcam";
static esp_peer_handle_t peer;
static QueueHandle_t commands;
static SemaphoreHandle_t answer_lock;
static char *pending_answer;
static char pending_answer_session[64];
static int64_t pending_answer_at;
static char active_session_id[64];
static uint16_t stream_id;
static atomic_bool channel_open;
static bool jpeg_channel_created;
static volatile int peer_state;
static uint32_t frame_id, awaiting_ack, last_ack_id;
static bool ack_pending;
static int64_t ack_since;
static int64_t last_ack_us;
static atomic_int client_motion = -1;
static atomic_int effective_fps = CONFIG_WEBCAM_TARGET_FPS;
// Real-device HD benchmarks were faster with ChaCha20 than hardware AES-GCM.
static atomic_int cipher_pref = ESP_PEER_DTLS_CIPHER_CHACHA;
static atomic_int burst_chunks = SENDER_BURST_CHUNKS;
static atomic_int burst_time_us = SENDER_BURST_US;
static atomic_bool tuning_saved;
static atomic_bool fast_rx = true;
static char stun_url[] = CONFIG_WEBCAM_STUN_URL;
static esp_peer_ice_server_cfg_t ice_server = {.stun_url = stun_url};
typedef struct { char *sdp; char session_id[64]; } command_t;

typedef struct {
    uint32_t sent_frames, send_errors, dropped_frames;
    uint32_t skipped_identical, stale_frames, acked_frames, no_ack;
    uint32_t last_ack_rtt_ms;
    uint32_t would_block, max_send_ms;
    uint32_t send_fps_milli, display_fps_milli;
    uint64_t send_time_us;
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

static void stats_frame_sent(size_t bytes, int64_t elapsed)
{
    portENTER_CRITICAL(&stats_mux);
    stats.sent_frames++;
    stats.sent_bytes += bytes;
    stats.send_time_us += elapsed;
    if (elapsed / 1000 > stats.max_send_ms) stats.max_send_ms = elapsed / 1000;
    portEXIT_CRITICAL(&stats_mux);
}

static void stats_reset(void)
{
    portENTER_CRITICAL(&stats_mux);
    memset(&stats, 0, sizeof(stats));
    portEXIT_CRITICAL(&stats_mux);
}

bool webrtc_video_active(void) { return atomic_load(&channel_open); }

bool webrtc_fast_rx(void) { return atomic_load(&fast_rx); }

bool webrtc_configure(int cipher, unsigned chunks, unsigned burst_us, bool poll)
{
    if (cipher < 0 || cipher > 2 || chunks < 1 || chunks > 16 || burst_us < 1000 || burst_us > 64000)
        return false;
    atomic_store(&cipher_pref, cipher);
    atomic_store(&burst_chunks, chunks);
    atomic_store(&burst_time_us, burst_us);
    atomic_store(&fast_rx, poll);
    atomic_store(&tuning_saved, false);
    return true;
}

bool webrtc_save_tuning(void)
{
    nvs_handle_t handle;
    if (nvs_open("rtc_tuning", NVS_READWRITE, &handle) != ESP_OK) return false;
    esp_err_t err = nvs_set_u8(handle, "cipher", atomic_load(&cipher_pref));
    if (err == ESP_OK) err = nvs_set_u8(handle, "chunks", atomic_load(&burst_chunks));
    if (err == ESP_OK) err = nvs_set_u16(handle, "burst_us", atomic_load(&burst_time_us));
    if (err == ESP_OK) err = nvs_set_u8(handle, "fast_rx", atomic_load(&fast_rx));
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    atomic_store(&tuning_saved, err == ESP_OK);
    return err == ESP_OK;
}

static void load_tuning(void)
{
    nvs_handle_t handle;
    if (nvs_open("rtc_tuning", NVS_READONLY, &handle) != ESP_OK) return;
    uint8_t cipher, chunks, poll = 1; uint16_t us;
    bool found = nvs_get_u8(handle, "cipher", &cipher) == ESP_OK &&
        nvs_get_u8(handle, "chunks", &chunks) == ESP_OK &&
        nvs_get_u16(handle, "burst_us", &us) == ESP_OK;
    nvs_get_u8(handle, "fast_rx", &poll);
    nvs_close(handle);
    if (found && webrtc_configure(cipher, chunks, us, poll != 0)) atomic_store(&tuning_saved, true);
}

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
        "\"last_ack_rtt_ms\":%"PRIu32",\"ack_pending\":%s,"
        "\"would_block\":%"PRIu32",\"max_send_ms\":%"PRIu32","
        "\"mean_send_ms\":%"PRIu64",\"send_fps\":%.3f,\"display_fps\":%.3f,"
        "\"cipher_pref\":%d,\"burst_chunks\":%d,\"burst_us\":%d,\"fast_rx\":%s,\"tuning_saved\":%s}",
        peer_state, atomic_load(&channel_open) ? "true" : "false", stream_id,
        frame_id, atomic_load(&effective_fps), atomic_load(&client_motion),
        s.sent_frames, s.sent_bytes, s.send_errors, s.dropped_frames,
        s.skipped_identical, s.stale_frames, s.acked_frames, s.no_ack,
        s.last_ack_rtt_ms, ack_pending ? "true" : "false",
        s.would_block, s.max_send_ms,
        s.sent_frames ? s.send_time_us / s.sent_frames / 1000 : 0,
        s.send_fps_milli / 1000.0, s.display_fps_milli / 1000.0,
        atomic_load(&cipher_pref), atomic_load(&burst_chunks), atomic_load(&burst_time_us),
        webrtc_fast_rx() ? "true" : "false",
        atomic_load(&tuning_saved) ? "true" : "false");
}

static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (msg->type == ESP_PEER_MSG_TYPE_SDP) {
        char *copy = strndup((char *)msg->data, msg->size);
        // Only publish an answer. Initial ICE gathering may report an offer.
        // Hold the answer briefly so the last candidate update from gathering
        // wins before cloud signaling writes it to Firestore.
        if (!copy || strstr(copy, "a=setup:actpass")) { free(copy); return 0; }
        xSemaphoreTake(answer_lock, portMAX_DELAY);
        free(pending_answer);
        pending_answer = copy;
        memcpy(pending_answer_session, active_session_id, sizeof(pending_answer_session));
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
        last_ack_us = esp_timer_get_time();
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
    esp_peer_set_dtls_cipher_pref(atomic_load(&cipher_pref));
    esp_peer_default_cfg_t extra = {
        .agent_recv_timeout = 1,
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
    uint8_t *packet = heap_caps_malloc(JPEG_CHUNK + 20, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    assert(frame && packet);
    size_t size = 0, offset = 0, packet_size = 0;
    camera_frame_info_t info = {0};
    int64_t next_frame = 0, send_since = 0, last_log = 0;
    uint32_t sampled_sent = 0, sampled_acked = 0;
    ESP_ERROR_CHECK(esp_peer_pre_generate_cert());
    for (;;) {
        command_t cmd;
        if (peer_authorization_take_disconnect()) {
            if (peer) { esp_peer_close(peer); peer = NULL; }
            channel_open = false; jpeg_channel_created = false; ack_pending = false;
            size = 0; offset = 0; packet_size = 0; next_frame = 0;
            active_session_id[0] = 0;
            stream_id = 0; last_ack_us = 0; last_ack_id = 0;
            while (xQueueReceive(commands, &cmd, 0) == pdTRUE) free(cmd.sdp);
            xSemaphoreTake(answer_lock, portMAX_DELAY);
            free(pending_answer);
            pending_answer = NULL;
            pending_answer_session[0] = 0; pending_answer_at = 0;
            xSemaphoreGive(answer_lock);
        }
        if (xQueueReceive(commands, &cmd, 0) == pdTRUE) {
            if (peer) { esp_peer_close(peer); peer = NULL; }
            memcpy(active_session_id, cmd.session_id, sizeof(active_session_id));
            channel_open = false; jpeg_channel_created = false; ack_pending = false;
            size = 0; offset = 0; packet_size = 0; info.sequence = 0;
            next_frame = 0; last_ack_id = 0;
            last_ack_us = esp_timer_get_time();
            atomic_store(&client_motion, -1);
            atomic_store(&effective_fps, CONFIG_WEBCAM_TARGET_FPS);
            stats_reset();
            perf_reset();
            sampled_sent = 0; sampled_acked = 0; last_log = esp_timer_get_time();
            xSemaphoreTake(answer_lock, portMAX_DELAY);
            free(pending_answer);
            pending_answer = NULL;
            pending_answer_session[0] = 0; pending_answer_at = 0;
            xSemaphoreGive(answer_lock);
            if (!peer_authorization_blocked() && cmd.sdp && open_peer() == 0) {
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
        if (peer) {
            int64_t before = esp_timer_get_time();
            esp_peer_main_loop(peer);
            perf_record(PERF_PEER_LOOP, esp_timer_get_time() - before, 0, 0);
        }
        int64_t now = esp_timer_get_time();
        if (ack_pending && now - ack_since > 2000000) {
            ack_pending = false;
            stats_bump(&stats.no_ack);
            ESP_LOGW(TAG, "No frame acknowledgement for 2 s");
        }
        if (peer && channel_open && last_ack_us && now - last_ack_us > 10000000) {
            // The viewer vanished without closing SCTP. Drop the peer so the
            // signaling task can resume offer polling for a new viewer.
            ESP_LOGW(TAG, "No acknowledgement for 10 s; closing peer");
            esp_peer_close(peer);
            peer = NULL;
            channel_open = false;
            ack_pending = false;
        }
        int cm = atomic_load(&client_motion);
        int fps = CONFIG_WEBCAM_TARGET_FPS;
        if (fps < SENDER_MIN_FPS) fps = SENDER_MIN_FPS;
        atomic_store(&effective_fps, fps);
        const int64_t period_us = 1000000 / fps;
        const int64_t budget_us = SENDER_FRAME_BUDGET_US;
        if (!channel_open) { size = 0; next_frame = 0; }
        // Read at most once per target period. The ACK is a freshness hint,
        // not a transmit lock: waiting for it used to cap the video below the
        // 6 fps goal. A slow viewer now loses old frames instead of stopping
        // the capture pipeline.
        if (!peer_authorization_blocked() && channel_open && !size && now >= next_frame) {
            camera_frame_info_t got = info;
            int64_t before = esp_timer_get_time();
            size_t n = camera_read_frame(frame, &got);
            perf_record(PERF_COPY, esp_timer_get_time() - before, n, 0);
            info = got;
            bool stale = n && got.captured_us && now - got.captured_us > budget_us;
            if (!n) {
                // Keep the pacing phase. The loop sleeps and pumps ICE/SCTP
                // before trying again; an empty slot must not cost 25-42 ms.
            } else if (stale) {
                stats_bump(&stats.stale_frames);
            } else {
                offset = 0; packet_size = 0; send_since = now;
                next_frame = sender_next_slot(next_frame, now, period_us);
                size = n;
                frame_id++;
            }
        }
        if (size && now - send_since > budget_us) {
            // Never keep transmitting an old frame that missed its slot.
            stats_bump(&stats.dropped_frames);
            size = 0; next_frame = now;
        }
        // Amortize the event-loop receive timeout and task delay across up to
        // four chunks. Bound each burst so ACK/SACK/ICE processing still runs.
        int64_t burst_start = esp_timer_get_time();
        for (int chunk = 0; size && !peer_authorization_blocked() && chunk < atomic_load(&burst_chunks); chunk++) {
            now = esp_timer_get_time();
            if (now - burst_start >= atomic_load(&burst_time_us)) break;
            if (now - send_since > budget_us) {
                stats_bump(&stats.dropped_frames);
                size = 0; next_frame = now;
                break;
            }
            size_t n = size - offset;
            if (n > JPEG_CHUNK) n = JPEG_CHUNK;
            if (!packet_size) {
                memcpy(packet, "JPG1", 4);
                put32(packet + 4, frame_id); put32(packet + 8, size);
                put32(packet + 12, offset); put32(packet + 16, (uint32_t)(send_since / 1000));
                memcpy(packet + 20, frame + offset, n);
                packet_size = n + 20;
            }
            esp_peer_data_frame_t data = {.type = ESP_PEER_DATA_CHANNEL_DATA,
                .stream_id = stream_id, .data = packet, .size = packet_size};
            int64_t before = esp_timer_get_time();
            int ret = esp_peer_send_data(peer, &data);
            perf_record(PERF_SEND, esp_timer_get_time() - before, ret ? 0 : data.size, ret);
            if (!ret) {
                offset += n;
                packet_size = 0;
                if (offset == size) {
                    now = esp_timer_get_time();
                    stats_frame_sent(size, now - send_since);
                    awaiting_ack = frame_id; ack_since = now; ack_pending = true;
                    size = 0;
                }
            } else {
                if (ret == ESP_PEER_ERR_WOULD_BLOCK) {
                    stats_bump(&stats.would_block);
                } else {
                    stats_bump(&stats.send_errors);
                    size = 0; next_frame = now;
                }
                break;
            }
        }
        if (now - last_log >= 5000000) {
            sender_stats_t s;
            portENTER_CRITICAL(&stats_mux);
            stats.send_fps_milli = (uint64_t)(stats.sent_frames - sampled_sent) * 1000000000ULL / (now - last_log);
            stats.display_fps_milli = (uint64_t)(stats.acked_frames - sampled_acked) * 1000000000ULL / (now - last_log);
            s = stats;
            portEXIT_CRITICAL(&stats_mux);
            sampled_sent = s.sent_frames; sampled_acked = s.acked_frames;
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

bool webrtc_submit_offer(const char *sdp, const char *session_id)
{
    if (peer_authorization_blocked() || !commands || !sdp || !session_id) return false;
    char *copy = strdup(sdp);
    if (!copy) return false;
    command_t cmd = {.sdp = copy};
    snprintf(cmd.session_id, sizeof(cmd.session_id), "%s", session_id);
    if (xQueueSend(commands, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Offer queue full; dropping session %s", session_id);
        free(copy);
        return false;
    }
    xSemaphoreTake(answer_lock, portMAX_DELAY);
    free(pending_answer);
    pending_answer = NULL;
    xSemaphoreGive(answer_lock);
    return true;
}

bool webrtc_take_answer(char **sdp, char *session_id, size_t size)
{
    if (!sdp || !session_id || !size) return false;
    *sdp = NULL;
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(answer_lock, portMAX_DELAY);
    if (pending_answer && now >= pending_answer_at) {
        *sdp = pending_answer;
        pending_answer = NULL;
        snprintf(session_id, size, "%s", pending_answer_session);
    }
    xSemaphoreGive(answer_lock);
    return *sdp != NULL;
}

void webrtc_start(void)
{
    load_tuning();
    commands = xQueueCreate(2, sizeof(command_t));
    answer_lock = xSemaphoreCreateMutex();
    assert(commands && answer_lock);
    esp_peer_set_dtls_cipher_pref(atomic_load(&cipher_pref));
    if (stun_url[0]) ESP_LOGI(TAG, "ICE STUN server: %s", stun_url);
    else ESP_LOGW(TAG, "ICE STUN server disabled; using host candidates only");
    assert(xTaskCreatePinnedToCore(peer_task, "webrtc", 16384, NULL, 6, NULL, 1) == pdPASS);
    if (network_settings_active()->signaling_url[0]) {
        ESP_LOGI(TAG, "Signaling server: %s", network_settings_active()->signaling_url);
        cloud_signaling_start();
    } else {
        ESP_LOGW(TAG, "Signaling server disabled; WebRTC will not start");
    }
}

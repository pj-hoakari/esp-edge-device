#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_peer_default.h"
#include "sdkconfig.h"
#include "camera_h264.h"
#include "cloud_signaling.h"
#include "network_settings.h"
#include "peer_authorization.h"
#include "webrtc.h"

static const char *TAG = "webcam";
static esp_peer_handle_t peer;
static QueueHandle_t commands;
static SemaphoreHandle_t answer_lock;
static char *pending_answer;
static char pending_answer_session[64];
static int64_t pending_answer_at;
static char active_session_id[64];
static atomic_int peer_state;
static atomic_bool media_connected;
static atomic_uint sent_frames, send_errors, sent_kib;
static char stun_url[] = CONFIG_WEBCAM_STUN_URL;
static esp_peer_ice_server_cfg_t ice_server = {.stun_url = stun_url};
typedef struct { char *sdp; char session_id[64]; } command_t;

static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (msg->type != ESP_PEER_MSG_TYPE_SDP) return 0;
    char *copy = strndup((char *)msg->data, msg->size);
    if (!copy || strstr(copy, "a=setup:actpass")) { free(copy); return 0; }
    xSemaphoreTake(answer_lock, portMAX_DELAY);
    free(pending_answer);
    pending_answer = copy;
    memcpy(pending_answer_session, active_session_id, sizeof(pending_answer_session));
    pending_answer_at = esp_timer_get_time() + 300000;
    xSemaphoreGive(answer_lock);
    return 0;
}

static int on_state(esp_peer_state_t state, void *ctx)
{
    atomic_store(&peer_state, state);
    ESP_LOGI(TAG, "Peer state %d", state);
    if (state == ESP_PEER_STATE_CONNECTED) {
        atomic_store(&media_connected, true);
    } else if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CONNECT_FAILED ||
               state == ESP_PEER_STATE_CLOSED) {
        atomic_store(&media_connected, false);
    }
    return 0;
}

static int on_channel_open(esp_peer_data_channel_info_t *ch, void *ctx) { return 0; }
static int on_channel_close(esp_peer_data_channel_info_t *ch, void *ctx) { return 0; }
static int on_data(esp_peer_data_frame_t *frame, void *ctx) { return 0; }

static int open_peer(void)
{
    esp_peer_default_cfg_t extra = {
        .agent_recv_timeout = 1,
        .keep_role = true,
        .alive_binding_retries = 2,
        .data_ch_cfg = {.cache_timeout = 250, .recv_cache_size = 4096, .send_cache_size = 4096},
    };
    bool use_stun = CONFIG_WEBCAM_STUN_URL[0] != '\0';
    esp_peer_cfg_t cfg = {
        .server_lists = use_stun ? &ice_server : NULL,
        .server_num = use_stun ? 1 : 0,
        .role = ESP_PEER_ROLE_CONTROLLED,
        .video_info = {.codec = ESP_PEER_VIDEO_CODEC_H264, .width = CAMERA_WIDTH, .height = CAMERA_HEIGHT,
                       .fps = CONFIG_TAB5_VIDEO_FPS},
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        .enable_data_channel = true, .manual_ch_create = true, .no_auto_reconnect = true,
        .on_msg = on_msg, .on_state = on_state, .on_channel_open = on_channel_open,
        .on_channel_close = on_channel_close, .on_data = on_data,
        .extra_cfg = &extra, .extra_size = sizeof(extra),
    };
    return esp_peer_open(&cfg, esp_peer_get_default_impl(), &peer);
}

static void drop_peer(void)
{
    if (peer) { esp_peer_close(peer); peer = NULL; }
    atomic_store(&media_connected, false);
    active_session_id[0] = 0;
    xSemaphoreTake(answer_lock, portMAX_DELAY);
    free(pending_answer);
    pending_answer = NULL;
    pending_answer_session[0] = 0;
    pending_answer_at = 0;
    xSemaphoreGive(answer_lock);
}

static void send_frames(void)
{
    camera_h264_frame_t frame;
    while (camera_h264_acquire(&frame)) {
        if (peer && atomic_load(&media_connected) && !peer_authorization_blocked()) {
            esp_peer_video_frame_t video = {.pts = frame.pts_ms, .data = frame.data, .size = frame.size};
            if (esp_peer_send_video(peer, &video) == ESP_PEER_ERR_NONE) {
                atomic_fetch_add(&sent_frames, 1);
                atomic_fetch_add(&sent_kib, (frame.size + 1023) / 1024);
            } else {
                atomic_fetch_add(&send_errors, 1);
            }
        }
        camera_h264_release(&frame);
    }
}

static void peer_task(void *arg)
{
    int64_t last_log = esp_timer_get_time();
    unsigned logged_frames = 0;
    ESP_ERROR_CHECK(esp_peer_pre_generate_cert());
    for (;;) {
        command_t cmd;
        if (peer_authorization_take_disconnect()) {
            drop_peer();
            while (xQueueReceive(commands, &cmd, 0) == pdTRUE) free(cmd.sdp);
        }
        if (xQueueReceive(commands, &cmd, 0) == pdTRUE) {
            drop_peer();
            memcpy(active_session_id, cmd.session_id, sizeof(active_session_id));
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
        if (peer) esp_peer_main_loop(peer);
        send_frames();
        int64_t now = esp_timer_get_time();
        if (now - last_log >= 5000000) {
            unsigned frames = atomic_load(&sent_frames);
            ESP_LOGI(TAG, "video sent %u frames (%.1f fps), %u KiB, err %u, state %d",
                     frames, (frames - logged_frames) * 1e6 / (now - last_log),
                     atomic_load(&sent_kib), atomic_load(&send_errors), atomic_load(&peer_state));
            logged_frames = frames;
            last_log = now;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

bool webrtc_video_active(void)
{
    return atomic_load(&media_connected);
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
    commands = xQueueCreate(2, sizeof(command_t));
    answer_lock = xSemaphoreCreateMutex();
    assert(commands && answer_lock);
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

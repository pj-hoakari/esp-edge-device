#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdarg.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "usb/usb_host.h"
#include "camera.h"
#include "webrtc.h"

static const char *TAG = "mjpeg";
extern const char page_start[] asm("_binary_mjpeg_html_start");
static vprintf_like_t original_log;
static portMUX_TYPE log_mux = portMUX_INITIALIZER_UNLOCKED;
#define LOG_LINES 32
#define LOG_LINE_SIZE 224
static char usb_logs[8][192];
static unsigned usb_log_count;
static char rtc_logs[LOG_LINES][LOG_LINE_SIZE];
static unsigned rtc_log_count;

static int capture_usb_log(const char *format, va_list args)
{
    char line[LOG_LINE_SIZE] = {0};
    va_list copy;
    va_copy(copy, args);
    vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    // LOG_VERSION_1 supplies a full tagged line. Keep only USB/camera logs.
    if (strstr(line, "ENUM:") || strstr(line, "HUB:") || strstr(line, "HCD DWC:") ||
        strstr(line, "uvc:") || strstr(line, "camera:") || strstr(line, "USBH:")) {
        portENTER_CRITICAL(&log_mux);
        memcpy(usb_logs[usb_log_count % 8], line, sizeof(usb_logs[0]));
        usb_logs[usb_log_count % 8][sizeof(usb_logs[0]) - 1] = 0;
        usb_log_count++;
        portEXIT_CRITICAL(&log_mux);
    }
    if (strstr(line, "DTLS") || strstr(line, "esp_peer") || strstr(line, "SCTP") ||
        strstr(line, "ICE") || strstr(line, "webcam:")) {
        portENTER_CRITICAL(&log_mux);
        memcpy(rtc_logs[rtc_log_count % LOG_LINES], line, LOG_LINE_SIZE);
        rtc_logs[rtc_log_count % LOG_LINES][LOG_LINE_SIZE - 1] = 0;
        rtc_log_count++;
        portEXIT_CRITICAL(&log_mux);
    }
    return original_log ? original_log(format, args) : vprintf(format, args);
}

static esp_err_t usb_log_page(httpd_req_t *req)
{
    char lines[8][192];
    unsigned count;
    portENTER_CRITICAL(&log_mux);
    memcpy(lines, usb_logs, sizeof(lines));
    count = usb_log_count;
    portEXIT_CRITICAL(&log_mux);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    unsigned start = count > 8 ? count - 8 : 0;
    for (unsigned i = start; i < count; i++) {
        esp_err_t err = httpd_resp_sendstr_chunk(req, lines[i % 8]);
        if (err != ESP_OK) return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t rtc_log_page(httpd_req_t *req)
{
    char line[LOG_LINE_SIZE];
    unsigned count;
    portENTER_CRITICAL(&log_mux);
    count = rtc_log_count;
    portEXIT_CRITICAL(&log_mux);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    unsigned start = count > LOG_LINES ? count - LOG_LINES : 0;
    for (unsigned i = start; i < count; i++) {
        portENTER_CRITICAL(&log_mux);
        memcpy(line, rtc_logs[i % LOG_LINES], sizeof(line));
        portEXIT_CRITICAL(&log_mux);
        esp_err_t err = httpd_resp_sendstr_chunk(req, line);
        if (err != ESP_OK) return err;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t page(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, page_start);
}

static esp_err_t diagnostics(httpd_req_t *req)
{
    camera_status_t s;
    camera_get_status(&s);
    uint8_t addresses[16];
    int devices = 0;
    esp_err_t list_error = usb_host_device_addr_list_fill(16, addresses, &devices);
    const char *states[] = {"waiting", "opening", "streaming", "disconnected", "error"};
#ifdef CONFIG_USB_HOST_HW_BUFFER_BIAS_IN
    const char *fifo_bias = "in";
#else
    const char *fifo_bias = "other";
#endif
    int64_t age = s.last_valid_us ? (esp_timer_get_time() - s.last_valid_us) / 1000 : -1;
    char json[1600];
    int len = snprintf(json, sizeof(json),
        "{\"mode\":\"mjpeg\",\"state\":\"%s\",\"usb_devices\":%d,\"usb_list_error\":%d,"
        "\"uvc_detections\":%"PRIu32",\"open_attempts\":%"PRIu32","
        "\"frames_received\":%"PRIu32",\"valid_jpegs\":%"PRIu32","
        "\"invalid_jpegs\":%"PRIu32",\"oversized_jpegs\":%"PRIu32","
        "\"buffer_overflows\":%"PRIu32",\"transfer_errors\":%"PRIu32","
        "\"last_frame_bytes\":%"PRIu32",\"width\":%u,\"height\":%u,"
        "\"last_frame_age_ms\":%"PRId64",\"last_open_error\":\"%s\","
        "\"requested_width\":%d,\"requested_height\":%d,\"capture_fps\":%d,"
        "\"send_fps\":%d,\"idle_fps\":%d,\"motion_threshold\":%d,"
        "\"expected_vid\":%d,\"expected_pid\":%d,\"psram_free\":%u,"
        "\"usb_control_max\":%d,\"usb_fifo_bias\":\"%s\","
        "\"trimmed_jpegs\":%"PRIu32",\"trimmed_bytes\":%"PRIu32","
        "\"missing_soi\":%"PRIu32",\"missing_eoi\":%"PRIu32","
        "\"invalid_no_soi\":%"PRIu32",\"invalid_marker\":%"PRIu32","
        "\"invalid_truncated\":%"PRIu32",\"invalid_no_sof\":%"PRIu32","
        "\"invalid_dimensions\":%"PRIu32",\"invalid_no_sos\":%"PRIu32","
        "\"invalid_no_eoi\":%"PRIu32",\"copy_drops\":%"PRIu32"}",
        states[s.state], devices, list_error, s.uvc_detections, s.open_attempts,
        s.frames_received, s.valid_jpegs, s.invalid_jpegs, s.oversized_jpegs,
        s.buffer_overflows, s.transfer_errors, s.last_frame_bytes, s.width, s.height,
        age, esp_err_to_name(s.last_open_error),
        CAMERA_WIDTH, CAMERA_HEIGHT, CONFIG_WEBCAM_CAMERA_FPS,
        CONFIG_WEBCAM_TARGET_FPS, CONFIG_WEBCAM_IDLE_FPS, CONFIG_WEBCAM_MOTION_THRESHOLD,
        CONFIG_WEBCAM_CAMERA_VID, CONFIG_WEBCAM_CAMERA_PID,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        CONFIG_USB_HOST_CONTROL_TRANSFER_MAX_SIZE, fifo_bias,
        s.trimmed_jpegs, s.trimmed_bytes, s.missing_soi, s.missing_eoi,
        s.invalid_no_soi, s.invalid_marker, s.invalid_truncated, s.invalid_no_sof,
        s.invalid_dimensions, s.invalid_no_sos, s.invalid_no_eoi, s.copy_drops);
    if (len < 0 || len >= sizeof(json))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Status buffer too small");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

static esp_err_t rtc_status(httpd_req_t *req)
{
    char json[640];
    int len = webrtc_status_json(json, sizeof(json));
    if (len < 0 || len >= sizeof(json))
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "RTC status buffer too small");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, json, len);
}

static esp_err_t stream(httpd_req_t *req)
{
    uint8_t *jpeg = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!jpeg) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No JPEG buffer");
    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=esp_webcam_frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send_chunk(req, "\r\n", 2);
    int64_t last_write = esp_timer_get_time();
    int64_t next_send = 0;
    while (err == ESP_OK) {
        int64_t now = esp_timer_get_time();
        size_t size = 0;
        if (!webrtc_video_active() && now >= next_send) {
            size = camera_take(jpeg);
            // Decimate at the HTTP layer too. Capture stays at an advertised
            // UVC rate, but the diagnostic stream costs no more than WebRTC.
            if (size) next_send = now + 1000000 / CONFIG_WEBCAM_TARGET_FPS;
        }
        if (size) {
            char header[128];
            int n = snprintf(header, sizeof(header),
                "--esp_webcam_frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", (unsigned)size);
            err = httpd_resp_send_chunk(req, header, n);
            if (err == ESP_OK) err = httpd_resp_send_chunk(req, (char *)jpeg, size);
            if (err == ESP_OK) err = httpd_resp_send_chunk(req, "\r\n", 2);
            last_write = esp_timer_get_time();
        } else if (esp_timer_get_time() - last_write > 2000000) {
            // Whitespace before the next boundary also detects disconnected viewers.
            err = httpd_resp_send_chunk(req, "\r\n", 2);
            last_write = esp_timer_get_time();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    free(jpeg);
    return err;
}

static void start_servers(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    cfg.lru_purge_enable = true;
    httpd_handle_t control, video;
    ESP_ERROR_CHECK(httpd_start(&control, &cfg));
    const httpd_uri_t home = {.uri = "/", .method = HTTP_GET, .handler = page};
    const httpd_uri_t status = {.uri = "/status", .method = HTTP_GET, .handler = diagnostics};
    const httpd_uri_t log_uri = {.uri = "/usb-log", .method = HTTP_GET, .handler = usb_log_page};
    const httpd_uri_t rtc_uri = {.uri = "/rtc-status", .method = HTTP_GET, .handler = rtc_status};
    const httpd_uri_t rtc_log_uri = {.uri = "/log", .method = HTTP_GET, .handler = rtc_log_page};
    ESP_ERROR_CHECK(httpd_register_uri_handler(control, &home));
    ESP_ERROR_CHECK(httpd_register_uri_handler(control, &status));
    ESP_ERROR_CHECK(httpd_register_uri_handler(control, &log_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(control, &rtc_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(control, &rtc_log_uri));
    // A separate server keeps /status responsive during a long-running stream.
    cfg.server_port = 81;
    cfg.ctrl_port++;
    cfg.send_wait_timeout = 3;
    ESP_ERROR_CHECK(httpd_start(&video, &cfg));
    const httpd_uri_t video_uri = {.uri = "/stream", .method = HTTP_GET, .handler = stream};
    ESP_ERROR_CHECK(httpd_register_uri_handler(video, &video_uri));
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "MJPEG diagnostics: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
    }
}

void app_main(void)
{
    original_log = esp_log_set_vprintf(capture_usb_log);
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) < 4 * 1024 * 1024) {
        ESP_LOGE(TAG, "Octal PSRAM missing"); return;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t wifi = {0};
    size_t ssid_len = strlen(CONFIG_WEBCAM_WIFI_SSID), pass_len = strlen(CONFIG_WEBCAM_WIFI_PASSWORD);
    if (ssid_len > sizeof(wifi.sta.ssid) || pass_len > sizeof(wifi.sta.password)) return;
    memcpy(wifi.sta.ssid, CONFIG_WEBCAM_WIFI_SSID, ssid_len);
    memcpy(wifi.sta.password, CONFIG_WEBCAM_WIFI_PASSWORD, pass_len);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    err = esp_wifi_set_config(WIFI_IF_STA, &wifi);
    if (err != ESP_OK || !wifi.sta.ssid[0]) {
        ESP_LOGE(TAG, "Configure Wi-Fi in menuconfig"); return;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    camera_start();
    start_servers();
    webrtc_start();
}

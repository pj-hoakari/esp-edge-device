#include <string.h>
#include <assert.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "usb/usb_host.h"
#include "usb/uvc_host.h"
#include "camera.h"
#include "jpeg_frame.h"

static SemaphoreHandle_t lock;
static SemaphoreHandle_t unplugged;
static uint8_t *latest;
static size_t latest_size;
static uint32_t latest_sequence;
static int64_t latest_at;
static const char *TAG = "camera";
static portMUX_TYPE status_mux = portMUX_INITIALIZER_UNLOCKED;
static camera_status_t status;

void camera_get_status(camera_status_t *out)
{
    portENTER_CRITICAL(&status_mux);
    *out = status;
    portEXIT_CRITICAL(&status_mux);
}

static void driver_event(const uvc_host_driver_event_data_t *event, void *ctx)
{
    if (event->type == UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        portENTER_CRITICAL(&status_mux);
        status.uvc_detections++;
        portEXIT_CRITICAL(&status_mux);
        ESP_LOGI(TAG, "UVC detected: address %u, stream %u, formats %u",
                 event->device_connected.dev_addr, event->device_connected.uvc_stream_index,
                 (unsigned)event->device_connected.frame_info_num);
    }
}

static bool frame_cb(const uvc_host_frame_t *f, void *ctx)
{
    jpeg_frame_error_t reason = JPEG_FRAME_TRUNCATED;
    size_t jpeg_size = f->data_len <= JPEG_MAX
        ? jpeg_frame_length_ex(f->data, f->data_len,
                               CAMERA_WIDTH,
                               CAMERA_HEIGHT, &reason) : 0;
    bool invalid = f->vs_format.format != UVC_VS_FORMAT_MJPEG ||
        f->vs_format.h_res != CAMERA_WIDTH ||
        f->vs_format.v_res != CAMERA_HEIGHT ||
        !jpeg_size;
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL(&status_mux);
    status.frames_received++;
    status.last_frame_bytes = f->data_len;
    status.width = f->vs_format.h_res;
    status.height = f->vs_format.v_res;
    if (f->data_len > JPEG_MAX) status.oversized_jpegs++;
    if (invalid) {
        status.invalid_jpegs++;
        switch (reason) {
        case JPEG_FRAME_NO_SOI: status.invalid_no_soi++; break;
        case JPEG_FRAME_BAD_MARKER: status.invalid_marker++; break;
        case JPEG_FRAME_TRUNCATED: status.invalid_truncated++; break;
        case JPEG_FRAME_NO_SOF: status.invalid_no_sof++; break;
        case JPEG_FRAME_BAD_DIMENSIONS: status.invalid_dimensions++; break;
        case JPEG_FRAME_NO_SOS: status.invalid_no_sos++; break;
        case JPEG_FRAME_NO_EOI: status.invalid_no_eoi++; break;
        default: break;
        }
    } else {
        status.valid_jpegs++; status.last_valid_us = now;
        if (jpeg_size < f->data_len) {
            status.trimmed_jpegs++; status.trimmed_bytes += f->data_len - jpeg_size;
        }
    }
    if (f->data_len < 2 || f->data[0] != 255 || f->data[1] != 216) status.missing_soi++;
    if (f->data_len < 2 || f->data[f->data_len - 2] != 255 || f->data[f->data_len - 1] != 217) status.missing_eoi++;
    portEXIT_CRITICAL(&status_mux);
    if (invalid) {
        ESP_LOGW(TAG, "Discard invalid/oversize/non-HD JPEG: %u bytes (reason %d)",
                 (unsigned)f->data_len, (int)reason);
        return true;
    }
    if (xSemaphoreTake(lock, 0) == pdTRUE) {
        memcpy(latest, f->data, jpeg_size);
        latest_size = jpeg_size;
        latest_sequence++;
        latest_at = esp_timer_get_time();
        xSemaphoreGive(lock);
    } else {
        portENTER_CRITICAL(&status_mux);
        status.copy_drops++;
        portEXIT_CRITICAL(&status_mux);
    }
    return true; // Never retain a driver-owned frame, including on unplug.
}

size_t camera_take(uint8_t *out)
{
    static uint32_t sequence;
    return camera_read(out, &sequence);
}

size_t camera_read(uint8_t *out, uint32_t *sequence)
{
    if (xSemaphoreTake(lock, 0) != pdTRUE) return 0;
    size_t n = *sequence != latest_sequence && esp_timer_get_time() - latest_at <= 1000000 ? latest_size : 0;
    if (n) memcpy(out, latest, n);
    *sequence = latest_sequence;
    xSemaphoreGive(lock);
    return n;
}

size_t camera_read_frame(uint8_t *out, camera_frame_info_t *info)
{
    if (!info) return 0;
    if (xSemaphoreTake(lock, 0) != pdTRUE) return 0;
    size_t n = 0;
    if (info->sequence != latest_sequence && esp_timer_get_time() - latest_at <= 1000000) {
        n = latest_size;
        memcpy(out, latest, n);
    }
    info->sequence = latest_sequence;
    info->size = latest_size;
    info->captured_us = latest_at;
    xSemaphoreGive(lock);
    return n;
}

static void stream_event(const uvc_host_stream_event_data_t *e, void *ctx)
{
    if (e->type == UVC_HOST_DEVICE_DISCONNECTED) {
        portENTER_CRITICAL(&status_mux);
        status.state = CAMERA_DISCONNECTED;
        portEXIT_CRITICAL(&status_mux);
        xSemaphoreTake(lock, portMAX_DELAY);
        latest_size = 0;
        xSemaphoreGive(lock);
        ESP_LOGW(TAG, "Camera unplugged");
        xSemaphoreGive(unplugged);
    } else {
        portENTER_CRITICAL(&status_mux);
        if (e->type == UVC_HOST_FRAME_BUFFER_OVERFLOW) status.buffer_overflows++;
        if (e->type == UVC_HOST_TRANSFER_ERROR) status.transfer_errors++;
        portEXIT_CRITICAL(&status_mux);
        ESP_LOGW(TAG, "UVC stream event %d", e->type);
    }
}

static void usb_events(void *arg)
{
    for (;;) {
        uint32_t flags;
        ESP_ERROR_CHECK(usb_host_lib_handle_events(portMAX_DELAY, &flags));
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void camera_task(void *arg)
{
    for (;;) {
        uvc_host_stream_hdl_t stream;
        xSemaphoreTake(unplugged, 0);
        portENTER_CRITICAL(&status_mux);
        status.state = CAMERA_OPENING;
        status.open_attempts++;
        portEXIT_CRITICAL(&status_mux);
        const uvc_host_stream_config_t cfg = {
            .event_cb = stream_event, .frame_cb = frame_cb,
            .usb = {.vid = CONFIG_WEBCAM_CAMERA_VID, .pid = CONFIG_WEBCAM_CAMERA_PID},
            .vs_format = {.h_res = CAMERA_WIDTH,
                          .v_res = CAMERA_HEIGHT,
                          .fps = CONFIG_WEBCAM_CAMERA_FPS, .format = UVC_VS_FORMAT_MJPEG},
            .advanced = {.number_of_frame_buffers = 3, .frame_size = JPEG_MAX,
                         .frame_heap_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                         .number_of_urbs = 4, .urb_size = 8192},
        };
        esp_err_t err = uvc_host_stream_open(&cfg, pdMS_TO_TICKS(5000), &stream);
        portENTER_CRITICAL(&status_mux);
        status.last_open_error = err;
        portEXIT_CRITICAL(&status_mux);
        if (err == ESP_OK) {
            uvc_host_desc_print(stream);
            err = uvc_host_stream_start(stream);
            if (err == ESP_OK) {
                portENTER_CRITICAL(&status_mux);
                status.state = CAMERA_STREAMING;
                portEXIT_CRITICAL(&status_mux);
                ESP_LOGI(TAG, "%dx%d MJPEG capture started at %d FPS",
                         CAMERA_WIDTH, CAMERA_HEIGHT,
                         CONFIG_WEBCAM_CAMERA_FPS);
                xSemaphoreTake(unplugged, portMAX_DELAY);
                ESP_ERROR_CHECK(uvc_host_stream_close(stream));
            } else {
                portENTER_CRITICAL(&status_mux);
                status.state = CAMERA_ERROR;
                status.last_open_error = err;
                portEXIT_CRITICAL(&status_mux);
                ESP_LOGE(TAG, "Stream start failed: %s", esp_err_to_name(err));
                uvc_host_stream_close(stream);
            }
        } else {
            portENTER_CRITICAL(&status_mux);
            status.state = status.uvc_detections ? CAMERA_ERROR : CAMERA_WAITING;
            portEXIT_CRITICAL(&status_mux);
            ESP_LOGW(TAG, "Waiting for Full Speed %dx%d MJPEG camera: %s",
                     CAMERA_WIDTH, CAMERA_HEIGHT, esp_err_to_name(err));
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void camera_start(void)
{
    lock = xSemaphoreCreateMutex();
    unplugged = xSemaphoreCreateBinary();
    latest = heap_caps_malloc(JPEG_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    assert(lock && unplugged && latest);
    const usb_host_config_t host = {.intr_flags = ESP_INTR_FLAG_LOWMED};
    ESP_ERROR_CHECK(usb_host_install(&host));
    assert(xTaskCreate(usb_events, "usb_events", 4096, NULL, 15, NULL) == pdPASS);
    const uvc_host_driver_config_t uvc = {
        .driver_task_stack_size = 6144, .driver_task_priority = 16,
        // USB host, Wi-Fi and HTTP stay on CPU0; the WebRTC sender owns CPU1.
        .xCoreID = 0, .create_background_task = true,
        .event_cb = driver_event,
    };
    ESP_ERROR_CHECK(uvc_host_install(&uvc));
    assert(xTaskCreate(camera_task, "camera", 6144, NULL, 8, NULL) == pdPASS);
}

#include "camera_h264.h"
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_io_expander_pi4ioe5v6408.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "linux/videodev2.h"
#include "sdkconfig.h"

#define CAPTURE_BUFFERS 3
#define FRAME_QUEUE_DEPTH 8

static const char *TAG = "tab5cam";

static i2c_master_bus_handle_t i2c_bus;
static int cap_fd = -1, enc_fd = -1;
static uint8_t *cap_buffers[CAPTURE_BUFFERS];
static uint8_t *enc_buffer;
static QueueHandle_t frames;

static esp_err_t expander_output(uint32_t address, uint32_t pin, bool level)
{
    esp_io_expander_handle_t expander = NULL;
    ESP_RETURN_ON_ERROR(esp_io_expander_new_i2c_pi4ioe5v6408(i2c_bus, address, &expander), TAG, "expander");
    esp_err_t err = esp_io_expander_set_dir(expander, pin, IO_EXPANDER_OUTPUT);
    if (err == ESP_OK) err = esp_io_expander_set_level(expander, pin, level);
    if (err == ESP_OK) err = esp_io_expander_set_output_mode(expander, pin, IO_EXPANDER_OUTPUT_MODE_PUSH_PULL);
    esp_io_expander_del(expander);
    return err;
}

esp_err_t tab5_board_init(void)
{
    i2c_master_bus_config_t bus = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = GPIO_NUM_31,
        .scl_io_num = GPIO_NUM_32,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .flags.enable_internal_pullup = true,
    };
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus, &i2c_bus), TAG, "i2c");
    ESP_RETURN_ON_ERROR(expander_output(ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_HIGH, IO_EXPANDER_PIN_NUM_0, true), TAG, "wifi power");
    ESP_RETURN_ON_ERROR(expander_output(ESP_IO_EXPANDER_I2C_PI4IOE5V6408_ADDRESS_LOW, IO_EXPANDER_PIN_NUM_6, true), TAG, "camera power");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static void set_codec_control(uint32_t id, int32_t value)
{
    struct v4l2_ext_control control = {.id = id, .value = value};
    struct v4l2_ext_controls controls = {.ctrl_class = V4L2_CID_CODEC_CLASS, .count = 1, .controls = &control};
    if (ioctl(enc_fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) ESP_LOGW(TAG, "codec control 0x%" PRIx32 " rejected", id);
}

static esp_err_t set_format(int fd, uint32_t type, uint32_t pixelformat)
{
    struct v4l2_format format = {.type = type};
    format.fmt.pix.width = CAMERA_WIDTH;
    format.fmt.pix.height = CAMERA_HEIGHT;
    format.fmt.pix.pixelformat = pixelformat;
    return ioctl(fd, VIDIOC_S_FMT, &format) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t request_buffers(int fd, uint32_t type, uint32_t memory, uint32_t count)
{
    struct v4l2_requestbuffers req = {.count = count, .type = type, .memory = memory};
    return ioctl(fd, VIDIOC_REQBUFS, &req) == 0 ? ESP_OK : ESP_FAIL;
}

static uint8_t *map_buffer(int fd, uint32_t type, uint32_t index)
{
    struct v4l2_buffer buf = {.type = type, .memory = V4L2_MEMORY_MMAP, .index = index};
    if (ioctl(fd, VIDIOC_QUERYBUF, &buf) != 0) return NULL;
    uint8_t *mapped = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
    if (!mapped || ioctl(fd, VIDIOC_QBUF, &buf) != 0) return NULL;
    return mapped;
}

static esp_err_t stream_on(int fd, int type)
{
    return ioctl(fd, VIDIOC_STREAMON, &type) == 0 ? ESP_OK : ESP_FAIL;
}

static bool encode_one(camera_h264_frame_t *out, int64_t *next, int64_t period_us)
{
    struct v4l2_buffer cap = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    if (ioctl(cap_fd, VIDIOC_DQBUF, &cap) != 0) return false;
    int64_t now = esp_timer_get_time();
    if (now < *next) {
        ioctl(cap_fd, VIDIOC_QBUF, &cap);
        out->data = NULL;
        return true;
    }
    *next = now - *next > period_us ? now + period_us : *next + period_us;
    struct v4l2_buffer in = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT, .memory = V4L2_MEMORY_USERPTR, .index = 0,
        .m.userptr = (unsigned long)cap_buffers[cap.index], .length = cap.bytesused,
    };
    struct v4l2_buffer enc = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP};
    bool ok = ioctl(enc_fd, VIDIOC_QBUF, &in) == 0 && ioctl(enc_fd, VIDIOC_DQBUF, &enc) == 0;
    ioctl(cap_fd, VIDIOC_QBUF, &cap);
    if (!ok) return false;
    ioctl(enc_fd, VIDIOC_DQBUF, &in);
    out->size = enc.bytesused;
    out->pts_ms = esp_timer_get_time() / 1000;
    out->data = out->size > 0 ? heap_caps_malloc(out->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (out->data) memcpy(out->data, enc_buffer, out->size);
    struct v4l2_buffer back = {.type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = 0};
    ioctl(enc_fd, VIDIOC_QBUF, &back);
    return true;
}

static void capture_task(void *arg)
{
    const int64_t period_us = 1000000 / CONFIG_TAB5_VIDEO_FPS;
    int64_t next = esp_timer_get_time();
    unsigned dropped = 0;
    for (;;) {
        camera_h264_frame_t frame;
        if (!encode_one(&frame, &next, period_us)) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        if (!frame.data) continue;
        if (xQueueSend(frames, &frame, 0) != pdTRUE) {
            free(frame.data);
            if (++dropped % 30 == 1) ESP_LOGW(TAG, "encoded frame queue full (%u dropped)", dropped);
        }
    }
}

esp_err_t camera_h264_start(void)
{
    const esp_video_init_csi_config_t csi = {
        .sccb_config = {.init_sccb = false, .i2c_handle = i2c_bus, .freq = 400000},
        .reset_pin = -1,
        .pwdn_pin = -1,
    };
    const esp_video_init_config_t video = {.csi = &csi};
    ESP_RETURN_ON_ERROR(esp_video_init(&video), TAG, "esp_video_init");

    cap_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    enc_fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(cap_fd >= 0 && enc_fd >= 0, ESP_FAIL, TAG, "open video devices");

    set_codec_control(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, CONFIG_TAB5_VIDEO_GOP);
    set_codec_control(V4L2_CID_MPEG_VIDEO_BITRATE, CONFIG_TAB5_VIDEO_BITRATE);
    set_codec_control(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, 25);
    set_codec_control(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, 40);

    ESP_RETURN_ON_ERROR(set_format(cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_YUV420), TAG, "camera format");
    ESP_RETURN_ON_ERROR(request_buffers(cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, CAPTURE_BUFFERS), TAG, "camera buffers");
    for (int i = 0; i < CAPTURE_BUFFERS; i++) {
        cap_buffers[i] = map_buffer(cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i);
        ESP_RETURN_ON_FALSE(cap_buffers[i], ESP_FAIL, TAG, "camera buffer %d", i);
    }
    ESP_RETURN_ON_ERROR(set_format(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_PIX_FMT_YUV420), TAG, "encoder input");
    ESP_RETURN_ON_ERROR(request_buffers(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, V4L2_MEMORY_USERPTR, 1), TAG, "encoder input buffer");
    ESP_RETURN_ON_ERROR(set_format(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_PIX_FMT_H264), TAG, "encoder output");
    ESP_RETURN_ON_ERROR(request_buffers(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_MEMORY_MMAP, 1), TAG, "encoder output buffer");
    enc_buffer = map_buffer(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0);
    ESP_RETURN_ON_FALSE(enc_buffer, ESP_FAIL, TAG, "encoder buffer");

    ESP_RETURN_ON_ERROR(stream_on(enc_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE), TAG, "encoder stream");
    ESP_RETURN_ON_ERROR(stream_on(enc_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT), TAG, "encoder stream");
    ESP_RETURN_ON_ERROR(stream_on(cap_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE), TAG, "camera stream");

    frames = xQueueCreate(FRAME_QUEUE_DEPTH, sizeof(camera_h264_frame_t));
    ESP_RETURN_ON_FALSE(frames, ESP_ERR_NO_MEM, TAG, "frame queue");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(capture_task, "h264", 6144, NULL, 5, NULL, 0) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "capture task");
    ESP_LOGI(TAG, "H.264 %dx%d@%d %d bps GOP %d", CAMERA_WIDTH, CAMERA_HEIGHT, CONFIG_TAB5_VIDEO_FPS,
             CONFIG_TAB5_VIDEO_BITRATE, CONFIG_TAB5_VIDEO_GOP);
    return ESP_OK;
}

bool camera_h264_acquire(camera_h264_frame_t *frame)
{
    return frames && xQueueReceive(frames, frame, 0) == pdTRUE;
}

void camera_h264_release(camera_h264_frame_t *frame)
{
    free(frame->data);
    frame->data = NULL;
}

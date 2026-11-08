#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define CAMERA_WIDTH 1280
#define CAMERA_HEIGHT 720

typedef struct {
    uint8_t *data;
    int size;
    uint32_t pts_ms;
} camera_h264_frame_t;

esp_err_t tab5_board_init(void);
esp_err_t camera_h264_start(void);
bool camera_h264_acquire(camera_h264_frame_t *frame);
void camera_h264_release(camera_h264_frame_t *frame);

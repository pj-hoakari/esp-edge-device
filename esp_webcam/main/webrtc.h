#pragma once
#include <stdbool.h>
#include <stddef.h>
void webrtc_start(void);
bool webrtc_video_active(void);
int webrtc_status_json(char *out, size_t size);

#pragma once
#include <stdbool.h>
#include <stddef.h>
void webrtc_start(void);
bool webrtc_video_active(void);
int webrtc_status_json(char *out, size_t size);
/* Changes apply to new handshakes for cipher, immediately for burst settings. */
bool webrtc_configure(int cipher, unsigned chunks, unsigned burst_us, bool fast_rx);
bool webrtc_fast_rx(void);
bool webrtc_save_tuning(void);

#pragma once
#include <stdint.h>
#include "esp_err.h"

esp_err_t status_display_start(const char *ssid);
void status_display_wifi_connected(uint32_t ipv4);
void status_display_wifi_disconnected(int reason);

#pragma once

typedef enum {
    ESP_SNTP_OPMODE_POLL,
} esp_sntp_operatingmode_t;

void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t mode);
void esp_sntp_setservername(unsigned index, const char *server);
void esp_sntp_init(void);

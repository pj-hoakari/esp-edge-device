#include <stdio.h>
#include <stdarg.h>
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "eh_host_core.h"
#include "camera_h264.h"
#include "cloud_settings.h"
#include "device_auth.h"
#include "network_settings.h"
#include "nvs_boot.h"
#include "webrtc.h"
#include "wifi_auth.h"

static const char *TAG = "tab5";
static vprintf_like_t original_log;

static int gated_log(const char *format, va_list args)
{
    if (!network_settings_uart_logs_enabled()) return 0;
    return original_log ? original_log(format, args) : vprintf(format, args);
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Wi-Fi connected: " IPSTR, IP2STR(&event->ip_info.ip));
    }
}

void app_main(void)
{
    original_log = esp_log_set_vprintf(gated_log);
    ESP_ERROR_CHECK(nvs_boot_init());
    network_settings_load();
    cloud_settings_load();
    device_auth_initialize();
    network_settings_console_start();
    ESP_ERROR_CHECK(tab5_board_init());
    if (eh_host_init(NULL) != 0 || eh_host_connect_to_slave() != 0) {
        printf("ESP32-C6 Wi-Fi co-processor did not respond.\n");
        return;
    }
    ESP_ERROR_CHECK(camera_h264_start());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    esp_err_t err = wifi_auth_configure(network_settings_active());
    if (err != ESP_OK) {
        printf("Wi-Fi setup failed: %s. Use wifi/eap/credentials, then reboot.\n", esp_err_to_name(err));
        return;
    }
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    webrtc_start();
}

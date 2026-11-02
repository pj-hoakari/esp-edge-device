#include "network_diagnostics.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"

static bool current_ipv4(esp_netif_ip_info_t *info)
{
    esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    wifi_ap_record_t ap;
    /* get_ip_info can return cached addresses while down. Check the current
     * association/interface too, so show/ping cannot use a previous lease. */
    return sta && esp_netif_is_netif_up(sta) && esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
        esp_netif_get_ip_info(sta, info) == ESP_OK && info->ip.addr != 0;
}

bool network_diagnostics_has_ipv4(void)
{
    esp_netif_ip_info_t info = {0};
    return current_ipv4(&info);
}

void network_diagnostics_show(void)
{
    esp_netif_ip_info_t info = {0};
    puts("Network (current):");
    if (!current_ipv4(&info)) {
        puts("  IPv4: (not connected / waiting for DHCP)");
        return;
    }
    printf("  IPv4: " IPSTR "\n  Netmask: " IPSTR "\n  Gateway: " IPSTR "\n",
        IP2STR(&info.ip), IP2STR(&info.netmask), IP2STR(&info.gw));
}

static void ping_success(esp_ping_handle_t handle, void *arg)
{
    uint16_t seq = 0;
    uint32_t elapsed = 0;
    ip_addr_t address = {0};
    char text[16];
    esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    esp_ping_get_profile(handle, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(handle, ESP_PING_PROF_IPADDR, &address, sizeof(address));
    ipaddr_ntoa_r(&address, text, sizeof(text));
    printf("Reply from %s: seq=%u time=%" PRIu32 " ms\n", text, seq, elapsed);
}

static void ping_timeout(esp_ping_handle_t handle, void *arg)
{
    uint16_t seq = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    printf("Request timeout: seq=%u\n", seq);
}

static void ping_end(esp_ping_handle_t handle, void *arg)
{
    uint32_t sent = 0, received = 0;
    esp_ping_get_profile(handle, ESP_PING_PROF_REQUEST, &sent, sizeof(sent));
    esp_ping_get_profile(handle, ESP_PING_PROF_REPLY, &received, sizeof(received));
    printf("Ping: %" PRIu32 " sent, %" PRIu32 " received\n", sent, received);
    fflush(stdout);
    /* SDK supports deletion in on_ping_end and releases its task/socket after
     * this callback returns. The console task handle stays valid throughout. */
    esp_ping_delete_session(handle);
    xTaskNotifyGive((TaskHandle_t)arg);
}

void network_diagnostics_ping(const char *target)
{
    esp_netif_ip_info_t info = {0};
    if (!current_ipv4(&info)) {
        puts("Ping unavailable: Wi-Fi is not connected or has no IPv4 address.");
        return;
    }
    esp_ping_config_t config = ESP_PING_DEFAULT_CONFIG();
    if (!target || !target[0]) {
        if (!info.gw.addr) { puts("Ping unavailable: no gateway. Use ping <IPv4>."); return; }
        ip_addr_set_ip4_u32(&config.target_addr, info.gw.addr);
    } else if (!ipaddr_aton(target, &config.target_addr) || !IP_IS_V4(&config.target_addr) ||
               !ip4_addr_get_u32(ip_2_ip4(&config.target_addr))) {
        puts("Usage: ping [IPv4] (3 requests; default target is the gateway)");
        return;
    }
    config.count = 3;
    config.timeout_ms = 1000;
    config.interval_ms = 1000;
    config.data_size = 32;
    esp_ping_callbacks_t callbacks = {
        .cb_args = (void *)xTaskGetCurrentTaskHandle(),
        .on_ping_success = ping_success,
        .on_ping_timeout = ping_timeout,
        .on_ping_end = ping_end,
    };
    esp_ping_handle_t handle = NULL;
    esp_err_t err = esp_ping_new_session(&config, &callbacks, &handle);
    if (err != ESP_OK) { printf("Ping could not start: %s\n", esp_err_to_name(err)); return; }
    char text[16];
    ipaddr_ntoa_r(&config.target_addr, text, sizeof(text));
    printf("PING %s: 3 requests, timeout 1000 ms\n", text);
    fflush(stdout);
    ulTaskNotifyTake(pdTRUE, 0);
    err = esp_ping_start(handle);
    if (err != ESP_OK) {
        esp_ping_delete_session(handle);
        printf("Ping could not start: %s\n", esp_err_to_name(err));
        return;
    }
    /* Keep the prompt below the last reply/summary. This task is not involved
     * in video transport; camera and WebRTC keep running independently. */
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

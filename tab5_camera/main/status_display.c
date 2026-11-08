#include "status_display.h"
#include <stdatomic.h>
#include <stdio.h>
#include "bsp/m5stack_tab5.h"
#include "esp_check.h"
#include "esp_netif_ip_addr.h"
#include "lvgl.h"
#include "webrtc.h"

typedef enum { WIFI_CONNECTING, WIFI_CONNECTED, WIFI_DISCONNECTED, WIFI_NOT_CONFIGURED } wifi_state_t;

static const char *TAG = "status";
static char ssid_text[40];
static atomic_int wifi_state = WIFI_CONNECTING;
static atomic_uint wifi_ipv4;
static atomic_int wifi_reason;
static lv_obj_t *wifi_value, *ssid_value, *ip_value, *observe_value;

static lv_obj_t *add_row(lv_obj_t *parent, const char *caption, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, caption);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0x9aa0a6), 0);
    lv_obj_t *value = lv_label_create(parent);
    lv_label_set_text(value, "-");
    lv_obj_set_style_text_font(value, font, 0);
    lv_obj_set_style_text_color(value, lv_color_white(), 0);
    lv_obj_set_width(value, lv_pct(100));
    lv_label_set_long_mode(value, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_bottom(value, 32, 0);
    return value;
}

static void refresh(lv_timer_t *timer)
{
    char text[64];
    switch (atomic_load(&wifi_state)) {
    case WIFI_CONNECTED:
        lv_label_set_text(wifi_value, "Connected");
        lv_obj_set_style_text_color(wifi_value, lv_color_hex(0x34c759), 0);
        esp_ip4_addr_t ip = {.addr = atomic_load(&wifi_ipv4)};
        snprintf(text, sizeof(text), IPSTR, IP2STR(&ip));
        lv_label_set_text(ip_value, text);
        break;
    case WIFI_DISCONNECTED:
        snprintf(text, sizeof(text), "Disconnected (reason %d)", atomic_load(&wifi_reason));
        lv_label_set_text(wifi_value, text);
        lv_obj_set_style_text_color(wifi_value, lv_color_hex(0xff453a), 0);
        lv_label_set_text(ip_value, "-");
        break;
    case WIFI_NOT_CONFIGURED:
        lv_label_set_text(wifi_value, "Not configured");
        lv_obj_set_style_text_color(wifi_value, lv_color_hex(0xff453a), 0);
        lv_label_set_text(ip_value, "-");
        break;
    default:
        lv_label_set_text(wifi_value, "Connecting...");
        lv_obj_set_style_text_color(wifi_value, lv_color_hex(0xffd60a), 0);
        lv_label_set_text(ip_value, "-");
        break;
    }
    bool observing = webrtc_video_active();
    lv_label_set_text(observe_value, observing ? "Observing" : "Not observing");
    lv_obj_set_style_text_color(observe_value, observing ? lv_color_hex(0x34c759) : lv_color_hex(0x9aa0a6), 0);
}

esp_err_t status_display_start(const char *ssid)
{
    snprintf(ssid_text, sizeof(ssid_text), "%s", ssid && ssid[0] ? ssid : "-");
    if (!ssid || !ssid[0]) atomic_store(&wifi_state, WIFI_NOT_CONFIGURED);
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * 64,
        .flags = {.buff_spiram = true, .sw_rotate = true},
    };
    ESP_RETURN_ON_FALSE(bsp_display_start_with_config(&cfg), ESP_FAIL, TAG, "display");
    bsp_display_brightness_set(60);
    bsp_display_lock(0);
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x111214), 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(screen, 48, 0);
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "TOLO Camera");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_set_style_pad_bottom(title, 64, 0);
    wifi_value = add_row(screen, "Wi-Fi", &lv_font_montserrat_48);
    ssid_value = add_row(screen, "SSID", &lv_font_montserrat_40);
    lv_label_set_text(ssid_value, ssid_text);
    ip_value = add_row(screen, "IP address", &lv_font_montserrat_40);
    observe_value = add_row(screen, "Observation", &lv_font_montserrat_48);
    refresh(NULL);
    lv_timer_create(refresh, 500, NULL);
    bsp_display_unlock();
    return ESP_OK;
}

void status_display_wifi_connected(uint32_t ipv4)
{
    atomic_store(&wifi_ipv4, ipv4);
    atomic_store(&wifi_state, WIFI_CONNECTED);
}

void status_display_wifi_disconnected(int reason)
{
    atomic_store(&wifi_reason, reason);
    atomic_store(&wifi_state, WIFI_DISCONNECTED);
}

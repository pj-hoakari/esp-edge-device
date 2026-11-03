#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_SIGNALING_SIZE 224
#define NETWORK_SETTINGS_VERSION 2

typedef enum {
    NETWORK_EAP_NONE, NETWORK_EAP_TLS, NETWORK_EAP_PEAP,
    NETWORK_EAP_TTLS_EAP, NETWORK_EAP_TTLS_MSCHAPV2, NETWORK_EAP_TTLS_MSCHAP,
    NETWORK_EAP_TTLS_PAP, NETWORK_EAP_TTLS_CHAP, NETWORK_EAP_FAST, NETWORK_EAP_COUNT
} network_eap_t;
typedef enum { NETWORK_LINK_WPA, NETWORK_LINK_WPA2, NETWORK_LINK_WPA3, NETWORK_LINK_SUITEB } network_link_t;
typedef enum { NETWORK_TRUST_CA, NETWORK_TRUST_BUNDLE, NETWORK_TRUST_NONE } network_trust_t;

typedef struct {
    uint32_t version;
    char ssid[33];
    char password[65];
    char signaling_url[NETWORK_SIGNALING_SIZE];
    uint32_t eap_method, enterprise_link, server_trust, check_cert_time;
    char identity[128];
    char username[128];
    char eap_password[129];
    char key_password[129];
    char server_domain[256];
} network_settings_t;

bool network_wifi_valid(const char *ssid, const char *password);
bool network_eap_valid(const network_settings_t *s);
const char *network_eap_name(unsigned method);
/* IPv4/hostname[:port] defaults to http and port 8080. "https://host" uses
 * port 443. "off" disables RTC. Explicit URLs must be HTTP(S) origins with no
 * credentials, query or path. */
bool network_signaling_normalize(const char *input, char *out, size_t size);
void network_settings_load(void);
const network_settings_t *network_settings_active(void);
/* Saves the profile for the next boot without printing human console text. */
bool network_settings_save(const network_settings_t *settings);
void network_settings_console_start(void);
/* UART output only; HTTP diagnostic log capture remains active. */
bool network_settings_uart_logs_enabled(void);

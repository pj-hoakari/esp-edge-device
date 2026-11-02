#pragma once
#include "esp_err.h"
#include "network_settings.h"

#define WIFI_CREDENTIAL_MAX 4096
typedef enum { WIFI_CRED_CA, WIFI_CRED_CLIENT, WIFI_CRED_KEY, WIFI_CRED_PAC, WIFI_CRED_COUNT } wifi_credential_t;
bool wifi_auth_method_available(unsigned method);
bool wifi_auth_link_available(unsigned link);
const char *wifi_credential_name(unsigned type);
esp_err_t wifi_credential_size(unsigned type, size_t *size);
esp_err_t wifi_credential_save(unsigned type, const unsigned char *data, size_t size, const char *key_password);
esp_err_t wifi_credential_clear(unsigned type);
/* Called once after esp_wifi_init/set_mode, before esp_wifi_start. */
esp_err_t wifi_auth_configure(const network_settings_t *settings);
esp_err_t wifi_auth_set_time(int64_t unix_time);

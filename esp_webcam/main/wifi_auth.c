#include "wifi_auth.h"
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include <sys/time.h>
#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"
#include "nvs.h"

static const char *const credential_names[] = {"ca", "client", "key", "pac"};
/* Retain immutable boot credentials for SDK APIs that keep pointers. Uploads
 * update NVS only and cannot replace data used by an active authentication. */
static unsigned char *boot_credentials[WIFI_CRED_COUNT];
static size_t boot_sizes[WIFI_CRED_COUNT];

const char *wifi_credential_name(unsigned type)
{
    return type < WIFI_CRED_COUNT ? credential_names[type] : "unknown";
}

bool wifi_auth_method_available(unsigned method)
{
    if (method >= NETWORK_EAP_COUNT) return false;
    if (method == NETWORK_EAP_NONE) return true;
#ifndef CONFIG_ESP_WIFI_ENTERPRISE_SUPPORT
    return false;
#else
#ifdef CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT
    return method != NETWORK_EAP_FAST;
#else
    return true;
#endif
#endif
}

bool wifi_auth_link_available(unsigned link)
{
    if (link > NETWORK_LINK_SUITEB) return false;
#ifndef CONFIG_ESP_WIFI_SUITE_B_192
    if (link == NETWORK_LINK_SUITEB) return false;
#endif
#ifndef CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT
    /* Internal TLS lacks server-name validation required by these profiles. */
    if (link >= NETWORK_LINK_WPA3) return false;
#endif
    return true;
}

esp_err_t wifi_credential_size(unsigned type, size_t *size)
{
    if (type >= WIFI_CRED_COUNT || !size) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_certs", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_blob(handle, credential_names[type], NULL, size);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_credential_save(unsigned type, const unsigned char *data, size_t size, const char *password)
{
    if (type >= WIFI_CRED_COUNT || !data || !size || size > WIFI_CREDENTIAL_MAX) return ESP_ERR_INVALID_ARG;
    /* Import PEM with its terminating NUL. PAC is opaque binary. */
    if (type != WIFI_CRED_PAC) {
        if (data[size - 1] != 0 || memchr(data, 0, size - 1)) return ESP_ERR_INVALID_ARG;
        /* Imports must work even before Wi-Fi/WebRTC start (e.g. the first
         * boot has no SSID/CA). MbedTLS 4 key parsing uses PSA internally. */
        if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
        int ret;
        if (type == WIFI_CRED_KEY) {
            mbedtls_pk_context key;
            mbedtls_pk_init(&key);
            ret = mbedtls_pk_parse_key(&key, data, size, (const unsigned char *)password,
                password ? strlen(password) : 0);
            mbedtls_pk_free(&key);
        } else {
            mbedtls_x509_crt cert;
            mbedtls_x509_crt_init(&cert);
            ret = mbedtls_x509_crt_parse(&cert, data, size);
            mbedtls_x509_crt_free(&cert);
        }
        if (ret != 0) return ESP_ERR_INVALID_ARG;
    } else if (size < 512) {
        /* The IDF PAC API silently replaces smaller files with an empty PAC. */
        return ESP_ERR_INVALID_SIZE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_certs", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_blob(handle, credential_names[type], data, size);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t wifi_auth_set_time(int64_t unix_time)
{
    if (unix_time < 1577836800LL || unix_time > 4102444800LL) return ESP_ERR_INVALID_ARG;
    struct timeval now = {.tv_sec = unix_time};
    return settimeofday(&now, NULL) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t wifi_credential_clear(unsigned type)
{
    if (type >= WIFI_CRED_COUNT) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_certs", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_key(handle, credential_names[type]);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t load_credential(unsigned type)
{
    if (boot_credentials[type]) return ESP_OK;
    size_t size = 0;
    esp_err_t err = wifi_credential_size(type, &size);
    if (err != ESP_OK) return err;
    if (!size || size > WIFI_CREDENTIAL_MAX) return ESP_ERR_INVALID_SIZE;
    unsigned char *data = calloc(1, size + 1);
    if (!data) return ESP_ERR_NO_MEM;
    nvs_handle_t handle;
    err = nvs_open("wifi_certs", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_blob(handle, credential_names[type], data, &size);
        nvs_close(handle);
    }
    if (err != ESP_OK) { free(data); return err; }
    boot_credentials[type] = data;
    boot_sizes[type] = size;
    return ESP_OK;
}

#define CHECK(call) do { esp_err_t result = (call); if (result != ESP_OK) return result; } while (0)

esp_err_t wifi_auth_configure(const network_settings_t *s)
{
    if (!network_eap_valid(s) || !s->ssid[0]) return ESP_ERR_INVALID_ARG;
    if (!wifi_auth_method_available(s->eap_method) || !wifi_auth_link_available(s->enterprise_link))
        return ESP_ERR_NOT_SUPPORTED;
    wifi_config_t wifi = {0};
    memcpy(wifi.sta.ssid, s->ssid, strlen(s->ssid));
    if (s->eap_method == NETWORK_EAP_NONE) {
        memcpy(wifi.sta.password, s->password, strlen(s->password));
        wifi.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        return esp_wifi_set_config(WIFI_IF_STA, &wifi);
    }
    static const wifi_auth_mode_t modes[] = {WIFI_AUTH_WPA_ENTERPRISE, WIFI_AUTH_WPA2_ENTERPRISE,
        WIFI_AUTH_WPA3_ENTERPRISE, WIFI_AUTH_WPA3_ENT_192};
    wifi.sta.threshold.authmode = modes[s->enterprise_link];
    wifi.sta.pmf_cfg.required = s->enterprise_link >= NETWORK_LINK_WPA3;
    CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi));
    CHECK(esp_eap_client_set_identity((const unsigned char *)s->identity, strlen(s->identity)));
    CHECK(esp_eap_client_set_disable_time_check(!s->check_cert_time));
    if (s->eap_method != NETWORK_EAP_FAST) {
        if (s->server_trust == NETWORK_TRUST_CA) {
            CHECK(load_credential(WIFI_CRED_CA));
            CHECK(esp_eap_client_set_ca_cert(boot_credentials[WIFI_CRED_CA], boot_sizes[WIFI_CRED_CA]));
        } else if (s->server_trust == NETWORK_TRUST_BUNDLE) {
#ifdef CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT
            CHECK(esp_eap_client_use_default_cert_bundle(true));
#else
            return ESP_ERR_NOT_SUPPORTED;
#endif
        }
        if (s->server_domain[0]) CHECK(esp_eap_client_set_domain_name(s->server_domain));
    }
    esp_eap_method_t method = ESP_EAP_TYPE_PEAP;
    if (s->eap_method == NETWORK_EAP_TLS) {
        CHECK(load_credential(WIFI_CRED_CLIENT));
        CHECK(load_credential(WIFI_CRED_KEY));
        CHECK(esp_eap_client_set_certificate_and_key(boot_credentials[WIFI_CRED_CLIENT], boot_sizes[WIFI_CRED_CLIENT],
            boot_credentials[WIFI_CRED_KEY], boot_sizes[WIFI_CRED_KEY],
            s->key_password[0] ? (const unsigned char *)s->key_password : NULL, strlen(s->key_password)));
        method = ESP_EAP_TYPE_TLS;
    } else {
        CHECK(esp_eap_client_set_username((const unsigned char *)s->username, strlen(s->username)));
        CHECK(esp_eap_client_set_password((const unsigned char *)s->eap_password, strlen(s->eap_password)));
        if (s->eap_method >= NETWORK_EAP_TTLS_EAP && s->eap_method <= NETWORK_EAP_TTLS_CHAP) {
            static const esp_eap_ttls_phase2_types phases[] = {ESP_EAP_TTLS_PHASE2_EAP, ESP_EAP_TTLS_PHASE2_MSCHAPV2,
                ESP_EAP_TTLS_PHASE2_MSCHAP, ESP_EAP_TTLS_PHASE2_PAP, ESP_EAP_TTLS_PHASE2_CHAP};
            CHECK(esp_eap_client_set_ttls_phase2_method(phases[s->eap_method - NETWORK_EAP_TTLS_EAP]));
            method = ESP_EAP_TYPE_TTLS;
        } else if (s->eap_method == NETWORK_EAP_FAST) {
            CHECK(load_credential(WIFI_CRED_PAC));
            esp_eap_fast_config fast = {.fast_provisioning = 0, .fast_max_pac_list_len = 1, .fast_pac_format_binary = true};
            CHECK(esp_eap_client_set_pac_file(boot_credentials[WIFI_CRED_PAC], boot_sizes[WIFI_CRED_PAC]));
            CHECK(esp_eap_client_set_fast_params(fast));
            method = ESP_EAP_TYPE_FAST;
        }
    }
    if (s->enterprise_link == NETWORK_LINK_SUITEB) CHECK(esp_eap_client_set_suiteb_192bit_certification(true));
    CHECK(esp_eap_client_set_eap_methods(method));
    return esp_wifi_sta_enterprise_enable();
}

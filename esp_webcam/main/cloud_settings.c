#include "cloud_settings.h"
#include "network_settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "sdkconfig.h"

static const char *TAG = "cloud_config";
static cloud_settings_t active, saved;

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool terminated(const char *text, size_t size)
{
    return memchr(text, 0, size) != NULL;
}

static bool printable(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool hex16(const char *text)
{
    if (strlen(text) != 16) return false;
    for (const char *p = text; *p; p++)
        if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}

/* emulator host: either empty (production) or host[:port] with no scheme/path. */
static bool emulator_host(const char *text)
{
    if (!text[0]) return true;
    if (!printable(text) || strlen(text) >= CLOUD_HOST_SIZE) return false;
    if (strchr(text, '/') || strchr(text, ' ')) return false;
    const char *colon = strchr(text, ':');
    if (!colon) return true;
    if (strchr(colon + 1, ':')) return false;
    unsigned port = 0;
    for (const char *p = colon + 1; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        port = port * 10 + (unsigned)(*p - '0');
        if (port > 65535) return false;
    }
    return port > 0;
}

bool cloud_settings_valid(const cloud_settings_t *s)
{
    return s->version == 1 &&
        terminated(s->edge_device_id, sizeof(s->edge_device_id)) &&
        terminated(s->firebase_project_id, sizeof(s->firebase_project_id)) &&
        terminated(s->firebase_api_key, sizeof(s->firebase_api_key)) &&
        terminated(s->firestore_emulator_host, sizeof(s->firestore_emulator_host)) &&
        terminated(s->auth_emulator_host, sizeof(s->auth_emulator_host)) &&
        terminated(s->event_access, sizeof(s->event_access)) &&
        terminated(s->tenant_id, sizeof(s->tenant_id)) &&
        terminated(s->event_id, sizeof(s->event_id)) &&
        hex16(s->edge_device_id) &&
        s->firebase_project_id[0] && printable(s->firebase_project_id) &&
        s->firebase_api_key[0] && printable(s->firebase_api_key) &&
        emulator_host(s->firestore_emulator_host) &&
        emulator_host(s->auth_emulator_host);
}

static bool base64url_decode(const char *input, size_t length,
                             unsigned char *out, size_t out_size, size_t *out_length)
{
    if (length == 0 || length > CLOUD_TOKEN_SIZE || length % 4 == 1) return false;
    char *padded = calloc(1, length + 5);
    if (!padded) return false;
    for (size_t i = 0; i < length; i++) {
        char c = input[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        padded[i] = c;
    }
    while (length % 4) {
        padded[length++] = '=';
    }
    int ret = mbedtls_base64_decode(out, out_size, out_length,
                                    (const unsigned char *)padded, length);
    wipe(padded, length + 5);
    free(padded);
    return ret == 0 && *out_length > 0;
}

static bool json_string(const cJSON *root, const char *name, char *out, size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] ||
        strlen(item->valuestring) >= size) return false;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

bool cloud_settings_apply_token(cloud_settings_t *settings, const char *token)
{
    if (!token || strlen(token) >= CLOUD_TOKEN_SIZE) return false;
    const char *first = strchr(token, '.');
    const char *second = first ? strchr(first + 1, '.') : NULL;
    if (!first || !second || second == first + 1) return false;

    unsigned char *payload = calloc(1, CLOUD_TOKEN_SIZE);
    if (!payload) return false;
    size_t decoded = 0;
    bool ok = base64url_decode(first + 1, (size_t)(second - first - 1),
                               payload, CLOUD_TOKEN_SIZE - 1, &decoded);
    if (!ok) {
        ESP_LOGW(TAG, "event_access payload is not base64url");
        wipe(payload, CLOUD_TOKEN_SIZE);
        free(payload);
        return false;
    }
    payload[decoded] = 0;

    cJSON *root = cJSON_Parse((const char *)payload);
    if (!root) {
        ESP_LOGW(TAG, "event_access payload is not JSON");
        wipe(payload, CLOUD_TOKEN_SIZE);
        free(payload);
        return false;
    }

    char tenant[CLOUD_TENANT_ID_SIZE] = {0};
    char event[CLOUD_EVENT_ID_SIZE] = {0};
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "exp");
    const cJSON *use = cJSON_GetObjectItemCaseSensitive(root, "token_use");
    bool valid = json_string(root, "tenant_id", tenant, sizeof(tenant)) &&
        json_string(root, "event_id", event, sizeof(event)) &&
        cJSON_IsNumber(exp) && exp->valuedouble > 0 &&
        (!cJSON_IsString(use) || !strcmp(use->valuestring, "event_access"));

    if (valid) {
        memcpy(settings->event_access, token, strlen(token) + 1);
        memcpy(settings->tenant_id, tenant, sizeof(tenant));
        memcpy(settings->event_id, event, sizeof(event));
        settings->event_access_expires_at = (int64_t)exp->valuedouble;
        ESP_LOGI(TAG, "event_access loaded: tenant=%s event=%s exp=%lld",
                 settings->tenant_id, settings->event_id,
                 (long long)settings->event_access_expires_at);
    } else {
        ESP_LOGW(TAG, "event_access is missing tenant_id/event_id/exp or has token_use != event_access");
    }

    cJSON_Delete(root);
    wipe(payload, CLOUD_TOKEN_SIZE);
    free(payload);
    wipe(tenant, sizeof(tenant));
    wipe(event, sizeof(event));
    return valid;
}

void cloud_settings_load(void)
{
    active = (cloud_settings_t){.version = 1};
    snprintf(active.edge_device_id, sizeof(active.edge_device_id), "%s", CONFIG_WEBCAM_EDGE_DEVICE_ID);
    snprintf(active.firebase_project_id, sizeof(active.firebase_project_id), "%s", CONFIG_WEBCAM_FIREBASE_PROJECT_ID);
    snprintf(active.firebase_api_key, sizeof(active.firebase_api_key), "%s", CONFIG_WEBCAM_FIREBASE_API_KEY);
    snprintf(active.firestore_emulator_host, sizeof(active.firestore_emulator_host), "%s", CONFIG_WEBCAM_FIRESTORE_EMULATOR_HOST);
    snprintf(active.auth_emulator_host, sizeof(active.auth_emulator_host), "%s", CONFIG_WEBCAM_AUTH_EMULATOR_HOST);

    nvs_handle_t handle;
    esp_err_t err = nvs_open("cloud", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = 0;
        err = nvs_get_blob(handle, "settings", NULL, &size);
        cloud_settings_t *stored = calloc(1, sizeof(*stored));
        if (stored && err == ESP_OK && size == sizeof(*stored)) {
            err = nvs_get_blob(handle, "settings", stored, &size);
            if (err == ESP_OK && cloud_settings_valid(stored)) {
                active = *stored;
                ESP_LOGI(TAG, "saved cloud profile loaded");
            } else {
                ESP_LOGW(TAG, "saved cloud profile invalid; using build defaults");
            }
        } else if (err != ESP_ERR_NVS_NOT_FOUND) {
            ESP_LOGW(TAG, "cloud profile read failed: %s", esp_err_to_name(err));
        }
        if (stored) { wipe(stored, sizeof(*stored)); free(stored); }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "cloud namespace read failed: %s", esp_err_to_name(err));
    }
    saved = active;
}

const cloud_settings_t *cloud_settings_active(void) { return &active; }
const cloud_settings_t *cloud_settings_saved(void) { return &saved; }

bool cloud_settings_save(const cloud_settings_t *settings)
{
    if (!cloud_settings_valid(settings)) return false;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("cloud", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "settings", settings, sizeof(*settings));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        saved = *settings;
        return true;
    }
    ESP_LOGW(TAG, "cloud profile save failed: %s", esp_err_to_name(err));
    return false;
}

bool cloud_settings_ready(void)
{
    const cloud_settings_t *s = &active;
    return cloud_settings_valid(s) &&
        s->event_access[0] && s->tenant_id[0] && s->event_id[0] &&
        s->event_access_expires_at > 0 &&
        network_settings_active()->signaling_url[0];
}

void cloud_settings_show(const char *label, const cloud_settings_t *s)
{
    printf("%s\n  Edge device ID: %s\n  Firebase project: %s\n  Firebase API key: %s\n"
           "  Firestore: %s\n  Auth: %s\n  Edge Bridge: %s\n"
           "  event_access: %s\n  Tenant / Event: %s / %s\n  event_access expires at: %lld\n",
        label,
        s->edge_device_id[0] ? s->edge_device_id : "(not configured)",
        s->firebase_project_id[0] ? s->firebase_project_id : "(not configured)",
        s->firebase_api_key[0] ? "(set, hidden)" : "(not configured)",
        s->firestore_emulator_host[0] ? s->firestore_emulator_host : "(production)",
        s->auth_emulator_host[0] ? s->auth_emulator_host : "(production)",
        network_settings_active()->signaling_url[0] ? network_settings_active()->signaling_url : "(disabled)",
        s->event_access[0] ? "(set, hidden)" : "(not configured)",
        s->tenant_id[0] ? s->tenant_id : "(not parsed)",
        s->event_id[0] ? s->event_id : "(not parsed)",
        (long long)s->event_access_expires_at);
}

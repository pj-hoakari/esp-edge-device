#include "cloud_settings.h"
#include "cloud_settings_values.h"
#include "network_settings.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_log.h"
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

static const char *without_scheme(const char *text)
{
    if (!strncmp(text, "https://", 8)) return text + 8;
    if (!strncmp(text, "http://", 7)) return text + 7;
    return text;
}

/* Emulator endpoint: empty (production), host[:port], http://host[:port], or
 * https://host[:port]. A path is not allowed; the REST paths are added later. */
static bool emulator_host(const char *text)
{
    if (!text[0]) return true;
    if (!printable(text) || strlen(text) >= CLOUD_HOST_SIZE) return false;
    const char *host = without_scheme(text);
    if (!host[0] || strchr(host, '/') || strchr(host, ' ') || strchr(host, '?')) return false;
    const char *colon = strchr(host, ':');
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
    return s->version == CLOUD_SETTINGS_VERSION &&
        terminated(s->edge_device_id, sizeof(s->edge_device_id)) &&
        terminated(s->firebase_project_id, sizeof(s->firebase_project_id)) &&
        terminated(s->firebase_api_key, sizeof(s->firebase_api_key)) &&
        terminated(s->firestore_emulator_host, sizeof(s->firestore_emulator_host)) &&
        terminated(s->auth_emulator_host, sizeof(s->auth_emulator_host)) &&
        terminated(s->idp_url, sizeof(s->idp_url)) &&
        terminated(s->event_access, sizeof(s->event_access)) &&
        terminated(s->refresh_token, sizeof(s->refresh_token)) &&
        terminated(s->token_type, sizeof(s->token_type)) &&
        terminated(s->tenant_id, sizeof(s->tenant_id)) &&
        terminated(s->event_id, sizeof(s->event_id)) &&
        terminated(s->device_code, sizeof(s->device_code)) &&
        terminated(s->user_code, sizeof(s->user_code)) &&
        terminated(s->verification_uri, sizeof(s->verification_uri)) &&
        hex16(s->edge_device_id) &&
        (!s->idp_url[0] || cloud_idp_url_valid(s->idp_url)) &&
        s->firebase_project_id[0] && printable(s->firebase_project_id) &&
        s->firebase_api_key[0] && printable(s->firebase_api_key) &&
        emulator_host(s->firestore_emulator_host) &&
        emulator_host(s->auth_emulator_host) &&
        s->authorization_state <= CLOUD_AUTH_REAPPROVAL_REQUIRED &&
        s->token_state <= CLOUD_TOKEN_STATE_REJECTED &&
        s->refresh_in_progress <= 1 &&
        s->poll_interval_s <= 3600;
}

bool cloud_settings_apply_token(cloud_settings_t *settings, const char *token)
{
    cloud_token_claims_t claims = {0};
    if (!settings || !token) return false;
    if (!cloud_token_parse(token, settings->edge_device_id, (int64_t)time(NULL), &claims)) {
        ESP_LOGW(TAG, "event_access claims are invalid");
        wipe(&claims, sizeof(claims));
        return false;
    }
    memcpy(settings->event_access, token, strlen(token) + 1);
    memcpy(settings->tenant_id, claims.tenant_id, sizeof(settings->tenant_id));
    memcpy(settings->event_id, claims.event_id, sizeof(settings->event_id));
    settings->event_access_expires_at = claims.expires_at;
    settings->token_state = CLOUD_TOKEN_STATE_VALID;
    settings->authorization_state = CLOUD_AUTH_APPROVED;
    snprintf(settings->token_type, sizeof(settings->token_type), "Bearer");
    ESP_LOGI(TAG, "event_access loaded: tenant=%s event=%s exp=%lld",
             settings->tenant_id, settings->event_id,
             (long long)settings->event_access_expires_at);
    wipe(&claims, sizeof(claims));
    return true;
}

static void load_defaults(void)
{
    active = (cloud_settings_t){.version = CLOUD_SETTINGS_VERSION};
    snprintf(active.edge_device_id, sizeof(active.edge_device_id), "%s", CONFIG_WEBCAM_EDGE_DEVICE_ID);
    snprintf(active.firebase_project_id, sizeof(active.firebase_project_id), "%s", CONFIG_WEBCAM_FIREBASE_PROJECT_ID);
    snprintf(active.firebase_api_key, sizeof(active.firebase_api_key), "%s", CONFIG_WEBCAM_FIREBASE_API_KEY);
    snprintf(active.firestore_emulator_host, sizeof(active.firestore_emulator_host), "%s", CONFIG_WEBCAM_FIRESTORE_EMULATOR_HOST);
    snprintf(active.auth_emulator_host, sizeof(active.auth_emulator_host), "%s", CONFIG_WEBCAM_AUTH_EMULATOR_HOST);
#ifdef CONFIG_WEBCAM_IDP_URL
    snprintf(active.idp_url, sizeof(active.idp_url), "%s", CONFIG_WEBCAM_IDP_URL);
#endif
}

void cloud_settings_load(void)
{
    load_defaults();

    nvs_handle_t handle;
    esp_err_t err = nvs_open("cloud", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = 0;
        err = nvs_get_blob(handle, "settings", NULL, &size);
        cloud_settings_t *stored = calloc(1, sizeof(*stored));
        if (stored && err == ESP_OK && size > 0 && size <= sizeof(*stored)) {
            err = nvs_get_blob(handle, "settings", stored, &size);
            cloud_settings_t migrated;
            memset(&migrated, 0, sizeof(migrated));
            if (err == ESP_OK && cloud_settings_migrate(stored, size, &migrated)) {
                if (cloud_settings_valid(&migrated)) {
                    active = migrated;
                    ESP_LOGI(TAG, "saved cloud profile loaded (%s)",
                             size == sizeof(cloud_settings_v1_t) ? "v1 migrated" : "v2");
                } else {
                    ESP_LOGW(TAG, "saved cloud profile invalid; using build defaults");
                }
            } else if (err == ESP_OK) {
                ESP_LOGW(TAG, "saved cloud profile has an unknown version/layout; using build defaults");
            }
            wipe(&migrated, sizeof(migrated));
        } else if (err != ESP_ERR_NVS_NOT_FOUND && err != ESP_OK) {
            ESP_LOGW(TAG, "cloud profile read failed: %s", esp_err_to_name(err));
        } else if (err == ESP_OK && size > sizeof(*stored)) {
            ESP_LOGW(TAG, "saved cloud profile is larger than expected; using build defaults");
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

static bool write_blob(const cloud_settings_t *settings)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("cloud", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "settings", settings, sizeof(*settings));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cloud profile save failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool cloud_settings_save(const cloud_settings_t *settings)
{
    if (!cloud_settings_valid(settings)) return false;
    if (!write_blob(settings)) return false;
    saved = *settings;
    return true;
}

bool cloud_settings_store(const cloud_settings_t *settings)
{
    if (!cloud_settings_valid(settings)) return false;
    if (!write_blob(settings)) return false;
    saved = *settings;
    active = *settings;
    return true;
}

void cloud_settings_activate(const cloud_settings_t *settings)
{
    if (settings) active = *settings;
}

bool cloud_settings_configured(void)
{
    const cloud_settings_t *s = &active;
    return cloud_settings_valid(s) && s->edge_device_id[0] && s->idp_url[0] &&
        s->firebase_project_id[0] && s->firebase_api_key[0] &&
        network_settings_active()->signaling_url[0];
}

const char *cloud_authorization_name(const cloud_settings_t *s)
{
    int64_t now = (int64_t)time(NULL);
    if (s->authorization_state == CLOUD_AUTH_APPROVED) {
        if (!s->event_access[0] && !s->refresh_token[0]) return "required";
        if (s->grant_expires_at > 0 && now > 0 && now >= s->grant_expires_at)
            return "required";
        if (s->event_access_expires_at > 0 && now > 0 &&
            now >= s->event_access_expires_at && !s->refresh_token[0]) return "required";
        return "approved";
    }
    if (s->authorization_state == CLOUD_AUTH_PENDING) {
        if (s->device_code_expires_at > 0 && now > 0 && now >= s->device_code_expires_at)
            return "required";
        return "pending";
    }
    return "required";
}

void cloud_settings_show(const char *label, const cloud_settings_t *s)
{
    printf("%s\n  Edge device ID: %s\n  Firebase project: %s\n  Firebase API key: %s\n"
           "  Firestore: %s\n  Auth: %s\n  IdP: %s\n  Edge Bridge: %s\n"
           "  Authorization: %s\n  event_access: %s\n  refresh token: %s\n"
           "  Tenant / Event: %s / %s\n  event_access expires at: %lld\n"
           "  Grant expires at: %lld\n  Refresh in progress: %s\n",
        label,
        s->edge_device_id[0] ? s->edge_device_id : "(not configured)",
        s->firebase_project_id[0] ? s->firebase_project_id : "(not configured)",
        s->firebase_api_key[0] ? "(set, hidden)" : "(not configured)",
        s->firestore_emulator_host[0] ? s->firestore_emulator_host : "(production)",
        s->auth_emulator_host[0] ? s->auth_emulator_host : "(production)",
        s->idp_url[0] ? s->idp_url : "(not configured)",
        network_settings_active()->signaling_url[0] ? network_settings_active()->signaling_url : "(disabled)",
        cloud_authorization_name(s),
        s->event_access[0] ? "(set, hidden)" : "(not configured)",
        s->refresh_token[0] ? "(set, hidden)" : "(not configured)",
        s->tenant_id[0] ? s->tenant_id : "(not parsed)",
        s->event_id[0] ? s->event_id : "(not parsed)",
        (long long)s->event_access_expires_at,
        (long long)s->grant_expires_at,
        s->refresh_in_progress ? "yes" : "no");
}

#include "device_auth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "cloud_settings.h"
#include "cloud_settings_values.h"
#include "device_auth_values.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "device_auth";

#define IDP_HTTP_TIMEOUT_MS 15000
#define IDP_URL_MAX 320
#define IDP_BODY_MAX 2048
#define IDP_RESPONSE_MAX 8192
#define RETRY_MIN_SECONDS 5
#define RETRY_MAX_SECONDS 300
#define TOKEN_REFRESH_PERCENT 75

static SemaphoreHandle_t lock;
static int64_t refresh_at_ms;
static int64_t next_start_ms;
static int64_t next_poll_ms;
static unsigned start_backoff = RETRY_MIN_SECONDS;
static unsigned poll_backoff = RETRY_MIN_SECONDS;

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static int64_t wall_now(void)
{
    time_t now = time(NULL);
    return now > 0 ? (int64_t)now : 0;
}

static int64_t mono_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool take_lock(void)
{
    if (!lock) lock = xSemaphoreCreateRecursiveMutex();
    return lock && xSemaphoreTakeRecursive(lock, portMAX_DELAY) == pdTRUE;
}

static void give_lock(void)
{
    if (lock) xSemaphoreGiveRecursive(lock);
}

static void reset_backoff(unsigned *value)
{
    *value = RETRY_MIN_SECONDS;
}

static unsigned grow_backoff(unsigned *value)
{
    unsigned current = *value;
    if (current < RETRY_MAX_SECONDS) {
        current *= 2;
        if (current > RETRY_MAX_SECONDS) current = RETRY_MAX_SECONDS;
        *value = current;
    }
    return current;
}

static void print_json(cJSON *root)
{
    if (!root) return;
    char *line = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!line) return;
    printf("%s\n", line);
    fflush(stdout);
    cJSON_free(line);
}

static void print_error(const char *message)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "error", message);
    print_json(root);
}

static void print_authorization_result(bool ok)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "authorization_result");
    cJSON_AddBoolToObject(root, "ok", ok);
    print_json(root);
}

static void print_start(const device_auth_start_t *start)
{
    char line[512];
    if (device_auth_format_start(start, line, sizeof(line))) {
        printf("%s\n", line);
        fflush(stdout);
    }
    wipe(line, sizeof(line));
}

static int http_post_form(const char *url, const char *body, char *out, size_t out_size,
                          int *status)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = IDP_HTTP_TIMEOUT_MS,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
    esp_http_client_set_header(client, "Accept", "application/json");

    int body_length = body ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(client, body_length);
    if (err == ESP_OK && body &&
        esp_http_client_write(client, body, body_length) != body_length) err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;

    int total = 0;
    if (err == ESP_OK && out && out_size) {
        while (total < (int)out_size - 1) {
            int n = esp_http_client_read(client, out + total, (int)out_size - 1 - total);
            if (n < 0) { err = ESP_FAIL; break; }
            if (n == 0) break;
            total += n;
        }
        out[total] = 0;
    }
    *status = esp_http_client_get_status_code(client);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IdP request failed (%s), status=%d", esp_err_to_name(err), *status);
        return -1;
    }
    return total;
}

static bool form_append(char *out, size_t size, const char *name, const char *value)
{
    size_t used = strlen(out);
    int n = snprintf(out + used, size - used, "%s%s=", used ? "&" : "", name);
    if (n < 0 || (size_t)n >= size - used) return false;
    used += (size_t)n;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        bool safe = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '-' || *p == '.' || *p == '_' || *p == '~';
        if (safe) {
            if (used + 1 >= size) return false;
            out[used++] = (char)*p;
        } else {
            if (used + 3 >= size) return false;
            int written = snprintf(out + used, size - used, "%%%02X", *p);
            if (written != 3) return false;
            used += 3;
        }
    }
    out[used] = 0;
    return true;
}

static void schedule_refresh(void)
{
    const cloud_settings_t *s = cloud_settings_active();
    if (s->event_access_expires_in <= 0) {
        refresh_at_ms = 0;
        return;
    }
    int64_t lifetime = s->event_access_expires_in;
    int64_t target = lifetime * TOKEN_REFRESH_PERCENT / 100;
    int64_t now = wall_now();
    if (s->event_access_obtained_at <= 0) {
        /* The token was issued before the clock was known. Use the response
         * lifetime instead of the absolute exp claim. */
        refresh_at_ms = mono_ms() + target * 1000;
    } else if (now > 0 && now >= s->event_access_obtained_at) {
        int64_t elapsed = now - s->event_access_obtained_at;
        int64_t remaining = target - elapsed;
        if (remaining < 0) remaining = 0;
        refresh_at_ms = mono_ms() + remaining * 1000;
    } else {
        refresh_at_ms = mono_ms();
    }
}

static bool token_expired(int64_t now)
{
    const cloud_settings_t *s = cloud_settings_active();
    return s->event_access_expires_at > 0 && now > 0 &&
        now + 30 >= s->event_access_expires_at;
}

static bool start_locked(void)
{
    const cloud_settings_t *current = cloud_settings_active();
    int64_t now = wall_now();
    if (current->authorization_state == CLOUD_AUTH_PENDING &&
        (!current->device_code_expires_at || !now ||
         now < current->device_code_expires_at)) {
        device_auth_start_t pending = {0};
        snprintf(pending.user_code, sizeof(pending.user_code), "%s", current->user_code);
        snprintf(pending.verification_uri, sizeof(pending.verification_uri), "%s",
                 current->verification_uri);
        pending.expires_in = current->device_code_expires_at > 0 && now > 0 ?
            current->device_code_expires_at - now : 0;
        if (pending.expires_in < 0) pending.expires_in = 0;
        print_start(&pending);
        wipe(&pending, sizeof(pending));
        return true;
    }
    if (!cloud_idp_url_valid(current->idp_url)) {
        print_error("idp_url is not configured");
        return false;
    }

    char url[IDP_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s/oauth2/device_authorization", current->idp_url);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        print_error("idp_url is too long");
        return false;
    }
    char body[IDP_BODY_MAX] = {0};
    if (!form_append(body, sizeof(body), "client_id", "esp-camera") ||
        !form_append(body, sizeof(body), "scope", "events.stream")) {
        print_error("device authorization request is too long");
        return false;
    }

    char *response = calloc(1, IDP_RESPONSE_MAX);
    if (!response) {
        print_error("no memory for device authorization");
        return false;
    }
    int status = 0;
    int length = http_post_form(url, body, response, IDP_RESPONSE_MAX, &status);
    device_auth_start_t start = {0};
    bool ok = length >= 0 && status == 200 &&
        device_auth_parse_start(response, &start);
    wipe(response, IDP_RESPONSE_MAX);
    free(response);
    if (!ok) {
        ESP_LOGW(TAG, "device authorization start failed: HTTP %d", status);
        print_error("device authorization start failed");
        wipe(&start, sizeof(start));
        return false;
    }

    cloud_settings_t next = *cloud_settings_active();
    device_auth_state_begin(&next, &start, now);
    if (!cloud_settings_store(&next)) {
        print_error("could not save device authorization");
        wipe(&next, sizeof(next));
        wipe(&start, sizeof(start));
        return false;
    }
    next_start_ms = mono_ms() + (int64_t)start.interval * 1000;
    next_poll_ms = next_start_ms;
    reset_backoff(&start_backoff);
    ESP_LOGI(TAG, "device authorization pending (%us interval)", (unsigned)start.interval);
    print_start(&start);
    wipe(&next, sizeof(next));
    wipe(&start, sizeof(start));
    return true;
}

static bool poll_locked(void)
{
    const cloud_settings_t *current = cloud_settings_active();
    if (current->authorization_state != CLOUD_AUTH_PENDING) return false;
    int64_t now = wall_now();
    if (current->device_code_expires_at > 0 && now > 0 &&
        now >= current->device_code_expires_at) {
        cloud_settings_t next = *current;
        device_auth_state_clear_pending(&next);
        next.authorization_state = CLOUD_AUTH_NONE;
        next.token_state = CLOUD_TOKEN_STATE_NONE;
        cloud_settings_store(&next);
        wipe(&next, sizeof(next));
        print_error("expired_token");
        return false;
    }
    if (next_poll_ms && mono_ms() < next_poll_ms) return false;
    if (!cloud_idp_url_valid(current->idp_url)) {
        print_error("idp_url is not configured");
        return false;
    }

    char url[IDP_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s/oauth2/token", current->idp_url);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        print_error("idp_url is too long");
        return false;
    }
    char body[IDP_BODY_MAX] = {0};
    bool form = form_append(body, sizeof(body), "grant_type",
                             "urn:ietf:params:oauth:grant-type:device_code") &&
        form_append(body, sizeof(body), "device_code", current->device_code) &&
        form_append(body, sizeof(body), "client_id", "esp-camera");
    if (!form) {
        print_error("device code request is too long");
        return false;
    }

    char *response = calloc(1, IDP_RESPONSE_MAX);
    if (!response) {
        print_error("no memory for device authorization");
        return false;
    }
    int status = 0;
    int length = http_post_form(url, body, response, IDP_RESPONSE_MAX, &status);
    device_auth_tokens_t tokens = {0};
    device_auth_result_t result = length >= 0 ?
        device_auth_parse_token(response, &tokens) : DEVICE_AUTH_INVALID;
    if (result == DEVICE_AUTH_OK && status != 200) result = DEVICE_AUTH_INVALID;
    wipe(response, IDP_RESPONSE_MAX);
    free(response);

    if (length < 0) {
        unsigned wait = grow_backoff(&poll_backoff);
        next_poll_ms = mono_ms() + (int64_t)wait * 1000;
        ESP_LOGW(TAG, "device authorization poll failed; retrying in %us", wait);
        print_error("device authorization poll failed");
        wipe(&tokens, sizeof(tokens));
        return false;
    }

    if (result == DEVICE_AUTH_OK) {
        cloud_settings_t next = *cloud_settings_active();
        if (!device_auth_state_approved(&next, &tokens, now)) {
            device_auth_state_reapproval(&next);
            cloud_settings_store(&next);
            cloud_settings_activate(&next);
            print_error("event_access claims are invalid");
            wipe(&next, sizeof(next));
            wipe(&tokens, sizeof(tokens));
            return false;
        }
        if (!cloud_settings_store(&next)) {
            device_auth_state_reapproval(&next);
            cloud_settings_store(&next);
            cloud_settings_activate(&next);
            print_error("could not save authorization");
            wipe(&next, sizeof(next));
            wipe(&tokens, sizeof(tokens));
            return false;
        }
        schedule_refresh();
        reset_backoff(&poll_backoff);
        ESP_LOGI(TAG, "device authorization approved: tenant=%s event=%s",
                 next.tenant_id, next.event_id);
        print_authorization_result(true);
        wipe(&next, sizeof(next));
        wipe(&tokens, sizeof(tokens));
        return true;
    }

    if (result == DEVICE_AUTH_PENDING) {
        next_poll_ms = mono_ms() + (int64_t)(current->poll_interval_s ?
            current->poll_interval_s : 5) * 1000;
        reset_backoff(&poll_backoff);
        wipe(&tokens, sizeof(tokens));
        return false;
    }
    if (result == DEVICE_AUTH_SLOW_DOWN) {
        cloud_settings_t next = *cloud_settings_active();
        next.poll_interval_s += 5;
        if (next.poll_interval_s > 3600) next.poll_interval_s = 3600;
        cloud_settings_store(&next);
        next_poll_ms = mono_ms() + (int64_t)next.poll_interval_s * 1000;
        ESP_LOGW(TAG, "device authorization slow_down; interval=%us",
                 (unsigned)next.poll_interval_s);
        wipe(&next, sizeof(next));
        wipe(&tokens, sizeof(tokens));
        return false;
    }

    cloud_settings_t next = *cloud_settings_active();
    if (result == DEVICE_AUTH_EXPIRED || result == DEVICE_AUTH_DENIED ||
        result == DEVICE_AUTH_INVALID_GRANT) {
        device_auth_state_clear_pending(&next);
        next.authorization_state = CLOUD_AUTH_NONE;
        next.token_state = CLOUD_TOKEN_STATE_NONE;
        cloud_settings_store(&next);
        cloud_settings_activate(&next);
        print_error(result == DEVICE_AUTH_EXPIRED ? "expired_token" :
                    result == DEVICE_AUTH_DENIED ? "access_denied" : "invalid_grant");
    } else {
        unsigned wait = grow_backoff(&poll_backoff);
        next_poll_ms = mono_ms() + (int64_t)wait * 1000;
        print_error("device authorization failed");
    }
    wipe(&next, sizeof(next));
    wipe(&tokens, sizeof(tokens));
    return false;
}

static bool refresh_locked(bool force)
{
    const cloud_settings_t *current = cloud_settings_active();
    if (!current->refresh_token[0]) return false;
    int64_t now = wall_now();
    if (current->grant_expires_at > 0 && now > 0 && now >= current->grant_expires_at) {
        cloud_settings_t next = *current;
        device_auth_state_reapproval(&next);
        cloud_settings_store(&next);
        cloud_settings_activate(&next);
        wipe(&next, sizeof(next));
        print_error("authorization_required");
        return false;
    }
    if (!force && refresh_at_ms && mono_ms() < refresh_at_ms) return true;
    if (!cloud_idp_url_valid(current->idp_url)) {
        print_error("idp_url is not configured");
        return false;
    }

    cloud_settings_t next = *current;
    next.refresh_in_progress = 1;
    if (!cloud_settings_store(&next)) {
        wipe(&next, sizeof(next));
        print_error("could not record refresh in progress");
        return false;
    }

    char url[IDP_URL_MAX];
    int n = snprintf(url, sizeof(url), "%s/oauth2/token", current->idp_url);
    if (n < 0 || (size_t)n >= sizeof(url)) {
        next.refresh_in_progress = 0;
        cloud_settings_store(&next);
        wipe(&next, sizeof(next));
        print_error("idp_url is too long");
        return false;
    }
    char body[IDP_BODY_MAX] = {0};
    if (!form_append(body, sizeof(body), "grant_type", "refresh_token") ||
        !form_append(body, sizeof(body), "refresh_token", current->refresh_token) ||
        !form_append(body, sizeof(body), "client_id", "esp-camera")) {
        next.refresh_in_progress = 0;
        cloud_settings_store(&next);
        wipe(&next, sizeof(next));
        print_error("refresh request is too long");
        return false;
    }

    char *response = calloc(1, IDP_RESPONSE_MAX);
    if (!response) {
        next.refresh_in_progress = 0;
        cloud_settings_store(&next);
        wipe(&next, sizeof(next));
        print_error("no memory for token refresh");
        return false;
    }
    int status = 0;
    int length = http_post_form(url, body, response, IDP_RESPONSE_MAX, &status);
    device_auth_tokens_t tokens = {0};
    device_auth_result_t result = length >= 0 ?
        device_auth_parse_token(response, &tokens) : DEVICE_AUTH_INVALID;
    if (result == DEVICE_AUTH_OK && status != 200) result = DEVICE_AUTH_INVALID;
    wipe(response, IDP_RESPONSE_MAX);
    free(response);

    if (result == DEVICE_AUTH_OK) {
        if (!device_auth_state_approved(&next, &tokens, now)) {
            device_auth_state_reapproval(&next);
            cloud_settings_store(&next);
            cloud_settings_activate(&next);
            print_error("event_access claims are invalid");
            wipe(&next, sizeof(next));
            wipe(&tokens, sizeof(tokens));
            return false;
        }
        /* state_approved puts the new refresh token in next. Persist it before
         * the active profile drops the old one. */
        if (!cloud_settings_store(&next)) {
            cloud_settings_t failed = *current;
            device_auth_state_reapproval(&failed);
            cloud_settings_store(&failed);
            cloud_settings_activate(&failed);
            wipe(&failed, sizeof(failed));
            print_error("could not save refreshed token");
            wipe(&next, sizeof(next));
            wipe(&tokens, sizeof(tokens));
            return false;
        }
        schedule_refresh();
        ESP_LOGI(TAG, "event_access refreshed: tenant=%s event=%s",
                 next.tenant_id, next.event_id);
        wipe(&next, sizeof(next));
        wipe(&tokens, sizeof(tokens));
        return true;
    }

    if (result == DEVICE_AUTH_INVALID_GRANT) {
        device_auth_state_reapproval(&next);
        cloud_settings_store(&next);
        cloud_settings_activate(&next);
        ESP_LOGW(TAG, "refresh token rejected; reapproval required");
        print_error("authorization_required");
        wipe(&next, sizeof(next));
        wipe(&tokens, sizeof(tokens));
        return false;
    }

    next.refresh_in_progress = 0;
    cloud_settings_store(&next);
    unsigned wait = grow_backoff(&poll_backoff);
    next_poll_ms = mono_ms() + (int64_t)wait * 1000;
    ESP_LOGW(TAG, "token refresh failed; retrying in %us", wait);
    print_error("token refresh failed");
    wipe(&next, sizeof(next));
    wipe(&tokens, sizeof(tokens));
    return false;
}

void device_auth_initialize(void)
{
    if (!take_lock()) return;
    cloud_settings_t next = *cloud_settings_active();
    int64_t now = wall_now();
    bool changed = false;
    if (next.refresh_in_progress) {
        ESP_LOGW(TAG, "refresh was interrupted; reapproval required");
        device_auth_state_reapproval(&next);
        changed = true;
    } else if (next.authorization_state == CLOUD_AUTH_PENDING &&
               next.device_code_expires_at > 0 && now > 0 &&
               now >= next.device_code_expires_at) {
        device_auth_state_clear_pending(&next);
        next.authorization_state = CLOUD_AUTH_NONE;
        changed = true;
    } else if (next.authorization_state == CLOUD_AUTH_APPROVED &&
               next.grant_expires_at > 0 && now > 0 &&
               now >= next.grant_expires_at) {
        ESP_LOGW(TAG, "device grant expired; reapproval required");
        device_auth_state_reapproval(&next);
        changed = true;
    }
    if (next.event_access[0]) {
        schedule_refresh();
    } else {
        refresh_at_ms = 0;
    }
    if (next.authorization_state == CLOUD_AUTH_PENDING) {
        next_poll_ms = mono_ms();
    }
    if (changed) {
        cloud_settings_store(&next);
        cloud_settings_activate(&next);
    }
    wipe(&next, sizeof(next));
    give_lock();
}

bool device_auth_start(void)
{
    if (!take_lock()) return false;
    bool ok = start_locked();
    give_lock();
    return ok;
}

bool device_auth_force_refresh(void)
{
    if (!take_lock()) return false;
    bool ok = refresh_locked(true);
    give_lock();
    return ok;
}

bool device_auth_ensure_access(void)
{
    if (!take_lock()) return false;
    const cloud_settings_t *current = cloud_settings_active();
    bool ok = false;
    int64_t now = wall_now();

    if (current->authorization_state == CLOUD_AUTH_REAPPROVAL_REQUIRED) {
        give_lock();
        return false;
    }

    if (current->authorization_state == CLOUD_AUTH_PENDING) {
        poll_locked();
        current = cloud_settings_active();
        ok = current->authorization_state == CLOUD_AUTH_APPROVED &&
            current->event_access[0];
        give_lock();
        return ok;
    }

    if (current->authorization_state == CLOUD_AUTH_NONE ||
        !current->event_access[0]) {
        if (current->refresh_token[0] && current->authorization_state == CLOUD_AUTH_APPROVED) {
            refresh_locked(true);
        } else if (current->authorization_state == CLOUD_AUTH_NONE) {
            if (!next_start_ms || mono_ms() >= next_start_ms) start_locked();
        } else {
            cloud_settings_t next = *current;
            device_auth_state_reapproval(&next);
            cloud_settings_store(&next);
            cloud_settings_activate(&next);
            wipe(&next, sizeof(next));
        }
        current = cloud_settings_active();
        ok = current->authorization_state == CLOUD_AUTH_APPROVED &&
            current->event_access[0];
        give_lock();
        return ok;
    }

    if (token_expired(now)) {
        if (current->refresh_token[0]) refresh_locked(false);
        else {
            cloud_settings_t next = *current;
            device_auth_state_reapproval(&next);
            cloud_settings_store(&next);
            cloud_settings_activate(&next);
            wipe(&next, sizeof(next));
            print_error("authorization_required");
        }
    } else if (current->event_access_expires_in > 0 && current->refresh_token[0] &&
               (!refresh_at_ms || mono_ms() >= refresh_at_ms)) {
        refresh_locked(false);
    }

    current = cloud_settings_active();
    ok = current->authorization_state == CLOUD_AUTH_APPROVED &&
        current->event_access[0];
    give_lock();
    return ok;
}

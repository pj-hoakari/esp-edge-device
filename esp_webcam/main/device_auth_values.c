#include "device_auth_values.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "cloud_settings_values.h"

#define DEFAULT_POLL_INTERVAL 5
#define REFRESH_TOKEN_TTL_SECONDS (24 * 60 * 60)
#define GRANT_MAX_SECONDS (7 * 24 * 60 * 60)

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool json_string(const cJSON *root, const char *name, char *out, size_t size,
                        bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring) return !required;
    if (!item->valuestring[0] || strlen(item->valuestring) >= size) return false;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static bool json_seconds(const cJSON *root, const char *name, int64_t *out, bool required)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!item) return !required;
    if (cJSON_IsNumber(item) && item->valuedouble >= 0) {
        *out = (int64_t)item->valuedouble;
        return true;
    }
    if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
        char *end = NULL;
        long long value = strtoll(item->valuestring, &end, 10);
        if (!end || *end || value < 0) return false;
        *out = value;
        return true;
    }
    return false;
}

bool device_auth_parse_start(const char *json, device_auth_start_t *out)
{
    if (!json || !out) return false;
    cJSON *root = cJSON_Parse(json);
    if (!root) return false;
    device_auth_start_t parsed = {0};
    int64_t interval = DEFAULT_POLL_INTERVAL;
    bool valid = json_string(root, "device_code", parsed.device_code,
                             sizeof(parsed.device_code), true) &&
        json_string(root, "user_code", parsed.user_code, sizeof(parsed.user_code), true) &&
        json_string(root, "verification_uri", parsed.verification_uri,
                    sizeof(parsed.verification_uri), true) &&
        json_seconds(root, "expires_in", &parsed.expires_in, true) &&
        parsed.expires_in > 0 &&
        json_seconds(root, "interval", &interval, false) &&
        interval > 0 && interval <= 3600;
    if (valid) {
        parsed.interval = (uint32_t)interval;
        *out = parsed;
    }
    wipe(&parsed, sizeof(parsed));
    cJSON_Delete(root);
    return valid;
}

device_auth_result_t device_auth_parse_token(const char *json, device_auth_tokens_t *out)
{
    if (!json || !out) return DEVICE_AUTH_INVALID;
    cJSON *root = cJSON_Parse(json);
    if (!root) return DEVICE_AUTH_INVALID;

    const cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsString(error) && error->valuestring) {
        device_auth_result_t result = DEVICE_AUTH_INVALID;
        if (!strcmp(error->valuestring, "authorization_pending")) result = DEVICE_AUTH_PENDING;
        else if (!strcmp(error->valuestring, "slow_down")) result = DEVICE_AUTH_SLOW_DOWN;
        else if (!strcmp(error->valuestring, "expired_token")) result = DEVICE_AUTH_EXPIRED;
        else if (!strcmp(error->valuestring, "access_denied")) result = DEVICE_AUTH_DENIED;
        else if (!strcmp(error->valuestring, "invalid_grant")) result = DEVICE_AUTH_INVALID_GRANT;
        cJSON_Delete(root);
        return result;
    }

    device_auth_tokens_t parsed = {0};
    int64_t expires_in = 0;
    bool valid = json_string(root, "access_token", parsed.access_token,
                             sizeof(parsed.access_token), true) &&
        json_string(root, "refresh_token", parsed.refresh_token,
                    sizeof(parsed.refresh_token), false) &&
        json_seconds(root, "expires_in", &expires_in, true) &&
        expires_in > 0 &&
        json_seconds(root, "refresh_expires_in", &parsed.refresh_expires_in, false) &&
        json_seconds(root, "grant_expires_in", &parsed.grant_expires_in, false);
    if (valid) {
        parsed.expires_in = expires_in;
        if (!json_string(root, "token_type", parsed.token_type,
                         sizeof(parsed.token_type), false) || !parsed.token_type[0])
            snprintf(parsed.token_type, sizeof(parsed.token_type), "Bearer");
        *out = parsed;
    }
    wipe(&parsed, sizeof(parsed));
    cJSON_Delete(root);
    return valid ? DEVICE_AUTH_OK : DEVICE_AUTH_INVALID;
}

bool device_auth_format_start(const device_auth_start_t *start, char *out, size_t size)
{
    if (!start || !out || !size) return false;
    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "type", "device_authorization");
    cJSON_AddStringToObject(root, "user_code", start->user_code);
    cJSON_AddStringToObject(root, "verification_uri", start->verification_uri);
    cJSON_AddNumberToObject(root, "expires_in", (double)start->expires_in);
    char *line = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!line) return false;
    bool ok = strlen(line) < size;
    if (ok) memcpy(out, line, strlen(line) + 1);
    cJSON_free(line);
    return ok;
}

static void clear_tokens(cloud_settings_t *s)
{
    wipe(s->event_access, sizeof(s->event_access));
    wipe(s->refresh_token, sizeof(s->refresh_token));
    wipe(s->token_type, sizeof(s->token_type));
    wipe(s->tenant_id, sizeof(s->tenant_id));
    wipe(s->event_id, sizeof(s->event_id));
    s->event_access_expires_at = 0;
    s->event_access_obtained_at = 0;
    s->event_access_expires_in = 0;
    s->refresh_token_expires_at = 0;
    s->grant_expires_at = 0;
    s->token_state = CLOUD_TOKEN_STATE_NONE;
}

void device_auth_state_clear_pending(cloud_settings_t *s)
{
    wipe(s->device_code, sizeof(s->device_code));
    wipe(s->user_code, sizeof(s->user_code));
    wipe(s->verification_uri, sizeof(s->verification_uri));
    s->device_code_expires_at = 0;
    s->poll_interval_s = 0;
}

bool device_auth_state_begin(cloud_settings_t *s, const device_auth_start_t *start, int64_t now)
{
    if (!s || !start) return false;
    clear_tokens(s);
    snprintf(s->device_code, sizeof(s->device_code), "%s", start->device_code);
    snprintf(s->user_code, sizeof(s->user_code), "%s", start->user_code);
    snprintf(s->verification_uri, sizeof(s->verification_uri), "%s", start->verification_uri);
    s->device_code_expires_at = now > 0 ? now + start->expires_in : 0;
    s->poll_interval_s = start->interval;
    s->authorization_state = CLOUD_AUTH_PENDING;
    s->refresh_in_progress = 0;
    return true;
}

bool device_auth_state_approved(cloud_settings_t *s, const device_auth_tokens_t *tokens,
                                int64_t now)
{
    if (!s || !tokens || !tokens->access_token[0]) return false;
    cloud_token_claims_t claims = {0};
    if (!cloud_token_parse(tokens->access_token, s->edge_device_id, now, &claims)) {
        wipe(&claims, sizeof(claims));
        return false;
    }

    int64_t grant = s->grant_expires_at;
    if (now > 0) {
        int64_t max_grant = now + GRANT_MAX_SECONDS;
        if (tokens->grant_expires_in > 0) {
            int64_t reported = now + tokens->grant_expires_in;
            if (reported < max_grant) max_grant = reported;
        }
        grant = (!grant || grant > max_grant) ? max_grant : grant;
    }

    snprintf(s->event_access, sizeof(s->event_access), "%s", tokens->access_token);
    snprintf(s->tenant_id, sizeof(s->tenant_id), "%s", claims.tenant_id);
    snprintf(s->event_id, sizeof(s->event_id), "%s", claims.event_id);
    snprintf(s->token_type, sizeof(s->token_type), "%s",
             tokens->token_type[0] ? tokens->token_type : "Bearer");
    if (tokens->refresh_token[0])
        snprintf(s->refresh_token, sizeof(s->refresh_token), "%s", tokens->refresh_token);
    s->event_access_expires_at = claims.expires_at;
    s->event_access_obtained_at = now > 0 ? now : 0;
    s->event_access_expires_in = tokens->expires_in;
    int64_t refresh_ttl = tokens->refresh_expires_in > 0 ?
        tokens->refresh_expires_in : REFRESH_TOKEN_TTL_SECONDS;
    if (now > 0 && s->refresh_token[0]) {
        s->refresh_token_expires_at = now + refresh_ttl;
        if (grant > 0 && s->refresh_token_expires_at > grant)
            s->refresh_token_expires_at = grant;
    } else {
        s->refresh_token_expires_at = 0;
    }
    s->grant_expires_at = grant;
    s->authorization_state = CLOUD_AUTH_APPROVED;
    s->token_state = CLOUD_TOKEN_STATE_VALID;
    s->refresh_in_progress = 0;
    device_auth_state_clear_pending(s);
    wipe(&claims, sizeof(claims));
    return true;
}

bool device_auth_state_reapproval(cloud_settings_t *s)
{
    if (!s) return false;
    clear_tokens(s);
    device_auth_state_clear_pending(s);
    s->authorization_state = CLOUD_AUTH_REAPPROVAL_REQUIRED;
    s->token_state = CLOUD_TOKEN_STATE_REJECTED;
    s->refresh_in_progress = 0;
    return true;
}

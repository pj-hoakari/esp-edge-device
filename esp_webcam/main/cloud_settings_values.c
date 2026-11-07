#include "cloud_settings_values.h"
#include "credential_transport.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool host_label_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.';
}

bool cloud_idp_url_valid(const char *url)
{
    if (!credential_url_allowed(url)) return false;
    const char *host;
    if (!strncmp(url, "https://", 8)) host = url + 8;
    else if (!strncmp(url, "http://", 7)) host = url + 7;
    else return false;
    size_t length = strlen(host);
    if (!length || strlen(url) >= CLOUD_IDP_URL_SIZE) return false;

    const char *colon = strchr(host, ':');
    size_t host_length = colon ? (size_t)(colon - host) : length;
    if (!host_length || host_length > 253) return false;
    if (host[host_length - 1] == '-') return false;
    if (host[0] == '-' || host[0] == '.') return false;
    if (host[host_length - 1] == '.') return false;

    size_t label = 0;
    for (size_t i = 0; i < host_length; i++) {
        char c = host[i];
        if (c == '.') {
            if (!label || host[i - 1] == '-') return false;
            label = 0;
            continue;
        }
        if (!host_label_char(c)) return false;
        if (!label && c == '-') return false;
        if (++label > 63) return false;
    }
    if (!label) return false;

    if (colon) {
        if (strchr(colon + 1, ':')) return false;
        size_t digits = length - host_length - 1;
        if (!digits || digits > 5) return false;
        unsigned port = 0;
        for (size_t i = host_length + 1; i < length; i++) {
            char c = host[i];
            if (c < '0' || c > '9') return false;
            port = port * 10 + (unsigned)(c - '0');
        }
        if (!port || port > 65535) return false;
    }
    return true;
}

bool cloud_emulator_host_valid(const char *host)
{
    if (!host) return false;
    if (!host[0]) return true;
    if (strlen(host) >= CLOUD_HOST_SIZE) return false;
    if (strstr(host, "://")) return cloud_idp_url_valid(host);
    char url[CLOUD_HOST_SIZE + 8];
    snprintf(url, sizeof(url), "http://%s", host);
    return cloud_idp_url_valid(url);
}

bool cloud_settings_migrate(const void *blob, size_t size, cloud_settings_t *out)
{
    if (!blob || !out) return false;
    if (size == sizeof(cloud_settings_t)) {
        const cloud_settings_t *v2 = blob;
        if (v2->version != CLOUD_SETTINGS_VERSION) return false;
        *out = *v2;
        return true;
    }
    if (size == sizeof(cloud_settings_v1_t)) {
        const cloud_settings_v1_t *v1 = blob;
        if (v1->version != 1) return false;
        memset(out, 0, sizeof(*out));
        out->version = CLOUD_SETTINGS_VERSION;
        memcpy(out->edge_device_id, v1->edge_device_id, sizeof(out->edge_device_id));
        memcpy(out->firebase_project_id, v1->firebase_project_id, sizeof(out->firebase_project_id));
        memcpy(out->firebase_api_key, v1->firebase_api_key, sizeof(out->firebase_api_key));
        memcpy(out->firestore_emulator_host, v1->firestore_emulator_host,
               sizeof(out->firestore_emulator_host));
        memcpy(out->auth_emulator_host, v1->auth_emulator_host, sizeof(out->auth_emulator_host));
        memcpy(out->event_access, v1->event_access, sizeof(out->event_access));
        memcpy(out->tenant_id, v1->tenant_id, sizeof(out->tenant_id));
        memcpy(out->event_id, v1->event_id, sizeof(out->event_id));
        out->event_access_expires_at = v1->event_access_expires_at;
        if (out->event_access[0]) {
            snprintf(out->token_type, sizeof(out->token_type), "Bearer");
            out->authorization_state = CLOUD_AUTH_APPROVED;
            out->token_state = CLOUD_TOKEN_STATE_VALID;
        }
        return true;
    }
    return false;
}

static int base64url_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

/* Decodes into out (NUL terminated) so cJSON can parse the payload. */
static bool base64url_decode(const char *input, size_t length, char *out, size_t out_size)
{
    if (!length || length > CLOUD_TOKEN_SIZE || length % 4 == 1) return false;
    size_t written = 0;
    unsigned buffer = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < length; i++) {
        char c = input[i];
        if (c == '=') break;
        int value = base64url_value(c);
        if (value < 0) return false;
        buffer = (buffer << 6) | (unsigned)value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written + 1 >= out_size) return false;
            out[written++] = (char)((buffer >> bits) & 0xff);
        }
    }
    out[written] = 0;
    return written > 0;
}

static bool json_string(const cJSON *root, const char *name, char *out, size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] ||
        strlen(item->valuestring) >= size) return false;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static bool scope_has(const cJSON *root, const char *wanted)
{
    const cJSON *scope = cJSON_GetObjectItemCaseSensitive(root, "scope");
    if (cJSON_IsString(scope) && scope->valuestring) {
        const char *p = scope->valuestring;
        size_t wanted_length = strlen(wanted);
        while (*p) {
            while (*p == ' ') p++;
            const char *start = p;
            while (*p && *p != ' ') p++;
            if ((size_t)(p - start) == wanted_length && !strncmp(start, wanted, wanted_length))
                return true;
        }
        return false;
    }
    if (cJSON_IsArray(scope)) {
        const cJSON *item = NULL;
        cJSON_ArrayForEach(item, scope) {
            if (cJSON_IsString(item) && item->valuestring &&
                !strcmp(item->valuestring, wanted)) return true;
        }
    }
    return false;
}

bool cloud_token_parse(const char *token, const char *expected_edge_device_id,
                       int64_t now, cloud_token_claims_t *out)
{
    if (!token || !out) return false;
    size_t length = strlen(token);
    if (!length || length >= CLOUD_TOKEN_SIZE) return false;
    const char *first = strchr(token, '.');
    const char *second = first ? strchr(first + 1, '.') : NULL;
    if (!first || !second || second == first + 1) return false;

    char *payload = calloc(1, CLOUD_TOKEN_SIZE);
    if (!payload) return false;
    bool ok = base64url_decode(first + 1, (size_t)(second - first - 1),
                               payload, CLOUD_TOKEN_SIZE);
    if (!ok) {
        wipe(payload, CLOUD_TOKEN_SIZE);
        free(payload);
        return false;
    }

    cJSON *root = cJSON_Parse(payload);
    wipe(payload, CLOUD_TOKEN_SIZE);
    free(payload);
    if (!root) return false;

    cloud_token_claims_t claims = {0};
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "exp");
    const cJSON *use = cJSON_GetObjectItemCaseSensitive(root, "token_use");
    const cJSON *edge = cJSON_GetObjectItemCaseSensitive(root, "edge_device_id");
    bool valid = json_string(root, "tenant_id", claims.tenant_id, sizeof(claims.tenant_id)) &&
        json_string(root, "event_id", claims.event_id, sizeof(claims.event_id)) &&
        cJSON_IsNumber(exp) && exp->valuedouble > 0 &&
        cJSON_IsString(use) && use->valuestring && !strcmp(use->valuestring, "event_access") &&
        scope_has(root, "events.stream");

    if (valid && now > 0 && (int64_t)exp->valuedouble <= now) valid = false;

    if (valid && cJSON_IsString(edge) && edge->valuestring) {
        if (!edge->valuestring[0] || strlen(edge->valuestring) >= sizeof(claims.edge_device_id) ||
            (expected_edge_device_id && expected_edge_device_id[0] &&
             strcmp(edge->valuestring, expected_edge_device_id)))
            valid = false;
        else
            memcpy(claims.edge_device_id, edge->valuestring, strlen(edge->valuestring) + 1);
    } else if (edge && !cJSON_IsNull(edge) && !cJSON_IsString(edge)) {
        valid = false;
    }

    if (valid) {
        claims.expires_at = (int64_t)exp->valuedouble;
        *out = claims;
    }
    cJSON_Delete(root);
    wipe(&claims, sizeof(claims));
    return valid;
}

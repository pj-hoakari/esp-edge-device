#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "../main/cloud_settings.h"
#include "../main/cloud_settings_values.h"
#include "../main/device_auth_values.h"

static const char *const EDGE = "0123456789abcdef";

static size_t b64url(const unsigned char *in, size_t length, char *out)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t written = 0;
    unsigned buffer = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < length; i++) {
        buffer = (buffer << 8) | in[i];
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            out[written++] = table[(buffer >> bits) & 63];
        }
    }
    if (bits) out[written++] = table[(buffer << (6 - bits)) & 63];
    out[written] = 0;
    return written;
}

static void make_token(cJSON *claims, char *out, size_t size)
{
    char *payload = cJSON_PrintUnformatted(claims);
    assert(payload);
    char encoded[4096];
    b64url((const unsigned char *)payload, strlen(payload), encoded);
    snprintf(out, size, "e30.%s.c2ln", encoded);
    free(payload);
}

static void url_checks(void)
{
    const char *good[] = {
        "http://localhost:18080", "https://idp.example", "https://auth.example:8443",
        "http://192.168.1.20:8080",
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++)
        assert(cloud_idp_url_valid(good[i]));
    const char *bad[] = {
        "", "idp.example", "ftp://idp.example", "https://idp.example/",
        "https://idp.example/path", "https://idp.example?x=1", "https://idp.example#x",
        "https://user:pass@idp.example", "https://idp.example:", "https://idp.example:0",
        "https://idp.example:65536", "https://idp.example:abc", "http://",
        "https://idp..example", "https://-idp.example", "https://idp example",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(!cloud_idp_url_valid(bad[i]));
}

static void migration_checks(void)
{
    cloud_settings_v1_t v1 = {.version = 1};
    strcpy(v1.edge_device_id, EDGE);
    strcpy(v1.firebase_project_id, "project-1");
    strcpy(v1.firebase_api_key, "api-key-1");
    strcpy(v1.event_access, "old.token.value");
    strcpy(v1.tenant_id, "tenant-1");
    strcpy(v1.event_id, "event-1");
    v1.event_access_expires_at = 12345;

    cloud_settings_t out;
    memset(&out, 0xa5, sizeof(out));
    assert(cloud_settings_migrate(&v1, sizeof(v1), &out));
    assert(out.version == CLOUD_SETTINGS_VERSION);
    assert(!strcmp(out.edge_device_id, EDGE));
    assert(!strcmp(out.event_access, "old.token.value"));
    assert(!strcmp(out.tenant_id, "tenant-1"));
    assert(out.event_access_expires_at == 12345);
    assert(out.authorization_state == CLOUD_AUTH_APPROVED);
    assert(out.token_state == CLOUD_TOKEN_STATE_VALID);
    assert(out.idp_url[0] == 0 && out.refresh_token[0] == 0);

    cloud_settings_t v2 = {.version = CLOUD_SETTINGS_VERSION};
    strcpy(v2.edge_device_id, EDGE);
    assert(cloud_settings_migrate(&v2, sizeof(v2), &out));
    assert(!strcmp(out.edge_device_id, EDGE));

    v2.version = 3;
    assert(!cloud_settings_migrate(&v2, sizeof(v2), &out));
    v2.version = 1;
    assert(!cloud_settings_migrate(&v2, sizeof(v2), &out));
    v1.version = 2;
    assert(!cloud_settings_migrate(&v1, sizeof(v1), &out));
    assert(!cloud_settings_migrate(&v1, sizeof(v1) - 1, &out));
}

static cJSON *claims(int64_t exp, const char *edge, const char *scope)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tenant_id", "tenant-1");
    cJSON_AddStringToObject(root, "event_id", "event-1");
    cJSON_AddStringToObject(root, "token_use", "event_access");
    cJSON_AddNumberToObject(root, "exp", (double)exp);
    cJSON_AddStringToObject(root, "scope", scope);
    if (edge) cJSON_AddStringToObject(root, "edge_device_id", edge);
    return root;
}

static void token_checks(void)
{
    int64_t now = (int64_t)time(NULL);
    char token[4096];
    cloud_token_claims_t out;

    cJSON *root = claims(now + 600, EDGE, "events.stream");
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    assert(cloud_token_parse(token, EDGE, now, &out));
    assert(!strcmp(out.tenant_id, "tenant-1") && !strcmp(out.event_id, "event-1"));
    assert(out.expires_at == now + 600);
    assert(!cloud_token_parse(token, "fedcba9876543210", now, &out));

    root = claims(now + 600, NULL, "openid events.stream");
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    assert(cloud_token_parse(token, EDGE, now, &out));

    root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "tenant_id", "tenant-1");
    cJSON_AddStringToObject(root, "event_id", "event-1");
    cJSON_AddNumberToObject(root, "exp", (double)(now + 600));
    cJSON_AddStringToObject(root, "token_use", "event_access");
    cJSON_AddItemToObject(root, "scope", cJSON_CreateString("events.stream"));
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    assert(cloud_token_parse(token, EDGE, now, &out));

    root = claims(now + 600, EDGE, "events.report");
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    assert(!cloud_token_parse(token, EDGE, now, &out));

    root = claims(now + 600, EDGE, "events.stream");
    cJSON_ReplaceItemInObject(root, "token_use", cJSON_CreateString("tenant_access"));
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    assert(!cloud_token_parse(token, EDGE, now, &out));

    root = claims(now - 1, EDGE, "events.stream");
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    assert(!cloud_token_parse(token, EDGE, now, &out));

    root = claims(now + 600, EDGE, "events.stream");
    cJSON_DeleteItemFromObject(root, "event_id");
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    assert(!cloud_token_parse(token, EDGE, now, &out));

    assert(!cloud_token_parse("not-a-jwt", EDGE, now, &out));
    assert(!cloud_token_parse("e30..sig", EDGE, now, &out));
}

static void device_response_checks(void)
{
    device_auth_start_t start;
    assert(device_auth_parse_start(
        "{\"device_code\":\"device\",\"user_code\":\"ABCD-EFGH\","
        "\"verification_uri\":\"https://idp.example/device\",\"expires_in\":600,"
        "\"interval\":5}", &start));
    assert(!strcmp(start.device_code, "device") && start.expires_in == 600);
    assert(start.interval == 5);
    assert(device_auth_parse_start(
        "{\"device_code\":\"device\",\"user_code\":\"ABCD-EFGH\","
        "\"verification_uri\":\"https://idp.example/device\",\"expires_in\":600}", &start));
    assert(start.interval == 5);
    assert(!device_auth_parse_start("{\"user_code\":\"x\"}", &start));
    assert(!device_auth_parse_start(
        "{\"device_code\":\"d\",\"user_code\":\"u\",\"verification_uri\":\"v\","
        "\"expires_in\":0}", &start));

    char line[512];
    start.interval = 5;
    assert(device_auth_format_start(&start, line, sizeof(line)));
    assert(strstr(line, "\"type\":\"device_authorization\""));
    assert(strstr(line, "\"user_code\":\"ABCD-EFGH\""));
    assert(!strstr(line, "device_code"));
    assert(!strstr(line, "access_token"));
    assert(!strstr(line, "refresh_token"));

    device_auth_tokens_t tokens;
    assert(device_auth_parse_token(
        "{\"access_token\":\"access\",\"refresh_token\":\"refresh\","
        "\"token_type\":\"Bearer\",\"expires_in\":600}", &tokens) == DEVICE_AUTH_OK);
    assert(!strcmp(tokens.access_token, "access") && !strcmp(tokens.refresh_token, "refresh"));
    assert(tokens.expires_in == 600);
    assert(device_auth_parse_token(
        "{\"error\":\"authorization_pending\"}", &tokens) == DEVICE_AUTH_PENDING);
    assert(device_auth_parse_token(
        "{\"error\":\"slow_down\"}", &tokens) == DEVICE_AUTH_SLOW_DOWN);
    assert(device_auth_parse_token(
        "{\"error\":\"expired_token\"}", &tokens) == DEVICE_AUTH_EXPIRED);
    assert(device_auth_parse_token(
        "{\"error\":\"access_denied\"}", &tokens) == DEVICE_AUTH_DENIED);
    assert(device_auth_parse_token(
        "{\"error\":\"invalid_grant\"}", &tokens) == DEVICE_AUTH_INVALID_GRANT);
    assert(device_auth_parse_token("{\"error\":\"other\"}", &tokens) == DEVICE_AUTH_INVALID);
    assert(device_auth_parse_token("{\"access_token\":\"x\"}", &tokens) == DEVICE_AUTH_INVALID);
}

static void state_checks(void)
{
    int64_t now = (int64_t)time(NULL);
    cloud_settings_t settings = {.version = CLOUD_SETTINGS_VERSION};
    strcpy(settings.edge_device_id, EDGE);
    strcpy(settings.firebase_project_id, "project-1");
    strcpy(settings.firebase_api_key, "key");
    strcpy(settings.idp_url, "https://idp.example");
    settings.grant_expires_at = now + 1000;

    device_auth_start_t start = {0};
    strcpy(start.device_code, "device");
    strcpy(start.user_code, "ABCD-EFGH");
    strcpy(start.verification_uri, "https://idp.example/device");
    start.expires_in = 600;
    start.interval = 5;
    assert(device_auth_state_begin(&settings, &start, now));
    assert(settings.authorization_state == CLOUD_AUTH_PENDING);
    assert(settings.grant_expires_at == 0 && !settings.event_access[0]);

    char token[4096];
    cJSON *root = claims(now + 600, EDGE, "events.stream");
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    device_auth_tokens_t tokens = {.expires_in = 600};
    strcpy(tokens.access_token, token);
    strcpy(tokens.refresh_token, "refresh-1");
    strcpy(tokens.token_type, "Bearer");
    assert(device_auth_state_approved(&settings, &tokens, now));
    assert(settings.authorization_state == CLOUD_AUTH_APPROVED);
    assert(settings.token_state == CLOUD_TOKEN_STATE_VALID);
    assert(!strcmp(settings.tenant_id, "tenant-1") && !strcmp(settings.event_id, "event-1"));
    assert(!strcmp(settings.refresh_token, "refresh-1"));
    assert(settings.event_access_obtained_at == now);
    assert(settings.event_access_expires_in == 600);
    assert(settings.grant_expires_at == now + 7 * 24 * 3600);
    assert(settings.refresh_token_expires_at == now + 24 * 3600);
    assert(!settings.device_code[0] && !settings.user_code[0]);

    root = claims(now + 1200, EDGE, "events.stream");
    make_token(root, token, sizeof(token));
    cJSON_Delete(root);
    strcpy(tokens.access_token, token);
    strcpy(tokens.refresh_token, "refresh-2");
    tokens.expires_in = 600;
    assert(device_auth_state_approved(&settings, &tokens, now + 300));
    assert(!strcmp(settings.refresh_token, "refresh-2"));
    assert(settings.grant_expires_at == now + 7 * 24 * 3600);

    assert(device_auth_state_reapproval(&settings));
    assert(settings.authorization_state == CLOUD_AUTH_REAPPROVAL_REQUIRED);
    assert(settings.token_state == CLOUD_TOKEN_STATE_REJECTED);
    assert(!settings.event_access[0] && !settings.refresh_token[0]);
    assert(!settings.grant_expires_at && !settings.refresh_in_progress);

    settings.refresh_in_progress = 1;
    strcpy(settings.device_code, "pending");
    device_auth_state_clear_pending(&settings);
    assert(!settings.device_code[0] && !settings.device_code_expires_at);
}

int main(void)
{
    url_checks();
    migration_checks();
    token_checks();
    device_response_checks();
    state_checks();
    puts("cloud settings, device authorization, and token tests passed");
    return 0;
}

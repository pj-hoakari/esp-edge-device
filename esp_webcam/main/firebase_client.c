#include "firebase_client.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "firebase";

#define HTTP_URL_MAX 768
#define HTTP_BODY_MAX 2048
#define HTTP_ERROR_MAX 4096
#define HTTP_RESPONSE_MAX 8192

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool json_string(const cJSON *root, const char *name, char *out, size_t size)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0] ||
        strlen(item->valuestring) >= size) return false;
    memcpy(out, item->valuestring, strlen(item->valuestring) + 1);
    return true;
}

static void log_error(const char *what, const char *body, int status)
{
    const char *message = NULL;
    cJSON *root = body && body[0] ? cJSON_Parse(body) : NULL;
    if (root) {
        cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
        cJSON *item = error ? cJSON_GetObjectItemCaseSensitive(error, "message") : NULL;
        if (!item) item = cJSON_GetObjectItemCaseSensitive(root, "message");
        if (cJSON_IsString(item)) message = item->valuestring;
    }
    if (message) ESP_LOGW(TAG, "%s failed: HTTP %d, %s", what, status, message);
    else ESP_LOGW(TAG, "%s failed: HTTP %d", what, status);
    cJSON_Delete(root);
}

/* Small blocking request. Signaling JSON is bounded; the caller owns the
 * response buffer. Returns the body length or -1. */
static int http_request(const char *url, esp_http_client_method_t method,
                        const char *auth, const char *content_type,
                        const char *body, char *out, size_t out_size, int *status,
                        bool connect_protocol)
{
    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 10000,
        .keep_alive_enable = false,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return -1;

    esp_http_client_set_method(client, method);
    if (auth) esp_http_client_set_header(client, "Authorization", auth);
    if (content_type) esp_http_client_set_header(client, "Content-Type", content_type);
    if (connect_protocol) {
        esp_http_client_set_header(client, "Connect-Protocol-Version", "1");
        esp_http_client_set_header(client, "Accept", "application/json");
    }

    int body_len = body ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(client, body_len);
    if (err == ESP_OK && body &&
        esp_http_client_write(client, body, body_len) != body_len) err = ESP_FAIL;
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
    return err == ESP_OK ? total : -1;
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

bool firebase_issue_custom_token(const char *signaling_url, const char *event_access,
                                 char *custom_token, size_t size)
{
    char url[HTTP_URL_MAX];
    if (snprintf(url, sizeof(url), "%s/tolo.edgebridge.v1.SignalingService/IssueFirestoreToken",
                 signaling_url) >= (int)sizeof(url)) return false;

    char *out = calloc(1, HTTP_ERROR_MAX);
    if (!out) return false;
    int status = 0;
    int length = http_request(url, HTTP_METHOD_POST, event_access, "application/json", "{}",
                              out, HTTP_ERROR_MAX, &status, true);
    bool ok = false;
    if (length >= 0 && status == 200) {
        cJSON *root = cJSON_Parse(out);
        if (root) {
            ok = json_string(root, "firebaseCustomToken", custom_token, size);
            cJSON_Delete(root);
        }
    }
    if (!ok) log_error("IssueFirestoreToken", out, status);
    wipe(out, HTTP_ERROR_MAX);
    free(out);
    return ok;
}

bool firebase_sign_in(const cloud_settings_t *settings, const char *custom_token,
                      firebase_session_t *session)
{
    char url[HTTP_URL_MAX];
    if (settings->auth_emulator_host[0]) {
        snprintf(url, sizeof(url),
                 "http://%s/identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=%s",
                 settings->auth_emulator_host, settings->firebase_api_key);
    } else {
        snprintf(url, sizeof(url),
                 "https://identitytoolkit.googleapis.com/v1/accounts:signInWithCustomToken?key=%s",
                 settings->firebase_api_key);
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "token", custom_token);
    cJSON_AddBoolToObject(root, "returnSecureToken", true);
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;

    char *out = calloc(1, HTTP_RESPONSE_MAX);
    if (!out) { free(body); return false; }
    int status = 0;
    int length = http_request(url, HTTP_METHOD_POST, NULL, "application/json", body,
                              out, HTTP_RESPONSE_MAX, &status, false);
    bool ok = false;
    if (length >= 0 && status == 200) {
        cJSON *reply = cJSON_Parse(out);
        if (reply) {
            char expires[16] = {0};
            ok = json_string(reply, "idToken", session->id_token, sizeof(session->id_token)) &&
                json_string(reply, "refreshToken", session->refresh_token, sizeof(session->refresh_token)) &&
                json_string(reply, "expiresIn", expires, sizeof(expires));
            if (ok) {
                session->id_token_expires_at = (int64_t)time(NULL) + atoi(expires);
            }
            cJSON_Delete(reply);
        }
    }
    if (!ok) {
        log_error("Firebase sign-in", out, status);
        session->id_token[0] = 0;
        session->refresh_token[0] = 0;
        session->id_token_expires_at = 0;
    }
    wipe(out, HTTP_RESPONSE_MAX);
    free(out);
    wipe(body, strlen(body));
    free(body);
    return ok;
}

bool firebase_refresh(const cloud_settings_t *settings, firebase_session_t *session)
{
    if (!session->refresh_token[0]) return false;
    char url[HTTP_URL_MAX];
    if (settings->auth_emulator_host[0]) {
        snprintf(url, sizeof(url), "http://%s/securetoken.googleapis.com/v1/token?key=%s",
                 settings->auth_emulator_host, settings->firebase_api_key);
    } else {
        snprintf(url, sizeof(url), "https://securetoken.googleapis.com/v1/token?key=%s",
                 settings->firebase_api_key);
    }

    char body[HTTP_BODY_MAX];
    body[0] = 0;
    if (!form_append(body, sizeof(body), "grant_type", "refresh_token") ||
        !form_append(body, sizeof(body), "refresh_token", session->refresh_token)) {
        return false;
    }

    char *out = calloc(1, HTTP_RESPONSE_MAX);
    if (!out) return false;
    int status = 0;
    int length = http_request(url, HTTP_METHOD_POST, NULL,
                              "application/x-www-form-urlencoded", body,
                              out, HTTP_RESPONSE_MAX, &status, false);
    bool ok = false;
    if (length >= 0 && status == 200) {
        cJSON *reply = cJSON_Parse(out);
        if (reply) {
            char expires[16] = {0};
            char *id_token = calloc(1, FIREBASE_TOKEN_SIZE);
            char *refresh_token = calloc(1, FIREBASE_REFRESH_SIZE);
            if (id_token && refresh_token) {
                ok = json_string(reply, "id_token", id_token, FIREBASE_TOKEN_SIZE) &&
                    json_string(reply, "expires_in", expires, sizeof(expires));
                if (ok) {
                    if (!json_string(reply, "refresh_token", refresh_token, FIREBASE_REFRESH_SIZE)) {
                        memcpy(refresh_token, session->refresh_token, FIREBASE_REFRESH_SIZE);
                    }
                    memcpy(session->id_token, id_token, sizeof(session->id_token));
                    memcpy(session->refresh_token, refresh_token, sizeof(session->refresh_token));
                    session->id_token_expires_at = (int64_t)time(NULL) + atoi(expires);
                }
            }
            if (id_token) { wipe(id_token, FIREBASE_TOKEN_SIZE); free(id_token); }
            if (refresh_token) { wipe(refresh_token, FIREBASE_REFRESH_SIZE); free(refresh_token); }
            cJSON_Delete(reply);
        }
    }
    if (!ok) {
        log_error("Firebase token refresh", out, status);
        session->id_token[0] = 0;
        session->refresh_token[0] = 0;
        session->id_token_expires_at = 0;
    }
    wipe(out, HTTP_RESPONSE_MAX);
    free(out);
    return ok;
}

static bool bearer_header(const firebase_session_t *session, char *out, size_t size)
{
    int n = snprintf(out, size, "Bearer %s", session->id_token);
    return n > 0 && (size_t)n < size;
}

bool firebase_firestore_list(const cloud_settings_t *settings,
                             const firebase_session_t *session,
                             const char *collection_path, const char *query,
                             char *out, size_t out_size, int *status)
{
    char url[HTTP_URL_MAX];
    int n;
    if (settings->firestore_emulator_host[0]) {
        n = snprintf(url, sizeof(url),
                     "http://%s/v1/projects/%s/databases/(default)/documents/%s%s",
                     settings->firestore_emulator_host, settings->firebase_project_id,
                     collection_path, query ? query : "");
    } else {
        n = snprintf(url, sizeof(url),
                     "https://firestore.googleapis.com/v1/projects/%s/databases/(default)/documents/%s%s",
                     settings->firebase_project_id, collection_path, query ? query : "");
    }
    if (n < 0 || (size_t)n >= sizeof(url)) return false;

    char *auth = calloc(1, FIREBASE_TOKEN_SIZE + 8);
    if (!auth) return false;
    bool ok = bearer_header(session, auth, FIREBASE_TOKEN_SIZE + 8);
    int length = ok ? http_request(url, HTTP_METHOD_GET, auth, NULL, NULL, out, out_size,
                                   status, false) : -1;
    wipe(auth, FIREBASE_TOKEN_SIZE + 8);
    free(auth);
    return length >= 0 && *status == 200;
}

bool firebase_firestore_batch_write(const cloud_settings_t *settings,
                                    const firebase_session_t *session,
                                    const char *body, int *status)
{
    char url[HTTP_URL_MAX];
    int n;
    if (settings->firestore_emulator_host[0]) {
        n = snprintf(url, sizeof(url),
                     "http://%s/v1/projects/%s/databases/(default)/documents:batchWrite",
                     settings->firestore_emulator_host, settings->firebase_project_id);
    } else {
        n = snprintf(url, sizeof(url),
                     "https://firestore.googleapis.com/v1/projects/%s/databases/(default)/documents:batchWrite",
                     settings->firebase_project_id);
    }
    if (n < 0 || (size_t)n >= sizeof(url)) return false;

    char *auth = calloc(1, FIREBASE_TOKEN_SIZE + 8);
    if (!auth) return false;
    char *out = calloc(1, HTTP_ERROR_MAX);
    if (!out) { free(auth); return false; }
    bool ok = bearer_header(session, auth, FIREBASE_TOKEN_SIZE + 8);
    int length = ok ? http_request(url, HTTP_METHOD_POST, auth, "application/json", body,
                                   out, HTTP_ERROR_MAX, status, false) : -1;
    ok = length >= 0 && *status == 200;
    if (!ok) log_error("Firestore batchWrite", out, *status);
    wipe(auth, FIREBASE_TOKEN_SIZE + 8);
    free(auth);
    wipe(out, HTTP_ERROR_MAX);
    free(out);
    return ok;
}

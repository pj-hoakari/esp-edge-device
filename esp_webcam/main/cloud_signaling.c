#include "cloud_signaling.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cloud_auth_logic.h"
#include "cloud_settings.h"
#include "device_auth.h"
#include "firebase_client.h"
#include "network_settings.h"
#include "webrtc.h"

static const char *TAG = "cloud";

#define SESSION_ID_SIZE 64
#define CREATED_AT_SIZE 40
#define CLOUD_STACK_SIZE 20480
#define CLOUD_HTTP_MAX (32 * 1024)
#define SESSION_POLL_MS 1000
#define SIGNAL_POLL_MS 300
#define ANSWER_POLL_MS 100
#define SESSION_GIVE_UP_MS 30000

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static const char *json_string(const cJSON *root, const char *name)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

static const cJSON *fs_fields(const cJSON *document)
{
    return cJSON_GetObjectItemCaseSensitive(document, "fields");
}

static const char *fs_string(const cJSON *fields, const char *name)
{
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(fields, name);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "stringValue");
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static const cJSON *fs_map_fields(const cJSON *fields, const char *name)
{
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(fields, name);
    const cJSON *map = cJSON_GetObjectItemCaseSensitive(entry, "mapValue");
    return cJSON_GetObjectItemCaseSensitive(map, "fields");
}

/* Firestore returns server timestamps as timestampValue (RFC3339 string),
 * not stringValue. createdAt is written with a server timestamp. */
static const char *fs_timestamp(const cJSON *fields, const char *name)
{
    const cJSON *entry = cJSON_GetObjectItemCaseSensitive(fields, name);
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(entry, "timestampValue");
    return cJSON_IsString(value) ? value->valuestring : NULL;
}

static bool document_id(const char *name, char *out, size_t size)
{
    if (!name) return false;
    const char *slash = strrchr(name, '/');
    const char *id = slash ? slash + 1 : name;
    if (!id[0] || strlen(id) >= size) return false;
    memcpy(out, id, strlen(id) + 1);
    return true;
}

static bool find_new_session(const char *body, const char *handled_created,
                             char *session, size_t session_size,
                             char *created, size_t created_size)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    bool found = false;
    char best_created[CREATED_AT_SIZE] = {0};
    const cJSON *documents = cJSON_GetObjectItemCaseSensitive(root, "documents");
    const cJSON *document = NULL;
    cJSON_ArrayForEach(document, documents) {
        const cJSON *fields = fs_fields(document);
        const char *status = fs_string(fields, "status");
        const char *created_at = fs_timestamp(fields, "createdAt");
        if (!status || strcmp(status, "requested") || !created_at) continue;
        if (handled_created && handled_created[0] && strcmp(created_at, handled_created) <= 0) continue;
        if (best_created[0] && strcmp(created_at, best_created) <= 0) continue;
        char id[SESSION_ID_SIZE];
        if (!document_id(json_string(document, "name"), id, sizeof(id))) continue;
        if (strlen(id) >= session_size) continue;
        memcpy(session, id, strlen(id) + 1);
        snprintf(created, created_size, "%s", created_at);
        snprintf(best_created, sizeof(best_created), "%s", created_at);
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

static bool find_offer(const char *body, char *sdp, size_t sdp_size)
{
    cJSON *root = cJSON_Parse(body);
    if (!root) return false;
    bool found = false;
    char best_created[CREATED_AT_SIZE] = {0};
    const cJSON *documents = cJSON_GetObjectItemCaseSensitive(root, "documents");
    const cJSON *document = NULL;
    cJSON_ArrayForEach(document, documents) {
        const cJSON *fields = fs_fields(document);
        const char *from = fs_string(fields, "from");
        if (!from || strcmp(from, "console")) continue;
        const char *created_at = fs_timestamp(fields, "createdAt");
        if (!created_at) continue;

        const cJSON *payload = fs_map_fields(fields, "payload");
        const char *kind = fs_string(payload, "kind");
        if (!kind || strcmp(kind, "description")) continue;
        const cJSON *description = fs_map_fields(payload, "description");
        const char *type = fs_string(description, "type");
        const char *sdp_value = fs_string(description, "sdp");
        if (!type || strcmp(type, "offer") || !sdp_value) continue;
        if (strlen(sdp_value) >= sdp_size) continue;
        if (best_created[0] && strcmp(created_at, best_created) <= 0) continue;
        memcpy(sdp, sdp_value, strlen(sdp_value) + 1);
        snprintf(best_created, sizeof(best_created), "%s", created_at);
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

static void fs_add_string(cJSON *fields, const char *name, const char *value)
{
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "stringValue", value);
    cJSON_AddItemToObject(fields, name, entry);
}

static cJSON *fs_add_map(cJSON *fields, const char *name)
{
    cJSON *entry = cJSON_CreateObject();
    cJSON *map = cJSON_AddObjectToObject(entry, "mapValue");
    cJSON *inner = cJSON_AddObjectToObject(map, "fields");
    cJSON_AddItemToObject(fields, name, entry);
    return inner;
}

static bool issue_custom_token(const cloud_settings_t *settings, char *custom, int *status)
{
    bool ok = firebase_issue_custom_token(network_settings_active()->signaling_url,
                                          settings->event_access, custom,
                                          FIREBASE_TOKEN_SIZE, status);
    if (!ok && cloud_firestore_auth_failed(*status)) {
        ESP_LOGW(TAG, "event_access rejected; refreshing device authorization");
        if (device_auth_force_refresh())
            ok = firebase_issue_custom_token(network_settings_active()->signaling_url,
                                             settings->event_access, custom,
                                             FIREBASE_TOKEN_SIZE, status);
    }
    return ok;
}

static bool sign_in_with_custom_token(const cloud_settings_t *settings,
                                      firebase_session_t *session)
{
    char *custom = calloc(1, FIREBASE_TOKEN_SIZE);
    if (!custom) return false;
    int status = 0;
    bool ok = issue_custom_token(settings, custom, &status) &&
        firebase_sign_in(settings, custom, session);
    wipe(custom, FIREBASE_TOKEN_SIZE);
    free(custom);
    if (!ok) ESP_LOGW(TAG, "Firebase sign-in failed; will retry");
    return ok;
}

static bool ensure_session(const cloud_settings_t *settings, firebase_session_t *session)
{
    if (!device_auth_ensure_access()) return false;

    cloud_session_action_t action =
        cloud_session_next_action(session, settings, (int64_t)time(NULL));
    if (action == CLOUD_SESSION_USE) return true;
    if (action == CLOUD_SESSION_REFRESH && firebase_refresh(settings, session)) {
        ESP_LOGI(TAG, "Firebase ID token refreshed");
        return true;
    }
    if (action == CLOUD_SESSION_REISSUE)
        ESP_LOGI(TAG, "reissuing Firebase token for the current event_access");
    return sign_in_with_custom_token(settings, session);
}

static bool recover_firestore_session(const cloud_settings_t *settings,
                                      firebase_session_t *session)
{
    ESP_LOGW(TAG, "Firestore rejected the session; reissuing Firebase token");
    session->id_token[0] = 0;
    return ensure_session(settings, session);
}

static bool write_answer(const cloud_settings_t *settings,
                         firebase_session_t *session,
                         const char *session_id, const char *sdp, int *status)
{
    char signal_id[33];
    snprintf(signal_id, sizeof(signal_id), "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
             esp_random(), esp_random(), esp_random());
    char document[384];
    snprintf(document, sizeof(document),
             "tenants/%s/events/%s/edgeDevices/%s/sessions/%s/signals/%s",
             settings->tenant_id, settings->event_id, settings->edge_device_id,
             session_id, signal_id);

    cJSON *root = cJSON_CreateObject();
    cJSON *writes = cJSON_AddArrayToObject(root, "writes");
    cJSON *write = cJSON_CreateObject();
    cJSON_AddItemToArray(writes, write);
    cJSON *update = cJSON_AddObjectToObject(write, "update");
    char name[512];
    snprintf(name, sizeof(name), "projects/%s/databases/(default)/documents/%s",
             settings->firebase_project_id, document);
    cJSON_AddStringToObject(update, "name", name);
    cJSON *fields = cJSON_AddObjectToObject(update, "fields");
    fs_add_string(fields, "from", "edge");
    cJSON *payload = fs_add_map(fields, "payload");
    fs_add_string(payload, "kind", "description");
    cJSON *description = fs_add_map(payload, "description");
    fs_add_string(description, "type", "answer");
    fs_add_string(description, "sdp", sdp);
    cJSON *current = cJSON_AddObjectToObject(write, "currentDocument");
    cJSON_AddBoolToObject(current, "exists", false);
    cJSON *transforms = cJSON_AddArrayToObject(write, "updateTransforms");
    cJSON *transform = cJSON_CreateObject();
    cJSON_AddStringToObject(transform, "fieldPath", "createdAt");
    cJSON_AddStringToObject(transform, "setToServerValue", "REQUEST_TIME");
    cJSON_AddItemToArray(transforms, transform);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return false;
    bool ok = firebase_firestore_batch_write(settings, session, body, status);
    if (!ok && cloud_firestore_auth_failed(*status) &&
        recover_firestore_session(settings, session)) {
        ok = firebase_firestore_batch_write(settings, session, body, status);
    }
    wipe(body, strlen(body));
    free(body);
    return ok;
}

static bool firestore_list(const cloud_settings_t *settings, firebase_session_t *session,
                           const char *path, const char *query, char *out,
                           size_t out_size, int *status)
{
    if (firebase_firestore_list(settings, session, path, query, out, out_size, status))
        return true;
    if (!cloud_firestore_auth_failed(*status) ||
        !recover_firestore_session(settings, session)) return false;
    return firebase_firestore_list(settings, session, path, query, out, out_size, status);
}

static void start_sntp(void)
{
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
}

static void cloud_task(void *arg)
{
    (void)arg;
    const cloud_settings_t *settings = cloud_settings_active();
    firebase_session_t session = {0};
    char *http = heap_caps_malloc(CLOUD_HTTP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *offer_sdp = heap_caps_malloc(CLOUD_HTTP_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!http || !offer_sdp) {
        ESP_LOGE(TAG, "no PSRAM buffer for signaling");
        if (http) free(http);
        if (offer_sdp) free(offer_sdp);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "cloud task started");
    char target[SESSION_ID_SIZE] = {0};
    char target_created[CREATED_AT_SIZE] = {0};
    char handled_created[CREATED_AT_SIZE] = {0};
    bool offer_submitted = false;
    bool answer_written = false;
    bool was_active = false;
    char *write_sdp = NULL;
    char write_session[SESSION_ID_SIZE] = {0};
    int64_t target_since = 0;
    int64_t next_session_poll = 0;
    int64_t next_signal_poll = 0;
    int64_t next_answer_poll = 0;

    start_sntp();
    bool reported_reapproval = false;

    for (;;) {
        if (!device_auth_ensure_access()) {
            if (settings->authorization_state == CLOUD_AUTH_REAPPROVAL_REQUIRED) {
                if (!reported_reapproval) {
                    printf("{\"type\":\"error\",\"error\":\"authorization_required\"}\n");
                    fflush(stdout);
                    ESP_LOGE(TAG, "reapproval required; signaling stopped");
                    reported_reapproval = true;
                }
            } else {
                reported_reapproval = false;
            }
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        reported_reapproval = false;
        if (!ensure_session(settings, &session)) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        char base[256];
        int base_length = snprintf(base, sizeof(base), "tenants/%s/events/%s/edgeDevices/%s",
                                   settings->tenant_id, settings->event_id,
                                   settings->edge_device_id);
        if (base_length < 0 || (size_t)base_length >= sizeof(base) ||
            !settings->tenant_id[0] || !settings->event_id[0]) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        int64_t now = esp_timer_get_time() / 1000;
        bool active = webrtc_video_active();
        if (was_active && !active) {
            ESP_LOGW(TAG, "peer closed; waiting for a new session");
            if (target_created[0]) {
                memcpy(handled_created, target_created, sizeof(handled_created));
            }
            target[0] = 0;
            offer_submitted = false;
            answer_written = false;
            if (write_sdp) { wipe(write_sdp, strlen(write_sdp)); free(write_sdp); write_sdp = NULL; }
        }
        was_active = active;

        if (!target[0] && now >= next_session_poll) {
            char path[320];
            snprintf(path, sizeof(path), "%s/sessions", base);
            int status = 0;
            if (firestore_list(settings, &session, path, "?pageSize=10",
                               http, CLOUD_HTTP_MAX, &status)) {
                char id[SESSION_ID_SIZE];
                char created[CREATED_AT_SIZE];
                if (find_new_session(http, handled_created, id, sizeof(id),
                                     created, sizeof(created))) {
                    memcpy(target, id, sizeof(target));
                    memcpy(target_created, created, sizeof(target_created));
                    target_since = now;
                    offer_submitted = false;
                    answer_written = false;
                    ESP_LOGI(TAG, "session %s requested", target);
                }
            }
            next_session_poll = now + SESSION_POLL_MS;
        }

        if (target[0] && !offer_submitted && !active && now >= next_signal_poll) {
            char path[384];
            snprintf(path, sizeof(path), "%s/sessions/%s/signals", base, target);
            int status = 0;
            if (firestore_list(settings, &session, path, "?pageSize=20",
                               http, CLOUD_HTTP_MAX, &status)) {
                offer_sdp[0] = 0;
                if (find_offer(http, offer_sdp, CLOUD_HTTP_MAX)) {
                    if (webrtc_submit_offer(offer_sdp, target)) {
                        offer_submitted = true;
                        ESP_LOGI(TAG, "offer %s submitted (%u bytes)", target,
                                 (unsigned)strlen(offer_sdp));
                    }
                }
                wipe(offer_sdp, CLOUD_HTTP_MAX);
            }
            next_signal_poll = now + SIGNAL_POLL_MS;
        }

        if (target[0] && offer_submitted && !answer_written && !write_sdp && now >= next_answer_poll) {
            if (webrtc_take_answer(&write_sdp, write_session, sizeof(write_session))) {
                ESP_LOGI(TAG, "answer %s received from peer", write_session);
            }
            next_answer_poll = now + ANSWER_POLL_MS;
        }

        if (target[0] && write_sdp && !answer_written && now >= next_answer_poll) {
            if (!strcmp(write_session, target)) {
                int status = 0;
                if (write_answer(settings, &session, target, write_sdp, &status)) {
                    answer_written = true;
                    ESP_LOGI(TAG, "answer %s written (%u bytes)", target,
                             (unsigned)strlen(write_sdp));
                    wipe(write_sdp, strlen(write_sdp));
                    free(write_sdp);
                    write_sdp = NULL;
                }
            } else {
                wipe(write_sdp, strlen(write_sdp));
                free(write_sdp);
                write_sdp = NULL;
            }
            next_answer_poll = now + ANSWER_POLL_MS;
        }

        if (target[0] && !active && now - target_since > SESSION_GIVE_UP_MS) {
            ESP_LOGW(TAG, "session %s did not connect; ignoring it", target);
            if (target_created[0]) {
                memcpy(handled_created, target_created, sizeof(handled_created));
            }
            target[0] = 0;
            offer_submitted = false;
            answer_written = false;
            if (write_sdp) { wipe(write_sdp, strlen(write_sdp)); free(write_sdp); write_sdp = NULL; }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

void cloud_signaling_start(void)
{
    const cloud_settings_t *settings = cloud_settings_active();
    if (!cloud_settings_valid(settings) || !settings->edge_device_id[0] ||
        !network_settings_active()->signaling_url[0]) {
        ESP_LOGW(TAG, "cloud signaling disabled (run cloud/signaling, then reboot)");
        return;
    }
    ESP_LOGI(TAG, "edge=%s idp=%s bridge=%s", settings->edge_device_id,
             settings->idp_url, network_settings_active()->signaling_url);
    if (xTaskCreatePinnedToCore(cloud_task, "cloud", CLOUD_STACK_SIZE, NULL, 4, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "failed to start cloud signaling task");
    }
}

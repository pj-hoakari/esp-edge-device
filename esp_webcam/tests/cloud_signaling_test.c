#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../main/cloud_signaling.c"

#define EVENT_ACCESS_SECRET "event-access-secret-8c1d"
#define CUSTOM_TOKEN_SECRET "custom-token-secret-3b2e"
#define OLD_ID_TOKEN_SECRET "old-id-token-secret-6a4f"
#define SIGNIN_ID_TOKEN_SECRET "signin-id-token-secret-1d7c"
#define REFRESHED_ID_TOKEN_SECRET "refreshed-id-token-secret-5e9a"
#define REFRESH_TOKEN_SECRET "refresh-token-secret-2f8b"

typedef enum {
    CALL_ENSURE_ACCESS,
    CALL_FORCE_REFRESH,
    CALL_ISSUE_TOKEN,
    CALL_SIGN_IN,
    CALL_REFRESH,
    CALL_FIRESTORE_LIST,
    CALL_FIRESTORE_BATCH,
} fake_call_t;

#define CALL_MAX 32
#define LIST_RECORD_MAX 2
#define BATCH_BODY_MAX 8192

static fake_call_t recorded_calls[CALL_MAX];
static size_t recorded_call_count;
static char log_text[16384];
static size_t log_length;

static bool ensure_access_result;
static bool force_refresh_result;
static int issue_status;
static int issue_count;
static char issue_event_access[CLOUD_TOKEN_SIZE];
static int sign_in_count;
static char sign_in_custom_token[FIREBASE_TOKEN_SIZE];
static int refresh_count;
static bool refresh_result;
static int list_failures;
static int list_fail_status;
static int list_count;
static char list_paths[LIST_RECORD_MAX][320];
static char list_queries[LIST_RECORD_MAX][64];
static char list_id_tokens[LIST_RECORD_MAX][FIREBASE_TOKEN_SIZE];
static int batch_failures;
static int batch_fail_status;
static int batch_count;
static char batch_bodies[LIST_RECORD_MAX][BATCH_BODY_MAX];
static char batch_id_tokens[LIST_RECORD_MAX][FIREBASE_TOKEN_SIZE];
static network_settings_t fake_network;
static cloud_settings_t fake_active_settings;

static void record_call(fake_call_t call)
{
    assert(recorded_call_count < CALL_MAX);
    recorded_calls[recorded_call_count++] = call;
}

static void reset_fakes(void)
{
    memset(recorded_calls, 0, sizeof(recorded_calls));
    recorded_call_count = 0;
    log_length = 0;
    log_text[0] = 0;
    ensure_access_result = true;
    force_refresh_result = true;
    issue_status = 200;
    issue_count = 0;
    issue_event_access[0] = 0;
    sign_in_count = 0;
    sign_in_custom_token[0] = 0;
    refresh_count = 0;
    refresh_result = true;
    list_failures = 0;
    list_fail_status = 0;
    list_count = 0;
    memset(list_paths, 0, sizeof(list_paths));
    memset(list_queries, 0, sizeof(list_queries));
    memset(list_id_tokens, 0, sizeof(list_id_tokens));
    batch_failures = 0;
    batch_fail_status = 0;
    batch_count = 0;
    memset(batch_bodies, 0, sizeof(batch_bodies));
    memset(batch_id_tokens, 0, sizeof(batch_id_tokens));
    memset(&fake_network, 0, sizeof(fake_network));
    snprintf(fake_network.signaling_url, sizeof(fake_network.signaling_url),
             "%s", "http://bridge.test");
    memset(&fake_active_settings, 0, sizeof(fake_active_settings));
}

static void assert_call(size_t index, fake_call_t expected)
{
    assert(index < recorded_call_count);
    assert(recorded_calls[index] == expected);
}

static void assert_logs_hide_tokens(void)
{
    const char *const secrets[] = {
        EVENT_ACCESS_SECRET, CUSTOM_TOKEN_SECRET, OLD_ID_TOKEN_SECRET,
        SIGNIN_ID_TOKEN_SECRET, REFRESHED_ID_TOKEN_SECRET, REFRESH_TOKEN_SECRET,
    };
    for (size_t i = 0; i < sizeof(secrets) / sizeof(secrets[0]); i++)
        assert(!strstr(log_text, secrets[i]));
}

void cloud_test_log(const char *level, const char *tag, const char *fmt, ...)
{
    int used = snprintf(log_text + log_length, sizeof(log_text) - log_length,
                        "[%s] %s: ", level, tag);
    if (used > 0) log_length += (size_t)used;
    if (log_length >= sizeof(log_text)) log_length = sizeof(log_text) - 1;
    log_text[log_length] = 0;
    va_list args;
    va_start(args, fmt);
    int written = vsnprintf(log_text + log_length, sizeof(log_text) - log_length, fmt, args);
    va_end(args);
    if (written > 0) log_length += (size_t)written;
    if (log_length >= sizeof(log_text)) log_length = sizeof(log_text) - 1;
    log_text[log_length] = 0;
    if (log_length + 1 < sizeof(log_text)) {
        log_text[log_length++] = '\n';
        log_text[log_length] = 0;
    }
}

bool firebase_issue_custom_token(const char *signaling_url, const char *event_access,
                                 char *custom_token, size_t size, int *status)
{
    (void)signaling_url;
    record_call(CALL_ISSUE_TOKEN);
    issue_count++;
    snprintf(issue_event_access, sizeof(issue_event_access), "%s", event_access);
    snprintf(custom_token, size, "%s", CUSTOM_TOKEN_SECRET);
    if (status) *status = issue_status;
    return issue_status == 200;
}

bool firebase_sign_in(const cloud_settings_t *settings, const char *custom_token,
                      firebase_session_t *session)
{
    record_call(CALL_SIGN_IN);
    sign_in_count++;
    snprintf(sign_in_custom_token, sizeof(sign_in_custom_token), "%s", custom_token);
    snprintf(session->id_token, sizeof(session->id_token), "%s", SIGNIN_ID_TOKEN_SECRET);
    snprintf(session->refresh_token, sizeof(session->refresh_token), "%s",
             REFRESH_TOKEN_SECRET);
    session->id_token_expires_at = (int64_t)time(NULL) + 3600;
    session->event_access_obtained_at = settings->event_access_obtained_at;
    session->event_access_expires_at = settings->event_access_expires_at;
    return true;
}

bool firebase_refresh(const cloud_settings_t *settings, firebase_session_t *session)
{
    (void)settings;
    record_call(CALL_REFRESH);
    refresh_count++;
    if (!refresh_result) return false;
    snprintf(session->id_token, sizeof(session->id_token), "%s",
             REFRESHED_ID_TOKEN_SECRET);
    session->id_token_expires_at = (int64_t)time(NULL) + 3600;
    return true;
}

bool firebase_firestore_list(const cloud_settings_t *settings,
                             const firebase_session_t *session,
                             const char *collection_path, const char *query,
                             char *out, size_t out_size, int *status)
{
    (void)settings;
    record_call(CALL_FIRESTORE_LIST);
    size_t index = list_count < LIST_RECORD_MAX ? (size_t)list_count
                                                : LIST_RECORD_MAX - 1;
    list_count++;
    snprintf(list_paths[index], sizeof(list_paths[index]), "%s",
             collection_path ? collection_path : "");
    snprintf(list_queries[index], sizeof(list_queries[index]), "%s",
             query ? query : "");
    snprintf(list_id_tokens[index], sizeof(list_id_tokens[index]), "%s",
             session->id_token);
    if (list_failures > 0) {
        list_failures--;
        if (status) *status = list_fail_status;
        return false;
    }
    if (out && out_size) snprintf(out, out_size, "%s", "{\"documents\":[]}");
    if (status) *status = 200;
    return true;
}

bool firebase_firestore_batch_write(const cloud_settings_t *settings,
                                    const firebase_session_t *session,
                                    const char *body, int *status)
{
    (void)settings;
    record_call(CALL_FIRESTORE_BATCH);
    size_t index = batch_count < LIST_RECORD_MAX ? (size_t)batch_count
                                                 : LIST_RECORD_MAX - 1;
    batch_count++;
    snprintf(batch_bodies[index], sizeof(batch_bodies[index]), "%s", body);
    snprintf(batch_id_tokens[index], sizeof(batch_id_tokens[index]), "%s",
             session->id_token);
    if (batch_failures > 0) {
        batch_failures--;
        if (status) *status = batch_fail_status;
        return false;
    }
    if (status) *status = 200;
    return true;
}

bool device_auth_ensure_access(void)
{
    record_call(CALL_ENSURE_ACCESS);
    return ensure_access_result;
}

bool device_auth_force_refresh(void)
{
    record_call(CALL_FORCE_REFRESH);
    return force_refresh_result;
}

const network_settings_t *network_settings_active(void)
{
    return &fake_network;
}

const cloud_settings_t *cloud_settings_active(void)
{
    return &fake_active_settings;
}

bool cloud_settings_valid(const cloud_settings_t *settings)
{
    (void)settings;
    return true;
}

bool webrtc_video_active(void)
{
    return false;
}

bool webrtc_submit_offer(const char *sdp, const char *session_id)
{
    (void)sdp;
    (void)session_id;
    return false;
}

bool webrtc_take_answer(char **sdp, char *session_id, size_t size)
{
    (void)sdp;
    (void)session_id;
    (void)size;
    return false;
}

void *heap_caps_malloc(size_t size, unsigned caps)
{
    (void)caps;
    return malloc(size);
}

uint32_t esp_random(void)
{
    static uint32_t value = 0x10203040;
    return value += 0x01010101;
}

void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t mode)
{
    (void)mode;
}

void esp_sntp_setservername(unsigned index, const char *server)
{
    (void)index;
    (void)server;
}

void esp_sntp_init(void)
{
}

int64_t esp_timer_get_time(void)
{
    return 0;
}

void vTaskDelay(TickType_t ticks)
{
    (void)ticks;
}

void vTaskDelete(TaskHandle_t task)
{
    (void)task;
}

int xTaskCreatePinnedToCore(void (*task)(void *), const char *name, unsigned stack,
                            void *arg, unsigned priority, TaskHandle_t *handle,
                            int core)
{
    (void)task;
    (void)name;
    (void)stack;
    (void)arg;
    (void)priority;
    (void)handle;
    (void)core;
    return pdPASS;
}

static void make_settings(cloud_settings_t *settings)
{
    memset(settings, 0, sizeof(*settings));
    settings->version = CLOUD_SETTINGS_VERSION;
    snprintf(settings->event_access, sizeof(settings->event_access), "%s",
             EVENT_ACCESS_SECRET);
    settings->event_access_obtained_at = 1000;
    settings->event_access_expires_at = 2000;
    snprintf(settings->tenant_id, sizeof(settings->tenant_id), "%s", "tenant-1");
    snprintf(settings->event_id, sizeof(settings->event_id), "%s", "event-1");
    snprintf(settings->edge_device_id, sizeof(settings->edge_device_id), "%s",
             "0123456789abcdef");
    snprintf(settings->firebase_project_id, sizeof(settings->firebase_project_id),
             "%s", "project-1");
    snprintf(settings->firebase_api_key, sizeof(settings->firebase_api_key),
             "%s", "api-key-1");
}

static void make_session(const cloud_settings_t *settings, firebase_session_t *session,
                         int64_t id_token_expires_at)
{
    memset(session, 0, sizeof(*session));
    snprintf(session->id_token, sizeof(session->id_token), "%s", OLD_ID_TOKEN_SECRET);
    snprintf(session->refresh_token, sizeof(session->refresh_token), "%s",
             REFRESH_TOKEN_SECRET);
    session->id_token_expires_at = id_token_expires_at;
    session->event_access_obtained_at = settings->event_access_obtained_at;
    session->event_access_expires_at = settings->event_access_expires_at;
}

static void rotated_event_access_reissues_without_refresh(void)
{
    reset_fakes();
    cloud_settings_t settings;
    make_settings(&settings);
    firebase_session_t session;
    make_session(&settings, &session, (int64_t)time(NULL) + 30);
    session.event_access_obtained_at = 900;
    session.event_access_expires_at = 1900;

    assert(ensure_session(&settings, &session));
    assert(recorded_call_count == 3);
    assert_call(0, CALL_ENSURE_ACCESS);
    assert_call(1, CALL_ISSUE_TOKEN);
    assert_call(2, CALL_SIGN_IN);
    assert(refresh_count == 0);
    assert(issue_count == 1);
    assert(sign_in_count == 1);
    assert(!strcmp(issue_event_access, EVENT_ACCESS_SECRET));
    assert(!strcmp(sign_in_custom_token, CUSTOM_TOKEN_SECRET));
    assert(!strcmp(session.id_token, SIGNIN_ID_TOKEN_SECRET));
    assert(session.event_access_obtained_at == settings.event_access_obtained_at);
    assert(session.event_access_expires_at == settings.event_access_expires_at);
    assert_logs_hide_tokens();
}

static void unchanged_near_expiry_uses_refresh(void)
{
    reset_fakes();
    cloud_settings_t settings;
    make_settings(&settings);
    firebase_session_t session;
    make_session(&settings, &session, (int64_t)time(NULL) + 30);

    assert(ensure_session(&settings, &session));
    assert(recorded_call_count == 2);
    assert_call(0, CALL_ENSURE_ACCESS);
    assert_call(1, CALL_REFRESH);
    assert(refresh_count == 1);
    assert(issue_count == 0);
    assert(sign_in_count == 0);
    assert(!strcmp(session.id_token, REFRESHED_ID_TOKEN_SECRET));
    assert_logs_hide_tokens();
}

static void unchanged_valid_token_is_used(void)
{
    reset_fakes();
    cloud_settings_t settings;
    make_settings(&settings);
    firebase_session_t session;
    make_session(&settings, &session, (int64_t)time(NULL) + 3600);

    assert(ensure_session(&settings, &session));
    assert(recorded_call_count == 1);
    assert_call(0, CALL_ENSURE_ACCESS);
    assert(refresh_count == 0);
    assert(issue_count == 0);
    assert(sign_in_count == 0);
    assert_logs_hide_tokens();
}

static void firestore_auth_failure_reissues_and_retries(int fail_status)
{
    reset_fakes();
    cloud_settings_t settings;
    make_settings(&settings);
    firebase_session_t session;
    make_session(&settings, &session, (int64_t)time(NULL) + 3600);
    list_failures = 1;
    list_fail_status = fail_status;
    const char *path = "tenants/tenant-1/events/event-1/edgeDevices/0123456789abcdef/sessions";
    const char *query = "?pageSize=10";
    char body[512] = {0};
    int status = 0;

    assert(firestore_list(&settings, &session, path, query, body, sizeof(body), &status));
    assert(status == 200);
    assert(recorded_call_count == 5);
    assert_call(0, CALL_FIRESTORE_LIST);
    assert_call(1, CALL_ENSURE_ACCESS);
    assert_call(2, CALL_ISSUE_TOKEN);
    assert_call(3, CALL_SIGN_IN);
    assert_call(4, CALL_FIRESTORE_LIST);
    assert(list_count == 2);
    assert(refresh_count == 0);
    assert(issue_count == 1);
    assert(sign_in_count == 1);
    for (int i = 0; i < 2; i++) {
        assert(!strcmp(list_paths[i], path));
        assert(!strcmp(list_queries[i], query));
    }
    assert(!strcmp(list_id_tokens[0], OLD_ID_TOKEN_SECRET));
    assert(!strcmp(list_id_tokens[1], SIGNIN_ID_TOKEN_SECRET));
    assert(strstr(body, "documents"));
    assert_logs_hide_tokens();
}

static void firestore_auth_failure_retries_only_once(void)
{
    reset_fakes();
    cloud_settings_t settings;
    make_settings(&settings);
    firebase_session_t session;
    make_session(&settings, &session, (int64_t)time(NULL) + 3600);
    list_failures = 2;
    list_fail_status = 401;
    char body[512] = {0};
    int status = 0;

    assert(!firestore_list(&settings, &session, "sessions", "?pageSize=10",
                           body, sizeof(body), &status));
    assert(status == 401);
    assert(recorded_call_count == 5);
    assert_call(0, CALL_FIRESTORE_LIST);
    assert_call(1, CALL_ENSURE_ACCESS);
    assert_call(2, CALL_ISSUE_TOKEN);
    assert_call(3, CALL_SIGN_IN);
    assert_call(4, CALL_FIRESTORE_LIST);
    assert(list_count == 2);
    assert(refresh_count == 0);
    assert(issue_count == 1);
    assert_logs_hide_tokens();
}

static void write_answer_retries_the_same_request(void)
{
    reset_fakes();
    cloud_settings_t settings;
    make_settings(&settings);
    firebase_session_t session;
    make_session(&settings, &session, (int64_t)time(NULL) + 3600);
    batch_failures = 1;
    batch_fail_status = 403;
    int status = 0;

    assert(write_answer(&settings, &session, "session-1", "v=0\r\nsdp", &status));
    assert(status == 200);
    assert(recorded_call_count == 5);
    assert_call(0, CALL_FIRESTORE_BATCH);
    assert_call(1, CALL_ENSURE_ACCESS);
    assert_call(2, CALL_ISSUE_TOKEN);
    assert_call(3, CALL_SIGN_IN);
    assert_call(4, CALL_FIRESTORE_BATCH);
    assert(batch_count == 2);
    assert(!strcmp(batch_bodies[0], batch_bodies[1]));
    assert(!strcmp(batch_id_tokens[0], OLD_ID_TOKEN_SECRET));
    assert(!strcmp(batch_id_tokens[1], SIGNIN_ID_TOKEN_SECRET));
    assert(refresh_count == 0);
    assert(issue_count == 1);
    assert_logs_hide_tokens();
}

int main(void)
{
    rotated_event_access_reissues_without_refresh();
    unchanged_near_expiry_uses_refresh();
    unchanged_valid_token_is_used();
    firestore_auth_failure_reissues_and_retries(401);
    firestore_auth_failure_reissues_and_retries(403);
    firestore_auth_failure_retries_only_once();
    write_answer_retries_the_same_request();
    puts("cloud signaling token reissue and Firestore retry tests passed");
    return 0;
}

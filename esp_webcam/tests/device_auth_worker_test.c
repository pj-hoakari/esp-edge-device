/* Run the real authorization HTTP/state/queue paths with dummy credentials. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "platform.h"
#include "device_auth.h"
#include "cloud_settings.h"
#include "firebase_client.h"
#include "peer_authorization.h"

static cloud_settings_t active;
static int store_calls, fail_on_store;
static int requests;
static const char *reply;
static int http_status;
static bool reply_read;
static char request_url[768];
struct mock_queue { unsigned char value[128]; unsigned size; bool full; };
QueueHandle_t xQueueCreate(unsigned length, unsigned size) {
    assert(length == 1 && size <= 128);
    QueueHandle_t q = calloc(1, sizeof(struct mock_queue));
    q->size = size; return q;
}
int xQueueSend(QueueHandle_t q, const void *item, unsigned ticks) {
    assert(ticks == 0);
    if (q->full) return 0;
    memcpy(q->value, item, q->size); q->full = true; return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *item, unsigned ticks) {
    assert(ticks == 0);
    if (!q->full) return 0;
    memcpy(item, q->value, q->size); q->full = false; return pdTRUE;
}
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return (void *)1; }
int xSemaphoreTakeRecursive(SemaphoreHandle_t s, unsigned ticks) { return pdTRUE; }
int xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return pdTRUE; }
int64_t esp_timer_get_time(void) { return 1000000; }
int esp_crt_bundle_attach(void *config) { return 0; }
const char *esp_err_to_name(int error) { return "mock"; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
    assert(c->disable_auto_redirect);
    requests++; reply_read = false;
    snprintf(request_url, sizeof(request_url), "%s", c->url); return (void *)1;
}
int esp_http_client_set_method(void *c, int method) { return 0; }
int esp_http_client_set_header(void *c, const char *key, const char *value) { return 0; }
int esp_http_client_open(void *c, int length) { return 0; }
int esp_http_client_write(void *c, const char *data, int length) { return length; }
int esp_http_client_fetch_headers(void *c) { return 0; }
int esp_http_client_read(void *c, char *out, int size) {
    if (reply_read) return 0;
    assert(strlen(reply) < (size_t)size); strcpy(out, reply); reply_read = true;
    return (int)strlen(reply);
}
int esp_http_client_get_status_code(void *c) { return http_status; }
int esp_http_client_close(void *c) { return 0; }
int esp_http_client_cleanup(void *c) { return 0; }
const cloud_settings_t *cloud_settings_active(void) { return &active; }
bool cloud_settings_store(const cloud_settings_t *next) {
    if (++store_calls == fail_on_store) return false;
    active = *next; return true;
}
void cloud_settings_activate(const cloud_settings_t *next) { active = *next; }

static void approved(void) {
    memset(&active, 0, sizeof(active));
    active.version = CLOUD_SETTINGS_VERSION;
    strcpy(active.idp_url, "https://idp.example");
    strcpy(active.event_access, "dummy-event-access");
    strcpy(active.refresh_token, "dummy-refresh-token");
    active.authorization_state = CLOUD_AUTH_APPROVED;
    peer_authorization_take_disconnect();
    peer_authorization_allow();
}

int auth_worker_test(void) {
    approved();
    active.authorization_state = CLOUD_AUTH_NONE;
    assert(!device_auth_request_start()); /* no worker yet */
    device_auth_worker_initialize();
    assert(device_auth_request_start());
    assert(!device_auth_request_start()); /* queue full */
    assert(requests == 0); /* UART must not run HTTP */
    reply = "{\"device_code\":\"dummy-device-code\",\"user_code\":\"ABCD-EFGH\","
            "\"verification_uri\":\"https://idp.example/device\",\"expires_in\":600,\"interval\":5}";
    http_status = 200;
    device_auth_process_requests();
    assert(requests == 1 && active.authorization_state == CLOUD_AUTH_PENDING);
    assert(strstr(request_url, "/oauth2/device_authorization"));
    device_auth_process_requests();
    assert(requests == 1); /* one dequeue, one user_code JSON line */

    approved();
    reply = "{\"error\":\"invalid_grant\"}"; http_status = 400;
    assert(!device_auth_force_refresh());
    assert(active.authorization_state == CLOUD_AUTH_REAPPROVAL_REQUIRED);
    assert(peer_authorization_blocked());
    assert(peer_authorization_take_disconnect());
    assert(!peer_authorization_take_disconnect());
    assert(peer_authorization_blocked()); /* consumption cannot resume video */

    approved();
    active.grant_expires_at = time(NULL) - 1;
    int before = requests;
    assert(!device_auth_ensure_access());
    assert(requests == before && peer_authorization_take_disconnect());
    assert(peer_authorization_blocked());

    approved();
    fail_on_store = store_calls + 2; /* marker succeeds, revocation save fails */
    assert(!device_auth_force_refresh());
    fail_on_store = 0;
    assert(active.authorization_state == CLOUD_AUTH_REAPPROVAL_REQUIRED);
    assert(peer_authorization_take_disconnect() && peer_authorization_blocked());

    approved();
    strcpy(active.idp_url, "http://idp.example");
    before = requests;
    assert(!device_auth_force_refresh());
#if defined(CONFIG_WEBCAM_ALLOW_INSECURE_HTTP) && CONFIG_WEBCAM_ALLOW_INSECURE_HTTP
    assert(requests == before + 1);
#else
    assert(requests == before); /* refresh token never reaches HTTP */
#endif
    firebase_session_t session = {0};
    strcpy(session.refresh_token, "dummy-firebase-refresh");
    strcpy(active.auth_emulator_host, "http://auth.example:9099");
    strcpy(active.firestore_emulator_host, "firestore.example:8080");
    char out[256]; int status;
    before = requests;
    firebase_refresh(&active, &session);
    firebase_sign_in(&active, "dummy-custom-token", &session);
    firebase_issue_custom_token("http://bridge.example", "dummy-access", out, sizeof(out), &status);
    firebase_firestore_list(&active, &session, "test", "", out, sizeof(out), &status);
    firebase_firestore_batch_write(&active, &session, "{}", &status);
#if defined(CONFIG_WEBCAM_ALLOW_INSECURE_HTTP) && CONFIG_WEBCAM_ALLOW_INSECURE_HTTP
    assert(requests == before + 5);
#else
    assert(requests == before);
#endif
    puts("device_auth_worker_test: ok");
    return 0;
}

#ifndef PEER_TASK_TEST
int main(void) { return auth_worker_test(); }
#endif

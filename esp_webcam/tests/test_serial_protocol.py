"""Run the machine-readable serial console on a PC with fake UART/NVS I/O."""
from pathlib import Path
import json
import subprocess
import tempfile

MOCK = r'''
#pragma once
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>
#include "network_settings.h"
#include "cloud_settings.h"
#include "device_auth.h"
#define CONFIG_WEBCAM_WIFI_SSID ""
#define CONFIG_WEBCAM_WIFI_PASSWORD ""
#define CONFIG_WEBCAM_SIGNALING_URL "http://192.168.137.1:8080"
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
#define CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT 1
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NVS_NOT_FOUND 1
#define ESP_ERR_NO_MEM 2
#define ESP_ERR_INVALID_ARG 3
#define ESP_ERR_NVS_INVALID_LENGTH 4
#define NVS_READONLY 0
#define NVS_READWRITE 1
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
#define ESP_ERROR_CHECK(x) assert((x) == ESP_OK)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
typedef int esp_err_t;
typedef int nvs_handle_t;
#include "wifi_auth.h"

static jmp_buf end_input;
static jmp_buf rebooted;
static const unsigned char *rx;
static unsigned char persisted[sizeof(network_settings_t)];
static size_t persisted_size;
static bool has_blob;
static int commits;
static cloud_settings_t active_cloud, saved_cloud;
static int cloud_saves, device_auth_starts;
static char last_idp[CLOUD_IDP_URL_SIZE];

static const char *esp_err_to_name(int err) { return "mock error"; }
static int nvs_open(const char *name, int mode, int *h) {
    assert(!strcmp(name, "network")); *h = 1;
    return mode == NVS_READONLY && !has_blob ? ESP_ERR_NVS_NOT_FOUND : ESP_OK;
}
static int nvs_get_blob(int h, const char *key, void *out, size_t *size) {
    assert(!strcmp(key, "settings"));
    if (!out) { *size = persisted_size; return ESP_OK; }
    if (*size < persisted_size) return ESP_ERR_NVS_INVALID_LENGTH;
    *size = persisted_size; memcpy(out, persisted, *size); return ESP_OK;
}
static int nvs_set_blob(int h, const char *key, const void *value, size_t size) {
    assert(!strcmp(key, "settings") && size == sizeof(persisted));
    memcpy(persisted, value, size); persisted_size = size; has_blob = true; return ESP_OK;
}
static int nvs_commit(int h) { commits++; return ESP_OK; }
static void nvs_close(int h) { }
static int uart_read_bytes(int port, void *out, unsigned count, unsigned timeout) {
    assert(count == 1);
    if (!*rx) longjmp(end_input, 1);
    *(unsigned char *)out = *rx++; return 1;
}
static int uart_driver_install(int p, int rxsize, int txsize, int q, void *handle, int flags) { return 0; }
static int uart_set_baudrate(int p, int baud) { return 0; }
static int uart_wait_tx_done(int p, int timeout) { return 0; }
static int xTaskCreate(void (*task)(void *), const char *name, int stack, void *arg, int priority, void *handle) { return pdPASS; }
static void vTaskDelay(int ticks) { }
static void esp_restart(void) { longjmp(rebooted, 1); }
bool wifi_auth_method_available(unsigned m) { return m < NETWORK_EAP_COUNT && m != NETWORK_EAP_FAST; }
bool wifi_auth_link_available(unsigned m) { return m <= NETWORK_LINK_SUITEB; }
const char *wifi_credential_name(unsigned m) { static const char *names[] = {"ca", "client", "key", "pac"}; return names[m]; }
esp_err_t wifi_credential_size(unsigned m, size_t *size) { *size = 0; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t wifi_credential_clear(unsigned m) { return ESP_OK; }
esp_err_t wifi_credential_save(unsigned m, const unsigned char *data, size_t size, const char *password) { return ESP_OK; }
esp_err_t wifi_auth_set_time(int64_t now) { return ESP_OK; }
static int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *size, const unsigned char *in, size_t n) { return -1; }

static bool edge16(const char *s) {
    if (!s || strlen(s) != 16) return false;
    for (const char *p = s; *p; p++) if (!(isdigit((unsigned char)*p) || (*p >= 'a' && *p <= 'f'))) return false;
    return true;
}
bool cloud_idp_url_valid(const char *url);
bool cloud_settings_valid(const cloud_settings_t *s) {
    return s->version == CLOUD_SETTINGS_VERSION && edge16(s->edge_device_id) &&
        s->firebase_project_id[0] && s->firebase_api_key[0] &&
        (!s->idp_url[0] || cloud_idp_url_valid(s->idp_url));
}
const cloud_settings_t *cloud_settings_active(void) { return &active_cloud; }
const cloud_settings_t *cloud_settings_saved(void) { return &saved_cloud; }
bool cloud_settings_save(const cloud_settings_t *s) {
    if (!cloud_settings_valid(s)) return false;
    saved_cloud = *s; cloud_saves++; return true;
}
bool cloud_settings_store(const cloud_settings_t *s) { if (!cloud_settings_save(s)) return false; active_cloud = *s; return true; }
bool cloud_settings_apply_token(cloud_settings_t *s, const char *token) { return false; }
bool cloud_settings_configured(void) {
    return cloud_settings_valid(&active_cloud) && active_cloud.edge_device_id[0] && active_cloud.idp_url[0];
}
const char *cloud_authorization_name(const cloud_settings_t *s) {
    if (s->authorization_state == CLOUD_AUTH_APPROVED) return "approved";
    if (s->authorization_state == CLOUD_AUTH_PENDING) return "pending";
    return "required";
}
void cloud_settings_show(const char *label, const cloud_settings_t *s) { }
bool device_auth_request_start(void) {
    device_auth_starts++;
    return device_auth_starts == 1;
}
'''

TEST = r'''
static void run_json(const char *text) {
    char line[2048];
    rx = (const unsigned char *)text;
    memset(&input, 0, sizeof(input));
    if (setjmp(end_input)) abort();
    bool have = read_command_line("\nesp> ", line, sizeof(line));
    assert(have);
    handle_json_command(line);
}

int main(void) {
    memset(&active_cloud, 0, sizeof(active_cloud));
    active_cloud.version = CLOUD_SETTINGS_VERSION;
    strcpy(active_cloud.edge_device_id, "0123456789abcdef");
    strcpy(active_cloud.event_access, "token-secret-value");
    strcpy(active_cloud.refresh_token, "refresh-secret-value");
    memset(&saved_cloud, 0, sizeof(saved_cloud));
    network_settings_load();
    assert(!network_settings_active()->ssid[0]);

    run_json("{\"type\":\"status\"}\n");
    assert(!cloud_settings_configured());
    run_json("{\"type\":\"config\"}\n");
    assert(cloud_saves == 0);
    run_json("{\"type\":\"config\",\"version\":1,\"wifi_ssid\":\"reception\","
             "\"wifi_password\":\"WifiSecret123\",\"signaling_url\":\"http://10.1.2.3:8787\","
             "\"idp_url\":\"https://idp.example/path\",\"firebase_project_id\":\"p\","
             "\"firebase_api_key\":\"k\",\"firestore_emulator_host\":\"\","
             "\"firebase_auth_emulator_host\":\"\",\"edge_device_id\":\"0123456789abcdef\"}\n");
    assert(cloud_saves == 0 && commits == 0);
    run_json("{\"type\":\"config\",\"version\":1,\"wifi_ssid\":\"reception\","
             "\"wifi_password\":\"short\",\"signaling_url\":\"http://10.1.2.3:8787\","
             "\"idp_url\":\"https://idp.example\",\"firebase_project_id\":\"p\","
             "\"firebase_api_key\":\"k\",\"firestore_emulator_host\":\"\","
             "\"firebase_auth_emulator_host\":\"\",\"edge_device_id\":\"0123456789abcdef\"}\n");
    assert(cloud_saves == 0 && commits == 0);
    run_json("{\"type\":\"nonsense\"}\n");
    run_json("{\"type\":\"device_authorization\"}\n");
    assert(device_auth_starts == 1);
    run_json("{\"type\":\"device_authorization\"}\n");
    assert(device_auth_starts == 2);

    const char *valid =
        "{\"type\":\"config\",\"version\":1,\"wifi_ssid\":\"reception\","
        "\"wifi_password\":\"WifiSecret123\",\"signaling_url\":\"10.1.2.3:8787\","
        "\"idp_url\":\"https://idp.example\",\"firebase_project_id\":\"project-1\","
        "\"firebase_api_key\":\"api-key-1\",\"firestore_emulator_host\":\"\","
        "\"firebase_auth_emulator_host\":\"\",\"edge_device_id\":\"0123456789abcdef\"}\n";
#if !defined(CONFIG_WEBCAM_ALLOW_INSECURE_HTTP) || !CONFIG_WEBCAM_ALLOW_INSECURE_HTTP
    /* Exercise the production config command, not just its URL helper. */
    cJSON *insecure = cJSON_Parse(valid);
    assert(insecure);
    cJSON_ReplaceItemInObjectCaseSensitive(insecure, "idp_url", cJSON_CreateString("http://idp.example"));
    char rejected[2048];
    char *encoded = cJSON_PrintUnformatted(insecure);
    snprintf(rejected, sizeof(rejected), "%s\n", encoded); free(encoded);
    run_json(rejected);
    assert(cloud_saves == 0 && commits == 0);
    cJSON_ReplaceItemInObjectCaseSensitive(insecure, "idp_url", cJSON_CreateString("https://idp.example"));
    cJSON_ReplaceItemInObjectCaseSensitive(insecure, "signaling_url", cJSON_CreateString("http://bridge.example"));
    encoded = cJSON_PrintUnformatted(insecure);
    snprintf(rejected, sizeof(rejected), "%s\n", encoded); free(encoded);
    cJSON_Delete(insecure);
    run_json(rejected);
    assert(cloud_saves == 0 && commits == 0);
#endif
    if (!setjmp(rebooted)) {
        char line[2048];
        rx = (const unsigned char *)valid;
        memset(&input, 0, sizeof(input));
        if (setjmp(end_input)) abort();
        assert(read_command_line("\nesp> ", line, sizeof(line)));
        handle_json_command(line);
        abort();
    }
    assert(cloud_saves == 1 && commits == 1);
    assert(!strcmp(saved_cloud.idp_url, "https://idp.example"));
    assert(!strcmp(saved_cloud.edge_device_id, "0123456789abcdef"));
    assert(!strcmp(saved.ssid, "reception") && !strcmp(saved.password, "WifiSecret123"));
    assert(!strcmp(saved.signaling_url, "http://10.1.2.3:8787"));
    puts("{\"type\":\"test_done\"}");
    return 0;
}
'''


def run(development):
    project = Path(__file__).resolve().parents[1]
    temp_root = (project / '.tools').resolve()
    temp_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='webcam-serial-', dir=temp_root) as directory:
        temp = Path(directory)
        assert temp.resolve().is_relative_to(temp_root)
        (temp / 'mock.h').write_text(MOCK, encoding='utf-8')
        for include in ('sdkconfig.h', 'driver/uart.h', 'esp_log.h', 'esp_system.h',
                        'freertos/FreeRTOS.h', 'freertos/task.h', 'nvs.h', 'esp_err.h',
                        'mbedtls/base64.h'):
            header = temp / include
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "mock.h"\n', encoding='utf-8')
        unit = temp / 'serial_test.c'
        test = TEST
        if not development:
            test = test.replace('http://10.1.2.3:8787', 'https://10.1.2.3:8787')
            test = test.replace(r'\"signaling_url\":\"10.1.2.3:8787\"',
                                r'\"signaling_url\":\"https://10.1.2.3:8787\"')
        unit.write_text('#include "mock.h"\n#include "' + (project / 'main/network_settings.c').as_posix() +
                        '"\n' + test, encoding='utf-8')
        exe = temp / 'serial_test'
        cjson = project / 'managed_components/espressif__cjson/cJSON'
        compilation = subprocess.run(
            ['cc', *(['-DCONFIG_WEBCAM_ALLOW_INSECURE_HTTP=1'] if development else []), '-std=c11', '-Wall', '-Wextra', '-O1', '-I' + str(temp),
             '-I' + str(project / 'main'), '-I' + str(cjson),
             '-o', str(exe), str(unit),
             str(project / 'main/network_settings_values.c'),
             str(project / 'main/cloud_settings_values.c'),
             str(cjson / 'cJSON.c'), '-lm'],
            cwd=temp, capture_output=True, text=True)
        if compilation.returncode:
            raise RuntimeError(compilation.stdout + compilation.stderr)
        result = subprocess.run([str(exe)], capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        assert 'test_done' in result.stdout
        assert 'WifiSecret123' not in result.stdout
        assert 'token-secret-value' not in result.stdout
        assert 'refresh-secret-value' not in result.stdout
        assert 'wifi_password' not in result.stdout
        assert 'esp>' not in result.stdout
        assert 'UART logs paused' not in result.stdout
        lines = [line for line in result.stdout.splitlines() if line.strip()]
        for line in lines:
            parsed = json.loads(line)
            assert isinstance(parsed, dict) and isinstance(parsed.get('type'), str)
        assert any(json.loads(line)['type'] == 'config_result' for line in lines)
        assert any(json.loads(line).get('type') == 'status' for line in lines)
        assert not any(json.loads(line).get('type') == 'device_authorization' for line in lines)
        assert any(json.loads(line).get('error') == 'authorization_queue_full_or_unavailable' for line in lines)
        print('Serial protocol passed: JSON-only responses, validation, redaction, reboot.')


if __name__ == '__main__':
    run(False)
    run(True)

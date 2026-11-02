"""Run the production UART console on a PC with in-memory UART/NVS adapters.

Run from an MSVC developer prompt: python esp_webcam/tests/test_network_console.py
No device is accessed and no real network credentials are used.
"""
from pathlib import Path
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
#include "network_settings.h"
#define CONFIG_WEBCAM_WIFI_SSID ""
#define CONFIG_WEBCAM_WIFI_PASSWORD ""
#define CONFIG_WEBCAM_SIGNALING_URL "http://192.168.137.1:8080"
#define CONFIG_ESP_CONSOLE_UART_NUM 0
#define CONFIG_ESP_CONSOLE_UART_BAUDRATE 115200
#define CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT 1
#define ESP_OK 0
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
static const unsigned char *rx;
static unsigned char persisted[sizeof(network_settings_t)];
static size_t persisted_size;
static bool has_blob;
static int commits;
static int credential_saves;
static int diagnostic_shows, diagnostic_pings;
static char ping_target[32];
static int signaling_checks, stun_checks;
void network_diagnostics_show(void) { diagnostic_shows++; puts("Network (current): IPv4 adapter"); }
void network_diagnostics_ping(const char *target) { diagnostic_pings++; strcpy(ping_target, target); }
void network_probes_signaling(void) { signaling_checks++; }
void network_probes_stun(void) { stun_checks++; }
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
static void esp_restart(void) { abort(); }
bool wifi_auth_method_available(unsigned m) { return m < NETWORK_EAP_COUNT && m != NETWORK_EAP_FAST; }
bool wifi_auth_link_available(unsigned m) { return m <= NETWORK_LINK_SUITEB; }
const char *wifi_credential_name(unsigned m) { static const char *names[] = {"ca", "client", "key", "pac"}; return names[m]; }
esp_err_t wifi_credential_size(unsigned m, size_t *size) { *size = 0; return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t wifi_credential_clear(unsigned m) { return ESP_OK; }
esp_err_t wifi_credential_save(unsigned m, const unsigned char *data, size_t size, const char *password) {
    assert(m == WIFI_CRED_KEY && size == 5 && !memcmp(data, "test", 5)); credential_saves++; return ESP_OK;
}
esp_err_t wifi_auth_set_time(int64_t now) { return now >= 1577836800LL && now <= 4102444800LL ? ESP_OK : ESP_ERR_INVALID_ARG; }
/* Minimal adapter for the single known upload fixture. SDK crypto execution
 * is outside this console-adapter test; this checks transaction handling. */
static int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *size, const unsigned char *in, size_t n) {
    if (n != 8 || memcmp(in, "dGVzdA==", 8) || cap < 4) return -1;
    memcpy(out, "test", 4); *size = 4; return 0;
}
'''

TEST = r'''
static void run(const char *text) {
    rx = (const unsigned char *)text;
    memset(&input, 0, sizeof(input));
    if (!setjmp(end_input)) console_task(NULL);
}
int main(void) {
    network_settings_load();
    network_settings_console_start();
    assert(!network_settings_uart_logs_enabled());
    run("logs on\r\n");
    assert(network_settings_uart_logs_enabled()); /* CRLF must not pause logging */
    run("\r\n");
    assert(!network_settings_uart_logs_enabled()); /* Enter stops scrolling */
    run("logs on\r\nshow\r\n");
    assert(!network_settings_uart_logs_enabled()); /* First real key also pauses */
    run("wifi\r\nConsoleSSID\r\nConsoleSecret123\r\n");
    assert(commits == 1 && !strcmp(saved.ssid, "ConsoleSSID"));
    assert(!strcmp(saved.password, "ConsoleSecret123"));
    assert(!active.ssid[0]); /* Persistence does not change a live Wi-Fi session */
    run("signaling\r\n10.1.2.3\r\n");
    assert(commits == 2 && !strcmp(saved.signaling_url, "http://10.1.2.3:8080"));
    assert(!strcmp(saved.password, "ConsoleSecret123"));
    run("wifi\r\nChangedSSID\r\npartial\003\r\n");
    assert(commits == 2 && !strcmp(saved.ssid, "ConsoleSSID"));
    network_settings_load();
    assert(!strcmp(active.ssid, "ConsoleSSID"));
    assert(!strcmp(active.signaling_url, "http://10.1.2.3:8080"));
    run("show\r\n");
    assert(diagnostic_shows > 0);
    run("ping\r\n"); assert(diagnostic_pings == 1 && !ping_target[0]);
    run("ping 192.0.2.1\r\n"); assert(diagnostic_pings == 2 && !strcmp(ping_target, "192.0.2.1"));
    run("pinging\r\n"); assert(diagnostic_pings == 2);
    int commits_before_checks = commits;
    run("check signaling\r\ncheck stun\r\ncheck\r\n");
    assert(signaling_checks == 1 && stun_checks == 1 && commits == commits_before_checks);
    /* PEAP/TTLS passwords are not constrained by the personal PSK rules. */
    int before = commits;
    run("eap\r\npeap\r\nCorpSSID\r\nuser@example\r\np\r\nanonymous@example\r\nwpa2\r\nca\r\nradius.example\r\noff\r\n");
    assert(commits == before + 1 && saved.eap_method == NETWORK_EAP_PEAP);
    assert(!strcmp(saved.eap_password, "p") && !saved.password[0]);
    network_settings_load();
    assert(active.eap_method == NETWORK_EAP_PEAP && !strcmp(active.identity, "anonymous@example"));
    for (unsigned m = NETWORK_EAP_TTLS_EAP; m <= NETWORK_EAP_TTLS_CHAP; m++) {
        char script[512];
        snprintf(script, sizeof(script), "eap\r\n%s\r\nCorpSSID\r\nuser\r\nEapSecret123\r\n\r\nwpa2\r\nbundle\r\nradius.example\r\noff\r\n", network_eap_name(m));
        run(script);
        assert(saved.eap_method == m && !strcmp(saved.identity, "user"));
    }
    run("eap\r\ntls\r\nCorpSSID\r\nclient@example\r\nKeySecret123\r\nsuiteb\r\nca\r\nradius.example\r\non\r\n");
    assert(saved.eap_method == NETWORK_EAP_TLS && saved.check_cert_time == 1);
    assert(!saved.eap_password[0] && !strcmp(saved.key_password, "KeySecret123"));
    before = commits;
    run("eap\r\npeap\r\nCorpSSID\r\nuser\r\nEapSecret123\r\n\r\nwpa3\r\nnone\r\noff\r\n");
    assert(commits == before); /* Refuse WPA3 without server validation. */
    run("eap\r\nfast\r\n");
    assert(commits == before); /* Unsupported method never silently falls back. */
    run("credential key\r\ndGVzdA==\r\n.\r\n");
    assert(credential_saves == 1);
    run("credential key\r\ndGVzdA==\r\n\003");
    assert(credential_saves == 1); /* Aborted uploads cannot replace a key. */
    run("wifi\r\nConsoleSSID\r\nConsoleSecret123\r\n");
    assert(saved.eap_method == NETWORK_EAP_NONE && !saved.eap_password[0] && !saved.key_password[0]);
    /* Migrate the exact deployed v1 format, preserving Wi-Fi and signaling. */
    network_settings_v1_t old = {.version = 1};
    strcpy(old.ssid, "OldSSID"); strcpy(old.password, "OldSecret123");
    strcpy(old.signaling_url, "http://10.9.8.7:8080");
    memcpy(persisted, &old, sizeof(old)); persisted_size = sizeof(old);
    before = commits;
    network_settings_load();
    assert(commits == before && active.version == NETWORK_SETTINGS_VERSION);
    assert(!strcmp(active.ssid, old.ssid) && !strcmp(active.password, old.password));
    assert(!strcmp(active.signaling_url, old.signaling_url) && active.eap_method == NETWORK_EAP_NONE);
    old.version = 99; memcpy(persisted, &old, sizeof(old));
    network_settings_load();
    assert(!active.ssid[0]);
    puts("CONSOLE_TEST_PASSED");
    return 0;
}
'''


def main():
    project = Path(__file__).resolve().parents[1]
    source = project / 'main/network_settings.c'
    temp_root = (project / '.tools').resolve()
    temp_root.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='webcam-console-', dir=temp_root) as directory:
        temp = Path(directory)
        assert temp.resolve().is_relative_to(temp_root)
        (temp / 'mock.h').write_text(MOCK, encoding='utf-8')
        for include in ('sdkconfig.h', 'driver/uart.h', 'esp_log.h', 'esp_system.h',
                        'freertos/FreeRTOS.h', 'freertos/task.h', 'nvs.h', 'esp_err.h', 'mbedtls/base64.h'):
            header = temp / include
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "mock.h"\n', encoding='utf-8')
        unit = temp / 'console_test.c'
        unit.write_text('#include "mock.h"\n#include "' + source.as_posix() + '"\n' + TEST,
                        encoding='utf-8')
        exe = temp / 'console_test.exe'
        compilation = subprocess.run(['cl', '/nologo', '/std:c11', '/experimental:c11atomics',
                        '/O2', '/I' + str(temp), '/I' + str(project / 'main'),
                        '/Fe' + str(exe), str(unit), str(project / 'main/network_settings_values.c')],
                       cwd=temp, capture_output=True, text=True)
        if compilation.returncode:
            raise RuntimeError(compilation.stdout + compilation.stderr)
        result = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
        assert 'CONSOLE_TEST_PASSED' in result.stdout
        assert 'ConsoleSecret123' not in result.stdout
        assert 'partial' not in result.stdout
        assert 'EapSecret123' not in result.stdout
        assert 'KeySecret123' not in result.stdout
        assert 'dGVzdA==' not in result.stdout
        assert '*' * len('ConsoleSecret123') in result.stdout
        assert 'ConsoleSSID' in result.stdout and '10.1.2.3' in result.stdout
        assert 'UART logs paused.' in result.stdout
        print('Console integration passed: EAP methods, masking, credential transactions, NVS v1 migration/reload, logs/CRLF.')


if __name__ == '__main__':
    main()

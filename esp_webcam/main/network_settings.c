#include "network_settings.h"
#include "serial_input.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "wifi_auth.h"
#include "mbedtls/base64.h"
#include "network_diagnostics.h"
#include "network_probes.h"

static const char *TAG = "network_config";
/* Active settings are immutable after load: HTTP/peer tasks can read without
 * locks. Changes are saved separately and take effect together on reboot. */
static network_settings_t active, saved;
static serial_input_t input;
static atomic_bool uart_logs = true;
/* Exact original NVS layout. Never reinterpret a v1 blob as v2. */
typedef struct {
    uint32_t version;
    char ssid[33], password[65], signaling_url[NETWORK_SIGNALING_SIZE];
} network_settings_v1_t;

bool network_settings_uart_logs_enabled(void) { return atomic_load(&uart_logs); }

static void wipe(void *data, size_t size)
{
    volatile unsigned char *p = data;
    while (size--) *p++ = 0;
}

static bool valid_settings(const network_settings_t *s)
{
    if (s->version != NETWORK_SETTINGS_VERSION || !network_eap_valid(s) ||
        !memchr(s->signaling_url, 0, sizeof(s->signaling_url))) return false;
    char url[NETWORK_SIGNALING_SIZE];
    return network_signaling_normalize(s->signaling_url, url, sizeof(url)) && !strcmp(url, s->signaling_url);
}

void network_settings_load(void)
{
    active = (network_settings_t){.version = NETWORK_SETTINGS_VERSION, .enterprise_link = NETWORK_LINK_WPA2};
    if (strlen(CONFIG_WEBCAM_WIFI_SSID) < sizeof(active.ssid) &&
        strlen(CONFIG_WEBCAM_WIFI_PASSWORD) < sizeof(active.password)) {
        strcpy(active.ssid, CONFIG_WEBCAM_WIFI_SSID);
        strcpy(active.password, CONFIG_WEBCAM_WIFI_PASSWORD);
        if (active.ssid[0] && !network_wifi_valid(active.ssid, active.password)) {
            wipe(active.ssid, sizeof(active.ssid));
            wipe(active.password, sizeof(active.password));
            ESP_LOGW(TAG, "Invalid build-time Wi-Fi settings; use serial 'wifi'");
        }
    }
    if (!network_signaling_normalize(CONFIG_WEBCAM_SIGNALING_URL, active.signaling_url,
                                      sizeof(active.signaling_url)))
        ESP_LOGW(TAG, "Invalid build-time signaling URL; use serial 'signaling'");
    nvs_handle_t handle;
    esp_err_t err = nvs_open("network", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        size_t size = 0;
        err = nvs_get_blob(handle, "settings", NULL, &size);
        network_settings_t *stored = calloc(1, sizeof(*stored));
        bool found = false;
        if (stored && err == ESP_OK && size == sizeof(*stored)) {
            err = nvs_get_blob(handle, "settings", stored, &size);
            found = err == ESP_OK && valid_settings(stored);
        } else if (stored && err == ESP_OK && size == sizeof(network_settings_v1_t)) {
            network_settings_v1_t old = {0};
            err = nvs_get_blob(handle, "settings", &old, &size);
            if (err == ESP_OK && old.version == 1 && memchr(old.ssid, 0, sizeof(old.ssid)) &&
                memchr(old.password, 0, sizeof(old.password)) && memchr(old.signaling_url, 0, sizeof(old.signaling_url))) {
                stored->version = NETWORK_SETTINGS_VERSION;
                stored->enterprise_link = NETWORK_LINK_WPA2;
                memcpy(stored->ssid, old.ssid, sizeof(old.ssid));
                memcpy(stored->password, old.password, sizeof(old.password));
                memcpy(stored->signaling_url, old.signaling_url, sizeof(old.signaling_url));
                found = valid_settings(stored);
            }
            wipe(&old, sizeof(old));
        }
        if (found) active = *stored;
        else ESP_LOGW(TAG, "Saved settings unavailable/invalid; using build defaults");
        if (stored) { wipe(stored, sizeof(*stored)); free(stored); }
        nvs_close(handle);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "Settings read failed: %s", esp_err_to_name(err));
    }
    saved = active;
}

const network_settings_t *network_settings_active(void) { return &active; }

static void save_settings(const network_settings_t *next)
{
    if (!valid_settings(next)) { puts("Invalid settings; nothing saved."); return; }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("network", NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, "settings", next, sizeof(*next));
        if (err == ESP_OK) err = nvs_commit(handle);
        nvs_close(handle);
    }
    if (err == ESP_OK) {
        saved = *next;
        puts("Saved. Type reboot to apply all saved settings.");
    } else printf("Save failed: %s. Retry before rebooting.\n", esp_err_to_name(err));
}

/* echo_mode: 0 = ordinary text, 1 = password mask, 2 = hidden file upload. */
static bool read_line(const char *prompt, char *out, size_t size, unsigned echo_mode)
{
    printf("%s", prompt);
    fflush(stdout);
    for (;;) {
        unsigned char c;
        if (uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, pdMS_TO_TICKS(100)) != 1) continue;
        bool crlf_tail = input.skip_lf && c == '\n';
        size_t before = input.length;
        serial_result_t result = serial_input_feed(&input, out, size, c);
        /* CRLF's LF belongs to the previous command, not a new user key. */
        if (crlf_tail) continue;
        /* Any key returns from log monitoring to a stable prompt. In
         * particular, Enter alone is enough; no command must be typed blind. */
        if (atomic_exchange(&uart_logs, false)) {
            printf("\nUART logs paused.\n%s", prompt);
            fflush(stdout);
        }
        if (result == SERIAL_MORE) {
            if (echo_mode != 2) {
                if (input.length > before) putchar(echo_mode == 1 ? '*' : c);
                else if (input.length < before) printf("\b \b");
            }
            fflush(stdout);
            continue;
        }
        puts("");
        if (result != SERIAL_READY) {
            wipe(out, size);
            puts(result == SERIAL_CANCEL ? "Cancelled." : "Invalid/overlong input; nothing saved.");
        }
        return result == SERIAL_READY;
    }
}

static void configure_wifi(void)
{
    network_settings_t next = saved;
    next.eap_method = NETWORK_EAP_NONE;
    wipe(next.identity, sizeof(next.identity)); wipe(next.username, sizeof(next.username));
    wipe(next.eap_password, sizeof(next.eap_password)); wipe(next.key_password, sizeof(next.key_password));
    wipe(next.server_domain, sizeof(next.server_domain));
    next.enterprise_link = NETWORK_LINK_WPA2;
    next.server_trust = NETWORK_TRUST_CA;
    next.check_cert_time = 0;
    puts("Wi-Fi setup. Password is masked. Ctrl-C cancels.");
    if (read_line("SSID: ", next.ssid, sizeof(next.ssid), false) &&
        read_line("Password (empty for open network): ", next.password, sizeof(next.password), true)) {
        if (network_wifi_valid(next.ssid, next.password)) save_settings(&next);
        else puts("Invalid Wi-Fi settings: SSID 1-32 bytes; password empty, 8-63 bytes, or 64 hex digits.");
    }
    wipe(&next, sizeof(next));
}

static void list_protocols(void)
{
    puts("wifi: open/WPA2/WPA3 personal (existing PSK setup)");
    for (unsigned i = 1; i < NETWORK_EAP_COUNT; i++)
        printf("eap: %-14s %s\n", network_eap_name(i), wifi_auth_method_available(i) ? "available" : "not in this build");
    puts("PEAP uses inner EAP-MSCHAPv2. TTLS inner methods are selected explicitly.");
    puts("FAST needs ESP_WIFI_MBEDTLS_TLS_CLIENT disabled and an imported binary PAC (no auto provisioning).");
    printf("Enterprise links: wpa, wpa2, wpa3%s, suiteb%s\n",
        wifi_auth_link_available(NETWORK_LINK_WPA3) ? "" : " (not in this build)",
        wifi_auth_link_available(NETWORK_LINK_SUITEB) ? "" : " (not in this build)");
}

static void configure_eap(void)
{
    network_settings_t next = saved;
    char text[40];
    list_protocols();
    if (!read_line("EAP method: ", text, sizeof(text), false)) goto done;
    unsigned method;
    for (method = 1; method < NETWORK_EAP_COUNT; method++) if (!strcmp(text, network_eap_name(method))) break;
    if (!wifi_auth_method_available(method)) { puts("Method unavailable. Type protocols for build requirements."); goto done; }
    next.eap_method = method;
    wipe(next.password, sizeof(next.password));
    wipe(next.identity, sizeof(next.identity)); wipe(next.username, sizeof(next.username));
    wipe(next.eap_password, sizeof(next.eap_password)); wipe(next.key_password, sizeof(next.key_password));
    wipe(next.server_domain, sizeof(next.server_domain));
    next.enterprise_link = NETWORK_LINK_WPA2;
    next.server_trust = NETWORK_TRUST_CA;
    next.check_cert_time = 0;
    if (!read_line("SSID: ", next.ssid, sizeof(next.ssid), false)) goto done;
    if (method != NETWORK_EAP_TLS) {
        if (!read_line("Inner username: ", next.username, sizeof(next.username), false) ||
            !read_line("EAP password: ", next.eap_password, sizeof(next.eap_password), true)) goto done;
    }
    if (!read_line("Outer identity (empty = username): ", next.identity, sizeof(next.identity), false)) goto done;
    if (!next.identity[0]) memcpy(next.identity, next.username, sizeof(next.identity));
    if (method == NETWORK_EAP_TLS &&
        !read_line("Client key password (empty = unencrypted): ", next.key_password, sizeof(next.key_password), true)) goto done;
    if (method != NETWORK_EAP_FAST) {
        if (!read_line("Enterprise link: wpa/wpa2/wpa3/suiteb [wpa2]: ", text, sizeof(text), false)) goto done;
        if (!text[0] || !strcmp(text, "wpa2")) next.enterprise_link = NETWORK_LINK_WPA2;
        else if (!strcmp(text, "wpa")) next.enterprise_link = NETWORK_LINK_WPA;
        else if (!strcmp(text, "wpa3")) next.enterprise_link = NETWORK_LINK_WPA3;
        else if (!strcmp(text, "suiteb")) next.enterprise_link = NETWORK_LINK_SUITEB;
        else { puts("Invalid enterprise link."); goto done; }
        if (!wifi_auth_link_available(next.enterprise_link)) { puts("Link unavailable in this build."); goto done; }
        if (!read_line("Server trust: ca/bundle/none [ca]: ", text, sizeof(text), false)) goto done;
        if (!text[0] || !strcmp(text, "ca")) next.server_trust = NETWORK_TRUST_CA;
        else if (!strcmp(text, "bundle")) next.server_trust = NETWORK_TRUST_BUNDLE;
        else if (!strcmp(text, "none")) { next.server_trust = NETWORK_TRUST_NONE; puts("Server certificate verification disabled for this profile."); }
        else { puts("Invalid trust mode."); goto done; }
        if (next.server_trust != NETWORK_TRUST_NONE &&
            !read_line("Server certificate name (recommended; empty = CA only): ", next.server_domain, sizeof(next.server_domain), false)) goto done;
#ifndef CONFIG_ESP_WIFI_MBEDTLS_TLS_CLIENT
        if (next.server_domain[0] || next.server_trust == NETWORK_TRUST_BUNDLE) {
            puts("Internal TLS does not support server-name matching or the CA bundle. Use a custom CA or rebuild with MbedTLS."); goto done;
        }
#endif
        if (!read_line("Certificate time check: on/off [off; no RTC clock]: ", text, sizeof(text), false)) goto done;
        if (!strcmp(text, "on")) next.check_cert_time = 1;
        else if (text[0] && strcmp(text, "off")) { puts("Invalid time-check option."); goto done; }
    }
    if (!network_eap_valid(&next)) { puts("Invalid EAP profile. TLS needs an identity; other methods need username/password. WPA3 needs server validation; Suite-B needs TLS."); goto done; }
    save_settings(&next);
    puts(method == NETWORK_EAP_FAST ? "Import credential pac before reboot." :
        "Import required credentials before reboot (ca for custom CA; client and key for TLS).");
done:
    wipe(&next, sizeof(next));
}

static void import_credential(unsigned type)
{
    unsigned char *data = calloc(1, WIFI_CREDENTIAL_MAX + 1);
    if (!data) { puts("No credential buffer."); return; }
    char line[129];
    size_t total = 0;
    puts("CREDENTIAL_READY: send base64 in lines <=128 chars; . ends; Ctrl-C cancels. Input is hidden.");
    for (;;) {
        /* 2 means no echo at all, including masks, for machine uploads. */
        if (!read_line("data> ", line, sizeof(line), 2)) goto done;
        if (!strcmp(line, ".")) break;
        unsigned char decoded[96];
        size_t size = 0;
        int ret = mbedtls_base64_decode(decoded, sizeof(decoded), &size, (unsigned char *)line, strlen(line));
        if (ret || !size || total + size > WIFI_CREDENTIAL_MAX) {
            puts("CREDENTIAL_ERROR: invalid base64 or file too large.");
            wipe(decoded, sizeof(decoded));
            goto done;
        }
        memcpy(data + total, decoded, size); total += size;
        wipe(decoded, sizeof(decoded)); wipe(line, sizeof(line));
    }
    if (type != WIFI_CRED_PAC && total && data[total - 1] != 0) {
        if (total == WIFI_CREDENTIAL_MAX) { puts("CREDENTIAL_ERROR: no room for PEM terminator."); goto done; }
        data[total++] = 0;
    }
    esp_err_t err = wifi_credential_save(type, data, total, saved.key_password);
    if (err == ESP_OK) printf("CREDENTIAL_SAVED %s. Reboot to apply.\n", wifi_credential_name(type));
    else printf("CREDENTIAL_ERROR: %s (check PEM/key password/PAC size and NVS space).\n", esp_err_to_name(err));
done:
    wipe(line, sizeof(line)); wipe(data, WIFI_CREDENTIAL_MAX + 1); free(data);
}

static void credential_command(const char *name, bool clear)
{
    for (unsigned type = 0; type < WIFI_CRED_COUNT; type++) if (!strcmp(name, wifi_credential_name(type))) {
        if (clear) printf("Credential clear: %s. Reboot to apply.\n", esp_err_to_name(wifi_credential_clear(type)));
        else import_credential(type);
        return;
    }
    puts("Credential types: ca, client, key, pac.");
}

static void configure_signaling(void)
{
    char text[NETWORK_SIGNALING_SIZE];
    network_settings_t next = saved;
    puts("Enter IPv4/hostname[:port] (default 8080), http://host:port, or off.");
    if (read_line("Signaling server: ", text, sizeof(text), false)) {
        if (text[0] && network_signaling_normalize(text, next.signaling_url, sizeof(next.signaling_url)))
            save_settings(&next);
        else puts("Invalid server. Use an HTTP origin with no path, credentials or query; use off to disable.");
    }
    wipe(&next, sizeof(next));
}

static void show_settings(const char *label, const network_settings_t *s)
{
    printf("%s\n  SSID: %s\n  Password: %s\n  Signaling: %s\n", label,
        s->ssid[0] ? s->ssid : "(not configured)", s->password[0] ? "(set, hidden)" : "(empty)",
        s->signaling_url[0] ? s->signaling_url : "(disabled)");
    printf("  Authentication: %s\n", network_eap_name(s->eap_method));
    if (s->eap_method) {
        static const char *const links[] = {"wpa", "wpa2", "wpa3", "suiteb"};
        static const char *const trust[] = {"custom CA", "system CA bundle", "no verification"};
        printf("  Identity: %s\n  Username: %s\n  EAP password: %s\n  Key password: %s\n"
            "  Link: %s\n  Trust: %s\n  Server name: %s\n  Certificate time check: %s\n",
            s->identity, s->username, s->eap_password[0] ? "(set, hidden)" : "(empty)",
            s->key_password[0] ? "(set, hidden)" : "(empty)", links[s->enterprise_link], trust[s->server_trust],
            s->server_domain[0] ? s->server_domain : "(not set)", s->check_cert_time ? "on" : "off");
    }
}

static void console_task(void *arg)
{
    puts("\nNetwork setup ready. Commands: help, show, ping [IPv4], check signaling/stun, wifi, eap, protocols, credentials, clock, signaling, reboot, logs on/off.");
    puts("UART logs paused. Disable terminal local echo. Password is masked.");
    puts("Use logs on to monitor; press Enter to pause logs and return to setup.");
    for (;;) {
        char command[32];
        if (!read_line("\nesp> ", command, sizeof(command), false)) continue;
        if (!strcmp(command, "wifi")) configure_wifi();
        else if (!strcmp(command, "eap")) configure_eap();
        else if (!strcmp(command, "protocols")) list_protocols();
        else if (!strcmp(command, "clock")) {
            char text[24], *end;
            if (read_line("Current UTC Unix seconds (2020-2100): ", text, sizeof(text), false)) {
                long long value = strtoll(text, &end, 10);
                if (!text[0] || *end || wifi_auth_set_time(value) != ESP_OK) puts("Invalid time.");
                else puts("System time set for this boot. Pending EAP retries use this clock.");
            }
        }
        else if (!strncmp(command, "credential ", 11)) credential_command(command + 11, false);
        else if (!strncmp(command, "credential-clear ", 17)) credential_command(command + 17, true);
        else if (!strcmp(command, "credentials")) {
            for (unsigned type = 0; type < WIFI_CRED_COUNT; type++) {
                size_t size = 0;
                esp_err_t err = wifi_credential_size(type, &size);
                printf("%s: %s (%u bytes)\n", wifi_credential_name(type), err == ESP_OK ? "stored" : "not available", (unsigned)size);
            }
            puts("Import: credential ca/client/key/pac. Clear: credential-clear ca/client/key/pac.");
        }
        else if (!strcmp(command, "signaling")) configure_signaling();
        else if (!strcmp(command, "ping")) network_diagnostics_ping("");
        else if (!strncmp(command, "ping ", 5)) network_diagnostics_ping(command + 5);
        else if (!strcmp(command, "check signaling")) network_probes_signaling();
        else if (!strcmp(command, "check stun")) network_probes_stun();
        else if (!strcmp(command, "check")) puts("Usage: check signaling | check stun (connectivity only)");
        else if (!strcmp(command, "show")) {
            network_diagnostics_show();
            show_settings("Active (this boot):", &active);
            show_settings("Saved (next boot):", &saved);
        } else if (!strcmp(command, "logs on")) {
            puts("UART logs enabled. Press Enter to pause and return to setup.");
            fflush(stdout);
            atomic_store(&uart_logs, true);
        } else if (!strcmp(command, "logs off")) {
            puts("UART logs paused.");
        } else if (!strcmp(command, "reboot")) {
            puts("Restarting with saved settings...");
            fflush(stdout);
            uart_wait_tx_done(CONFIG_ESP_CONSOLE_UART_NUM, pdMS_TO_TICKS(500));
            esp_restart();
        } else if (command[0]) puts("Commands: help, show, ping [IPv4], check signaling/stun, wifi, eap, protocols, credentials, clock, signaling, reboot, logs on/off. Settings apply on reboot.");
        wipe(command, sizeof(command));
    }
}

void network_settings_console_start(void)
{
    ESP_ERROR_CHECK(uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 1024, 0, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_set_baudrate(CONFIG_ESP_CONSOLE_UART_NUM, CONFIG_ESP_CONSOLE_UART_BAUDRATE));
    atomic_store(&uart_logs, false);
    if (xTaskCreate(console_task, "net_console", 8192, NULL, 2, NULL) != pdPASS)
        ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
}

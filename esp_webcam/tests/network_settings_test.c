#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../main/network_settings.h"
#include "../main/serial_input.h"
#include "../main/stun_probe.h"

static void server(const char *input, const char *expected)
{
    char out[NETWORK_SIGNALING_SIZE] = "unchanged";
    bool ok = network_signaling_normalize(input, out, sizeof(out));
    if (ok != (expected != NULL) || strcmp(out, expected ? expected : "unchanged")) {
        fprintf(stderr, "signaling '%s' -> ok=%d out='%s' expected='%s'\n",
            input, ok, out, expected ? expected : "(invalid)");
        assert(0);
    }
}

static serial_result_t feed(serial_input_t *state, char *out, size_t cap, const char *text)
{
    serial_result_t r = SERIAL_MORE;
    while (*text) r = serial_input_feed(state, out, cap, (unsigned char)*text++);
    return r;
}

int main(void)
{
    char host[128], port[6];
    assert(stun_probe_server("stun:example.test", host, sizeof(host), port));
    assert(!strcmp(host, "example.test") && !strcmp(port, "3478"));
    assert(stun_probe_server("stun:192.0.2.1:19302", host, sizeof(host), port));
    assert(!strcmp(port, "19302"));
    const char *invalid_stun[] = {"", "stuns:example.test", "stun:", "stun:host:", "stun:host:0",
        "stun:host:65536", "stun:host:abc", "stun:host/path", "stun:[::1]:3478", "stun:host name"};
    for (size_t i = 0; i < sizeof(invalid_stun)/sizeof(invalid_stun[0]); i++)
        assert(!stun_probe_server(invalid_stun[i], host, sizeof(host), port));
    uint8_t request[20] = {0,1,0,0,0x21,0x12,0xa4,0x42,1,2,3,4,5,6,7,8,9,10,11,12};
    uint8_t reply[32] = {0};
    memcpy(reply, request, 20); reply[0]=1; reply[1]=1; reply[3]=12;
    reply[21]=0x20; reply[23]=8; reply[25]=1;
    assert(stun_probe_response(reply, sizeof(reply), request) == STUN_PROBE_SUCCESS);
    for (size_t i=0; i<sizeof(reply); i++) assert(stun_probe_response(reply, i, request) == STUN_PROBE_INVALID);
    reply[8]++; assert(stun_probe_response(reply, sizeof(reply), request) == STUN_PROBE_INVALID); reply[8]--;
    reply[4]++; assert(stun_probe_response(reply, sizeof(reply), request) == STUN_PROBE_INVALID); reply[4]--;
    reply[23]=100; assert(stun_probe_response(reply, sizeof(reply), request) == STUN_PROBE_INVALID); reply[23]=8;
    reply[25]=2; assert(stun_probe_response(reply, sizeof(reply), request) == STUN_PROBE_INVALID); reply[25]=1;
    reply[1]=0x11; reply[3]=8; reply[21]=9; reply[23]=4; reply[25]=0; reply[26]=4; reply[27]=1;
    assert(stun_probe_response(reply, 28, request) == STUN_PROBE_ERROR);

    char ssid[34], pass[66];
    memset(ssid, 's', sizeof(ssid)); ssid[32] = 0;
    memset(pass, 'a', sizeof(pass)); pass[64] = 0;
    assert(network_wifi_valid(ssid, pass));
    assert(network_wifi_valid("network with spaces", "password with spaces"));
    assert(network_wifi_valid("open", ""));
    assert(!network_wifi_valid("", "password"));
    assert(!network_wifi_valid("ssid", "short"));
    assert(!network_wifi_valid("ssid\n", "password"));
    assert(!network_wifi_valid("ssid", "password\t"));
    pass[0] = 'g'; assert(!network_wifi_valid(ssid, pass));
    pass[63] = 0; assert(network_wifi_valid(ssid, pass));
    pass[63] = 'a'; pass[64] = 'a'; pass[65] = 0;
    assert(!network_wifi_valid(ssid, pass));
    ssid[32] = 's'; ssid[33] = 0;
    assert(!network_wifi_valid(ssid, "password"));

    network_settings_t profile = {.version = NETWORK_SETTINGS_VERSION, .eap_method = NETWORK_EAP_PEAP,
        .enterprise_link = NETWORK_LINK_WPA2, .server_trust = NETWORK_TRUST_CA};
    strcpy(profile.ssid, "CorpSSID"); strcpy(profile.username, "user");
    strcpy(profile.identity, "anonymous"); strcpy(profile.eap_password, "p");
    for (unsigned m = NETWORK_EAP_PEAP; m <= NETWORK_EAP_TTLS_CHAP; m++) {
        profile.eap_method = m; assert(network_eap_valid(&profile));
    }
    profile.eap_method = NETWORK_EAP_TLS; profile.eap_password[0] = 0; profile.username[0] = 0;
    profile.enterprise_link = NETWORK_LINK_SUITEB;
    assert(network_eap_valid(&profile));
    profile.server_trust = NETWORK_TRUST_NONE; assert(!network_eap_valid(&profile));
    profile.server_trust = NETWORK_TRUST_CA; profile.identity[0] = 0;
    assert(!network_eap_valid(&profile));
    strcpy(profile.identity, "client"); profile.eap_method = NETWORK_EAP_FAST;
    strcpy(profile.username, "user"); strcpy(profile.eap_password, "pass");
    assert(!network_eap_valid(&profile)); /* FAST is WPA/WPA2 only. */
    profile.enterprise_link = NETWORK_LINK_WPA2; assert(network_eap_valid(&profile));
    strcpy(profile.server_domain, "radius.example"); assert(!network_eap_valid(&profile));
    profile.server_domain[0] = 0; profile.eap_method = NETWORK_EAP_COUNT;
    assert(!network_eap_valid(&profile));
    profile.eap_method = NETWORK_EAP_PEAP;
    memset(profile.username, 'u', sizeof(profile.username)); assert(!network_eap_valid(&profile));

#if defined(CONFIG_WEBCAM_ALLOW_INSECURE_HTTP) && CONFIG_WEBCAM_ALLOW_INSECURE_HTTP
    server("192.168.137.1", "http://192.168.137.1:8080");
    server("192.168.1.20:9000", "http://192.168.1.20:9000");
    server("viewer.local", "http://viewer.local:8080");
    server("http://viewer.local", "http://viewer.local");
    server("http://viewer.local:8080/", "http://viewer.local:8080");
    server("http://viewer.local:65535", "http://viewer.local:65535");
#else
    const char *http[] = {"192.168.137.1", "viewer.local:8080", "http://viewer.local",
                          "http://viewer.local:8080/"};
    for (size_t i = 0; i < sizeof(http) / sizeof(http[0]); i++) {
        char out[192];
        assert(!network_signaling_normalize(http[i], out, sizeof(out)));
    }
#endif
    server("https://viewer.local", "https://viewer.local:443");
    server("https://viewer.local/", "https://viewer.local:443");
    server("https://viewer.local:8443", "https://viewer.local:8443");
    server("https://192.168.1.20", "https://192.168.1.20:443");
    server("off", ""); server("", "");
    const char *bad[] = {"http://", "https://", "192.168.1.1:0", "host:65536",
        "host:", "host:abc", "http://host/path", "http://host?x=1", "http://user:pass@host",
        "https://host/path", "https://host?x=1", "https://user:pass@host",
        "host name", "host\r", "-host", "host-", "a..b", "host/", "http://host//",
        "999.1.1.1", "192.168.1", "[::1]:8080", "12345678901234567890.1.1.1"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) server(bad[i], NULL);
    char tiny[8] = "keep";
    assert(!network_signaling_normalize("192.168.1.1", tiny, sizeof(tiny)));
    assert(!strcmp(tiny, "keep"));
    char long_host[NETWORK_SIGNALING_SIZE];
    memset(long_host, 'a', sizeof(long_host));
    long_host[50] = long_host[101] = long_host[152] = long_host[203] = '.';
    long_host[sizeof(long_host) - 1] = 0;
    server(long_host, NULL); /* Output including scheme/port would overflow. */

    serial_input_t s = {0};
    char line[8] = {0};
    assert(feed(&s, line, sizeof(line), "wifi\r") == SERIAL_READY && !strcmp(line, "wifi"));
    assert(feed(&s, line, sizeof(line), "\n") == SERIAL_MORE); /* CRLF across prompts */
    assert(feed(&s, line, sizeof(line), "ab\bc\177d\n") == SERIAL_READY && !strcmp(line, "ad"));
    assert(feed(&s, line, sizeof(line), "1234567\n") == SERIAL_READY);
    assert(feed(&s, line, sizeof(line), "12345678\bwifi\n") == SERIAL_INVALID);
    assert(feed(&s, line, sizeof(line), "show\n") == SERIAL_READY && !strcmp(line, "show"));
    assert(feed(&s, line, sizeof(line), "secret\003") == SERIAL_CANCEL);
    assert(feed(&s, line, sizeof(line), "\r") == SERIAL_READY && !line[0]);
    assert(feed(&s, line, sizeof(line), "a\033[A\n") == SERIAL_INVALID);
    assert(feed(&s, line, sizeof(line), "\n") == SERIAL_READY && !line[0]);
    puts("network settings validation and serial input tests passed");
    return 0;
}

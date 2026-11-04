#include "network_settings.h"
#include "credential_transport.h"
#include <stdio.h>
#include <string.h>

static bool printable(const char *text)
{
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if (*p < 32 || *p == 127) return false;
    return true;
}

static bool hex_digit(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool network_wifi_valid(const char *ssid, const char *password)
{
    size_t s = strlen(ssid), p = strlen(password);
    if (!s || s > 32 || !printable(ssid) || !printable(password)) return false;
    if (!p || (p >= 8 && p <= 63)) return true;
    if (p != 64) return false;
    for (size_t i = 0; i < p; i++) if (!hex_digit(password[i])) return false;
    return true;
}

const char *network_eap_name(unsigned method)
{
    static const char *const names[] = {"personal", "tls", "peap", "ttls-eap", "ttls-mschapv2",
        "ttls-mschap", "ttls-pap", "ttls-chap", "fast"};
    return method < NETWORK_EAP_COUNT ? names[method] : "unknown";
}

bool network_eap_valid(const network_settings_t *s)
{
    if (s->eap_method >= NETWORK_EAP_COUNT || s->enterprise_link > NETWORK_LINK_SUITEB ||
        s->server_trust > NETWORK_TRUST_NONE || s->check_cert_time > 1 ||
        !memchr(s->ssid, 0, sizeof(s->ssid)) || !memchr(s->password, 0, sizeof(s->password)) ||
        !memchr(s->identity, 0, sizeof(s->identity)) || !memchr(s->username, 0, sizeof(s->username)) ||
        !memchr(s->eap_password, 0, sizeof(s->eap_password)) ||
        !memchr(s->key_password, 0, sizeof(s->key_password)) ||
        !memchr(s->server_domain, 0, sizeof(s->server_domain))) return false;
    if (s->eap_method == NETWORK_EAP_NONE)
        return (!s->ssid[0] && !s->password[0]) || network_wifi_valid(s->ssid, s->password);
    if (!s->ssid[0] || !printable(s->ssid) || !s->identity[0] || !printable(s->identity) ||
        !printable(s->username) || !printable(s->eap_password) || !printable(s->key_password) ||
        !printable(s->server_domain)) return false;
    if (s->eap_method != NETWORK_EAP_TLS && (!s->username[0] || !s->eap_password[0])) return false;
    if (s->eap_method == NETWORK_EAP_FAST)
        return s->enterprise_link <= NETWORK_LINK_WPA2 && !s->server_domain[0] &&
            s->server_trust == NETWORK_TRUST_CA; /* A pre-provisioned PAC authenticates FAST. */
    if (s->enterprise_link == NETWORK_LINK_SUITEB && s->eap_method != NETWORK_EAP_TLS) return false;
    if (s->enterprise_link >= NETWORK_LINK_WPA3 && s->server_trust == NETWORK_TRUST_NONE) return false;
    return true;
}

bool network_signaling_normalize(const char *input, char *out, size_t size)
{
    if (!size || strlen(input) >= NETWORK_SIGNALING_SIZE) return false;
    if (!strcmp(input, "off") || !input[0]) { out[0] = 0; return true; }
    bool tls = !strncmp(input, "https://", 8);
    if (!tls && !credential_url_allowed("http://")) return false;
    bool url = tls || !strncmp(input, "http://", 7);
    const char *host = url ? input + (tls ? 8 : 7) : input;
    size_t len = strlen(host);
    if (url && len && host[len - 1] == '/') len--;
    const char *colon = memchr(host, ':', len);
    size_t host_len = colon ? (size_t)(colon - host) : len;
    if (!host_len || host_len > 253) return false;
    size_t label = 0;
    for (size_t i = 0; i < host_len; i++) {
        char c = host[i];
        bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        if (c == '.') {
            if (!label || host[i - 1] == '-') return false;
            label = 0;
        } else {
            if (!alnum && c != '-') return false;
            if (!label && c == '-') return false;
            if (++label > 63) return false;
        }
    }
    if (!label || host[host_len - 1] == '-') return false;
    /* A numeric dotted host is an IPv4 address, not a DNS fallback. */
    bool numeric = true;
    for (size_t i = 0; i < host_len; i++)
        if ((host[i] < '0' || host[i] > '9') && host[i] != '.') numeric = false;
    if (numeric) {
        unsigned octets = 0, value = 0;
        for (size_t i = 0; i <= host_len; i++) {
            if (i == host_len || host[i] == '.') {
                if (value > 255) return false;
                octets++;
                value = 0;
            } else {
                value = value * 10 + (unsigned)(host[i] - '0');
                if (value > 255) return false;
            }
        }
        if (octets != 4) return false;
    }
    if (colon) {
        size_t digits = len - host_len - 1;
        if (!digits || digits > 5) return false;
        unsigned port = 0;
        for (size_t i = host_len + 1; i < len; i++) {
            if (host[i] < '0' || host[i] > '9') return false;
            port = port * 10 + (unsigned)(host[i] - '0');
        }
        if (!port || port > 65535) return false;
    }
    char normalized[NETWORK_SIGNALING_SIZE];
    const char *port_suffix = !url ? (colon ? "" : ":8080") : (tls && !colon ? ":443" : "");
    int n = snprintf(normalized, sizeof(normalized), "%s://%.*s%s", tls ? "https" : "http",
        (int)len, host, port_suffix);
    if (n < 0 || (size_t)n >= sizeof(normalized) || (size_t)n >= size) return false;
    memcpy(out, normalized, (size_t)n + 1);
    return true;
}

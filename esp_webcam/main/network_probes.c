#include "network_probes.h"
#include "network_diagnostics.h"
#include "network_settings.h"
#include "stun_probe.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

void network_probes_signaling(void)
{
    const char *base = network_settings_active()->signaling_url;
    if (!base[0]) { puts("Signaling check: server is disabled."); return; }
    if (!network_diagnostics_has_ipv4()) { puts("Signaling check: no Wi-Fi IPv4 connection."); return; }
    char url[NETWORK_SIGNALING_SIZE + 9];
    snprintf(url, sizeof(url), "%s/healthz", base);
    printf("Signaling check: GET %s\n", url);
    esp_http_client_config_t config = {
        .url = url, .timeout_ms = 3000, .disable_auto_redirect = true,
        .keep_alive_enable = false, .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) { puts("Signaling check failed: no HTTP client memory."); return; }
    esp_err_t err = esp_http_client_set_method(client, HTTP_METHOD_GET);
    int64_t start = esp_timer_get_time();
    /* /healthz is a read-only readiness endpoint. It does not create a
     * session or touch the signaling documents. */
    if (err == ESP_OK) err = esp_http_client_open(client, 0);
    if (err == ESP_OK && esp_http_client_fetch_headers(client) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        printf("Signaling HTTP reachable: status=%d, time=%" PRId64 " ms%s\n", status,
            (esp_timer_get_time() - start) / 1000, status == 200 ? " (OK)" : " (check URL/server)");
    } else printf("Signaling check failed: %s\n", esp_err_to_name(err));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

void network_probes_stun(void)
{
    if (!CONFIG_WEBCAM_STUN_URL[0]) { puts("STUN check: server is disabled."); return; }
    if (!network_diagnostics_has_ipv4()) { puts("STUN check: no Wi-Fi IPv4 connection."); return; }
    char host[128], port[6];
    if (!stun_probe_server(CONFIG_WEBCAM_STUN_URL, host, sizeof(host), port)) {
        puts("STUN check: supports stun:host[:port] over IPv4 UDP only."); return;
    }
    printf("STUN check: %s:%s (one UDP Binding request)\n", host, port);
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_DGRAM};
    struct addrinfo *addresses = NULL;
    if (getaddrinfo(host, port, &hints, &addresses) || !addresses) {
        puts("STUN check failed: DNS/address resolution."); return;
    }
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { freeaddrinfo(addresses); puts("STUN check failed: no UDP socket."); return; }
    struct timeval timeout = {.tv_sec = 3};
    int result = setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    if (result == 0) result = connect(sock, addresses->ai_addr, addresses->ai_addrlen);
    freeaddrinfo(addresses);
    if (result < 0) { printf("STUN check failed: UDP setup (errno=%d).\n", errno); close(sock); return; }
    uint8_t request[20] = {0x00, 0x01, 0, 0, 0x21, 0x12, 0xa4, 0x42};
    esp_fill_random(request + 8, 12);
    int64_t start = esp_timer_get_time();
    if (send(sock, request, sizeof(request), 0) != sizeof(request)) {
        printf("STUN check failed: UDP send (errno=%d).\n", errno); close(sock); return;
    }
    uint8_t response[512];
    int size = recv(sock, response, sizeof(response), 0);
    close(sock);
    if (size < 0) { puts("STUN check: no response / receive error (3000 ms timeout)."); return; }
    stun_probe_result_t reply = stun_probe_response(response, (size_t)size, request);
    if (reply == STUN_PROBE_SUCCESS)
        printf("STUN UDP reachable: Binding response OK, time=%" PRId64 " ms\n", (esp_timer_get_time() - start) / 1000);
    else if (reply == STUN_PROBE_ERROR) puts("STUN UDP reachable: server returned a Binding error.");
    else puts("STUN check failed: invalid or unrelated response.");
}

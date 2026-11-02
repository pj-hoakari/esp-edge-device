#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_private/esp_clk.h"
#include "mbedtls/ssl.h"
#include "perf.h"

typedef struct {
    uint64_t us, bytes;
    uint32_t calls, max_us, errors;
} perf_metric_t;
static perf_metric_t metrics[PERF_COUNT];
static char cipher[96];
static portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

void perf_record(perf_kind_t kind, int64_t us, size_t bytes, int error)
{
    portENTER_CRITICAL(&mux);
    perf_metric_t *m = &metrics[kind];
    m->calls++; m->us += us; m->bytes += bytes; m->errors += error != 0;
    if (us > m->max_us) m->max_us = us;
    portEXIT_CRITICAL(&mux);
}

void perf_reset(void)
{
    portENTER_CRITICAL(&mux);
    memset(metrics, 0, sizeof(metrics));
    memset(cipher, 0, sizeof(cipher));
    portEXIT_CRITICAL(&mux);
}

int __real_mbedtls_ssl_write(mbedtls_ssl_context *, const unsigned char *, size_t);
int __real_mbedtls_ssl_read(mbedtls_ssl_context *, unsigned char *, size_t);

int __wrap_mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *buf, size_t len)
{
    int64_t start = esp_timer_get_time();
    int ret = __real_mbedtls_ssl_write(ssl, buf, len);
    perf_record(PERF_TLS_WRITE, esp_timer_get_time() - start, ret > 0 ? ret : 0,
                ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE);
    const char *name = mbedtls_ssl_get_ciphersuite(ssl);
    if (name) {
        portENTER_CRITICAL(&mux);
        if (!cipher[0]) strlcpy(cipher, name, sizeof(cipher));
        portEXIT_CRITICAL(&mux);
    }
    return ret;
}

int __wrap_mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t len)
{
    int64_t start = esp_timer_get_time();
    int ret = __real_mbedtls_ssl_read(ssl, buf, len);
    perf_record(PERF_TLS_READ, esp_timer_get_time() - start, ret > 0 ? ret : 0,
                ret < 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE);
    return ret;
}

int perf_json(char *out, size_t size)
{
    static const char *names[] = {"peer_loop", "send_api", "copy", "tls_write", "tls_read", "udp_send", "udp_wait", "udp_recv"};
    perf_metric_t snapshot[PERF_COUNT];
    char name[sizeof(cipher)];
    portENTER_CRITICAL(&mux);
    memcpy(snapshot, metrics, sizeof(snapshot)); memcpy(name, cipher, sizeof(name));
    portEXIT_CRITICAL(&mux);
    wifi_ap_record_t ap = {0};
    esp_wifi_sta_get_ap_info(&ap);
    char sha[17];
    const esp_app_desc_t *app = esp_app_get_description();
    for (int i = 0; i < 8; i++) snprintf(sha + 2*i, 3, "%02x", app->app_elf_sha256[i]);
    int used = snprintf(out, size,
        "{\"uptime_ms\":%"PRId64",\"reset_reason\":%d,\"cpu_mhz\":%d,\"fw_sha\":\"%s\","
        "\"cipher\":\"%s\",\"rssi\":%d,"
        "\"internal_free\":%u,\"internal_min\":%u,\"internal_largest\":%u,\"metrics\":{",
        esp_timer_get_time()/1000, esp_reset_reason(), esp_clk_cpu_freq()/1000000, sha, name, ap.rssi,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (used < 0 || used >= size) return -1;
    for (int i = 0; i < PERF_COUNT; i++) {
        perf_metric_t *m = &snapshot[i];
        int n = snprintf(out + used, size - used,
            "%s\"%s\":{\"calls\":%"PRIu32",\"us\":%"PRIu64",\"bytes\":%"PRIu64
            ",\"max_us\":%"PRIu32",\"errors\":%"PRIu32"}",
            i ? "," : "", names[i], m->calls, m->us, m->bytes, m->max_us, m->errors);
        if (n < 0 || n >= size - used) return -1;
        used += n;
    }
    int n = snprintf(out + used, size - used, "}}");
    return n < 0 || n >= size - used ? -1 : used + n;
}

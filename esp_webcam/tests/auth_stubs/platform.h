#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
#define ESP_FAIL -1
#define pdTRUE 1
#define portMAX_DELAY 0xffffffffU
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
typedef int esp_err_t;
const char *esp_err_to_name(int error);
typedef void *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
int xSemaphoreTakeRecursive(SemaphoreHandle_t lock, unsigned ticks);
int xSemaphoreGiveRecursive(SemaphoreHandle_t lock);
typedef struct mock_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(unsigned length, unsigned size);
int xQueueSend(QueueHandle_t queue, const void *item, unsigned ticks);
int xQueueReceive(QueueHandle_t queue, void *item, unsigned ticks);
int64_t esp_timer_get_time(void);
int esp_crt_bundle_attach(void *config);
typedef int esp_http_client_method_t;
#define HTTP_METHOD_POST 1
#define HTTP_METHOD_GET 0
typedef struct {
    const char *url;
    int timeout_ms, buffer_size, buffer_size_tx;
    bool keep_alive_enable, disable_auto_redirect;
    int (*crt_bundle_attach)(void *);
} esp_http_client_config_t;
typedef void *esp_http_client_handle_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
int esp_http_client_set_method(void *client, int method);
int esp_http_client_set_header(void *client, const char *key, const char *value);
int esp_http_client_open(void *client, int length);
int esp_http_client_write(void *client, const char *data, int length);
int esp_http_client_fetch_headers(void *client);
int esp_http_client_read(void *client, char *out, int size);
int esp_http_client_get_status_code(void *client);
int esp_http_client_close(void *client);
int esp_http_client_cleanup(void *client);

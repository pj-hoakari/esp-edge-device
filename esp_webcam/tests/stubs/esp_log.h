#pragma once

void cloud_test_log(const char *level, const char *tag, const char *fmt, ...);

#define ESP_LOGE(...) cloud_test_log("E", __VA_ARGS__)
#define ESP_LOGW(...) cloud_test_log("W", __VA_ARGS__)
#define ESP_LOGI(...) cloud_test_log("I", __VA_ARGS__)
#define ESP_LOGD(...) cloud_test_log("D", __VA_ARGS__)

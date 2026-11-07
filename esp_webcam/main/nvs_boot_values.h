#pragma once

#include <stdbool.h>

typedef enum {
    NVS_BOOT_PLAIN = 0,
    NVS_BOOT_SECURE_EXISTING,
    NVS_BOOT_SECURE_GENERATE,
} nvs_boot_action_t;

typedef enum {
    NVS_BOOT_KEYS_READ_OK = 0,
    NVS_BOOT_KEYS_READ_MISSING,
    NVS_BOOT_KEYS_READ_ERROR,
} nvs_boot_keys_state_t;

nvs_boot_action_t nvs_boot_decide(bool encryption_enabled, bool key_partition_found,
                                  nvs_boot_keys_state_t keys_state);

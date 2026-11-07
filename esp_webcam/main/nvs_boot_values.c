#include "nvs_boot_values.h"

nvs_boot_action_t nvs_boot_decide(bool encryption_enabled, bool key_partition_found,
                                  nvs_boot_keys_state_t keys_state)
{
    if (!encryption_enabled || !key_partition_found) return NVS_BOOT_PLAIN;
    switch (keys_state) {
    case NVS_BOOT_KEYS_READ_OK:
        return NVS_BOOT_SECURE_EXISTING;
    case NVS_BOOT_KEYS_READ_MISSING:
        return NVS_BOOT_SECURE_GENERATE;
    case NVS_BOOT_KEYS_READ_ERROR:
    default:
        return NVS_BOOT_PLAIN;
    }
}

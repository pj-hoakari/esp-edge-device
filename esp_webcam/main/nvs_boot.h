#pragma once

#include "esp_err.h"

/* Initialize the default NVS partition, using the nvs_keys partition when
 * encryption is enabled. Falls back to unencrypted NVS when encryption cannot
 * be used so that existing unencrypted devices remain bootable. */
esp_err_t nvs_boot_init(void);

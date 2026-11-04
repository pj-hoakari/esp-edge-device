#include "nvs_boot.h"
#include "nvs_boot_values.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

static const char *TAG = "nvs_boot";

static esp_err_t plain_init_with_retry(void);

static esp_err_t fail_or_plain(esp_err_t cause)
{
#if CONFIG_WEBCAM_NVS_REQUIRE_ENCRYPTION
    ESP_LOGE(TAG, "NVS encryption is required but unavailable; refusing plaintext NVS");
    return cause == ESP_OK ? ESP_FAIL : cause;
#else
    return plain_init_with_retry();
#endif
}

static esp_err_t plain_init_with_retry(void)
{
    /* nvs_flash_init() follows CONFIG_NVS_ENCRYPTION and would try the
     * configured key protection scheme. Use the partition API directly so the
     * fallback is always unencrypted. */
    esp_err_t err = nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Unencrypted NVS init failed (%s); erasing NVS and retrying",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init_partition(NVS_DEFAULT_PART_NAME);
    }
    return err;
}

#if CONFIG_WEBCAM_NVS_ENCRYPTION

static nvs_boot_keys_state_t classify_read_result(esp_err_t err)
{
    if (err == ESP_OK) return NVS_BOOT_KEYS_READ_OK;
    if (err == ESP_ERR_NVS_KEYS_NOT_INITIALIZED) return NVS_BOOT_KEYS_READ_MISSING;
    return NVS_BOOT_KEYS_READ_ERROR;
}

static esp_err_t secure_init_with_retry(nvs_sec_cfg_t *cfg)
{
    esp_err_t err = nvs_flash_secure_init(cfg);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Encrypted NVS init failed (%s); erasing NVS and retrying",
                 esp_err_to_name(err));
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_secure_init(cfg);
    }
    return err;
}

static void wipe_cfg(nvs_sec_cfg_t *cfg)
{
    volatile unsigned char *p = (volatile unsigned char *)cfg;
    for (size_t i = 0; i < sizeof(*cfg); i++) p[i] = 0;
}

esp_err_t nvs_boot_init(void)
{
#if defined(CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC) && CONFIG_NVS_SEC_KEY_PROTECT_USING_HMAC
    /* The NVS security provider derives the keys from an eFuse HMAC key.
     * Do not read the legacy plaintext nvs_keys partition in this profile. */
    esp_err_t hmac_err = nvs_flash_init();
    if (hmac_err == ESP_OK) {
        ESP_LOGI(TAG, "NVS is encrypted with the eFuse HMAC key");
        return ESP_OK;
    }
    ESP_LOGE(TAG, "HMAC-backed NVS init failed (%s)", esp_err_to_name(hmac_err));
    return fail_or_plain(hmac_err);
#else
    const esp_partition_t *keys = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS_KEYS, NULL);
    nvs_sec_cfg_t cfg = {0};
    esp_err_t read_err = ESP_ERR_NOT_FOUND;
    if (keys) read_err = nvs_flash_read_security_cfg(keys, &cfg);

    nvs_boot_action_t action = nvs_boot_decide(true, keys != NULL, classify_read_result(read_err));
    esp_err_t err = ESP_OK;

    switch (action) {
    case NVS_BOOT_SECURE_GENERATE:
        ESP_LOGW(TAG, "NVS key partition is blank; generating encryption keys");
        err = nvs_flash_generate_keys(keys, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Could not generate NVS keys (%s)", esp_err_to_name(err));
            wipe_cfg(&cfg);
            return fail_or_plain(err);
        }
        /* fall through */
    case NVS_BOOT_SECURE_EXISTING:
        err = secure_init_with_retry(&cfg);
        wipe_cfg(&cfg);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "NVS is encrypted");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "Encrypted NVS init failed (%s)", esp_err_to_name(err));
        return fail_or_plain(err);
    case NVS_BOOT_PLAIN:
    default:
        if (!keys) {
            ESP_LOGW(TAG, "No nvs_keys partition; using unencrypted NVS. "
                     "Flash the custom partition table to enable NVS encryption.");
        } else {
            ESP_LOGW(TAG, "Could not read NVS keys (%s); using unencrypted NVS. "
                     "Erase flash and reprovision to enable NVS encryption.",
                     esp_err_to_name(read_err));
        }
        wipe_cfg(&cfg);
        return fail_or_plain(read_err);
    }
#endif
}

#else

esp_err_t nvs_boot_init(void)
{
    ESP_LOGI(TAG, "NVS encryption is disabled; using unencrypted NVS");
    return plain_init_with_retry();
}

#endif

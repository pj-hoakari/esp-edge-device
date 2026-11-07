#include <assert.h>
#include <stdio.h>

#include "../main/nvs_boot_values.h"

int main(void)
{
    assert(nvs_boot_decide(false, false, NVS_BOOT_KEYS_READ_ERROR) == NVS_BOOT_PLAIN);
    assert(nvs_boot_decide(false, true, NVS_BOOT_KEYS_READ_OK) == NVS_BOOT_PLAIN);
    assert(nvs_boot_decide(false, true, NVS_BOOT_KEYS_READ_MISSING) == NVS_BOOT_PLAIN);
    assert(nvs_boot_decide(true, false, NVS_BOOT_KEYS_READ_OK) == NVS_BOOT_PLAIN);
    assert(nvs_boot_decide(true, true, NVS_BOOT_KEYS_READ_OK) == NVS_BOOT_SECURE_EXISTING);
    assert(nvs_boot_decide(true, true, NVS_BOOT_KEYS_READ_MISSING) == NVS_BOOT_SECURE_GENERATE);
    assert(nvs_boot_decide(true, true, NVS_BOOT_KEYS_READ_ERROR) == NVS_BOOT_PLAIN);
    puts("nvs_boot_test: ok");
    return 0;
}

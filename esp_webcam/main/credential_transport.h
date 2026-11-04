#pragma once
#include <stdbool.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

/* Check again at the HTTP boundary: persisted settings can predate policy. */
static inline bool credential_url_allowed(const char *url)
{
    if (!url) return false;
    if (!strncmp(url, "https://", 8)) return true;
#if defined(CONFIG_WEBCAM_ALLOW_INSECURE_HTTP) && CONFIG_WEBCAM_ALLOW_INSECURE_HTTP
    if (!strncmp(url, "http://", 7)) return true;
#endif
    return false;
}

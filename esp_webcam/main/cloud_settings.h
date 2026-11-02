#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOUD_EDGE_ID_SIZE 33
#define CLOUD_PROJECT_SIZE 65
#define CLOUD_API_KEY_SIZE 129
#define CLOUD_HOST_SIZE 129
#define CLOUD_TOKEN_SIZE 2048
#define CLOUD_TENANT_ID_SIZE 65
#define CLOUD_EVENT_ID_SIZE 65

typedef struct {
    uint32_t version;
    char edge_device_id[CLOUD_EDGE_ID_SIZE];
    char firebase_project_id[CLOUD_PROJECT_SIZE];
    char firebase_api_key[CLOUD_API_KEY_SIZE];
    /* Empty selects the production endpoints. Set host:port for emulators. */
    char firestore_emulator_host[CLOUD_HOST_SIZE];
    char auth_emulator_host[CLOUD_HOST_SIZE];
    char event_access[CLOUD_TOKEN_SIZE];
    char tenant_id[CLOUD_TENANT_ID_SIZE];
    char event_id[CLOUD_EVENT_ID_SIZE];
    int64_t event_access_expires_at;
} cloud_settings_t;

/* Loads the saved cloud profile, falling back to the Kconfig defaults. */
void cloud_settings_load(void);
const cloud_settings_t *cloud_settings_active(void);
const cloud_settings_t *cloud_settings_saved(void);

/* Validates the non-token fields. The token is validated by parsing it. */
bool cloud_settings_valid(const cloud_settings_t *settings);
/* Parses the event_access JWT and fills tenant/event/expiry. */
bool cloud_settings_apply_token(cloud_settings_t *settings, const char *token);
/* Saves the profile to NVS. Does not take effect until reboot. */
bool cloud_settings_save(const cloud_settings_t *settings);

/* True when the profile has everything needed to sign in to Firebase. */
bool cloud_settings_ready(void);

/* Prints the profile without exposing the event_access token. */
void cloud_settings_show(const char *label, const cloud_settings_t *settings);

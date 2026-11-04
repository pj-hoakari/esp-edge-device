#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CLOUD_SETTINGS_VERSION 2
#define CLOUD_EDGE_ID_SIZE 33
#define CLOUD_PROJECT_SIZE 65
#define CLOUD_API_KEY_SIZE 129
#define CLOUD_HOST_SIZE 129
#define CLOUD_TOKEN_SIZE 2048
#define CLOUD_REFRESH_TOKEN_SIZE 1024
#define CLOUD_TENANT_ID_SIZE 65
#define CLOUD_EVENT_ID_SIZE 65
#define CLOUD_IDP_URL_SIZE 192
#define CLOUD_TOKEN_TYPE_SIZE 16
#define CLOUD_DEVICE_CODE_SIZE 512
#define CLOUD_USER_CODE_SIZE 32
#define CLOUD_VERIFY_URI_SIZE 256

typedef enum {
    CLOUD_AUTH_NONE = 0,
    CLOUD_AUTH_PENDING,
    CLOUD_AUTH_APPROVED,
    CLOUD_AUTH_REAPPROVAL_REQUIRED,
} cloud_authorization_t;

typedef enum {
    CLOUD_TOKEN_STATE_NONE = 0,
    CLOUD_TOKEN_STATE_VALID,
    CLOUD_TOKEN_STATE_EXPIRED,
    CLOUD_TOKEN_STATE_REJECTED,
} cloud_token_state_t;

typedef struct {
    uint32_t version;
    char edge_device_id[CLOUD_EDGE_ID_SIZE];
    char firebase_project_id[CLOUD_PROJECT_SIZE];
    char firebase_api_key[CLOUD_API_KEY_SIZE];
    /* Empty selects the production endpoints. For emulators use
     * https://host[:port]. Bare hosts and HTTP require the development flag. */
    char firestore_emulator_host[CLOUD_HOST_SIZE];
    char auth_emulator_host[CLOUD_HOST_SIZE];
    char idp_url[CLOUD_IDP_URL_SIZE];
    char event_access[CLOUD_TOKEN_SIZE];
    char refresh_token[CLOUD_REFRESH_TOKEN_SIZE];
    char token_type[CLOUD_TOKEN_TYPE_SIZE];
    char tenant_id[CLOUD_TENANT_ID_SIZE];
    char event_id[CLOUD_EVENT_ID_SIZE];
    int64_t event_access_expires_at;
    int64_t event_access_obtained_at;
    int64_t event_access_expires_in;
    int64_t refresh_token_expires_at;
    int64_t grant_expires_at;
    uint32_t authorization_state;
    uint32_t token_state;
    uint8_t refresh_in_progress;
    char device_code[CLOUD_DEVICE_CODE_SIZE];
    char user_code[CLOUD_USER_CODE_SIZE];
    char verification_uri[CLOUD_VERIFY_URI_SIZE];
    int64_t device_code_expires_at;
    uint32_t poll_interval_s;
} cloud_settings_t;

/* Exact v1 NVS layout. Never reinterpret a v1 blob as v2. */
typedef struct {
    uint32_t version;
    char edge_device_id[CLOUD_EDGE_ID_SIZE];
    char firebase_project_id[CLOUD_PROJECT_SIZE];
    char firebase_api_key[CLOUD_API_KEY_SIZE];
    char firestore_emulator_host[CLOUD_HOST_SIZE];
    char auth_emulator_host[CLOUD_HOST_SIZE];
    char event_access[CLOUD_TOKEN_SIZE];
    char tenant_id[CLOUD_TENANT_ID_SIZE];
    char event_id[CLOUD_EVENT_ID_SIZE];
    int64_t event_access_expires_at;
} cloud_settings_v1_t;

typedef struct {
    char tenant_id[CLOUD_TENANT_ID_SIZE];
    char event_id[CLOUD_EVENT_ID_SIZE];
    char edge_device_id[CLOUD_EDGE_ID_SIZE];
    int64_t expires_at;
} cloud_token_claims_t;

/* Loads the saved cloud profile, falling back to the Kconfig defaults. */
void cloud_settings_load(void);
const cloud_settings_t *cloud_settings_active(void);
const cloud_settings_t *cloud_settings_saved(void);

/* Validates the non-token fields. Tokens are validated by parsing them. */
bool cloud_settings_valid(const cloud_settings_t *settings);
/* Parses and validates an event_access JWT and fills tenant/event/expiry. */
bool cloud_settings_apply_token(cloud_settings_t *settings, const char *token);
/* Saves the profile for the next boot. Does not change the active profile. */
bool cloud_settings_save(const cloud_settings_t *settings);
/* Saves and makes the profile active immediately (used for tokens). */
bool cloud_settings_store(const cloud_settings_t *settings);
/* Updates only the in-memory profile, for fail-closed state when NVS fails. */
void cloud_settings_activate(const cloud_settings_t *settings);

/* True when the profile has everything needed to obtain an event token. */
bool cloud_settings_configured(void);

const char *cloud_authorization_name(const cloud_settings_t *settings);

/* Prints the profile without exposing tokens or secrets. */
void cloud_settings_show(const char *label, const cloud_settings_t *settings);

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cloud_settings.h"

typedef struct {
    char device_code[CLOUD_DEVICE_CODE_SIZE];
    char user_code[CLOUD_USER_CODE_SIZE];
    char verification_uri[CLOUD_VERIFY_URI_SIZE];
    int64_t expires_in;
    uint32_t interval;
} device_auth_start_t;

typedef struct {
    char access_token[CLOUD_TOKEN_SIZE];
    char refresh_token[CLOUD_REFRESH_TOKEN_SIZE];
    char token_type[CLOUD_TOKEN_TYPE_SIZE];
    int64_t expires_in;
    int64_t refresh_expires_in;
    int64_t grant_expires_in;
} device_auth_tokens_t;

typedef enum {
    DEVICE_AUTH_OK = 0,
    DEVICE_AUTH_PENDING,
    DEVICE_AUTH_SLOW_DOWN,
    DEVICE_AUTH_EXPIRED,
    DEVICE_AUTH_DENIED,
    DEVICE_AUTH_INVALID_GRANT,
    DEVICE_AUTH_INVALID,
} device_auth_result_t;

bool device_auth_parse_start(const char *json, device_auth_start_t *out);
device_auth_result_t device_auth_parse_token(const char *json, device_auth_tokens_t *out);

/* Formats the unsolicited device_authorization line. Never includes a token. */
bool device_auth_format_start(const device_auth_start_t *start, char *out, size_t size);

bool device_auth_state_begin(cloud_settings_t *settings, const device_auth_start_t *start,
                             int64_t now);
bool device_auth_state_approved(cloud_settings_t *settings, const device_auth_tokens_t *tokens,
                                int64_t now);
bool device_auth_state_reapproval(cloud_settings_t *settings);
void device_auth_state_clear_pending(cloud_settings_t *settings);

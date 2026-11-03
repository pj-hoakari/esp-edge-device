#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cloud_settings.h"

/* An issuer base URL is an HTTP(S) origin without a path, query, fragment,
 * credentials or trailing slash. */
bool cloud_idp_url_valid(const char *url);

/* Copies a v2 blob or migrates the exact v1 layout into out. Returns false for
 * unknown versions and sizes so an old blob is never reinterpreted. */
bool cloud_settings_migrate(const void *blob, size_t size, cloud_settings_t *out);

/* Parses an event_access JWT and validates its claims. The token is never
 * logged. When expected_edge_device_id is non-empty, a present edge_device_id
 * claim must match it. */
bool cloud_token_parse(const char *token, const char *expected_edge_device_id,
                       int64_t now, cloud_token_claims_t *out);

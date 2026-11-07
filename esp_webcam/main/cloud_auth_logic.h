#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "cloud_settings.h"
#include "firebase_client.h"

typedef enum {
    CLOUD_SESSION_USE = 0,
    CLOUD_SESSION_REFRESH,
    CLOUD_SESSION_REISSUE,
} cloud_session_action_t;

/* True when the event_access used for the Firebase custom token differs from
 * the active device authorization (for example after a token refresh). */
bool cloud_event_access_rotated(const firebase_session_t *session,
                                const cloud_settings_t *settings);

/* Chooses how to obtain a usable Firebase session. Firebase ID token refresh is
 * only allowed while the event_access generation is unchanged. */
cloud_session_action_t cloud_session_next_action(const firebase_session_t *session,
                                                 const cloud_settings_t *settings,
                                                 int64_t now);

/* Firestore rejected the Firebase ID token or its custom claims. */
bool cloud_firestore_auth_failed(int status);

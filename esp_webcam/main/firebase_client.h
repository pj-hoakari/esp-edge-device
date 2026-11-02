#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cloud_settings.h"

#define FIREBASE_TOKEN_SIZE 4096
#define FIREBASE_REFRESH_SIZE 512

typedef struct {
    char id_token[FIREBASE_TOKEN_SIZE];
    char refresh_token[FIREBASE_REFRESH_SIZE];
    int64_t id_token_expires_at;
} firebase_session_t;

/* Calls tolo-edge-bridge IssueFirestoreToken with the pre-provisioned
 * event_access and returns the Firebase custom token. */
bool firebase_issue_custom_token(const char *signaling_url, const char *event_access,
                                 char *custom_token, size_t size);

bool firebase_sign_in(const cloud_settings_t *settings, const char *custom_token,
                      firebase_session_t *session);
bool firebase_refresh(const cloud_settings_t *settings, firebase_session_t *session);

/* GET a Firestore collection. collection_path is relative to documents/, e.g.
 * "tenants/t/events/e/edgeDevices/d/sessions". */
bool firebase_firestore_list(const cloud_settings_t *settings,
                             const firebase_session_t *session,
                             const char *collection_path, const char *query,
                             char *out, size_t out_size, int *status);

/* POST /documents:batchWrite with a caller-built JSON body. */
bool firebase_firestore_batch_write(const cloud_settings_t *settings,
                                    const firebase_session_t *session,
                                    const char *body, int *status);

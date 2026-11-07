#include "cloud_auth_logic.h"

bool cloud_event_access_rotated(const firebase_session_t *session,
                                const cloud_settings_t *settings)
{
    if (!session || !settings) return true;
    return session->event_access_obtained_at != settings->event_access_obtained_at ||
        session->event_access_expires_at != settings->event_access_expires_at;
}

cloud_session_action_t cloud_session_next_action(const firebase_session_t *session,
                                                 const cloud_settings_t *settings,
                                                 int64_t now)
{
    if (!session || !session->id_token[0]) return CLOUD_SESSION_REISSUE;
    if (cloud_event_access_rotated(session, settings)) return CLOUD_SESSION_REISSUE;
    if (now <= 0) return CLOUD_SESSION_USE;
    if (session->id_token_expires_at > 0 && now + 60 < session->id_token_expires_at)
        return CLOUD_SESSION_USE;
    return session->refresh_token[0] ? CLOUD_SESSION_REFRESH : CLOUD_SESSION_REISSUE;
}

bool cloud_firestore_auth_failed(int status)
{
    return status == 401 || status == 403;
}

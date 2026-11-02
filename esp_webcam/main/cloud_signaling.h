#pragma once

/* Starts the Firestore REST polling task that discovers a console session,
 * receives the SDP offer, and writes the ESP answer. Does nothing unless the
 * cloud profile, event_access token, and Edge Bridge URL are configured. */
void cloud_signaling_start(void);

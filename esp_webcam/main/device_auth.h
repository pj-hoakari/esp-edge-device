#pragma once

#include <stdbool.h>

/* Loads the persisted authorization state. If a refresh was interrupted by a
 * reboot, the device requires reapproval. */
void device_auth_initialize(void);

/* Obtains a usable event_access if possible. Starts or continues Device
 * Authorization and refreshes the token before expiry. Never logs tokens. */
bool device_auth_ensure_access(void);

/* Starts a new Device Authorization or prints the current pending one as a
 * JSON line. Also used by the serial device_authorization command. */
bool device_auth_start(void);

/* Forces a refresh even when the token is not near expiry (e.g. after 401). */
bool device_auth_force_refresh(void);

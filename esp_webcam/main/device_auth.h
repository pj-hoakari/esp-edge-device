#pragma once

#include <stdbool.h>

/* Loads the persisted authorization state. If a refresh was interrupted by a
 * reboot, the device requires reapproval. */
void device_auth_initialize(void);

/* Obtains a usable event_access if possible. Starts or continues Device
 * Authorization and refreshes the token before expiry. Never logs tokens. */
bool device_auth_ensure_access(void);

/* Starts a new Device Authorization or prints the current pending one as a
 * JSON line. Cloud task only; never call from the UART console. */
bool device_auth_start(void);

/* Forces a refresh even when the token is not near expiry (e.g. after 401). */
bool device_auth_force_refresh(void);

/* Cloud worker owns queue initialization and execution. The UART enqueues only.
 * A false request result means worker unavailable or queue full. */
void device_auth_worker_initialize(void);
bool device_auth_request_start(void);
void device_auth_process_requests(void);

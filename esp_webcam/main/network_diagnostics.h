#pragma once
#include <stdbool.h>

/* Live values only; do not store addresses in the network settings blob. */
void network_diagnostics_show(void);
bool network_diagnostics_has_ipv4(void);
/* Three IPv4 ICMP echo requests. Empty target means the current gateway.
 * Called only by the serial console; waits until the three probes finish. */
void network_diagnostics_ping(const char *target);

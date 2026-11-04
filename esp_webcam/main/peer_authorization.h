#pragma once
#include <stdbool.h>
void peer_authorization_revoke(void);
void peer_authorization_allow(void);
bool peer_authorization_blocked(void);
bool peer_authorization_take_disconnect(void);

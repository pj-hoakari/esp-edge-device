#include "peer_authorization.h"
#include <stdatomic.h>

static atomic_bool blocked;
static atomic_bool disconnect_pending;

void peer_authorization_revoke(void)
{
    atomic_store(&blocked, true);
    atomic_store(&disconnect_pending, true);
}

void peer_authorization_allow(void)
{
    atomic_store(&blocked, false);
}

bool peer_authorization_blocked(void)
{
    return atomic_load(&blocked) || atomic_load(&disconnect_pending);
}

bool peer_authorization_take_disconnect(void)
{
    return atomic_exchange(&disconnect_pending, false);
}

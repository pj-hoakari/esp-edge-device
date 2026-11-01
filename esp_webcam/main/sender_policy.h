#pragma once
#include <stdint.h>

/* Keep the pacing clock independent of task wakeup jitter. Skip missed slots
 * instead of building a backlog, but do not add work time to every period. */
static inline int64_t sender_next_slot(int64_t slot, int64_t now, int64_t period)
{
    if (!slot) return now + period;
    return slot + ((now - slot) / period + 1) * period;
}

#define SENDER_MIN_FPS 6
#define SENDER_FRAME_BUDGET_US (1000000LL / SENDER_MIN_FPS * 3 / 2)
#define SENDER_BURST_CHUNKS 4
#define SENDER_BURST_US 4000

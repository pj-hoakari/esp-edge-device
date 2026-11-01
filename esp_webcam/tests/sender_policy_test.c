#include <assert.h>
#include <stdio.h>
#include "../main/sender_policy.h"
#include "../main/udp_retry.h"

static int attempts, pauses, wait_ms, recover_after, failure;
static int fake_send(void *ctx)
{
    assert(ctx == &attempts);
    attempts++;
    if (attempts > recover_after) return 1200;
    errno = failure;
    return -1;
}
static void fake_pause(unsigned ms) { pauses++; wait_ms += ms; }
static void check_udp(int after, int err, int expected, int expected_attempts, int expected_wait)
{
    attempts = pauses = wait_ms = 0; recover_after = after; failure = err;
    assert(webcam_udp_send_bounded(fake_send, fake_pause, &attempts) == expected);
    assert(attempts == expected_attempts);
    assert(wait_ms == expected_wait && pauses <= 2);
}

int main(void)
{
    assert(SENDER_MIN_FPS == 6);
    assert(sender_next_slot(0, 123, 100000) == 100123);
    assert(sender_next_slot(100000, 100001, 100000) == 200000);
    assert(sender_next_slot(100000, 450001, 100000) == 500000);
    /* Scheduler and send work take 1-7 ms per wakeup. None of that latency
     * may accumulate into the next pacing deadline. */
    for (int fps = 6; fps <= 15; fps++) {
        int64_t period = 1000000 / fps, slot = period;
        for (int i = 0; i < 10000; i++) {
            int64_t now = slot + (i % 7 + 1) * 1000;
            int64_t next = sender_next_slot(slot, now, period);
            assert(next == slot + period);
            assert(next > now && next <= now + period);
            slot = next;
        }
        int64_t stalled = slot + period * 9 + 2000;
        int64_t next = sender_next_slot(slot, stalled, period);
        assert(next == slot + period * 10);
    }
    check_udp(0, ENOBUFS, 1200, 1, 0);
    check_udp(1, ENOBUFS, 1200, 2, 1);
    check_udp(2, ENOMEM, 1200, 3, 2);
    check_udp(99, EAGAIN, WEBCAM_UDP_RETRY, 3, 2);
    check_udp(99, ENOBUFS, WEBCAM_UDP_RETRY, 3, 2);
    check_udp(99, EINVAL, -1, 1, 0);
    puts("sender pacing and bounded UDP backpressure tests passed");
    return 0;
}

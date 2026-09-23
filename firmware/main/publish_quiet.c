/* publish_quiet.c — see publish_quiet.h for the module comment. */
#include "publish_quiet.h"

void publish_quiet_gate_init(publish_quiet_gate_t *g)
{
    g->in_flight = 0;
    g->quiet_until_us = 0;
    g->issued_us = 0;
}

void publish_quiet_gate_issued(publish_quiet_gate_t *g, int64_t now_us)
{
    // See this function's own doc comment in publish_quiet.h for why the
    // second condition (an already-expired hold) also resets issued_us, not
    // just the in_flight==0 case: it is what keeps a stale in-flight count
    // left behind by a lost URC from denying THIS publish its own hold
    // window.
    if (g->in_flight == 0 || (now_us - g->issued_us) >= PUBLISH_SLEEP_HOLD_MAX_US) {
        g->issued_us = now_us;
    }
    g->in_flight++;
}

void publish_quiet_gate_done(publish_quiet_gate_t *g, int64_t now_us)
{
    if (g->in_flight > 0) {
        g->in_flight--;
    }
    if (g->in_flight == 0) {
        g->quiet_until_us = now_us + PUBLISH_QUIET_WINDOW_US;
    } else {
        // Still >0 outstanding: refresh the "oldest outstanding" estimate to
        // this done() call's own timestamp rather than tracking a real FIFO
        // of issue times (simplest option, per publish_quiet.h's doc
        // comment) -- slightly under-counts how long the remaining
        // publish(es) have actually been outstanding, which only ever makes
        // publish_quiet_gate_hold_sleep() give up SOONER, never later, so it
        // cannot reintroduce the "pinned awake" failure mode.
        g->issued_us = now_us;
    }
}

bool publish_quiet_gate_should_wait(const publish_quiet_gate_t *g, int64_t now_us)
{
    if (g->in_flight > 0) {
        return true;
    }
    return now_us < g->quiet_until_us;
}

bool publish_quiet_gate_hold_sleep(const publish_quiet_gate_t *g, int64_t now_us)
{
    if (g->in_flight == 0) {
        return false;
    }
    return (now_us - g->issued_us) < PUBLISH_SLEEP_HOLD_MAX_US;
}

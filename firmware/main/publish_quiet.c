/* publish_quiet.c — see publish_quiet.h for the module comment. */
#include "publish_quiet.h"

void publish_quiet_gate_init(publish_quiet_gate_t *g)
{
    g->in_flight = 0;
    g->quiet_until_us = 0;
}

void publish_quiet_gate_issued(publish_quiet_gate_t *g)
{
    g->in_flight++;
}

void publish_quiet_gate_done(publish_quiet_gate_t *g, int64_t now_us)
{
    if (g->in_flight > 0) {
        g->in_flight--;
    }
    if (g->in_flight == 0) {
        g->quiet_until_us = now_us + PUBLISH_QUIET_WINDOW_US;
    }
}

bool publish_quiet_gate_should_wait(const publish_quiet_gate_t *g, int64_t now_us)
{
    if (g->in_flight > 0) {
        return true;
    }
    return now_us < g->quiet_until_us;
}

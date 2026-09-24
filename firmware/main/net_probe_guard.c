/* net_probe_guard.c — see net_probe_guard.h for the module comment. */
#include "net_probe_guard.h"

void net_probe_guard_init(net_probe_guard_t *g)
{
    g->outstanding = false;
    g->wakes_waited = 0;
    g->issued = 0;
    g->answered = 0;
    g->stuck = 0;
    g->noqueue = 0;
}

bool net_probe_guard_poll(net_probe_guard_t *g)
{
    if (!g->outstanding) {
        return true;
    }
    g->wakes_waited++;
    if (g->wakes_waited > NET_PROBE_GUARD_STUCK_WAKES) {
        g->stuck++;
        g->outstanding = false;
        g->wakes_waited = 0;
    }
    return false;
}

void net_probe_guard_attempt(net_probe_guard_t *g)
{
    g->outstanding = true;
    g->wakes_waited = 0;
}

void net_probe_guard_issued(net_probe_guard_t *g)
{
    g->issued++;
}

void net_probe_guard_noqueue(net_probe_guard_t *g)
{
    g->outstanding = false;
    g->wakes_waited = 0;
    g->noqueue++;
}

void net_probe_guard_failed(net_probe_guard_t *g)
{
    g->outstanding = false;
    g->wakes_waited = 0;
    g->stuck++;
}

void net_probe_guard_answered(net_probe_guard_t *g)
{
    g->outstanding = false;
    g->wakes_waited = 0;
    g->answered++;
}

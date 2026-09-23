/* net_connect_guard.c — see net_connect_guard.h for the module comment. */
#include "net_connect_guard.h"

void net_connect_guard_init(net_connect_guard_t *g)
{
    g->wait = false;
    g->issued_us = 0;
    g->fail_streak = 0;
}

void net_connect_guard_issued(net_connect_guard_t *g, int64_t now_us)
{
    g->wait = true;
    g->issued_us = now_us;
    g->fail_streak = 0;
}

void net_connect_guard_clear(net_connect_guard_t *g)
{
    g->wait = false;
}

bool net_connect_guard_check_timeout(net_connect_guard_t *g, int64_t now_us)
{
    if (!g->wait) {
        return false;
    }
    if ((now_us - g->issued_us) < NET_CONNECT_TIMEOUT_US) {
        return false;
    }
    g->wait = false; /* one-shot, same discipline as net.cpp's own s_resub_wait timeout */
    return true;
}

bool net_connect_guard_in_flight(const net_connect_guard_t *g)
{
    return g->wait;
}

void net_connect_guard_note_fail(net_connect_guard_t *g)
{
    g->fail_streak++;
}

bool net_connect_guard_should_escalate(const net_connect_guard_t *g)
{
    return g->fail_streak >= NET_SESSION_UP_FAIL_ESCALATE;
}

void net_connect_guard_reset_fail_streak(net_connect_guard_t *g)
{
    g->fail_streak = 0;
}

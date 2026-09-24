/* net_probe_guard.h — pure decision logic for S1's per-wake URC drain probe
 * single-slot in-flight guard (docs/SLEEP_URC_DESIGN.md §3(a)/§5).
 *
 * Split the same way net_connect_guard.h is (see that header's own module
 * comment): no ESP-IDF dependency. Unlike net_connect_guard's 30s timeout
 * this needs no clock at all -- "an outstanding probe not answered within 3
 * wake intervals is stuck" is a call count, not a time bound, so
 * firmware/host/test_net_probe_guard.c can run every transition without a
 * device or a fake clock. net.cpp is the only caller; it owns the actual
 * WalterModem::checkComm() call and reads the actual result out of the
 * callback -- this module only decides whether to attempt one this wake and
 * when to give up on one already outstanding.
 *
 * checkComm()'s async contract (WalterDefines.h's _runCmd/_returnAfterReply
 * macros) can invoke the user callback SYNCHRONOUSLY, from inside the
 * checkComm() call itself, if the command could not be queued at all
 * (8-slot queue/pool full) -- checkComm()'s own return value cannot tell
 * this apart from a genuinely queued probe. That is why "mark an attempt"
 * (net_probe_guard_attempt(), before the call) and "count it issued"
 * (net_probe_guard_issued(), after the call, only if still outstanding) are
 * two separate steps: a synchronous noqueue() in between must be able to
 * undo the attempt before it is ever counted as issued.
 */
#ifndef NET_PROBE_GUARD_H
#define NET_PROBE_GUARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* An outstanding probe not answered within this many further
 * net_probe_guard_poll() calls (wake intervals) is declared stuck
 * (docs/SLEEP_URC_DESIGN.md §5(3)(i)). */
#define NET_PROBE_GUARD_STUCK_WAKES 3u

typedef struct {
    bool outstanding;       /* an attempt is in flight, waiting on probe_cb() */
    uint32_t wakes_waited;  /* wake intervals since the attempt, while outstanding */
    uint32_t issued;        /* probes genuinely queued (checkComm() accepted them) */
    uint32_t answered;      /* queued probes whose "OK" came back */
    uint32_t stuck;         /* probes given up on after NET_PROBE_GUARD_STUCK_WAKES wakes unanswered */
    uint32_t noqueue;       /* checkComm() could not queue the probe at all, no retry, not counted as issued */
} net_probe_guard_t;

void net_probe_guard_init(net_probe_guard_t *g);

/* Call exactly once per wake, before deciding whether to attempt a probe.
 * Returns true if no probe is currently outstanding (the caller may attempt
 * one this wake -- see net_probe_guard_attempt()). Returns false if a probe
 * is already outstanding; in that case this call also ages it by one wake
 * interval and, if it has now gone unanswered for more than
 * NET_PROBE_GUARD_STUCK_WAKES calls, declares it stuck (counts `stuck`,
 * clears `outstanding`) so the very next poll() returns true again -- never
 * two true results in a row for the same probe (single-slot, "no double
 * queue"). */
bool net_probe_guard_poll(net_probe_guard_t *g);

/* Call after poll()==true, BEFORE the actual checkComm() call: marks an
 * attempt outstanding (but does not yet count it `issued`) so a synchronous
 * callback fired from inside that same checkComm() call (the queue-full
 * case) sees `outstanding` already true and can undo it via
 * net_probe_guard_noqueue(). */
void net_probe_guard_attempt(net_probe_guard_t *g);

/* Call right after the checkComm() call returns, only if `outstanding` is
 * still true at that point (i.e. no synchronous net_probe_guard_noqueue()
 * ran) -- counts the attempt as genuinely `issued`. */
void net_probe_guard_issued(net_probe_guard_t *g);

/* Call from the probe's own callback when checkComm() could not queue the
 * probe at all (synchronous NO_MEMORY failure) -- undoes the
 * net_probe_guard_attempt() call (so it is never counted `issued`) and
 * counts `noqueue` instead, no retry this wake. */
void net_probe_guard_noqueue(net_probe_guard_t *g);

/* Call from the probe's own callback when the "OK" genuinely came back.
 * Safe to call even if the guard already declared this probe stuck and
 * moved on (docs/SLEEP_URC_DESIGN.md §5(3)(iv): "a probe answered but no
 * page flushed [or arriving late]: indistinguishable and harmless") --
 * counts `answered` regardless of `outstanding`'s current value. */
void net_probe_guard_answered(net_probe_guard_t *g);

#ifdef __cplusplus
}
#endif

#endif /* NET_PROBE_GUARD_H */

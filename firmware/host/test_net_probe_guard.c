/* test_net_probe_guard.c — host test harness for main/net_probe_guard.c
 * (S1, docs/SLEEP_URC_TASKS.md / docs/SLEEP_URC_DESIGN.md §3(a)/§5).
 *
 * Builds with the plain host compiler, no ESP-IDF (net_probe_guard.c has no
 * ESP-IDF dependency at all — see net_probe_guard.h's own module comment).
 *
 * Coverage required by the task brief ("a host test for the guard state
 * machine (in-flight, stuck-after-3, no double queue)"):
 *  - poll() returns true when nothing is outstanding, and attempt() then
 *    marks it outstanding (issued() only counts, does not mark);
 *  - while outstanding, poll() returns false and never offers a second
 *    attempt (no double queue), for exactly NET_PROBE_GUARD_STUCK_WAKES
 *    further calls;
 *  - the (NET_PROBE_GUARD_STUCK_WAKES + 1)th poll() while still unanswered
 *    declares it stuck: counts `stuck`, clears `outstanding`, and the very
 *    next poll() returns true again;
 *  - answered() clears `outstanding` and counts `answered`, at any point
 *    (including after a stuck declaration -- a late answer is harmless);
 *  - noqueue() (the synchronous checkComm() NO_MEMORY case) undoes an
 *    attempt() from the same wake, is counted `noqueue` instead of
 *    `issued`, and leaves the guard ready to try again the very next
 *    poll().
 *
 * NOT host-testable here (see the report): the actual checkComm()/probe_cb()
 * plumbing in net.cpp — WalterModem is a vendor C++ static-method class with
 * no host build, so it cannot be linked or mocked here. That wiring is
 * verified by reading net.cpp's net_urc_probe()/probe_cb() and, ultimately,
 * on the bench (S7's probe_issued/probe_answered/probe_stuck/probe_noqueue
 * report line).
 */
#include "net_probe_guard.h"

#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, ...)                               \
    do {                                                \
        if (!(cond)) {                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            printf(__VA_ARGS__);                        \
            printf("\n");                               \
            g_failures++;                               \
        }                                                \
    } while (0)

/* Fresh guard: poll() offers a probe, attempt() marks it outstanding,
 * issued() counts it, and a second poll() before any answer refuses
 * (single-slot, "no double queue"). */
static void test_issue_and_no_double_queue(void)
{
    net_probe_guard_t g;
    net_probe_guard_init(&g);

    CHECK(net_probe_guard_poll(&g), "fresh guard must offer a probe");
    net_probe_guard_attempt(&g);
    CHECK(g.outstanding, "attempt() must mark outstanding");
    CHECK(g.issued == 0, "attempt() alone must not yet count issued, got %u", (unsigned) g.issued);
    net_probe_guard_issued(&g); /* checkComm() returned with the attempt still outstanding */
    CHECK(g.issued == 1, "issued() must count exactly one issue, got %u", (unsigned) g.issued);

    CHECK(!net_probe_guard_poll(&g), "must not offer a second probe while one is outstanding");
    CHECK(g.issued == 1, "a refused poll() must not itself count as issued");
}

/* An outstanding probe survives exactly NET_PROBE_GUARD_STUCK_WAKES further
 * poll() calls (all false, no stuck yet), then the next one declares it
 * stuck and immediately makes room for a new probe. */
static void test_stuck_after_exactly_n_wakes(void)
{
    net_probe_guard_t g;
    net_probe_guard_init(&g);

    CHECK(net_probe_guard_poll(&g), "fresh guard must offer a probe");
    net_probe_guard_attempt(&g);
    net_probe_guard_issued(&g);

    for (unsigned i = 0; i < NET_PROBE_GUARD_STUCK_WAKES; i++) {
        CHECK(!net_probe_guard_poll(&g), "must still be outstanding at wake %u", i + 1);
        CHECK(g.stuck == 0, "must not be stuck yet at wake %u, stuck=%u", i + 1, (unsigned) g.stuck);
    }

    /* One more unanswered wake past the bound: now it gives up. */
    CHECK(!net_probe_guard_poll(&g),
          "the wake that declares stuck must still return false for THIS call");
    CHECK(g.stuck == 1, "must count exactly one stuck probe, got %u", (unsigned) g.stuck);
    CHECK(!g.outstanding, "declaring stuck must clear outstanding");

    /* Probing resumes immediately after. */
    CHECK(net_probe_guard_poll(&g), "must offer a new probe the wake right after giving up");
}

/* answered() clears outstanding and counts `answered`, whether it arrives
 * promptly or late (after the guard already gave up and moved on --
 * docs/SLEEP_URC_DESIGN.md §5(3)(iv): "indistinguishable and harmless"). */
static void test_answered_clears_and_counts(void)
{
    net_probe_guard_t g;
    net_probe_guard_init(&g);

    net_probe_guard_poll(&g);
    net_probe_guard_attempt(&g);
    net_probe_guard_issued(&g);
    net_probe_guard_answered(&g);
    CHECK(!g.outstanding, "answered() must clear outstanding");
    CHECK(g.answered == 1, "answered() must count exactly one, got %u", (unsigned) g.answered);
    CHECK(net_probe_guard_poll(&g), "must offer a new probe right after an answer");

    /* A late answer after a stuck declaration: harmless, still counts. */
    net_probe_guard_attempt(&g);
    net_probe_guard_issued(&g);
    for (unsigned i = 0; i <= NET_PROBE_GUARD_STUCK_WAKES; i++) {
        net_probe_guard_poll(&g);
    }
    CHECK(g.stuck == 1, "test setup: must have declared exactly one probe stuck");
    net_probe_guard_answered(&g); /* the "lost" OK finally arrives */
    CHECK(g.answered == 2, "a late answer after stuck must still count, got %u", (unsigned) g.answered);
    CHECK(!g.outstanding, "a late answer must not resurrect outstanding");
}

/* noqueue() (checkComm()'s synchronous NO_MEMORY failure, called from inside
 * the same wake's checkComm() call, i.e. between attempt() and issued())
 * undoes the outstanding attempt() and leaves the guard ready to try again
 * the very next wake -- no retry THIS wake, and `issued` must never count a
 * probe that was never really queued. */
static void test_noqueue_undoes_attempt(void)
{
    net_probe_guard_t g;
    net_probe_guard_init(&g);

    CHECK(net_probe_guard_poll(&g), "fresh guard must offer a probe");
    net_probe_guard_attempt(&g);  /* before the real checkComm() call */
    net_probe_guard_noqueue(&g);  /* checkComm() failed synchronously (queue full) -- net.cpp
                                    * never calls issued() in this case */

    CHECK(!g.outstanding, "noqueue() must clear outstanding");
    CHECK(g.noqueue == 1, "noqueue() must count exactly one, got %u", (unsigned) g.noqueue);
    CHECK(g.issued == 0, "a synchronously-failed attempt must never be counted issued, got %u",
          (unsigned) g.issued);
    CHECK(net_probe_guard_poll(&g), "must be free to try again the very next wake");
}

/* failed() (the library's own per-command timeout on the probe's "AT", which
 * patch 1.14 makes a 2 s / 1-attempt budget and therefore a normal outcome)
 * clears outstanding and counts `timedout` -- NOT `noqueue`, which means
 * "checkComm() could not queue it at all" and is what S7 reads, and NOT
 * `stuck`, which modes.c's check_probe_stuck_escalation() escalates to a full
 * F4 modem reset after six in a row (docs/SLEEP_URC_DESIGN.md §9.2: six 2 s
 * timeouts are twelve seconds in ACTIVE mode, and that must not reset the
 * modem). Leaves the guard ready to probe again the very next wake. */
static void test_failed_counts_timedout_not_stuck(void)
{
    net_probe_guard_t g;
    net_probe_guard_init(&g);

    CHECK(net_probe_guard_poll(&g), "fresh guard must offer a probe");
    net_probe_guard_attempt(&g);
    net_probe_guard_issued(&g);
    net_probe_guard_failed(&g); /* WALTER_MODEM_STATE_TIMEOUT after 2 s */

    CHECK(!g.outstanding, "failed() must clear outstanding");
    CHECK(g.timedout == 1, "failed() must count exactly one timedout, got %u", (unsigned) g.timedout);
    CHECK(g.stuck == 0, "failed() must NOT count stuck (it drives the F4 reset), got %u",
          (unsigned) g.stuck);
    CHECK(g.noqueue == 0, "failed() must not touch noqueue, got %u", (unsigned) g.noqueue);
    CHECK(g.answered == 0, "failed() must not count an answer, got %u", (unsigned) g.answered);
    CHECK(g.issued == 1, "failed() must not un-count the issue, got %u", (unsigned) g.issued);
    CHECK(net_probe_guard_poll(&g), "must be free to try again the very next wake");

    /* The same probe must not be counted twice: poll() only ages a probe that
     * is still outstanding, and failed() already cleared it. */
    for (unsigned i = 0; i <= NET_PROBE_GUARD_STUCK_WAKES; i++) {
        net_probe_guard_poll(&g);
    }
    CHECK(g.timedout == 1, "an already-failed probe must not be counted again, timedout=%u",
          (unsigned) g.timedout);
    CHECK(g.stuck == 0, "an already-failed probe must not be aged out into stuck, stuck=%u",
          (unsigned) g.stuck);

    /* Six consecutive 2 s timeouts -- the exact shape of the S7b regression --
     * must leave `stuck` at zero, i.e. must not arm the F4 escalation. */
    net_probe_guard_init(&g);
    for (unsigned i = 0; i < 6; i++) {
        CHECK(net_probe_guard_poll(&g), "wake %u must offer a probe", i);
        net_probe_guard_attempt(&g);
        net_probe_guard_issued(&g);
        net_probe_guard_failed(&g);
    }
    CHECK(g.timedout == 6, "six 2 s timeouts must count six timedout, got %u", (unsigned) g.timedout);
    CHECK(g.stuck == 0, "six 2 s timeouts must leave stuck at 0 (no F4), got %u", (unsigned) g.stuck);
}

int main(void)
{
    test_issue_and_no_double_queue();
    test_stuck_after_exactly_n_wakes();
    test_answered_clears_and_counts();
    test_noqueue_undoes_attempt();
    test_failed_counts_timedout_not_stuck();

    if (g_failures == 0) {
        printf("PASS: net probe guard (in-flight / stuck-after-%u / no double queue), 0 failures\n",
               (unsigned) NET_PROBE_GUARD_STUCK_WAKES);
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

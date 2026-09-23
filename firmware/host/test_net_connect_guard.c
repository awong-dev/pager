/* test_net_connect_guard.c — host test harness for main/net_connect_guard.c
 * (v0.2 bug fixes M1/M3, 22 Sep 2026 outage + evening session).
 *
 * Builds with the plain host compiler, no ESP-IDF (net_connect_guard.c has
 * no ESP-IDF dependency at all — see net_connect_guard.h's own module
 * comment).
 *
 * Coverage required by the task brief:
 *  - the 30s connect timeout fires exactly once, not before its bound
 *  - cleared by SUBSCRIBED or a failed CONNECTED (net_connect_guard_clear(),
 *    no timeout after); a successful CONNECTED does not clear it (net.cpp)
 *  - cleared by net_session_down() (same call again -- net.cpp's
 *    net_session_down() calls net_connect_guard_clear() directly)
 *  - 3 consecutive net_session_up() failures -> should_escalate() true;
 *    a successful issue() (a queued connect) resets the streak
 *  - net_recover_modem()'s full reset resets both wait and the streak
 *    (net_connect_guard_init(), what net.cpp calls there)
 *
 * NOT host-testable here (see the report): the actual ordering guarantee
 * "net_session_down() (AT+SQNSMQTTDISCONNECT) precedes the next
 * AT+SQNSMQTTCFG" is a property of net.cpp's real WalterModem calls, which
 * this pure module knows nothing about — WalterModem is a vendor C++
 * static-method class with no host build, so it cannot be linked or mocked
 * here. That ordering is verified by reading net.cpp's net_service_session()
 * (the net_session_down() call sits directly in the same branch that
 * declares the session dead, before any code path can reach net_session_up()
 * again) and, ultimately, on the bench (M3's own Verify step).
 */
#include "net_connect_guard.h"

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

#define US_PER_S ((int64_t) 1000000)

/* ---------------------------------------------------------------------
 * M1: the connect timeout fires once, not before NET_CONNECT_TIMEOUT_US,
 * and stays false forever after (one-shot) until the next issue().
 * --------------------------------------------------------------------- */
static void test_timeout_fires_once_at_bound(void)
{
    net_connect_guard_t g;
    net_connect_guard_init(&g);

    int64_t t0 = 1000 * US_PER_S;
    net_connect_guard_issued(&g, t0);
    CHECK(net_connect_guard_in_flight(&g), "issued() must mark a connect in flight");

    CHECK(!net_connect_guard_check_timeout(&g, t0), "must not fire at t=0");
    CHECK(!net_connect_guard_check_timeout(&g, t0 + NET_CONNECT_TIMEOUT_US - 1),
          "must not fire 1us before the bound");
    CHECK(net_connect_guard_in_flight(&g), "must still be in flight just before the bound");

    CHECK(net_connect_guard_check_timeout(&g, t0 + NET_CONNECT_TIMEOUT_US),
          "must fire exactly at the bound");
    CHECK(!net_connect_guard_in_flight(&g), "firing must clear in-flight (one-shot)");
    CHECK(!net_connect_guard_check_timeout(&g, t0 + 10 * NET_CONNECT_TIMEOUT_US),
          "must never fire again for the same issue() without a new one");
}

/* No connect ever issued: never in flight, never times out, however long. */
static void test_timeout_does_not_fire_spuriously(void)
{
    net_connect_guard_t g;
    net_connect_guard_init(&g);

    CHECK(!net_connect_guard_in_flight(&g), "fresh guard must not be in flight");
    CHECK(!net_connect_guard_check_timeout(&g, 999999 * US_PER_S),
          "must never fire with nothing outstanding, however long now_us is");
}

/* ---------------------------------------------------------------------
 * Cleared by CONNECTED / SUBSCRIBED / net_session_down() -- net.cpp calls
 * the exact same net_connect_guard_clear() from all three sites, so one
 * test covers all three call sites' effect on this module.
 * --------------------------------------------------------------------- */
static void test_cleared_by_event_or_session_down(void)
{
    net_connect_guard_t g;
    net_connect_guard_init(&g);

    int64_t t0 = 5000 * US_PER_S;
    net_connect_guard_issued(&g, t0);
    net_connect_guard_clear(&g); /* stands in for CONNECTED, SUBSCRIBED, or net_session_down() */
    CHECK(!net_connect_guard_in_flight(&g), "clear() must end in-flight");
    CHECK(!net_connect_guard_check_timeout(&g, t0 + NET_CONNECT_TIMEOUT_US),
          "a cleared connect must never time out, even past the bound");

    /* clear() must also be a safe no-op when nothing is outstanding (every
     * one of the three real call sites can run with wait already false --
     * e.g. a SUBSCRIBED that answers a routine liveness-ping resubscribe,
     * not a fresh connect). */
    net_connect_guard_clear(&g);
    CHECK(!net_connect_guard_in_flight(&g), "clear() on an already-clear guard must stay clear");
}

/* ---------------------------------------------------------------------
 * M3: 3 consecutive net_session_up() failures escalate; a successful
 * issue() resets the streak.
 * --------------------------------------------------------------------- */
static void test_fail_streak_escalates_at_three(void)
{
    net_connect_guard_t g;
    net_connect_guard_init(&g);

    CHECK(!net_connect_guard_should_escalate(&g), "fresh guard must not escalate");

    net_connect_guard_note_fail(&g);
    CHECK(!net_connect_guard_should_escalate(&g), "1 failure must not escalate");
    net_connect_guard_note_fail(&g);
    CHECK(!net_connect_guard_should_escalate(&g), "2 failures must not escalate");
    net_connect_guard_note_fail(&g);
    CHECK(net_connect_guard_should_escalate(&g), "3 failures in a row must escalate");

    /* A successful queue (net_connect_guard_issued()) resets the streak --
     * the modem accepted CONFIG/CONNECT this time, whatever happens next. */
    net_connect_guard_issued(&g, 1 * US_PER_S);
    CHECK(!net_connect_guard_should_escalate(&g), "issued() must reset the fail streak");
    CHECK(g.fail_streak == 0, "issued() must reset fail_streak to exactly 0, got %u",
          (unsigned) g.fail_streak);
}

/* A full modem reset (net_recover_modem(), which calls net_connect_guard_init())
 * must reset both in-flight state and the fail streak. */
static void test_full_reset_clears_both(void)
{
    net_connect_guard_t g;
    net_connect_guard_init(&g);

    net_connect_guard_issued(&g, 42 * US_PER_S);
    net_connect_guard_note_fail(&g);
    net_connect_guard_note_fail(&g);
    net_connect_guard_note_fail(&g);
    CHECK(net_connect_guard_in_flight(&g), "test setup: must be in flight");
    CHECK(net_connect_guard_should_escalate(&g), "test setup: must be at the escalate threshold");

    net_connect_guard_init(&g); /* net.cpp's net_recover_modem() calls this */
    CHECK(!net_connect_guard_in_flight(&g), "full reset must clear in-flight");
    CHECK(!net_connect_guard_should_escalate(&g), "full reset must clear the fail streak");
}

/* net_connect_guard_reset_fail_streak() must reset only the streak, not
 * `wait`/`issued_us` -- used if a caller ever needs to clear the streak
 * without disturbing an outstanding connect (not currently exercised by
 * net.cpp, but part of the module's contract). */
static void test_reset_fail_streak_only(void)
{
    net_connect_guard_t g;
    net_connect_guard_init(&g);

    int64_t t0 = 7 * US_PER_S;
    net_connect_guard_issued(&g, t0);
    net_connect_guard_note_fail(&g);
    net_connect_guard_note_fail(&g);
    net_connect_guard_note_fail(&g);

    net_connect_guard_reset_fail_streak(&g);
    CHECK(!net_connect_guard_should_escalate(&g), "must reset the streak");
    CHECK(net_connect_guard_in_flight(&g), "must not disturb in-flight state");
    CHECK(!net_connect_guard_check_timeout(&g, t0), "must not disturb issued_us either");
}

int main(void)
{
    test_timeout_fires_once_at_bound();
    test_timeout_does_not_fire_spuriously();
    test_cleared_by_event_or_session_down();
    test_fail_streak_escalates_at_three();
    test_full_reset_clears_both();
    test_reset_fail_streak_only();

    if (g_failures == 0) {
        printf("PASS: net connect guard (M1 timeout / M3 fail streak), 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

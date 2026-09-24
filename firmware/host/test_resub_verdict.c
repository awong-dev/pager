/* test_resub_verdict.c — host test harness for main/resub_verdict.c
 * (S2, docs/SLEEP_URC_TASKS.md / docs/SLEEP_URC_DESIGN.md §6, "SUBACK
 * collision").
 *
 * Builds with the plain host compiler, no ESP-IDF (resub_verdict.c has no
 * ESP-IDF dependency at all -- see resub_verdict.h's own module comment).
 *
 * Coverage required by the task brief:
 *  - page-after-ping suppresses the verdict (a /down message ingested after
 *    the re-SUBSCRIBE was sent, even past the 30s bound, proves the session
 *    alive -- RESUB_VERDICT_ALIVE, not DEAD);
 *  - no page and one unanswered ping does not declare the session dead (it
 *    asks for a retry instead -- RESUB_VERDICT_RETRY);
 *  - two unanswered pings (second_try already true) does declare it dead
 *    (RESUB_VERDICT_DEAD);
 *  - nothing outstanding, or not timed out yet, is a no-op either way
 *    (RESUB_VERDICT_NONE), so the routine liveness-ping/resume-repair path
 *    below it in lte_service_session() is never starved by a spurious
 *    verdict.
 *
 * NOT host-testable here (see the report): the actual AT+SQNSMQTTSUBSCRIBE
 * re-send / lte_session_down() plumbing in xport_lte.cpp -- WalterModem is a
 * vendor C++ static-method class with no host build, so it cannot be linked
 * or mocked here. That wiring is verified by reading xport_lte.cpp's
 * lte_service_session() and, ultimately, on the bench (S7: two `page ...
 * received` lines and zero `MQTT session LOST` lines).
 */
#include "resub_verdict.h"

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

/* Nothing outstanding: always NONE, however long `now` is. */
static void test_none_when_not_waiting(void)
{
    resub_verdict_t v = resub_verdict_check(false, 0, 0, false, 999999 * US_PER_S);
    CHECK(v == RESUB_VERDICT_NONE, "must be NONE when wait is false, got %d", (int) v);
}

/* Outstanding but still inside the 30s bound: NONE, whether or not a /down
 * message has ever arrived. */
static void test_none_before_bound(void)
{
    int64_t sent = 1000 * US_PER_S;
    resub_verdict_t v1 = resub_verdict_check(true, sent, 0, false, sent);
    CHECK(v1 == RESUB_VERDICT_NONE, "must be NONE at t=0, got %d", (int) v1);

    resub_verdict_t v2 = resub_verdict_check(true, sent, 0, false, sent + RESUB_VERDICT_TIMEOUT_US - 1);
    CHECK(v2 == RESUB_VERDICT_NONE, "must be NONE 1us before the bound, got %d", (int) v2);

    /* Even with downlink proof, still NONE before the bound -- the verdict
     * only exists to interpret a timeout that has actually happened. */
    resub_verdict_t v3 =
        resub_verdict_check(true, sent, sent + 1, false, sent + RESUB_VERDICT_TIMEOUT_US - 1);
    CHECK(v3 == RESUB_VERDICT_NONE, "must be NONE before the bound even with downlink proof, got %d",
          (int) v3);
}

/* Page-after-ping suppresses the verdict: a /down message ingested after
 * `sent_us`, however the second_try flag reads, is ALIVE not DEAD. */
static void test_page_after_ping_is_alive(void)
{
    int64_t sent = 2000 * US_PER_S;
    int64_t now = sent + RESUB_VERDICT_TIMEOUT_US + 1;

    resub_verdict_t v1 = resub_verdict_check(true, sent, sent + 1, false, now);
    CHECK(v1 == RESUB_VERDICT_ALIVE, "a /down message just after sent_us must be ALIVE, got %d",
          (int) v1);

    resub_verdict_t v2 = resub_verdict_check(true, sent, sent + 1, true, now);
    CHECK(v2 == RESUB_VERDICT_ALIVE, "ALIVE must win even on an already-second-try ping, got %d",
          (int) v2);

    /* A /down message from BEFORE this re-SUBSCRIBE was sent (a stale one
     * from an earlier cycle) must NOT count. */
    resub_verdict_t v3 = resub_verdict_check(true, sent, sent - 1, false, now);
    CHECK(v3 != RESUB_VERDICT_ALIVE, "a /down message from before sent_us must not count, got %d",
          (int) v3);
}

/* No page and one unanswered ping: RETRY, not DEAD. */
static void test_one_unanswered_ping_retries(void)
{
    int64_t sent = 3000 * US_PER_S;
    int64_t now = sent + RESUB_VERDICT_TIMEOUT_US + 1;

    resub_verdict_t v = resub_verdict_check(true, sent, 0, false, now);
    CHECK(v == RESUB_VERDICT_RETRY, "one unanswered ping with no downlink proof must RETRY, got %d",
          (int) v);
}

/* No page and two unanswered pings (second_try already true): DEAD. */
static void test_two_unanswered_pings_die(void)
{
    int64_t sent = 4000 * US_PER_S;
    int64_t now = sent + RESUB_VERDICT_TIMEOUT_US + 1;

    resub_verdict_t v = resub_verdict_check(true, sent, 0, true, now);
    CHECK(v == RESUB_VERDICT_DEAD, "two unanswered pings with no downlink proof must be DEAD, got %d",
          (int) v);
}

int main(void)
{
    test_none_when_not_waiting();
    test_none_before_bound();
    test_page_after_ping_is_alive();
    test_one_unanswered_ping_retries();
    test_two_unanswered_pings_die();

    if (g_failures == 0) {
        printf("PASS: resub verdict (SUBACK collision: page-after-ping / retry-once / dead-after-"
               "second-miss), 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

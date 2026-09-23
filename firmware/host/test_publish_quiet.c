/* test_publish_quiet.c — host test harness for main/publish_quiet.c (23 Sep
 * 2026 display-corruption field failures: a panel SPI write starting while
 * a pager-originated MQTT publish's LTE uplink was in flight).
 *
 * Builds with the plain host compiler, no ESP-IDF (publish_quiet.c has no
 * ESP-IDF dependency at all — see publish_quiet.h's own module comment).
 *
 * Coverage required by the task brief:
 *  - a panel write must wait while a publish is outstanding (issued(), not
 *    yet done())
 *  - and for PUBLISH_QUIET_WINDOW_US after the completing event (done())
 *  - clear again once that window elapses
 *  - two overlapping publishes: the gate must not clear until BOTH have
 *    completed (in_flight is a count, not a flag)
 *  - a spurious/duplicate done() call with nothing outstanding must not
 *    underflow the count or otherwise misbehave
 *  - a fresh gate never waits
 *
 * Plus (23 Sep release-build fix, M4: the 44-byte publish corruption --
 * net_sleep() must not deassert RTS while a publish's own AT round trip is
 * still outstanding) publish_quiet_gate_hold_sleep() coverage:
 *  - true the instant a publish is issued
 *  - false once it is done()
 *  - false PUBLISH_SLEEP_HOLD_MAX_US + 1us after issue with no done() at all
 *    (bounded: a lost URC must not pin the device awake forever)
 *  - two overlapping publishes hold sleep off until BOTH are done()
 *  - a stale in-flight count left behind by a lost URC does not hold a
 *    later, genuinely fresh publish beyond that later publish's own 15s
 *
 * NOT host-testable here (see the report): that net.cpp actually calls
 * publish_quiet_gate_issued()/_done() at the right AT-command boundaries
 * (mqttPublish() returning true; the WALTER_MODEM_MQTT_EVENT_PUBLISHED
 * callback) is a property of net.cpp's real WalterModem calls, which this
 * pure module knows nothing about — WalterModem is a vendor C++
 * static-method class with no host build, so it cannot be linked or mocked
 * here. That wiring is verified by reading net.cpp (net_publish()/
 * net_publish_raw()/pager_mqtt_event_handler()) and, ultimately, on the
 * bench.
 */
#include "publish_quiet.h"

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

static void test_fresh_gate_never_waits(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    CHECK(!publish_quiet_gate_should_wait(&g, 0), "fresh gate must not wait at t=0");
    CHECK(!publish_quiet_gate_should_wait(&g, 999999 * US_PER_S),
          "fresh gate must never wait, however long now_us is");
}

static void test_waits_while_in_flight(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 1000 * US_PER_S;
    publish_quiet_gate_issued(&g, t0);
    CHECK(publish_quiet_gate_should_wait(&g, t0), "must wait the instant a publish is issued");
    CHECK(publish_quiet_gate_should_wait(&g, t0 + 999 * US_PER_S),
          "must keep waiting indefinitely while still in flight, however long");
}

/* Cleared by done(), but only after PUBLISH_QUIET_WINDOW_US has elapsed
 * from that call -- not immediately. */
static void test_quiet_window_after_done(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 2000 * US_PER_S;
    publish_quiet_gate_issued(&g, t0);
    publish_quiet_gate_done(&g, t0);

    CHECK(publish_quiet_gate_should_wait(&g, t0), "must still wait right at done()'s own timestamp");
    CHECK(publish_quiet_gate_should_wait(&g, t0 + PUBLISH_QUIET_WINDOW_US - 1),
          "must still wait 1us before the window elapses");
    CHECK(!publish_quiet_gate_should_wait(&g, t0 + PUBLISH_QUIET_WINDOW_US),
          "must clear exactly at the window bound");
    CHECK(!publish_quiet_gate_should_wait(&g, t0 + 10 * PUBLISH_QUIET_WINDOW_US),
          "must stay clear long after, with nothing else outstanding");
}

/* Two publishes issued close together (book.c/loc.c/modes.c/msg.c/setup.c/
 * sms.c all call net_publish_raw() independently -- nothing serialises
 * them): the gate must not clear until BOTH complete. */
static void test_overlapping_publishes_count(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 3000 * US_PER_S;
    publish_quiet_gate_issued(&g, t0); /* e.g. /status online */
    publish_quiet_gate_issued(&g, t0); /* e.g. a message ack, moments later */
    CHECK(publish_quiet_gate_should_wait(&g, t0), "must wait with two outstanding");

    publish_quiet_gate_done(&g, t0 + 1 * US_PER_S); /* first one completes */
    CHECK(publish_quiet_gate_should_wait(&g, t0 + 1 * US_PER_S),
          "must still wait -- the second publish is still outstanding");
    CHECK(publish_quiet_gate_should_wait(&g, t0 + 1 * US_PER_S + PUBLISH_QUIET_WINDOW_US + 1),
          "must not have armed a quiet window off the first done() while a second is still in flight");

    int64_t t1 = t0 + 2 * US_PER_S;
    publish_quiet_gate_done(&g, t1); /* second one completes */
    CHECK(publish_quiet_gate_should_wait(&g, t1), "must wait through the quiet window from the LAST done()");
    CHECK(!publish_quiet_gate_should_wait(&g, t1 + PUBLISH_QUIET_WINDOW_US),
          "must clear once the window from the last done() elapses");
}

/* A spurious/duplicate PUBLISHED event with nothing outstanding (net.cpp's
 * own defensive comment: "clamped at 0") must not underflow the count into
 * a permanently-waiting state. */
static void test_spurious_done_does_not_underflow(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    publish_quiet_gate_done(&g, 5 * US_PER_S); /* nothing was ever issued */
    CHECK(g.in_flight == 0, "in_flight must clamp at 0, not underflow, got %u",
          (unsigned) g.in_flight);
    /* done() with nothing outstanding still arms the quiet window from
     * `now_us` (in_flight reaches/stays 0) -- exercise that it behaves like
     * any other 0-transition rather than getting stuck. */
    CHECK(publish_quiet_gate_should_wait(&g, 5 * US_PER_S), "quiet window must still apply");
    CHECK(!publish_quiet_gate_should_wait(&g, 5 * US_PER_S + PUBLISH_QUIET_WINDOW_US),
          "must clear once that window elapses, same as any other done()");
}

/* hold_sleep() must go true the instant a publish is issued -- same edge as
 * should_wait(), different bound (see hold_sleep()'s own doc comment). */
static void test_hold_sleep_true_right_after_issued(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 4000 * US_PER_S;
    CHECK(!publish_quiet_gate_hold_sleep(&g, t0), "fresh gate must not hold sleep");
    publish_quiet_gate_issued(&g, t0);
    CHECK(publish_quiet_gate_hold_sleep(&g, t0), "must hold sleep the instant a publish is issued");
}

/* Unlike should_wait() (which keeps waiting for PUBLISH_QUIET_WINDOW_US
 * after done()), hold_sleep() drops immediately on done() -- there is
 * nothing left in flight for net_sleep() to race against. */
static void test_hold_sleep_false_after_done(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 5000 * US_PER_S;
    publish_quiet_gate_issued(&g, t0);
    publish_quiet_gate_done(&g, t0 + 1 * US_PER_S);
    CHECK(!publish_quiet_gate_hold_sleep(&g, t0 + 1 * US_PER_S),
          "must not hold sleep once the publish is done()");
}

/* Bounded: a lost PUBLISHED URC (done() never comes) must not pin the device
 * awake past PUBLISH_SLEEP_HOLD_MAX_US. */
static void test_hold_sleep_bounded_with_no_done(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 6000 * US_PER_S;
    publish_quiet_gate_issued(&g, t0);
    CHECK(publish_quiet_gate_hold_sleep(&g, t0 + PUBLISH_SLEEP_HOLD_MAX_US - 1),
          "must still hold sleep 1us before the bound");
    CHECK(!publish_quiet_gate_hold_sleep(&g, t0 + PUBLISH_SLEEP_HOLD_MAX_US),
          "must clear exactly at the bound, even with no done() ever");
    CHECK(!publish_quiet_gate_hold_sleep(&g, t0 + PUBLISH_SLEEP_HOLD_MAX_US + 1 * US_PER_S),
          "must stay clear well past the bound");
}

/* Two overlapping publishes: hold_sleep() must stay true until BOTH are
 * done(), same in_flight-is-a-count reasoning as should_wait(). */
static void test_hold_sleep_overlapping_until_both_done(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 7000 * US_PER_S;
    publish_quiet_gate_issued(&g, t0);                  /* e.g. /status online */
    publish_quiet_gate_issued(&g, t0 + 1 * US_PER_S);   /* e.g. a message ack, moments later */
    CHECK(publish_quiet_gate_hold_sleep(&g, t0 + 1 * US_PER_S), "must hold sleep with two outstanding");

    publish_quiet_gate_done(&g, t0 + 2 * US_PER_S); /* first one completes */
    CHECK(publish_quiet_gate_hold_sleep(&g, t0 + 2 * US_PER_S),
          "must still hold sleep -- the second publish is still outstanding");
    CHECK(publish_quiet_gate_hold_sleep(&g, t0 + 2 * US_PER_S + PUBLISH_SLEEP_HOLD_MAX_US - 1),
          "the still-outstanding second publish's own window was refreshed by done(), so this must "
          "still hold");

    publish_quiet_gate_done(&g, t0 + 3 * US_PER_S); /* second one completes */
    CHECK(!publish_quiet_gate_hold_sleep(&g, t0 + 3 * US_PER_S), "must clear once both are done()");
}

/* A stale in-flight count from a lost URC (first publish issued, done()
 * never called) must not hold sleep off forever, AND must not deny a later,
 * genuinely new publish its own fresh 15s window -- see
 * publish_quiet_gate_issued()'s own doc comment for why issued() resets the
 * timestamp when the existing hold has already expired. */
static void test_hold_sleep_stale_in_flight_does_not_pin_later_publish(void)
{
    publish_quiet_gate_t g;
    publish_quiet_gate_init(&g);

    int64_t t0 = 8000 * US_PER_S;
    publish_quiet_gate_issued(&g, t0); /* lost URC: done() never comes for this one */
    CHECK(!publish_quiet_gate_hold_sleep(&g, t0 + PUBLISH_SLEEP_HOLD_MAX_US + 5 * US_PER_S),
          "the stale publish's own hold must have already expired");

    /* A brand-new publish arrives long after the stale one's hold expired,
     * with the stale in_flight count (1) still sitting there uncleared. */
    int64_t t1 = t0 + PUBLISH_SLEEP_HOLD_MAX_US + 5 * US_PER_S;
    publish_quiet_gate_issued(&g, t1);
    CHECK(publish_quiet_gate_hold_sleep(&g, t1),
          "the new publish must get its own fresh hold, not inherit the stale timestamp");
    CHECK(publish_quiet_gate_hold_sleep(&g, t1 + PUBLISH_SLEEP_HOLD_MAX_US - 1),
          "the new publish's own window must run its own full 15s");
    CHECK(!publish_quiet_gate_hold_sleep(&g, t1 + PUBLISH_SLEEP_HOLD_MAX_US),
          "the new publish must not hold sleep beyond its OWN 15s either");
}

int main(void)
{
    test_fresh_gate_never_waits();
    test_waits_while_in_flight();
    test_quiet_window_after_done();
    test_overlapping_publishes_count();
    test_spurious_done_does_not_underflow();
    test_hold_sleep_true_right_after_issued();
    test_hold_sleep_false_after_done();
    test_hold_sleep_bounded_with_no_done();
    test_hold_sleep_overlapping_until_both_done();
    test_hold_sleep_stale_in_flight_does_not_pin_later_publish();

    if (g_failures == 0) {
        printf("PASS: publish quiet gate (23 Sep display-corruption fix), 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

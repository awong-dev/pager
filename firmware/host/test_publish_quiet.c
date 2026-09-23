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
    publish_quiet_gate_issued(&g);
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
    publish_quiet_gate_issued(&g);
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
    publish_quiet_gate_issued(&g); /* e.g. /status online */
    publish_quiet_gate_issued(&g); /* e.g. a message ack, moments later */
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

int main(void)
{
    test_fresh_gate_never_waits();
    test_waits_while_in_flight();
    test_quiet_window_after_done();
    test_overlapping_publishes_count();
    test_spurious_done_does_not_underflow();

    if (g_failures == 0) {
        printf("PASS: publish quiet gate (23 Sep display-corruption fix), 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

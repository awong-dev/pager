/* test_accel_rate.c — host test harness for main/accel.c's pure refractory
 * gate (docs/DEVICE_NEXT_TASKS.md A1: motion wake-storm guard).
 *
 * Builds with the plain host compiler, no ESP-IDF (accel.c's pure section
 * has no ESP-IDF dependency -- same `#ifdef ESP_PLATFORM` split as
 * sms.c/loc.c; see firmware/host/Makefile). Also links loc.c's pure section
 * + cbor.c so accel_edge_wanted() and loc_trigger_motion_event() can be
 * exercised together, in the same order accel_poll() calls them on-device.
 *
 * Coverage required by the task brief:
 *  - first edge always wanted (no prior reported edge)
 *  - an edge 19.9s after the last reported one is not wanted
 *  - an edge 20.1s after the last reported one is wanted
 *  - a sequence of wanted edges at 20s spacing, fed into
 *    loc_trigger_motion_event(), still fires at >=60s span (events at
 *    0/20/40/60s -> span 60s -> fires)
 *  - a negative/backwards clock never wedges (does not permanently return
 *    "not wanted")
 */
#include "accel.h"
#include "loc.h"
#include "cbor.h"

#include <stdint.h>
#include <stdio.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                          \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

#define US_PER_S ((int64_t) 1000000)
#define REFRACTORY_US ((int64_t) ACCEL_REFRACTORY_S * US_PER_S) /* 20s */

/* ---------------------------------------------------------------------
 * accel_edge_wanted() in isolation.
 * --------------------------------------------------------------------- */
static void test_first_edge_always_wanted(void)
{
    CHECK(accel_edge_wanted(0, 0, REFRACTORY_US), "first edge (last_reported_us=0) at t=0 is wanted");
    CHECK(accel_edge_wanted(12345 * US_PER_S, 0, REFRACTORY_US),
          "first edge is wanted regardless of what `now_us` is");
}

static void test_refractory_boundary(void)
{
    int64_t last = 1000 * US_PER_S;

    int64_t just_under = last + (int64_t) (19.9 * (double) US_PER_S);
    CHECK(!accel_edge_wanted(just_under, last, REFRACTORY_US),
          "an edge 19.9s after the last reported one must not be wanted (20s refractory)");

    int64_t just_over = last + (int64_t) (20.1 * (double) US_PER_S);
    CHECK(accel_edge_wanted(just_over, last, REFRACTORY_US),
          "an edge 20.1s after the last reported one must be wanted (20s refractory)");

    /* Exactly the boundary: >= refractory_us is wanted (accel.c's own
     * `elapsed_us >= refractory_us` contract, matching the task brief's
     * "events at 0, 20, 40, 60s spacing" sequence test below). */
    CHECK(accel_edge_wanted(last + REFRACTORY_US, last, REFRACTORY_US),
          "an edge exactly refractory_us after the last reported one must be wanted");
}

static void test_refractory_zero_means_off(void)
{
    int64_t last = 1000 * US_PER_S;
    CHECK(accel_edge_wanted(last + 1, last, 0), "refractory_us == 0 means every edge is wanted");
    CHECK(accel_edge_wanted(last, last, 0), "refractory_us == 0 is wanted even at the same instant");
}

static void test_backwards_clock_never_wedges(void)
{
    int64_t last = 1000 * US_PER_S;
    CHECK(accel_edge_wanted(last - 1, last, REFRACTORY_US),
          "a clock reading 1us before the last reported edge must not wedge (fail open)");
    CHECK(accel_edge_wanted(0, last, REFRACTORY_US),
          "a clock reading far before the last reported edge must not wedge (fail open)");
    /* And it must not get "stuck" -- a second, later backwards reading is
     * still wanted, not permanently blocked by the first one. */
    CHECK(accel_edge_wanted(last - 5, last, REFRACTORY_US),
          "a second backwards reading must also fail open, not wedge");
}

/* ---------------------------------------------------------------------
 * accel_edge_wanted() gating loc_trigger_motion_event(): the refractory
 * throttle must not cost the classifier its 60s-within-3min trigger.
 * --------------------------------------------------------------------- */
static void test_gated_sequence_still_fires_classifier(void)
{
    loc_policy_t p;
    loc_policy_init(&p);

    int64_t last_reported = 0;
    int64_t t[] = { 0, 20 * US_PER_S, 40 * US_PER_S, 60 * US_PER_S };
    bool fired = false;

    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        bool wanted = accel_edge_wanted(t[i], last_reported, REFRACTORY_US);
        CHECK(wanted, "event at t=%llds (20s spacing) must be wanted, not throttled",
              (long long) (t[i] / US_PER_S));
        if (!wanted) {
            continue;
        }
        last_reported = t[i];
        if (loc_trigger_motion_event(&p, t[i])) {
            fired = true;
        }
    }
    CHECK(fired, "events at 0/20/40/60s (span 60s) must still fire the classifier through the "
                 "refractory gate");
}

int main(void)
{
    test_first_edge_always_wanted();
    test_refractory_boundary();
    test_refractory_zero_means_off();
    test_backwards_clock_never_wedges();
    test_gated_sequence_still_fires_classifier();

    if (g_failures == 0) {
        printf("PASS: accel refractory gate, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

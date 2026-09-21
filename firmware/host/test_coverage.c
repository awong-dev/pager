/* test_coverage.c — host test harness for main/coverage.c (owner request,
 * 2026-09-20: slow, battery-aware behaviour when there is no coverage).
 *
 * Builds with the plain host compiler, no ESP-IDF (coverage.c has no
 * ESP-IDF dependency at all — see coverage.h's own module comment).
 *
 * Coverage required by the task brief:
 *  - grace period (no action before GRACE_S elapses)
 *  - each back-off step (2, 5, 10, 15 min)
 *  - the cap (stays at 15 min once reached)
 *  - reset on registration (successful registration resets the whole policy)
 *  - reset on motion (coverage_on_motion() resets the off-period backoff to
 *    its first step)
 *  - never acting while location owns the radio (a location attempt in
 *    flight defers both the first entry into the duty cycle and every later
 *    OFF re-entry, without advancing the backoff while deferring)
 *  - nothing fires while off (coverage_step() returns NONE for every call
 *    strictly inside an OFF phase's deadline; modes.c's own gating on
 *    coverage_owns_radio() is what turns that NONE into "no MQTT connect
 *    attempt, no watchdog, no health-check reset" — not host-testable
 *    without ESP-IDF, so this file only pins the policy's own half of the
 *    contract: it never asks the caller to do anything while OFF's deadline
 *    has not yet passed).
 */
#include "coverage.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* ---------------------------------------------------------------------
 * Grace period: no action of any kind before GRACE_S elapses, even though
 * the pager has been continuously unregistered the whole time.
 * --------------------------------------------------------------------- */
static void test_grace_period(void)
{
    coverage_policy_t p;
    coverage_policy_init(&p);

    int64_t now = 1000 * US_PER_S;
    coverage_action_t a = coverage_step(&p, now, /*registered=*/false, /*attempt=*/false);
    CHECK(a == COVERAGE_ACTION_NONE, "first unregistered observation must not act immediately");
    CHECK(!coverage_owns_radio(&p), "must not own the radio during grace");

    now += (COVERAGE_GRACE_S - 1) * US_PER_S;
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_NONE, "must still do nothing 1s before GRACE_S elapses");

    now += 2 * US_PER_S; // now just past GRACE_S since unregistered_since_us
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_ENTER_OFF, "must enter OFF exactly once GRACE_S has elapsed");
    CHECK(coverage_owns_radio(&p), "must own the radio once OFF has been entered");
}

/* ---------------------------------------------------------------------
 * Each back-off step (2, 5, 10, 15 min) and the cap (stays at 15 min).
 * --------------------------------------------------------------------- */
static void test_backoff_steps_and_cap(void)
{
    static const uint32_t expect_off_s[] = { 120, 300, 600, 900, 900, 900 };

    coverage_policy_t p;
    coverage_policy_init(&p);

    int64_t now = 0;
    /* Clear the grace window once. */
    coverage_action_t a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_NONE, "test setup: first call must not act");
    now += (int64_t) COVERAGE_GRACE_S * US_PER_S + 1 * US_PER_S;
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_ENTER_OFF, "test setup: must enter OFF after grace");

    for (size_t i = 0; i < sizeof(expect_off_s) / sizeof(expect_off_s[0]); i++) {
        CHECK(p.off_period_s == expect_off_s[i], "step %zu: expected off_period_s=%u, got %u", i,
              expect_off_s[i], p.off_period_s);

        /* Ride out the OFF phase: nothing happens until its deadline. */
        now += (int64_t) p.off_period_s * US_PER_S - 1;
        a = coverage_step(&p, now, false, false);
        CHECK(a == COVERAGE_ACTION_NONE, "step %zu: must not act before the OFF deadline", i);
        now += 1;
        a = coverage_step(&p, now, false, false);
        CHECK(a == COVERAGE_ACTION_ENTER_SEARCH, "step %zu: OFF deadline must yield ENTER_SEARCH", i);
        CHECK(!coverage_owns_radio(&p), "must not own the radio during SEARCH");

        /* Ride out the SEARCH window without registering -> next OFF step. */
        now += (int64_t) COVERAGE_SEARCH_S * US_PER_S - 1;
        a = coverage_step(&p, now, false, false);
        CHECK(a == COVERAGE_ACTION_NONE, "step %zu: must not act before the SEARCH deadline", i);
        now += 1;
        a = coverage_step(&p, now, false, false);
        CHECK(a == COVERAGE_ACTION_ENTER_OFF, "step %zu: SEARCH deadline must yield ENTER_OFF", i);
    }
}

/* ---------------------------------------------------------------------
 * Reset on registration: a successful registration resets the whole policy,
 * including the off-period backoff, and latches how long the stretch lasted.
 * --------------------------------------------------------------------- */
static void test_reset_on_registration(void)
{
    coverage_policy_t p;
    coverage_policy_init(&p);

    int64_t now = 0;
    coverage_step(&p, now, false, false);
    now += (int64_t) COVERAGE_GRACE_S * US_PER_S + 1 * US_PER_S;
    coverage_action_t a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_ENTER_OFF, "test setup: must enter OFF");

    /* Escalate to the second step before recovering. */
    now += (int64_t) p.off_period_s * US_PER_S;
    a = coverage_step(&p, now, false, false); /* -> SEARCH */
    CHECK(a == COVERAGE_ACTION_ENTER_SEARCH, "test setup: must enter SEARCH");
    now += (int64_t) COVERAGE_SEARCH_S * US_PER_S;
    a = coverage_step(&p, now, false, false); /* -> OFF, step 1 (300s) */
    CHECK(a == COVERAGE_ACTION_ENTER_OFF && p.off_period_s == 300,
          "test setup: must have escalated to the second off step (300s)");

    /* Registration comes back mid-OFF. */
    now += 42 * US_PER_S;
    a = coverage_step(&p, now, true, false);
    CHECK(a == COVERAGE_ACTION_NONE, "a registration edge itself asks for no radio action");
    CHECK(!p.active, "the policy must go fully inactive on registration");
    CHECK(p.off_period_s == 0, "the off-period backoff must reset to zero on registration");
    CHECK(!coverage_owns_radio(&p), "must not own the radio once registered");

    CHECK(p.last_dark_s > 0, "last_dark_s must be latched to a positive duration, got %u",
          (unsigned) p.last_dark_s);

    /* Next time it goes dark, the backoff starts over at the first step. */
    now += 10 * US_PER_S;
    coverage_step(&p, now, false, false); /* just went unregistered again */
    now += (int64_t) COVERAGE_GRACE_S * US_PER_S + 1 * US_PER_S;
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_ENTER_OFF && p.off_period_s == 120,
          "after a registration reset, the NEXT dark stretch must start at the first off step "
          "again, got %u",
          (unsigned) p.off_period_s);
}

/* ---------------------------------------------------------------------
 * Reset on motion: coverage_on_motion() resets the off-period backoff to
 * its first step without disturbing the phase currently in progress.
 * --------------------------------------------------------------------- */
static void test_reset_on_motion(void)
{
    coverage_policy_t p;
    coverage_policy_init(&p);

    int64_t now = 0;
    coverage_step(&p, now, false, false);
    now += (int64_t) COVERAGE_GRACE_S * US_PER_S + 1 * US_PER_S;
    coverage_step(&p, now, false, false); /* -> OFF, 120s */
    now += (int64_t) p.off_period_s * US_PER_S;
    coverage_step(&p, now, false, false); /* -> SEARCH */
    now += (int64_t) COVERAGE_SEARCH_S * US_PER_S;
    coverage_action_t a = coverage_step(&p, now, false, false); /* -> OFF, 300s */
    CHECK(a == COVERAGE_ACTION_ENTER_OFF && p.off_period_s == 300,
          "test setup: must have escalated to the second off step");

    coverage_on_motion(&p);
    CHECK(p.off_period_s == 0, "coverage_on_motion() must reset the backoff to zero immediately");
    CHECK(p.phase == COVERAGE_PHASE_OFF, "coverage_on_motion() must not disturb the current phase");

    /* The CURRENT off phase's own deadline (set from the pre-motion 300s
     * step) is untouched -- motion resets the NEXT computed period, not this
     * one already in flight. */
    now += 299 * US_PER_S;
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_NONE, "the in-flight OFF phase's own deadline must be unaffected by motion");
    now += 1 * US_PER_S;
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_ENTER_SEARCH, "the in-flight OFF phase must still end on its own schedule");

    /* But the NEXT off period (after this search fails) starts back at 120s. */
    now += (int64_t) COVERAGE_SEARCH_S * US_PER_S;
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_ENTER_OFF && p.off_period_s == 120,
          "after a motion reset, the NEXT off period must start back at the first step, got %u",
          (unsigned) p.off_period_s);
}

/* ---------------------------------------------------------------------
 * Never acting while location owns the radio: a location attempt in flight
 * defers both the first entry into the duty cycle and a later OFF
 * re-entry, without advancing the backoff while deferring.
 * --------------------------------------------------------------------- */
static void test_defers_while_location_attempt_in_flight(void)
{
    coverage_policy_t p;
    coverage_policy_init(&p);

    int64_t now = 0;
    coverage_step(&p, now, false, false);
    now += (int64_t) COVERAGE_GRACE_S * US_PER_S + 1 * US_PER_S;

    /* A location attempt is running right as grace elapses: must defer. */
    coverage_action_t a = coverage_step(&p, now, false, /*attempt_in_progress=*/true);
    CHECK(a == COVERAGE_ACTION_NONE, "must defer entering OFF while a location attempt is in flight");
    CHECK(!p.active, "must stay inactive while deferring");
    CHECK(p.off_period_s == 0, "must not consume/advance the backoff while deferring");

    /* Keep deferring for a while, then the attempt finishes. */
    now += 45 * US_PER_S;
    a = coverage_step(&p, now, false, true);
    CHECK(a == COVERAGE_ACTION_NONE, "must keep deferring for as long as the attempt runs");

    now += 5 * US_PER_S;
    a = coverage_step(&p, now, false, /*attempt_in_progress=*/false);
    CHECK(a == COVERAGE_ACTION_ENTER_OFF, "must enter OFF as soon as the attempt clears");
    CHECK(p.off_period_s == 120, "the deferred entry must still get the first (120s) off step");

    /* Same defer behaviour on a later OFF re-entry (after a SEARCH window
     * ends without registering). */
    now += (int64_t) p.off_period_s * US_PER_S;
    a = coverage_step(&p, now, false, false); /* -> SEARCH */
    CHECK(a == COVERAGE_ACTION_ENTER_SEARCH, "test setup: must enter SEARCH");
    now += (int64_t) COVERAGE_SEARCH_S * US_PER_S;
    a = coverage_step(&p, now, false, /*attempt_in_progress=*/true);
    CHECK(a == COVERAGE_ACTION_NONE, "must defer re-entering OFF while a location attempt is in flight");
    CHECK(p.off_period_s == 120, "must not advance the backoff while deferring a re-entry either");

    now += 30 * US_PER_S;
    a = coverage_step(&p, now, false, false);
    CHECK(a == COVERAGE_ACTION_ENTER_OFF && p.off_period_s == 300,
          "once the attempt clears, re-entry must proceed and advance the backoff exactly once, "
          "got %u",
          (unsigned) p.off_period_s);
}

int main(void)
{
    test_grace_period();
    test_backoff_steps_and_cap();
    test_reset_on_registration();
    test_reset_on_motion();
    test_defers_while_location_attempt_in_flight();

    if (g_failures == 0) {
        printf("PASS: coverage duty-cycle policy, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

// coverage.c — see coverage.h for the policy, the constants and their own
// UNVERIFIED cost estimates, and the radio-ownership rule shared with loc.c.
//
// Pure C, no ESP-IDF dependency (host-tested by firmware/host/test_coverage.c) —
// modes.c is the only caller and owns every actual net_radio_*()/
// net_session_down() call this policy's return value implies.

#include "coverage.h"

#include <string.h>

#define US_PER_S ((int64_t) 1000000)

static uint32_t next_off_period_s(uint32_t current_s)
{
    if (current_s < COVERAGE_OFF_STEP0_S) {
        return COVERAGE_OFF_STEP0_S;
    }
    if (current_s < COVERAGE_OFF_STEP1_S) {
        return COVERAGE_OFF_STEP1_S;
    }
    if (current_s < COVERAGE_OFF_STEP2_S) {
        return COVERAGE_OFF_STEP2_S;
    }
    return COVERAGE_OFF_STEP3_S; // already at or past the cap
}

void coverage_policy_init(coverage_policy_t *p)
{
    memset(p, 0, sizeof(*p));
}

static void reset_to_inactive(coverage_policy_t *p)
{
    p->active = false;
    p->phase = COVERAGE_PHASE_NONE;
    p->phase_deadline_us = 0;
    p->off_period_s = 0;
    p->cycles = 0;
}

coverage_action_t coverage_step(coverage_policy_t *p, int64_t now_us, bool registered,
                                 bool attempt_in_progress)
{
    if (registered) {
        if (p->have_unregistered_since) {
            int64_t dark_us = now_us - p->unregistered_since_us;
            p->last_dark_s = (dark_us > 0) ? (uint32_t) (dark_us / US_PER_S) : 0;
        }
        p->have_unregistered_since = false;
        if (p->active) {
            // §"Any successful registration resets the policy" -- next time
            // the pager goes dark, it starts back at GRACE_S/the first
            // off-period step, not wherever this stretch left off.
            reset_to_inactive(p);
        }
        return COVERAGE_ACTION_NONE;
    }

    if (!p->have_unregistered_since) {
        p->unregistered_since_us = now_us; // just went dark this call
        p->have_unregistered_since = true;
    }
    int64_t dark_us = now_us - p->unregistered_since_us;

    if (!p->active) {
        if (dark_us < (int64_t) COVERAGE_GRACE_S * US_PER_S) {
            return COVERAGE_ACTION_NONE; // still inside the grace window
        }
        if (attempt_in_progress) {
            // Ownership rule (coverage.h): defer taking the radio for as
            // long as a location attempt is running; off_period_s is still
            // 0 here so the eventual first off-period is still the full
            // GRACE_S-triggered first step, not shortchanged by the wait.
            return COVERAGE_ACTION_NONE;
        }
        p->active = true;
        p->off_period_s = next_off_period_s(p->off_period_s); // 0 -> COVERAGE_OFF_STEP0_S
        p->phase = COVERAGE_PHASE_OFF;
        p->phase_deadline_us = now_us + (int64_t) p->off_period_s * US_PER_S;
        return COVERAGE_ACTION_ENTER_OFF;
    }

    switch (p->phase) {
    case COVERAGE_PHASE_OFF:
        if (now_us < p->phase_deadline_us) {
            return COVERAGE_ACTION_NONE; // still waiting out the off period
        }
        p->phase = COVERAGE_PHASE_SEARCH;
        p->phase_deadline_us = now_us + (int64_t) COVERAGE_SEARCH_S * US_PER_S;
        return COVERAGE_ACTION_ENTER_SEARCH;

    case COVERAGE_PHASE_SEARCH:
        if (now_us < p->phase_deadline_us) {
            return COVERAGE_ACTION_NONE; // still searching, radio already FULL
        }
        if (attempt_in_progress) {
            // Defer re-entering OFF without advancing the backoff — same
            // reasoning as the initial-entry case above.
            return COVERAGE_ACTION_NONE;
        }
        p->cycles++;
        p->off_period_s = next_off_period_s(p->off_period_s);
        p->phase = COVERAGE_PHASE_OFF;
        p->phase_deadline_us = now_us + (int64_t) p->off_period_s * US_PER_S;
        return COVERAGE_ACTION_ENTER_OFF;

    case COVERAGE_PHASE_NONE:
    default:
        // Unreachable while p->active (only set alongside phase=OFF above),
        // but fail safe rather than get stuck: re-enter from scratch.
        p->off_period_s = 0;
        p->phase = COVERAGE_PHASE_OFF;
        p->off_period_s = next_off_period_s(p->off_period_s);
        p->phase_deadline_us = now_us + (int64_t) p->off_period_s * US_PER_S;
        return COVERAGE_ACTION_ENTER_OFF;
    }
}

void coverage_on_motion(coverage_policy_t *p)
{
    p->off_period_s = 0; // next computed off-period starts back at STEP0
}

bool coverage_owns_radio(const coverage_policy_t *p) { return p->phase == COVERAGE_PHASE_OFF; }

uint32_t coverage_last_dark_s(const coverage_policy_t *p) { return p->last_dark_s; }

uint32_t coverage_off_step_index(const coverage_policy_t *p)
{
    if (p->off_period_s <= COVERAGE_OFF_STEP0_S) {
        return 0;
    }
    if (p->off_period_s <= COVERAGE_OFF_STEP1_S) {
        return 1;
    }
    if (p->off_period_s <= COVERAGE_OFF_STEP2_S) {
        return 2;
    }
    return 3;
}

uint32_t coverage_phase_remaining_s(const coverage_policy_t *p, int64_t now_us)
{
    if (p->phase == COVERAGE_PHASE_NONE || p->phase_deadline_us <= now_us) {
        return 0;
    }
    return (uint32_t) ((p->phase_deadline_us - now_us + US_PER_S - 1) / US_PER_S); // round up
}

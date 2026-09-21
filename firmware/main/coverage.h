/* coverage.h — coverage duty-cycle policy (owner request, 2026-09-20).
 *
 * Split the same way loc.c/catrust.c are: everything in this file is pure C,
 * no ESP-IDF dependency, driven by a caller-supplied clock (`int64_t now_us`,
 * `esp_timer_get_time()`-shaped but never called directly from here) and a
 * caller-supplied `registered`/`loc_attempt_in_progress` snapshot, so
 * firmware/host/test_coverage.c can run every transition without a device.
 * modes.c owns all the actual modem/radio calls (net_radio_off()/
 * net_radio_on()/net_session_down()) — this module only decides WHEN, never
 * calls anything itself.
 *
 * Problem this solves (owner decision, 2026-09-20): out of coverage, the
 * modem searches continuously (expensive) and the ESP32 retries the MQTT
 * connect on a short backoff. The pager may sit in a signal-blocked building
 * or pouch for hours; the owner wants reconnect attempts to back off and
 * stay slow, accepts noticing coverage is back within about 5 minutes, and
 * wants a good battery trade-off over exact recovery latency.
 *
 * Policy: after the pager has been unregistered for GRACE_S (elevators and
 * brief dead spots recover by themselves, no need to ever touch the radio
 * for those), switch the radio off for an off-period, then on for a bounded
 * search window (SEARCH_S); repeat, with the off-period backing off
 * (2, 5, 10, 15 min, capped) each time a search window ends without
 * registering. Any successful registration resets the whole policy. Coming
 * back from a 15-minute-capped off period plus a 150s search window means
 * "coverage is back" is noticed within about 5 minutes worst case, per the
 * owner's own stated tolerance.
 *
 * Radio ownership (this task's own explicit rule, since loc.c's route 2 also
 * takes the radio off for a GNSS attempt): a location attempt never starts
 * while this policy owns the radio (loc.c checks modes_coverage_owns_radio()
 * before starting one — modes.h/loc.c), and this policy never takes the
 * radio while a location attempt is in flight (`loc_attempt_in_progress`,
 * checked every coverage_step() call — see its own doc comment). Location
 * requests cannot arrive over MQTT while the radio is deliberately off
 * anyway (no session), so in practice only the debug `gnsstest` path can
 * race this, which is what the loc.c-side check guards against.
 */
#ifndef COVERAGE_H
#define COVERAGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Constants. All cost figures below are ESTIMATES, not measurements — no
 * current-trace data exists yet for either "searching with the radio FULL
 * but unregistered" or "NO_RF" on this modem (same "no number here is a
 * measured number yet" caveat docs/PROTOCOL.md's own §7/§8 numbers carry).
 * Treat every mAh figure as an order-of-magnitude sanity check, not a
 * budget; firmware/README.md's existing measurement checklist is the right
 * place to bring a real current trace back to.
 * --------------------------------------------------------------------- */

/* 3 min: elevators and brief dead spots recover on their own (net.cpp's own
 * "Coverage loss and recovery" block comment: "a walk in and out of a
 * building"). The duty cycle does not touch the radio at all until the
 * pager has been unregistered continuously for this long. */
#define COVERAGE_GRACE_S 180u

/* 150 s: registration on the bench takes ~100 s on AT&T (this project's
 * primary SIM, GOTCHAS.md's "Dark Star" APN section) and 1-2 s on T-Mobile —
 * this gives the slow carrier real margin. If the pager registers, the
 * search window ends immediately (the caller's own net_take_registered_edge()
 * fires well before the deadline in the fast case). */
#define COVERAGE_SEARCH_S 150u

/* Off-period backoff: 2, 5, 10, 15 min, then steady at the cap. Doubling
 * roughly, capped low deliberately (unlike loc.c's 12h location backoff) —
 * the owner's own stated tolerance is "noticing coverage is back after about
 * 5 minutes is fine", which only holds if the off period stays in the
 * single-digit minutes even after it has grown. */
#define COVERAGE_OFF_STEP0_S 120u /* 2 min, first step after GRACE_S elapses */
#define COVERAGE_OFF_STEP1_S 300u /* 5 min */
#define COVERAGE_OFF_STEP2_S 600u /* 10 min */
#define COVERAGE_OFF_STEP3_S 900u /* 15 min, cap */

/* UNVERIFIED cost estimate (see the constants-block comment above): a bounded
 * 150 s search with the radio FULL but not yet registered is assumed to draw
 * somewhere between the ~1-2 mA sleep-mode floor (PROTOCOL.md §8.4) and a
 * full attach transient — call it ~40 mA average, an order-of-magnitude
 * guess, not a datasheet figure. Worst case (150 s full search, never
 * registers): 150/3600 h * 40 mA =~ 1.7 mAh per search. NO_RF itself is
 * assumed close to the light-sleep floor (~1-2 mA, no RF at all) — off-period
 * cost is small next to the search cost above: 15 min at ~1.2 mA =~ 0.3 mAh
 * at the worst (longest) off step. Cycling forever at the cap (900 s off +
 * 150 s search = 1050 s/cycle, ~82 cycles/day): ~82 * (1.7 + 0.3) =~
 * 164 mAh/day, UNVERIFIED — worse than the ~43-50 mAh/day registered sleep
 * budget (PROTOCOL.md §8.4), because being genuinely out of coverage is
 * inherently more expensive than being registered, but roughly 5-6x cheaper
 * than searching continuously with the radio FULL 24h/day (today's
 * behaviour, ~40 mA * 24 h =~ 960 mAh/day at the same current guess) — the
 * whole point of duty-cycling. Get a real current trace before trusting
 * these numbers for a battery-life claim. */

/* ---------------------------------------------------------------------
 * Policy state (RAM-only — deliberately NOT RTC-resident: this design never
 * deep sleeps, and losing this to a reset just means the policy restarts
 * from GRACE_S, the same conservative default a cold boot already gets).
 * --------------------------------------------------------------------- */
typedef enum {
    COVERAGE_PHASE_NONE = 0, /* registered, or unregistered but still inside GRACE_S */
    COVERAGE_PHASE_OFF,      /* policy owns the radio, deliberately NO_RF */
    COVERAGE_PHASE_SEARCH,   /* radio back FULL, bounded search window running */
} coverage_phase_t;

typedef struct {
    bool active;                    /* true once the duty cycle has taken over (past GRACE_S) */
    coverage_phase_t phase;
    bool have_unregistered_since;   /* false while registered -- NOT the same as
                                      * unregistered_since_us==0, which is a legitimate
                                      * esp_timer_get_time() value right after boot */
    int64_t unregistered_since_us;  /* meaningful only while have_unregistered_since */
    int64_t phase_deadline_us;      /* meaningful only while `phase != NONE` */
    uint32_t off_period_s;          /* current/most recent off-period; 0 = never started */
    uint32_t cycles;                /* off/search cycles completed this unregistered stretch (diagnostic) */
    uint32_t last_dark_s;           /* how long the *previous* unregistered stretch lasted, latched
                                      * the instant registration comes back (before unregistered_since_us
                                      * is cleared) -- for the "how long was the pager dark" log line */
} coverage_policy_t;

void coverage_policy_init(coverage_policy_t *p);

/* What the caller must do this call, and only this call — coverage_step()
 * assumes its own instruction was actually carried out before the next
 * call (same discipline loc.c's phase machine and route 2 use: the pure
 * decision and the device-side action are one call apart, never batched). */
typedef enum {
    COVERAGE_ACTION_NONE = 0,   /* nothing to do: registered, still in grace, or mid-search */
    COVERAGE_ACTION_ENTER_OFF,  /* caller must: tear down MQTT if up, then net_radio_off() */
    COVERAGE_ACTION_ENTER_SEARCH, /* caller must: net_radio_on() */
} coverage_action_t;

/* Call once per modes_run() iteration (cheap — RAM only, no I/O of its own).
 * `registered`: the pager's current registration state (net_unregistered_for_s()
 * == 0, a plain RAM read, never an AT round trip — see net.h). `attempt_in_progress`:
 * true while loc.c has a GNSS attempt in flight (loc_attempt_in_progress()) —
 * the ownership rule this header's own module comment documents: the duty
 * cycle defers taking the radio for as long as this is true, re-checking
 * every call, and never advances the off-period backoff while deferring (so
 * a long-running location attempt does not itself cost an extra escalation
 * step). Returns the ONE action the caller must perform before the next
 * call; COVERAGE_ACTION_NONE the overwhelming majority of calls (registered,
 * or mid-phase and not yet at a deadline). */
coverage_action_t coverage_step(coverage_policy_t *p, int64_t now_us, bool registered,
                                 bool attempt_in_progress);

/* Sustained motion (loc.c's own accelerometer classifier, loc_on_motion_event())
 * resets the off-period backoff to its first step — moving is when coverage
 * changes, so a device that has been dark and stationary for a while but has
 * just started moving should go back to checking often. Does NOT interrupt
 * whatever OFF/SEARCH phase is currently running (its deadline is
 * unaffected) — only the NEXT computed off-period starts over. Safe to call
 * any time, including while the policy is inactive (no-op then). */
void coverage_on_motion(coverage_policy_t *p);

/* True while `p` owns the radio (phase == COVERAGE_PHASE_OFF) — the
 * predicate loc.c checks before ever starting a GNSS attempt (this header's
 * own "radio ownership" note above). */
bool coverage_owns_radio(const coverage_policy_t *p);

/* Seconds the *previous* unregistered stretch lasted, latched the instant
 * registration comes back (0 if the pager has never lost registration, or is
 * still unregistered right now). For modes.c's own "network coverage
 * regained - dark for Ns" log line; not published on the wire (this task's
 * own instruction: add nothing to PROTOCOL.md tonight). */
uint32_t coverage_last_dark_s(const coverage_policy_t *p);

/* Debug console (`coverage`, main.c, debug build only): current off-period
 * step (0-3, meaningless if the policy has never gone active) and seconds
 * remaining until the current phase's own deadline (0 if `phase == NONE`). */
uint32_t coverage_off_step_index(const coverage_policy_t *p);
uint32_t coverage_phase_remaining_s(const coverage_policy_t *p, int64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* COVERAGE_H */

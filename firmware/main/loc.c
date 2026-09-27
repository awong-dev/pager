// loc.c — see loc.h for the module split, RTC ownership, and every
// function's doc comment.
//
// All power-effect comments are PENDING_HW/UNVERIFIED (no device attached
// while writing this — see the report this task ships with for exactly
// which log lines to look for on real hardware, gnsstest included).

#include "loc.h"

#include <string.h>

#include "cbor.h"

// ---------------------------------------------------------------------------
// Pure functions (no ESP-IDF dependency) — host-tested by
// firmware/host/test_loc.c.
// ---------------------------------------------------------------------------

void loc_policy_init(loc_policy_t *p)
{
    memset(p, 0, sizeof(*p));
    p->long_budget_next = true; // cold boot: no ephemeris trusted yet (V02_DESIGN.md §5)
}

uint32_t loc_next_backoff_s(uint32_t current_s)
{
    if (current_s == 0) {
        return LOC_BACKOFF_MIN_S;
    }
    uint64_t next = (uint64_t) current_s * 2;
    if (next > LOC_BACKOFF_MAX_S) {
        next = LOC_BACKOFF_MAX_S;
    }
    return (uint32_t) next;
}

bool loc_battery_ok(int batt_mv)
{
    // This task: an unknown reading must never block a GNSS attempt. Before
    // this fix the caller passed modes_get_batt_mv()'s 3300 placeholder
    // straight through, which happened to sit exactly on LOC_BATTERY_FLOOR_MV
    // (3300 >= 3300 is true) -- it passed today only by coincidence, not by
    // design, and would have silently started blocking GNSS the moment
    // either constant changed independently of the other.
    if (batt_mv == LOC_BATTERY_UNKNOWN_MV) {
        return true;
    }
    return batt_mv >= LOC_BATTERY_FLOOR_MV;
}

bool loc_fix_confidence_ok(double confidence_m)
{
    return confidence_m <= LOC_FIX_CONFIDENCE_MAX;
}

static bool loc_time_allowed(const loc_policy_t *p, int64_t now_us)
{
    int64_t backoff_until_us = p->last_attempt_us + (int64_t) p->backoff_s * 1000000;
    if (now_us < backoff_until_us) {
        return false;
    }
    if (p->trigger_floor_until_us != 0 && now_us < p->trigger_floor_until_us) {
        return false;
    }
    return true;
}

static bool enqueue_id_locked(loc_policy_t *p, const char *id)
{
    if (!id || id[0] == '\0') {
        return false;
    }
    if (p->queued_count >= LOC_MAX_QUEUED) {
        return false; // full: this requester's answer is lost — logged by the caller
    }
    strncpy(p->queued_ids[p->queued_count], id, LOC_ID_MAX - 1);
    p->queued_ids[p->queued_count][LOC_ID_MAX - 1] = '\0';
    p->queued_count++;
    return true;
}

loc_decision_t loc_on_request(loc_policy_t *p, int64_t now_us, int batt_mv, const char *req_id)
{
    if (p->attempt_in_progress) {
        enqueue_id_locked(p, req_id);
        return LOC_ANSWER_NONE;
    }
    if (!loc_time_allowed(p, now_us) || !loc_battery_ok(batt_mv)) {
        return LOC_ANSWER_CACHED;
    }
    p->attempt_in_progress = true;
    p->last_attempt_us = now_us;
    p->queued_count = 0;
    enqueue_id_locked(p, req_id);
    return LOC_ANSWER_START_ATTEMPT;
}

uint32_t loc_attempt_budget_s(const loc_policy_t *p)
{
    return p->long_budget_next ? LOC_FIRST_ATTEMPT_S : LOC_ATTEMPT_S;
}

void loc_policy_request_long_budget(loc_policy_t *p)
{
    p->long_budget_next = true;
}

void loc_on_attempt_done(loc_policy_t *p, bool success, double lat, double lon, bool have_acc,
                          int32_t acc_m, int64_t fix_ts_epoch_s, uint8_t src)
{
    p->attempt_in_progress = false;
    p->long_budget_next = false;
    p->trigger_floor_until_us = 0; // consumed: its only job was delaying this attempt

    if (success) {
        p->backoff_s = 0;
        p->have_fix = true;
        p->fix_lat = lat;
        p->fix_lon = lon;
        p->fix_have_acc = have_acc;
        p->fix_acc_m = acc_m;
        p->fix_ts_epoch_s = fix_ts_epoch_s;
        p->fix_src = src;
    } else {
        p->backoff_s = loc_next_backoff_s(p->backoff_s);
    }
}

bool loc_take_queued_id(loc_policy_t *p, char *out, size_t cap)
{
    if (p->queued_count <= 0 || !out || cap == 0) {
        return false;
    }
    p->queued_count--;
    strncpy(out, p->queued_ids[p->queued_count], cap - 1);
    out[cap - 1] = '\0';
    return true;
}

bool loc_get_cached(const loc_policy_t *p, double *lat, double *lon, bool *have_acc,
                    int32_t *acc_m, int64_t *fix_ts_epoch_s, uint8_t *src)
{
    if (!p->have_fix) {
        return false;
    }
    if (lat) *lat = p->fix_lat;
    if (lon) *lon = p->fix_lon;
    if (have_acc) *have_acc = p->fix_have_acc;
    if (acc_m) *acc_m = p->fix_acc_m;
    if (fix_ts_epoch_s) *fix_ts_epoch_s = p->fix_ts_epoch_s;
    if (src) *src = p->fix_src;
    return true;
}

// Shared by both triggers: backoff -> 0, plus the 10-minute floor measured
// from the last *attempt* (not from now — V02_DESIGN.md §5's own wording).
static void apply_trigger_reset_locked(loc_policy_t *p)
{
    p->backoff_s = 0;
    p->trigger_floor_until_us = p->last_attempt_us + (int64_t) LOC_TRIGGER_FLOOR_S * 1000000;
}

bool loc_trigger_cell_change(loc_policy_t *p, int64_t now_us, const char *cell_key)
{
    if (!cell_key || cell_key[0] == '\0') {
        return false;
    }
    if (!p->have_cell_key) {
        strncpy(p->cell_key, cell_key, sizeof(p->cell_key) - 1);
        p->cell_key[sizeof(p->cell_key) - 1] = '\0';
        p->have_cell_key = true;
        return false; // first observation ever — nothing "changed" yet
    }
    if (strncmp(p->cell_key, cell_key, sizeof(p->cell_key)) == 0) {
        return false; // not actually a change
    }
    strncpy(p->cell_key, cell_key, sizeof(p->cell_key) - 1);
    p->cell_key[sizeof(p->cell_key) - 1] = '\0';

    if (p->have_last_cell_trigger &&
        (now_us - p->last_cell_trigger_us) < (int64_t) LOC_CELL_DEBOUNCE_S * 1000000) {
        return false; // debounced: a stationary indoor modem flapping between cells
    }
    p->have_last_cell_trigger = true;
    p->last_cell_trigger_us = now_us;
    apply_trigger_reset_locked(p);
    return true;
}

static void motion_evict_locked(loc_motion_ring_t *r, int64_t now_us)
{
    int64_t cutoff_us = now_us - (int64_t) LOC_MOTION_WINDOW_S * 1000000;
    int i = 0;
    while (i < r->n && r->events_us[i] < cutoff_us) {
        i++;
    }
    if (i > 0) {
        memmove(&r->events_us[0], &r->events_us[i], (size_t) (r->n - i) * sizeof(int64_t));
        r->n -= i;
    }
}

// Shared by loc_trigger_motion_event() (loc_policy_t's own on-demand
// classifier) and loc_track_on_motion() (loc_track_t's independent copy,
// LOCATION_TRACKING_DESIGN.md task F1) -- the >=60s-within-3min sustained-
// motion test itself, with no opinion on what firing it should DO (the two
// callers reset different state). Returns true iff `now_us`'s edge makes
// the ring newly sustained, in which case the ring is cleared (see the
// caller-facing doc comments for why: a continuing stream of interrupts
// needs a fresh 60s span before firing again).
static bool motion_ring_sustained(loc_motion_ring_t *r, int64_t now_us)
{
    motion_evict_locked(r, now_us);

    if (r->n == LOC_MOTION_RING) {
        // Ring full within the window: drop the oldest to make room — this
        // only shortens the measured span (biased toward "not yet
        // sustained"), never fabricates a false positive.
        memmove(&r->events_us[0], &r->events_us[1], (LOC_MOTION_RING - 1) * sizeof(int64_t));
        r->n--;
    }
    r->events_us[r->n++] = now_us;

    if (r->n < 2) {
        return false;
    }
    int64_t span_us = r->events_us[r->n - 1] - r->events_us[0];
    if (span_us < (int64_t) LOC_MOTION_SUSTAIN_S * 1000000) {
        return false;
    }

    r->n = 0;
    return true;
}

bool loc_trigger_motion_event(loc_policy_t *p, int64_t now_us)
{
    if (!motion_ring_sustained(&p->motion, now_us)) {
        return false;
    }
    // Sustained motion confirmed -- reset the on-demand backoff/floor
    // (apply_trigger_reset_locked()'s own doc comment).
    apply_trigger_reset_locked(p);
    return true;
}

// ---------------------------------------------------------------------------
// Location tracking (docs/LOCATION_TRACKING_DESIGN.md, task F1) -- pure,
// host-tested by firmware/host/test_loc.c. See loc.h's own loc_track_t/
// loc_track_*() doc comments for what each piece decides; this section is
// just the implementation.
// ---------------------------------------------------------------------------

void loc_track_init(loc_track_t *t, bool have_lis3dh)
{
    memset(t, 0, sizeof(*t));
    t->have_lis3dh = have_lis3dh;
    t->state = LOC_MSTATE_STILL;
}

loc_mstate_t loc_track_state(const loc_track_t *t) { return t->state; }

// §1's "not in a 4-entry ring of distinct cells seen in the last 60 min"
// membership test. Ages out slots older than the window first (so a stale
// slot can never wrongly count as membership), then looks for `key`; a hit
// touches the slot's timestamp (this cell is still "recently seen", same as
// if it had just been (re-)inserted) and reports membership. A miss reports
// non-membership WITHOUT inserting -- the caller (loc_track_on_cell()) only
// inserts once it has also decided this is worth treating as "new".
static bool cell_ring_touch(loc_cell_ring_t *ring, int64_t now_us, const char *key)
{
    for (int i = 0; i < LOC_CELL_RING_N; i++) {
        if (ring->slot[i].used &&
            (now_us - ring->slot[i].last_seen_us) > (int64_t) LOC_CELL_RING_WINDOW_S * 1000000) {
            ring->slot[i].used = false; // aged out of the 60-min membership window
        }
    }
    for (int i = 0; i < LOC_CELL_RING_N; i++) {
        if (ring->slot[i].used && strncmp(ring->slot[i].key, key, LOC_CELL_KEY_MAX) == 0) {
            ring->slot[i].last_seen_us = now_us;
            return true;
        }
    }
    return false;
}

// Inserts a key the caller has already decided is "new" (cell_ring_touch()
// just returned false for it). Prefers an empty slot; once all
// LOC_CELL_RING_N are in use, evicts the least-recently-seen one -- an
// indoor modem cycling through more than 4 cells an hour loses the oldest
// one first, same "biased toward not yet sustained" bias
// motion_ring_sustained()'s own eviction uses.
static void cell_ring_insert(loc_cell_ring_t *ring, int64_t now_us, const char *key)
{
    int slot_idx = 0;
    int64_t oldest_us = INT64_MAX;
    for (int i = 0; i < LOC_CELL_RING_N; i++) {
        if (!ring->slot[i].used) {
            slot_idx = i;
            oldest_us = INT64_MIN; // force this pick even if a later iteration checks oldest_us
            break;
        }
        if (ring->slot[i].last_seen_us < oldest_us) {
            oldest_us = ring->slot[i].last_seen_us;
            slot_idx = i;
        }
    }
    strncpy(ring->slot[slot_idx].key, key, LOC_CELL_KEY_MAX - 1);
    ring->slot[slot_idx].key[LOC_CELL_KEY_MAX - 1] = '\0';
    ring->slot[slot_idx].last_seen_us = now_us;
    ring->slot[slot_idx].used = true;
}

// §1's "2 new cells within 15 min" classifier -- same shape as
// motion_ring_sustained() (evict outside the window, cap the ring, insert,
// test), just counting events rather than measuring a span. Unlike the
// motion ring, this one does NOT clear itself on firing: a third new cell a
// minute later should still read "2+ in the last 15 min" (it does, since the
// eviction above only drops entries older than the window), which matters
// for loc_track_on_cell()'s own "only actually transitions once, from
// STILL" gate -- this function reports the raw count-based fact every time,
// the state check happens one level up.
static bool newcell_ring_count(loc_newcell_ring_t *r, int64_t now_us)
{
    int i = 0;
    while (i < r->n && r->events_us[i] <= now_us - (int64_t) LOC_NEWCELL_WINDOW_S * 1000000) {
        i++;
    }
    if (i > 0) {
        memmove(&r->events_us[0], &r->events_us[i], (size_t) (r->n - i) * sizeof(int64_t));
        r->n -= i;
    }
    if (r->n >= (int) (sizeof(r->events_us) / sizeof(r->events_us[0]))) {
        memmove(&r->events_us[0], &r->events_us[1], (r->n - 1) * sizeof(int64_t));
        r->n--;
    }
    r->events_us[r->n++] = now_us;
    return r->n >= LOC_NEWCELL_COUNT;
}

// Shared by loc_track_on_motion()/loc_track_on_cell() below: the one place
// a STILL->MOVING transition is actually applied, so the "transition-only
// backoff reset" rule (§1, §2.3) can never be duplicated or missed by one
// of the two triggers. Resets the move-schedule's own backoff/attempt clock
// (NOT loc_policy_t's on-demand one -- they are independent, loc_track_t's
// own module comment) and starts a fresh episode (no GNSS fix published
// yet, no attempts yet).
static void track_enter_moving_locked(loc_track_t *t, int64_t now_us)
{
    t->state = LOC_MSTATE_MOVING;
    t->moving_since_us = now_us;
    t->gnss_backoff_s = 0;
    t->gnss_last_attempt_us = 0;
    t->gnss_published_this_episode = false;
}

bool loc_track_on_motion(loc_track_t *t, int64_t now_us)
{
    t->last_motion_edge_us = now_us;
    if (!t->have_lis3dh) {
        return false; // §1: without the chip, only the cell rule can transition
    }
    if (motion_ring_sustained(&t->motion, now_us) && t->state == LOC_MSTATE_STILL) {
        track_enter_moving_locked(t, now_us);
        return true;
    }
    return false;
}

bool loc_track_on_cell(loc_track_t *t, int64_t now_us, const char *cell_key)
{
    if (!cell_key || cell_key[0] == '\0') {
        return false;
    }
    bool first_ever = !t->have_cur_cell;
    strncpy(t->cur_cell_key, cell_key, LOC_CELL_KEY_MAX - 1);
    t->cur_cell_key[LOC_CELL_KEY_MAX - 1] = '\0';
    t->have_cur_cell = true;
    t->cur_cell_since_us = now_us; // every call is a genuine change (caller de-duplicates)

    if (first_ever) {
        // Same carve-out loc_trigger_cell_change() documents for the
        // on-demand mechanism: the very first cell this power session is a
        // starting point, not an observed CHANGE -- seed the membership
        // ring with it (so a return to it later is not "new" either) but
        // never count it toward "2 new cells within 15 min".
        cell_ring_insert(&t->cell_ring, now_us, cell_key);
        return false;
    }

    if (cell_ring_touch(&t->cell_ring, now_us, cell_key)) {
        return false; // seen in the last 60 min -- not "new" (flap protection, §1)
    }
    cell_ring_insert(&t->cell_ring, now_us, cell_key);
    t->last_new_cell_us = now_us;

    if (newcell_ring_count(&t->newcell_events, now_us) && t->state == LOC_MSTATE_STILL) {
        track_enter_moving_locked(t, now_us);
        return true;
    }
    return false;
}

bool loc_track_gate_ok(const loc_track_t *t, int64_t now_us)
{
    return t->last_loc_us == 0 ||
           (now_us - t->last_loc_us) >= (int64_t) LOC_UNSOLICITED_MIN_GAP_S * 1000000;
}

// Relative priority used to merge a newly-computed candidate with whatever
// is already latched in `pending_report` (loc_track_tick()'s own comment):
// higher wins. LOC_REPORT_GNSS never actually reaches this (loc_track_tick()
// never returns it -- see loc_report_t's own comment) but is given a
// sensible mid rank anyway rather than left implicitly 0, in case a future
// caller merges it in too.
static int report_priority(loc_report_t r)
{
    switch (r) {
    case LOC_REPORT_STOP: return 4;
    case LOC_REPORT_CELL: return 3;
    case LOC_REPORT_MOVE: return 2;
    case LOC_REPORT_STILL: return 2;
    case LOC_REPORT_GNSS: return 1;
    case LOC_REPORT_NONE:
    default: return 0;
    }
}

loc_report_t loc_track_tick(loc_track_t *t, int64_t now_us)
{
    loc_report_t wanted = LOC_REPORT_NONE;

    // 1. MOVING -> STILL check (§1). Without a LIS3DH there is no accel
    // signal at all, so "no motion for 300s" is vacuously true -- only the
    // "no new cell for 900s" half of the rule actually gates the transition
    // (§1: "Without a LIS3DH, only the cell rule applies").
    if (t->state == LOC_MSTATE_MOVING) {
        // `== 0` ("never happened at all this episode") satisfies each half
        // exactly like an old-enough timestamp would -- there is nothing to
        // wait out. Distinct from loc_track_gnss_due()'s own exit-tail
        // guard below, which fails CLOSED on a `== 0` reading (no signal
        // yet is not a reason to spend a GNSS attempt); this is the
        // opposite question ("has motion been ABSENT long enough"), so an
        // absence of any reading at all trivially answers it.
        bool no_motion = t->have_lis3dh
                             ? (t->last_motion_edge_us == 0 ||
                                (now_us - t->last_motion_edge_us) >=
                                    (int64_t) LOC_STILL_MOTION_GAP_S * 1000000)
                             : true;
        bool no_new_cell = t->last_new_cell_us == 0 ||
                            (now_us - t->last_new_cell_us) >= (int64_t) LOC_STILL_CELL_GAP_S * 1000000;
        if (no_motion && no_new_cell) {
            t->state = LOC_MSTATE_STILL;
            // §2.1 "End of motion": a report only when it says something new
            // -- the cell differs from the last one reported, or a GNSS fix
            // went out this episode. Otherwise the transition is silent.
            bool cell_differs = t->have_cur_cell &&
                                 (!t->have_last_reported_cell ||
                                  strncmp(t->last_reported_cell_key, t->cur_cell_key,
                                          LOC_CELL_KEY_MAX) != 0);
            if (cell_differs || t->gnss_published_this_episode) {
                wanted = LOC_REPORT_STOP;
            }
            t->gnss_published_this_episode = false; // next episode starts clean
        }
    }

    // 2. Cell-change report (§2.1): independent of STILL/MOVING -- a new
    // serving cell is worth a report in either state. Gated on having served
    // for LOC_CELL_REPORT_SERVING_S already (debounces a brief handover
    // blip) on top of differing from the last *reported* cell (a cell that
    // flapped back to one already reported is not "new" for this purpose).
    if (wanted == LOC_REPORT_NONE && t->have_cur_cell &&
        (!t->have_last_reported_cell ||
         strncmp(t->last_reported_cell_key, t->cur_cell_key, LOC_CELL_KEY_MAX) != 0) &&
        (now_us - t->cur_cell_since_us) >= (int64_t) LOC_CELL_REPORT_SERVING_S * 1000000) {
        wanted = LOC_REPORT_CELL;
    }

    // 3. Periodic reports -- mutually exclusive by state, so these two
    // never actually compete with each other.
    if (wanted == LOC_REPORT_NONE) {
        if (t->state == LOC_MSTATE_STILL) {
            if (t->last_loc_us == 0 ||
                (now_us - t->last_loc_us) >= (int64_t) LOC_HOURLY_CELL_S * 1000000) {
                wanted = LOC_REPORT_STILL;
            }
        } else if (t->last_loc_us == 0 ||
                   (now_us - t->last_loc_us) >= (int64_t) LOC_MOVE_REFRESH_S * 1000000) {
            wanted = LOC_REPORT_MOVE;
        }
    }

    // P1 (server-architect review, 26 Sep 2026): merge with anything already
    // latched waiting on the 120s gate -- a report due early is never
    // dropped, only ever superseded by something MORE urgent that becomes
    // due while it waits.
    if (report_priority(wanted) < report_priority(t->pending_report)) {
        wanted = t->pending_report;
    }
    if (wanted == LOC_REPORT_NONE) {
        return LOC_REPORT_NONE;
    }
    if (!loc_track_gate_ok(t, now_us)) {
        t->pending_report = wanted;
        return LOC_REPORT_NONE;
    }
    t->pending_report = LOC_REPORT_NONE;
    return wanted;
}

void loc_track_note_report_sent(loc_track_t *t, int64_t now_us, loc_report_t which, bool gnss_fix)
{
    t->last_loc_us = now_us;
    if (which == LOC_REPORT_GNSS && gnss_fix) {
        t->gnss_published_this_episode = true;
    }
    if (t->have_cur_cell) {
        strncpy(t->last_reported_cell_key, t->cur_cell_key, LOC_CELL_KEY_MAX - 1);
        t->last_reported_cell_key[LOC_CELL_KEY_MAX - 1] = '\0';
        t->have_last_reported_cell = true;
    }
}

bool loc_track_gnss_assist_due(const loc_track_t *t, int64_t now_us)
{
    return t->gnss_last_assist_refresh_us == 0 ||
           (now_us - t->gnss_last_assist_refresh_us) >= (int64_t) LOC_GNSS_ASSIST_MAX_AGE_S * 1000000;
}

void loc_track_gnss_assist_refreshed(loc_track_t *t, int64_t now_us)
{
    t->gnss_last_assist_refresh_us = now_us;
}

bool loc_track_gnss_cap_reached(loc_track_t *t, int64_t now_us)
{
    int i = 0;
    while (i < t->gnss_attempt_ring_n &&
           t->gnss_attempt_ring_us[i] <= now_us - (int64_t) LOC_GNSS_DAY_S * 1000000) {
        i++;
    }
    if (i > 0) {
        memmove(&t->gnss_attempt_ring_us[0], &t->gnss_attempt_ring_us[i],
                (size_t) (t->gnss_attempt_ring_n - i) * sizeof(int64_t));
        t->gnss_attempt_ring_n -= i;
    }
    return t->gnss_attempt_ring_n >= LOC_GNSS_DAILY_CAP;
}

void loc_track_gnss_attempt_started(loc_track_t *t, int64_t now_us)
{
    if (t->gnss_attempt_ring_n < LOC_GNSS_DAILY_CAP) {
        t->gnss_attempt_ring_us[t->gnss_attempt_ring_n++] = now_us;
    }
    t->gnss_last_attempt_us = now_us;
}

void loc_track_gnss_attempt_done(loc_track_t *t, bool success)
{
    t->gnss_backoff_s = success ? 0 : loc_next_backoff_s(t->gnss_backoff_s);
}

bool loc_track_gnss_due(const loc_track_t *t, int64_t now_us, uint32_t move_gnss_s)
{
    if (move_gnss_s == 0 || t->state != LOC_MSTATE_MOVING) {
        return false;
    }
    // §2.2: "the first attempt runs one full interval after MOVING starts" --
    // `base_us` is moving_since_us until the first scheduled attempt, then
    // gnss_last_attempt_us for every one after that (§2.3's own
    // max(last attempt + 600s, backoff) formula).
    int64_t base_us = t->gnss_last_attempt_us ? t->gnss_last_attempt_us : t->moving_since_us;
    int64_t interval_due_us = base_us + (int64_t) move_gnss_s * 1000000;
    int64_t backoff_due_us = t->gnss_last_attempt_us + (int64_t) t->gnss_backoff_s * 1000000;
    int64_t due_us = interval_due_us > backoff_due_us ? interval_due_us : backoff_due_us;
    if (now_us < due_us) {
        return false;
    }
    // §2.2's own exit-tail guard: a passing episode's tail must not spend
    // one more attempt after motion has effectively already stopped.
    if (t->have_lis3dh) {
        if (t->last_motion_edge_us == 0 ||
            (now_us - t->last_motion_edge_us) > (int64_t) LOC_MOVE_GNSS_EDGE_WINDOW_S * 1000000) {
            return false;
        }
    } else if (t->last_new_cell_us == 0 ||
               (now_us - t->last_new_cell_us) > (int64_t) LOC_MOVE_GNSS_CELL_WINDOW_S * 1000000) {
        return false;
    }
    return true;
}

uint32_t loc_web_gnss_budget_s(uint32_t base_budget_s, uint32_t assist_elapsed_s)
{
    if (assist_elapsed_s >= LOC_WEB_REQUEST_BOUND_S) {
        return 0;
    }
    uint32_t remaining_s = LOC_WEB_REQUEST_BOUND_S - assist_elapsed_s;
    return base_budget_s < remaining_s ? base_budget_s : remaining_s;
}

// PROTOCOL.md §10 keymap subset this file writes/reads.
#define LK_V 0
#define LK_ID 1
#define LK_TS 2
#define LK_KIND 6
#define LK_LOC 8
#define LK_REQ 9
#define LK_CACHED 10
#define LK_ERR 11
#define LK_N 12
#define LK_CELL 49 // this task, §13.2 -- see loc.h's own note on why this is written out of
                   // ascending-key order (right before `n`, matching §13.2's field table)
#define LK_WHY 60  // LOCATION_TRACKING_DESIGN.md §5 P3 -- same out-of-order placement as LK_CELL,
                   // right after it and before `n`, matching that doc's own envelope example
// `loc` sub-map (docs/PROTOCOL.md §10 "Sub-map keys").
#define LOCSUB_LAT 0
#define LOCSUB_LON 1
#define LOCSUB_ACC 2
#define LOCSUB_FIXTS 3
#define LOCSUB_SRC 4
// `cell` sub-map (docs/PROTOCOL.md §10 "Sub-map keys", this task).
#define CELLSUB_MCC 0
#define CELLSUB_MNC 1
#define CELLSUB_TAC 2
#define CELLSUB_CI 3
#define CELLSUB_RSRP 4

// `v` MUST be non-negative for cbor_w_uint(); rsrp is not -- same tiny local
// wrapper modes.c's build_status_cbor() defines for the same reason (rssi).
static bool cbor_w_int(cbor_w_t *w, uint32_t key, int64_t v)
{
    return (v < 0) ? cbor_w_nint(w, key, v) : cbor_w_uint(w, key, (uint64_t) v);
}

// NULL if `cell` is NULL or malformed (mcc not exactly 3 digits, mnc not
// 2-3 digits) -- loc_build_cbor()'s own doc comment explains why this is a
// silent downgrade to "no cell" rather than a build failure.
static bool cell_shape_ok(const loc_cell_t *cell)
{
    if (!cell) {
        return false;
    }
    size_t mcc_len = strlen(cell->mcc), mnc_len = strlen(cell->mnc);
    return mcc_len == 3 && (mnc_len == 2 || mnc_len == 3) && cell->tac <= 65535u &&
           cell->ci <= 268435455u;
}

bool loc_build_cbor(uint8_t *out, size_t cap, size_t *out_len, bool signed_env, uint64_t n,
                     const char *id, int64_t ts, bool have_fix, double lat, double lon,
                     bool have_acc, int32_t acc_m, int64_t fix_ts, bool src_cell, const char *req,
                     bool cached, const char *err, const loc_cell_t *cell, const char *why)
{
    if (!out || !out_len || !id) {
        return false;
    }
    if (have_fix == (err != NULL && err[0] != '\0')) {
        // §13.2: `err` present iff `loc` is null — these two must disagree.
        return false;
    }
    bool have_cell = cell_shape_ok(cell);
    bool have_why = (why != NULL && why[0] != '\0');

    uint32_t nfields = 3; // v, id, ts
    nfields += 1;         // loc (map or null)
    nfields += 1;         // req (tstr or null)
    if (cached) {
        nfields += 1;
    }
    if (!have_fix) {
        nfields += 1; // err
    }
    if (have_cell) {
        nfields += 1; // cell (this task)
    }
    if (have_why) {
        nfields += 1; // why (LOCATION_TRACKING_DESIGN.md §5 P3)
    }
    if (signed_env) {
        nfields += 2; // n (written below) + sig (appended by the caller's auth_sign())
    }

    cbor_w_t w;
    cbor_w_init(&w, out, cap);
    cbor_w_map(&w, nfields);
    cbor_w_uint(&w, LK_V, 1);
    cbor_w_tstr(&w, LK_ID, id, strlen(id));
    cbor_w_uint(&w, LK_TS, (uint64_t) ts);

    if (have_fix) {
        uint32_t loc_n = 2; // lat, lon
        loc_n += 1;         // fix_ts
        if (have_acc) {
            loc_n += 1;
        }
        if (src_cell) {
            loc_n += 1;
        }
        cbor_w_map_key(&w, LK_LOC, loc_n);
        cbor_w_f64(&w, LOCSUB_LAT, lat);
        cbor_w_f64(&w, LOCSUB_LON, lon);
        if (have_acc) {
            cbor_w_uint(&w, LOCSUB_ACC, (uint64_t) acc_m);
        }
        cbor_w_uint(&w, LOCSUB_FIXTS, (uint64_t) fix_ts);
        if (src_cell) {
            cbor_w_tstr(&w, LOCSUB_SRC, "cell", 4);
        }
    } else {
        cbor_w_null(&w, LK_LOC);
    }

    if (req) {
        cbor_w_tstr(&w, LK_REQ, req, strlen(req));
    } else {
        cbor_w_null(&w, LK_REQ);
    }
    if (cached) {
        cbor_w_bool(&w, LK_CACHED, true);
    }
    if (!have_fix) {
        cbor_w_tstr(&w, LK_ERR, err, strlen(err));
    }
    if (have_cell) {
        cbor_w_map_key(&w, LK_CELL, cell->have_rsrp ? 5 : 4);
        cbor_w_tstr(&w, CELLSUB_MCC, cell->mcc, strlen(cell->mcc));
        cbor_w_tstr(&w, CELLSUB_MNC, cell->mnc, strlen(cell->mnc));
        cbor_w_uint(&w, CELLSUB_TAC, cell->tac);
        cbor_w_uint(&w, CELLSUB_CI, cell->ci);
        if (cell->have_rsrp) {
            cbor_w_int(&w, CELLSUB_RSRP, cell->rsrp);
        }
    }
    if (have_why) {
        cbor_w_tstr(&w, LK_WHY, why, strlen(why));
    }
    if (signed_env) {
        cbor_w_uint(&w, LK_N, n);
    }

    if (w.err) {
        return false;
    }
    *out_len = w.len;
    return true;
}

bool loc_parse_req_cbor(const uint8_t *buf, uint16_t len, bool sig_pair_present, char *out_id,
                        size_t out_id_cap)
{
    if (out_id && out_id_cap > 0) {
        out_id[0] = '\0';
    }

    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count) || count == 0) {
        return false;
    }
    if (sig_pair_present) {
        // docs/DEVICE_PLAN.md §2.4's "no re-serialisation" rule — same
        // adjustment lock_parse_cfg()/msg.c's MK_* readers make.
        count -= 1;
    }

    bool is_loc_req = false;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            return false;
        }
        switch (key) {
        case LK_ID: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) {
                return false;
            }
            if (out_id && slen < out_id_cap) {
                memcpy(out_id, s, slen);
                out_id[slen] = '\0';
            }
            break;
        }
        case LK_KIND: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) {
                return false;
            }
            is_loc_req = (slen == 7 && memcmp(s, "loc_req", 7) == 0);
            break;
        }
        default:
            if (!cbor_r_skip(&r)) {
                return false;
            }
            break;
        }
    }

    return is_loc_req && out_id && out_id[0] != '\0';
}

#ifdef ESP_PLATFORM

#include "accel.h"
#include "ident.h"
#include "modes.h"
#include "net.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "loc";

// ---------------------------------------------------------------------------
// RTC + auth_rtc_t wiring (loc.h: loc_bind()) — same pattern as
// lock_bind_rtc()/book_bind(). Defaults are no-ops so an accessor called
// before loc_bind() (should not happen — modes_boot() binds before anything
// else can run) never dereferences NULL, same defensive pattern lock.c
// documents for the identical real-hardware finding.
// ---------------------------------------------------------------------------

static void loc_rtc_noop(void) {}

static loc_rtc_t *s_rtc = NULL;
static auth_rtc_t *s_auth_rtc = NULL;
static loc_rtc_lock_fn s_lock = loc_rtc_noop;
static loc_rtc_unlock_fn s_unlock = loc_rtc_noop;
static loc_rtc_save_fn s_save = loc_rtc_noop;
static loc_epoch_wrap_fn s_on_wrap = loc_rtc_noop;

void loc_bind(loc_rtc_t *rtc, auth_rtc_t *auth_rtc, loc_rtc_lock_fn lock, loc_rtc_unlock_fn unlock,
              loc_rtc_save_fn save, loc_epoch_wrap_fn on_wrap)
{
    s_rtc = rtc;
    s_auth_rtc = auth_rtc;
    s_lock = lock ? lock : loc_rtc_noop;
    s_unlock = unlock ? unlock : loc_rtc_noop;
    s_save = save ? save : loc_rtc_noop;
    s_on_wrap = on_wrap ? on_wrap : loc_rtc_noop;
}

// The one policy instance for normal operation (RAM-only, see loc.h). Guarded
// by s_lock/s_unlock (the same cross-task mutex modes.c's g_rtc uses) since
// loc_ingest_req_cbor() runs on WalterModem's _eventProcessingTask (via
// on_incoming_message()) while loc_service()/loc_debug_run() run on
// modes_run()'s task.
static loc_policy_t s_policy;

// LOCATION_TRACKING_DESIGN.md (task F1/F3): the background tracker's state.
// Guarded by the same s_lock/s_unlock -- loc_on_cell_change() (net.h's
// net_set_cell_change_cb() callback) can run on WalterModem's
// _eventProcessingTask, same reasoning s_policy's own comment gives.
static loc_track_t s_track;

// `loctrack on|off` / `locmove <s>` (main.c console commands, this task's
// own test plan §9) -- RAM-only runtime flags, not RTC-resident (bench/debug
// knobs, not something that needs to survive a reset). Guarded by s_lock/
// s_unlock for the same cross-task reason as s_track above.
static bool s_track_enabled = LOC_TRACK_DEFAULT_ENABLED;
static uint32_t s_move_gnss_s = LOC_MOVE_GNSS_DEFAULT_S;

// ---------------------------------------------------------------------------
// /status fields (docs/PROTOCOL.md §5.1, V02_DESIGN.md §5/§7,
// LOCATION_TRACKING_DESIGN.md §5 P2/P3).
// ---------------------------------------------------------------------------

uint32_t loc_get_min_s(void) { return LOC_STATUS_MIN_S; }

uint32_t loc_get_period_s(void)
{
    s_lock();
    bool on = s_track_enabled;
    s_unlock();
    return on ? LOC_STATUS_PERIOD_TRACKING_S : LOC_STATUS_PERIOD_S;
}

uint32_t loc_get_move_gnss_s(void)
{
    s_lock();
    uint32_t v = s_move_gnss_s;
    s_unlock();
    return v;
}

void loc_set_track_enabled(bool enabled)
{
    s_lock();
    bool changed = (s_track_enabled != enabled);
    s_track_enabled = enabled;
    s_unlock();
    if (changed) {
        ESP_LOGI(TAG, "loctrack: %s", enabled ? "on" : "off");
    }
}

bool loc_get_track_enabled(void)
{
    s_lock();
    bool on = s_track_enabled;
    s_unlock();
    return on;
}

void loc_set_move_gnss_s(uint32_t seconds)
{
    s_lock();
    s_move_gnss_s = seconds;
    s_unlock();
    ESP_LOGI(TAG, "locmove: %us%s", (unsigned) seconds, seconds == 0 ? " (off)" : "");
}

// coverage.c's own radio-ownership rule (this task, coverage.h's module
// comment): read every modes_run() iteration, so this must stay a cheap
// lock-guarded flag read, never an AT round trip.
bool loc_attempt_in_progress(void)
{
    s_lock();
    bool b = s_policy.attempt_in_progress;
    s_unlock();
    return b;
}

uint32_t loc_get_backoff_remaining_s(void)
{
    s_lock();
    int64_t now_us = esp_timer_get_time();
    int64_t backoff_until_us = s_policy.last_attempt_us + (int64_t) s_policy.backoff_s * 1000000;
    int64_t floor_us = s_policy.trigger_floor_until_us;
    int64_t deadline_us = backoff_until_us > floor_us ? backoff_until_us : floor_us;
    s_unlock();
    if (deadline_us <= now_us) {
        return 0;
    }
    return (uint32_t) ((deadline_us - now_us + 999999) / 1000000); // round up
}

// Forward declaration: defined with the rest of the GNSS attempt state
// machine further down (near s_phase et al), but loc_ingest_req_cbor()
// above that section needs to call it to actually arm an attempt.
static void begin_attempt(uint32_t budget_s, bool extendable);

// Forward declaration: defined with loc_init() further down (task F3), but
// loc_service() above that section calls it every iteration.
static void loc_track_poll(void);

// ---------------------------------------------------------------------------
// /loc publish (docs/PROTOCOL.md §13.1/§13.2, §14 signing).
// ---------------------------------------------------------------------------

// Converts net.h's modem-backed cache (net_cell_info_t) into loc.h's pure
// loc_cell_t, this module's own boundary between ESP-IDF and host-testable
// code (same split loc_build_cbor()/loc.h document). Returns false (out
// untouched) if the cell has never been read successfully this power
// session -- callers must then omit `cell` (net_get_cell_info()'s own doc
// comment, PROTOCOL.md §13.2: "if the cell cannot be read, send the answer
// without it").
static bool get_cell_snapshot(loc_cell_t *out)
{
    net_cell_info_t nci;
    if (!net_get_cell_info(&nci)) {
        return false;
    }
    strncpy(out->mcc, nci.mcc, sizeof(out->mcc) - 1);
    out->mcc[sizeof(out->mcc) - 1] = '\0';
    strncpy(out->mnc, nci.mnc, sizeof(out->mnc) - 1);
    out->mnc[sizeof(out->mnc) - 1] = '\0';
    out->tac = nci.tac;
    out->ci = nci.ci;
    out->have_rsrp = nci.have_rsrp;
    out->rsrp = nci.rsrp;
    return true;
}

// This task's own product decision (LOC_CELL_STALE_FIX_S's own comment,
// loc.h): attach `cell` to a *cached* /loc answer once the fix behind it is
// old enough that it may no longer reflect where the pager actually is.
// "Don't know" (no wall clock yet, or the fix carries no timestamp) is
// deliberately NOT treated as stale -- never guess into extra AT traffic.
static bool cell_fix_is_stale(int64_t fix_ts_epoch_s)
{
    if (fix_ts_epoch_s <= 0) {
        return false;
    }
    int64_t now_epoch = 0;
    if (!net_get_clock(&now_epoch)) {
        return false;
    }
    return (now_epoch - fix_ts_epoch_s) > (int64_t) LOC_CELL_STALE_FIX_S;
}

// req_id may be NULL for an unsolicited fix (never actually produced by this
// task — loc_period_s stays 0 — kept general per loc_build_cbor()'s own
// comment). QoS 1 iff req_id is non-NULL, PROTOCOL.md §13.1. `cell` (this
// task, §13.2): NULL to omit the sub-map, otherwise a snapshot the caller
// already decided belongs on this answer (loc_ingest_req_cbor()/
// finish_attempt() below own that decision; this function never queries
// net_get_cell_info() itself, so a shared queue-drain fetches the cell at
// most once per decision instead of once per queued requester).
// `why` (LOCATION_TRACKING_DESIGN.md §5 P3, this task): NULL for every
// on-demand answer (loc_req/gnsstest never carry it); the tracking report
// reason string for an unsolicited (req_id NULL, qos 0) publish.
static void publish_loc_answer(const char *req_id, bool success, double lat, double lon,
                                bool have_acc, int32_t acc_m, int64_t fix_ts, uint8_t src,
                                bool cached, const loc_cell_t *cell, const char *why)
{
    char id[LOC_ID_MAX];
    snprintf(id, sizeof(id), "l_%08x", (unsigned) esp_random());

    int64_t ts = 0;
    net_get_clock(&ts); // best-effort; §3.5 says publish ts:0 on failure, never retry

    bool signed_env = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    uint64_t n = 0;
    bool wrapped = false;
    if (signed_env) {
        s_lock();
        if (s_auth_rtc) {
            n = auth_next_up_n(s_auth_rtc, ident_get_n_epoch(), &wrapped);
        }
        s_save();
        s_unlock();
    }

    uint8_t buf[192]; // envelope + loc map, generous (§3.3: a real /loc is ~200B worst case)
    size_t len;
    bool src_cell = (src == LOC_SRC_CELL);
    if (!loc_build_cbor(buf, sizeof(buf), &len, signed_env, n, id, ts, success, lat, lon, have_acc,
                        acc_m, fix_ts, src_cell, req_id, cached, success ? NULL : "no_fix", cell,
                        why)) {
        ESP_LOGI(TAG, "/loc CBOR build failed for req=%s", req_id ? req_id : "(none)");
        return;
    }

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/loc", net_get_device_id());

    if (signed_env) {
        if (!auth_sign(topic, buf, &len, sizeof(buf))) {
            ESP_LOGI(TAG, "/loc auth_sign() failed for req=%s", req_id ? req_id : "(none)");
            return;
        }
        if (wrapped) {
            s_on_wrap();
        }
    }

    uint8_t qos = (req_id != NULL) ? 1 : 0; // §13.1
    if (net_publish_raw(topic, buf, (uint16_t) len, qos)) {
        ESP_LOGI(TAG, "/loc %s published: req=%s why=%s %s cached=%d cell=%d", id,
                 req_id ? req_id : "(none)", why ? why : "(none)", success ? "fix" : "no_fix",
                 (int) cached, (int) (cell != NULL));
    } else {
        ESP_LOGI(TAG, "/loc publish failed for req=%s why=%s", req_id ? req_id : "(none)",
                 why ? why : "(none)");
    }
}

// Drains every queued request id and gives each its own /loc, all carrying
// the one shared result just obtained (PROTOCOL.md §13.3 item 3) and the one
// shared `cell` snapshot (this task) — fetched at most once by the caller,
// never re-fetched per queued requester here.
static void drain_queue_and_publish(bool success, double lat, double lon, bool have_acc,
                                     int32_t acc_m, int64_t fix_ts, uint8_t src,
                                     const loc_cell_t *cell)
{
    char id[LOC_ID_MAX];
    s_lock();
    bool have = loc_take_queued_id(&s_policy, id, sizeof(id));
    s_unlock();
    while (have) {
        publish_loc_answer(id, success, lat, lon, have_acc, acc_m, fix_ts, src, false, cell, NULL);
        s_lock();
        have = loc_take_queued_id(&s_policy, id, sizeof(id));
        s_unlock();
    }
}

// ---------------------------------------------------------------------------
// kind:"loc_req" intercept.
// ---------------------------------------------------------------------------

bool loc_ingest_req_cbor(const uint8_t *buf, uint16_t len)
{
    bool sig_present = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    char id[LOC_ID_MAX] = "";
    if (!loc_parse_req_cbor(buf, len, sig_present, id, sizeof(id))) {
        return false; // not loc_req (or malformed) — caller falls through
    }

    if (modes_coverage_owns_radio()) {
        // Radio-ownership rule (coverage.h's own module comment, this task):
        // a location attempt must never start while the coverage duty cycle
        // owns the radio. In practice this path is close to unreachable —
        // MQTT (and therefore this loc_req) cannot even arrive while the
        // radio is deliberately NO_RF — but answer defensively from cache
        // rather than assume that can never change, and without touching
        // s_policy's attempt bookkeeping at all.
        s_lock();
        double lat = 0, lon = 0;
        bool have_acc = false;
        int32_t acc_m = 0;
        int64_t fix_ts = 0;
        uint8_t src = LOC_SRC_GNSS;
        bool have_cached = loc_get_cached(&s_policy, &lat, &lon, &have_acc, &acc_m, &fix_ts, &src);
        s_unlock();
        ESP_LOGI(TAG, "loc_req %s answered from %s -- coverage duty cycle owns the radio right now",
                 id, have_cached ? "cache" : "no_fix");
        publish_loc_answer(id, have_cached, lat, lon, have_acc, acc_m, fix_ts, src, have_cached, NULL,
                            NULL);
        return true;
    }

    // This task: modes_get_batt_mv() alone cannot be trusted for the battery
    // floor -- it returns a fixed 3300 mV placeholder (not a reading) before
    // the first good AT+SQNVMON response this boot, which coincidentally
    // equals LOC_BATTERY_FLOOR_MV. modes_batt_mv_known() tells them apart;
    // loc_battery_ok() treats LOC_BATTERY_UNKNOWN_MV as passing the floor.
    int batt_mv = modes_batt_mv_known() ? modes_get_batt_mv() : LOC_BATTERY_UNKNOWN_MV;
    int64_t now_us = esp_timer_get_time();

    s_lock();
    loc_decision_t d = loc_on_request(&s_policy, now_us, batt_mv, id);
    double lat = 0, lon = 0;
    bool have_acc = false;
    int32_t acc_m = 0;
    int64_t fix_ts = 0;
    uint8_t src = LOC_SRC_GNSS;
    bool have_cached = loc_get_cached(&s_policy, &lat, &lon, &have_acc, &acc_m, &fix_ts, &src);
    uint32_t budget_s = loc_attempt_budget_s(&s_policy);
    s_save();
    s_unlock();

    switch (d) {
    case LOC_ANSWER_NONE:
        ESP_LOGI(TAG, "loc_req %s queued behind an in-flight attempt", id);
        break;
    case LOC_ANSWER_CACHED: {
        // §13.2/this task: cell rides on every no-fix answer, and on a
        // cached-fix answer once that fix is stale (LOC_CELL_STALE_FIX_S) --
        // never on a fresh cached fix, so the common "just asked, backoff
        // still fresh" case costs no extra AT traffic at all.
        bool want_cell = !have_cached || cell_fix_is_stale(fix_ts);
        loc_cell_t cellbuf;
        const loc_cell_t *cellp = (want_cell && get_cell_snapshot(&cellbuf)) ? &cellbuf : NULL;
        if (batt_mv == LOC_BATTERY_UNKNOWN_MV) {
            ESP_LOGI(TAG, "loc_req %s answered from %s (batt=unknown) without powering GNSS%s", id,
                     have_cached ? "cache" : "no_fix", cellp ? ", cell attached" : "");
        } else {
            ESP_LOGI(TAG, "loc_req %s answered from %s (batt=%dmV) without powering GNSS%s", id,
                     have_cached ? "cache" : "no_fix", batt_mv, cellp ? ", cell attached" : "");
        }
        publish_loc_answer(id, have_cached, lat, lon, have_acc, acc_m, fix_ts, src, have_cached,
                            cellp, NULL);
        break;
    }
    case LOC_ANSWER_START_ATTEMPT:
        // loc_on_request() already marked s_policy.attempt_in_progress and
        // queued `id`; begin_attempt() below only arms the GNSS
        // state-machine side (s_phase et al) -- modes_run()'s own loop
        // (loc_service(), called every iteration) does all the actual work
        // from here, never this (MQTT event) task.
        ESP_LOGI(TAG, "loc_req %s starting a GNSS attempt (budget=%us)", id, (unsigned) budget_s);
        begin_attempt(budget_s, /*extendable=*/true);
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Trigger entry points (V02_DESIGN.md §5). Both are cheap RAM/RTC-only ops;
// neither starts an attempt (that only ever happens in response to a
// loc_req, in loc_ingest_req_cbor() above).
// ---------------------------------------------------------------------------

// Called from net.cpp's network event handler (via net_set_cell_change_cb())
// with the raw, already-deduplicated-against-the-immediately-previous-value
// "lac:ci" key; net.cpp's own module comment documents why that
// de-duplication happens there rather than here. Keep this fast: it can run
// on WalterModem's _eventProcessingTask, same rule as every other event
// handoff in this codebase.
static void loc_on_cell_change(const char *cell_key)
{
    int64_t now_us = esp_timer_get_time();
    s_lock();
    bool reset = loc_trigger_cell_change(&s_policy, now_us, cell_key);
    // LOCATION_TRACKING_DESIGN.md task F1/F3: the SAME cell-change event
    // also feeds the independent background tracker, gated on `loctrack`
    // (loc_policy_t's own on-demand backoff above is unaffected by this
    // flag -- it is not part of "location tracking", §2.3's own "the move
    // schedule is its own rate limit").
    bool track_transitioned = s_track_enabled && loc_track_on_cell(&s_track, now_us, cell_key);
    s_save();
    s_unlock();
    if (reset) {
        ESP_LOGI(TAG, "cell change (%s): backoff reset, 10min floor since last attempt applies",
                 cell_key ? cell_key : "?");
    }
    if (track_transitioned) {
        ESP_LOGI(TAG, "location tracking: STILL -> MOVING (2 new cells within 15min)");
    }
}

void loc_on_motion_event(void)
{
    int64_t now_us = esp_timer_get_time();
    s_lock();
    bool reset = loc_trigger_motion_event(&s_policy, now_us);
    bool track_transitioned = s_track_enabled && loc_track_on_motion(&s_track, now_us);
    s_save();
    s_unlock();
    if (reset) {
        ESP_LOGI(TAG, "sustained motion: backoff reset, 10min floor since last attempt applies");
        // Owner request, 2026-09-20: the same sustained-motion trigger also
        // resets coverage.c's off-period backoff to its first step (moving
        // is when coverage changes) -- modes.c owns the coverage policy
        // instance, this is a one-line cross-module hook.
        modes_note_motion_reset();
    }
    if (track_transitioned) {
        ESP_LOGI(TAG, "location tracking: STILL -> MOVING (sustained motion)");
    }
}

// ---------------------------------------------------------------------------
// GNSS attempt state machine — one small, bounded step per loc_service()
// call (never the whole 20/40s attempt in one call: modes_run()'s loop must
// keep servicing the button/keyboard/paging drain the entire time). Only the
// per-phase AT command itself may block briefly (same class of cost
// net_check()/net_get_battery_mv() already document elsewhere in this
// codebase's modes_run() loop) — the two genuinely slow waits (the GNSS fix
// itself, and re-attaching after route 2's CFUN=4 window) are polled once
// per call against a deadline stored here, never awaited in a loop.
// ---------------------------------------------------------------------------

typedef enum {
    LOC_PH_IDLE = 0,
    LOC_PH_ASSIST_CHECK,
    LOC_PH_ASSIST_UPDATE,
    LOC_PH_GNSS_START,
    LOC_PH_GNSS_WAIT,
    LOC_PH_DONE,
} loc_phase_t;

// s_phase is written from three tasks: the MQTT event task and the debug
// console task each make exactly one write (LOC_PH_IDLE ->
// LOC_PH_ASSIST_CHECK, in begin_attempt() below, guarded by s_lock/s_unlock
// so the handoff to modes_run()'s task is a proper release/acquire pair --
// same reasoning modes.c's own render_pending_t handoff documents); every
// other transition (ASSIST_CHECK through DONE/back to IDLE) is made only by
// modes_run()'s task, from loc_service(), which is why those inner writes
// below are plain (same-task, no cross-task visibility to establish). Only
// s_policy.attempt_in_progress (guarded by s_lock/s_unlock throughout, in
// loc_on_request()/loc_on_attempt_done()) is the cross-task "is anything in
// progress" signal loc_debug_run()'s busy-check and completion-wait use --
// see their own comments for why they do NOT poll s_phase directly.
//
// LOCATION_TRACKING_DESIGN.md §7 (task F5): the route-2 (CFUN=4) fallback
// phases (LOC_PH_ROUTE2_TEARDOWN/START/WAIT/RADIO_ON/REATTACH_WAIT) and
// LOC_REATTACH_CAP_S are gone -- the owner's rule is now simply "any GNSS
// failure sends a cell report and stops" (§7's own text), for scheduled and
// on-demand attempts alike (§2.2). What used to be the route-1 phases are
// renamed LOC_PH_GNSS_START/WAIT (there is only one route left, so "route 1"
// is no longer a meaningful qualifier); the route-to-the-radio RTC hint
// (loc_rtc_t's own `_reserved` byte) and rtc_get_route()/rtc_set_route() are
// gone with it.
static loc_phase_t s_phase = LOC_PH_IDLE;
static uint32_t s_budget_s = LOC_ATTEMPT_S;
static bool s_budget_extendable = true; // false only for loc_debug_run()'s explicit `seconds`
static int64_t s_phase_deadline_us = 0;
static int64_t s_attempt_start_us = 0;
static net_gnss_event_t s_last_event;

// gnsstest's own bookkeeping. s_debug_mode is read by finish_attempt() on
// modes_run()'s task; set true before begin_attempt() (whose own s_lock/
// s_unlock publishes it, same handoff as s_phase above) and false only
// after loc_debug_run() has observed the attempt finish, so finish_attempt()
// always sees the value that was current when the attempt it is finishing
// was started. s_debug_done is written inside finish_attempt()'s own locked
// section (see its own comment) so its value is visible by the time
// loc_debug_run()'s completion-wait unblocks.
static bool s_debug_mode = false;
static bool s_debug_done = false;

// LOCATION_TRACKING_DESIGN.md §2.2/§2.3 (task F3): true while the attempt
// currently in flight was kicked off by the move schedule (loc_service()'s
// own LOC_PH_IDLE case below), never a real loc_req/gnsstest. Same handoff
// discipline as s_debug_mode: set before begin_attempt(), read by
// finish_attempt() to decide loc_track_gnss_attempt_done()/the `why:gnss`
// report vs. the on-demand queue-drain path -- both this file's own task
// (loc_service()), so no cross-task visibility concern (unlike s_debug_mode,
// which the debug console task also writes).
static bool s_move_gnss_mode = false;

// The one `why:gnss` report a finished move-triggered attempt owes,
// stashed here because it must wait for the P1 120s unsolicited-report gate
// (loc_track_gate_ok()) -- possibly past the loc_service() call that
// produced it. net_liveness_ping_now() (called from finish_attempt() below)
// forces an early liveness SUBSCRIBE so the gate very rarely waits long in
// practice; loc_on_uplink_window() (this file's own net_set_uplink_window_cb()
// registration) is what actually publishes it and clears `pending`. Guarded
// by s_lock/s_unlock even though every writer/reader today runs on
// modes_run()'s task (finish_attempt()/loc_on_uplink_window(), both reached
// via loc_service()/the publish callback chain, themselves both invoked from
// modes_run()'s loop) -- same defensive consistency s_track's own comment
// documents.
typedef struct {
    bool pending;
    bool success;
    double lat, lon;
    bool have_acc;
    int32_t acc_m;
    int64_t fix_ts;
    bool have_cell;
    loc_cell_t cell;
} loc_track_gnss_result_t;
static loc_track_gnss_result_t s_track_gnss_result;

static loc_phase_t get_phase(void)
{
    s_lock();
    loc_phase_t ph = s_phase;
    s_unlock();
    return ph;
}

// Called from either the MQTT event task (a real loc_req, via
// loc_ingest_req_cbor()), the debug console task (loc_debug_run()), or (this
// task, F3) modes_run()'s own task (loc_service()'s LOC_PH_IDLE case,
// starting a scheduled move-triggered attempt). Sets
// s_policy.attempt_in_progress under the same lock as the phase kickoff so
// the two flags can never disagree about whether an attempt is running
// (loc_debug_run()'s busy-check and a real loc_req's loc_on_request() both
// consult attempt_in_progress, so a gnsstest run, a real request-driven
// attempt and a scheduled one can never overlap any other one of the three
// either -- LOCATION_TRACKING_DESIGN.md §2.3 "one in flight" holds across
// all three sources, not just loc_req/gnsstest).
static void begin_attempt(uint32_t budget_s, bool extendable)
{
    s_attempt_start_us = esp_timer_get_time();
    s_budget_s = budget_s;
    s_budget_extendable = extendable;
    memset(&s_last_event, 0, sizeof(s_last_event));
    s_lock();
    s_policy.attempt_in_progress = true;
    s_phase = LOC_PH_ASSIST_CHECK;
    s_unlock();
}

static void finish_attempt(bool success)
{
    double lat = s_last_event.lat, lon = s_last_event.lon;
    bool have_acc = success;
    int32_t acc_m = success ? (int32_t) (s_last_event.confidence + 0.5) : 0;
    int64_t fix_ts = s_last_event.fix_ts;

    ESP_LOGI(TAG, "gnss attempt done: %s sats=%u confidence=%.1f elapsed=%llds (budget=%us)%s",
             success ? "FIX" : "no_fix", (unsigned) s_last_event.sat_count, s_last_event.confidence,
             (long long) ((esp_timer_get_time() - s_attempt_start_us) / 1000000),
             (unsigned) s_budget_s, s_move_gnss_mode ? " [scheduled, moving]" : "");

    // Both branches update the cache/backoff (a real fix is a real fix even
    // when gnsstest found it) -- only whether there is a queue to drain
    // differs. s_debug_done is set INSIDE this same locked section, before
    // s_unlock(), so the mutex's release/acquire pairing guarantees
    // loc_debug_run()'s completion-wait (which polls attempt_in_progress
    // under the same lock) never observes "not in progress" before also
    // being able to observe s_debug_done's final value.
    s_lock();
    loc_on_attempt_done(&s_policy, success, lat, lon, have_acc, acc_m, fix_ts, LOC_SRC_GNSS);
    if (s_move_gnss_mode) {
        // §2.3: the move schedule's own backoff, entirely independent of
        // loc_policy_t's on-demand one just advanced/reset above.
        loc_track_gnss_attempt_done(&s_track, success);
    }
    if (s_debug_mode) {
        s_debug_done = true;
    }
    s_save();
    s_unlock();

    // §13.2: a GNSS attempt that ended in no_fix attaches the serving cell
    // (fetched at most once here, shared by every drained requester); a
    // real on-demand fix needs none (only a *cached*, stale fix does,
    // handled in loc_ingest_req_cbor()'s own branch above). §2.1's own
    // report table: a move-triggered `gnss` report attaches `cell` on
    // EITHER outcome ("A fix gives lat/lon plus cell; any failure gives
    // cell only") -- the one row this rule differs from the on-demand one.
    loc_cell_t cellbuf;
    const loc_cell_t *cellp = NULL;
    if (!success || s_move_gnss_mode) {
        if (get_cell_snapshot(&cellbuf)) {
            cellp = &cellbuf;
        }
    }

    if (!s_debug_mode) {
        drain_queue_and_publish(success, lat, lon, have_acc, acc_m, fix_ts, LOC_SRC_GNSS, cellp);
    }

    if (s_move_gnss_mode) {
        // Stash the tracking report itself -- published from
        // loc_on_uplink_window() once the P1 120s gate opens (this file's
        // own module comment on s_track_gnss_result explains why it cannot
        // just publish here). §4 item 4: this is an "urgent" report (cell/
        // stop/gnss), so force the early liveness SUBSCRIBE that opens that
        // window right now rather than waiting for a routine one.
        s_lock();
        s_track_gnss_result.pending = true;
        s_track_gnss_result.success = success;
        s_track_gnss_result.lat = lat;
        s_track_gnss_result.lon = lon;
        s_track_gnss_result.have_acc = have_acc;
        s_track_gnss_result.acc_m = acc_m;
        s_track_gnss_result.fix_ts = fix_ts;
        s_track_gnss_result.have_cell = (cellp != NULL);
        if (cellp) {
            s_track_gnss_result.cell = *cellp;
        }
        s_unlock();
        net_liveness_ping_now();
    }
    s_move_gnss_mode = false;
    s_phase = LOC_PH_IDLE; // same-task write; the only cross-task readers use attempt_in_progress
}

void loc_service(void)
{
    // LOCATION_TRACKING_DESIGN.md task F3: the background tracker's own
    // scheduling is entirely independent of the GNSS attempt phase machine
    // below (it never blocks on it, and vice versa -- an attempt in
    // progress just means loc_track_gnss_due() itself will not fire again
    // until this one finishes, via the shared attempt_in_progress flag).
    loc_track_poll();

    switch (get_phase()) {
    case LOC_PH_IDLE: {
        // LOCATION_TRACKING_DESIGN.md §2.2/§2.3 (task F3): a scheduled
        // GNSS-while-moving attempt shares the SAME "one in flight" slot as
        // loc_req/gnsstest (§2.3's own "a scheduled attempt holds the slot
        // with an empty queue") -- only ever kicked off from here, when
        // nothing else already owns the phase machine.
        int64_t now_us = esp_timer_get_time();
        s_lock();
        bool track_on = s_track_enabled;
        uint32_t move_s = s_move_gnss_s;
        bool busy = s_policy.attempt_in_progress;
        bool due = track_on && !busy && loc_track_gnss_due(&s_track, now_us, move_s);
        bool cap_reached = due && loc_track_gnss_cap_reached(&s_track, now_us);
        s_unlock();
        if (!due || cap_reached) {
            if (due && cap_reached) {
                ESP_LOGI(TAG, "location tracking: GNSS due but the daily cap (%u) is reached",
                         (unsigned) LOC_GNSS_DAILY_CAP);
            }
            return; // nothing to do — the common case, every wake cycle
        }
        // §2.3 "Battery floor (3.3V). It blocks all GNSS; cell reports
        // continue." -- same modes_batt_mv_known()/LOC_BATTERY_UNKNOWN_MV
        // fail-open convention loc_ingest_req_cbor() uses.
        int batt_mv = modes_batt_mv_known() ? modes_get_batt_mv() : LOC_BATTERY_UNKNOWN_MV;
        if (!loc_battery_ok(batt_mv)) {
            ESP_LOGI(TAG, "location tracking: GNSS due but the battery floor blocks it (batt=%dmV)",
                     batt_mv);
            return;
        }
        s_lock();
        loc_track_gnss_attempt_started(&s_track, now_us);
        s_unlock();
        s_move_gnss_mode = true;
        ESP_LOGI(TAG, "location tracking: scheduled GNSS attempt starting (moving)");
        begin_attempt(LOC_ATTEMPT_S, /*extendable=*/true);
        return;
    }

    case LOC_PH_ASSIST_CHECK: {
        // Mandatory refresh (LOCATION_TRACKING_DESIGN.md §1/§2.2, this
        // task): due if EITHER the modem's own gnssGetAssistanceStatus()
        // (net_gnss_assistance_due()) says so, OR loc_track_t's own
        // independent >=2h-since-last-refresh floor says so -- "Before any
        // attempt, scheduled or requested" (§2.2), so this check runs for
        // every attempt source, not just tracking ones.
        int32_t stale_s = 0;
        bool net_ok = net_gnss_assistance_due(&stale_s);
        bool net_fresh = net_ok && stale_s > 0;
        bool track_due = loc_track_gnss_assist_due(&s_track, esp_timer_get_time());
        if (net_fresh && !track_due) {
            ESP_LOGI(TAG, "gnss assistance fresh for another %lds, skipping refresh", (long) stale_s);
            s_phase = LOC_PH_GNSS_START;
        } else {
            ESP_LOGI(TAG, "gnss assistance due (%s)",
                     track_due ? ">=2h since last refresh" : (net_ok ? "modem says so" : "status unknown, assuming"));
            s_phase = LOC_PH_ASSIST_UPDATE;
        }
        break;
    }

    case LOC_PH_ASSIST_UPDATE: {
        // Bounded but UNVERIFIED duration (net_gnss_update_assistance()'s
        // own comment); one loc_service() call may therefore take a few
        // seconds here, same tolerated class as F4's checkComm() retries
        // elsewhere in modes_run()'s loop.
        int64_t t0 = esp_timer_get_time();
        bool refreshed = net_gnss_update_assistance();
        int64_t elapsed_us = esp_timer_get_time() - t0;
        if (!refreshed) {
            // §2.2: "A failed refresh means no attempt, and counts as a
            // failure." -- a behaviour change from this file's own
            // pre-this-task code, which used to ignore a refresh failure
            // and attempt the fix anyway; the owner's design text is
            // explicit that a failed refresh must abort, not merely warn.
            ESP_LOGI(TAG, "gnss assistance refresh failed; sending a cell report and stopping "
                          "(no GNSS wait attempted)");
            finish_attempt(false);
            break;
        }
        s_lock();
        loc_track_gnss_assist_refreshed(&s_track, esp_timer_get_time());
        s_unlock();
        if (s_budget_extendable) {
            s_budget_s = LOC_FIRST_ATTEMPT_S; // "no ephemeris in the receiver yet"
        }
        // Server-architect review (26 Sep 2026), PROTOCOL.md §13.3 item 2:
        // time spent on the refresh counts inside the 60s web-request
        // bound, for scheduled and on-demand attempts alike (P1's own text:
        // "A scheduled GNSS attempt obeys items 1-3 like a requested one").
        // Rounds elapsed time UP so this can only be conservative (never
        // let a slightly-under-counted refresh push the total over 60s).
        uint32_t assist_elapsed_s = (uint32_t) ((elapsed_us + 999999) / 1000000);
        uint32_t capped_budget_s = loc_web_gnss_budget_s(s_budget_s, assist_elapsed_s);
        if (capped_budget_s != s_budget_s) {
            ESP_LOGI(TAG, "assistance refresh took %us; GNSS-wait budget capped %us -> %us to hold "
                          "the 60s bound",
                     (unsigned) assist_elapsed_s, (unsigned) s_budget_s, (unsigned) capped_budget_s);
        }
        s_budget_s = capped_budget_s;
        if (s_budget_s == 0) {
            ESP_LOGI(TAG, "assistance refresh alone reached the 60s bound; sending a cell report "
                          "and stopping (no GNSS wait attempted)");
            finish_attempt(false);
            break;
        }
        s_phase = LOC_PH_GNSS_START;
        break;
    }

    case LOC_PH_GNSS_START: {
        net_gnss_config(); // idempotent, persists across reboots per the vendor doc
        if (!net_gnss_start_fix()) {
            // §7/owner decision: any GNSS failure (including a synchronous
            // refusal) sends a cell report and stops -- no CFUN=4 fallback.
            ESP_LOGI(TAG, "gnss start refused synchronously; sending a cell report and stopping");
            finish_attempt(false);
            break;
        }
        s_phase_deadline_us = esp_timer_get_time() + (int64_t) s_budget_s * 1000000;
        s_phase = LOC_PH_GNSS_WAIT;
        break;
    }

    case LOC_PH_GNSS_WAIT: {
        net_gnss_event_t ev;
        if (net_gnss_poll_event(&ev)) {
            if (ev.kind == NET_GNSS_EVT_REFUSED) {
                ESP_LOGI(TAG, "gnss refused mid-wait (LTE_CONCURRENCY); sending a cell report and "
                              "stopping");
                finish_attempt(false);
                break;
            }
            s_last_event = ev;
            finish_attempt(ev.kind == NET_GNSS_EVT_FIX && loc_fix_confidence_ok(ev.confidence));
            break;
        }
        if (esp_timer_get_time() >= s_phase_deadline_us) {
            net_gnss_cancel();
            finish_attempt(false); // it was allowed to run — just no sky (or over-confidence)
        }
        break; // still waiting; try again next loc_service() call
    }

    case LOC_PH_DONE:
    default:
        s_phase = LOC_PH_IDLE;
        break;
    }
}

// ---------------------------------------------------------------------------
// Location tracking: the report scheduler's two call sites (task F3).
// ---------------------------------------------------------------------------

static const char *report_why_str(loc_report_t r)
{
    switch (r) {
    case LOC_REPORT_STILL: return "still";
    case LOC_REPORT_CELL: return "cell";
    case LOC_REPORT_MOVE: return "move";
    case LOC_REPORT_STOP: return "stop";
    case LOC_REPORT_GNSS: return "gnss";
    case LOC_REPORT_NONE:
    default: return NULL;
    }
}

// Called once per loc_service() iteration (every wake-and-drain cycle),
// regardless of phase -- cheap (a few RAM reads, see loc_track_tick()'s own
// doc comment). §4 item 4: a newly-due "urgent" report (cell/stop -- gnss
// is handled separately, from finish_attempt()) forces an early liveness
// SUBSCRIBE rather than waiting out the routine idle-ping interval, so it
// reaches loc_on_uplink_window() (below) promptly; `still`/`move` simply
// wait for whichever window opens next on its own. Never publishes
// anything itself -- see loc_on_uplink_window()'s own doc comment for why
// calling loc_track_tick() twice per report is safe.
static void loc_track_poll(void)
{
    s_lock();
    bool on = s_track_enabled;
    s_unlock();
    if (!on) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    s_lock();
    loc_report_t due = loc_track_tick(&s_track, now_us);
    s_unlock();
    if (due == LOC_REPORT_CELL || due == LOC_REPORT_STOP) {
        net_liveness_ping_now();
    }
}

// net_set_uplink_window_cb() registration (LOCATION_TRACKING_DESIGN.md §4,
// task F3): fires on modes_run()'s own task right after a liveness
// SUBSCRIBE was sent or a QoS 1 publish succeeded -- an RRC window this
// file's own reports can ride for free (§4's own "rides the same RRC
// connection, never delays it" rule). Publishes AT MOST one report per
// call: a stashed `why:gnss` result from a just-finished move-triggered
// attempt takes priority (it already paid for its own radio time and
// forced this very window via finish_attempt()'s own net_liveness_ping_now()
// call); otherwise whatever loc_track_tick() decides is due right now.
// Calling loc_track_tick() again here (loc_track_poll() above already
// called it once this cycle) is safe: it is a pure function of wall-clock
// state, not a one-shot event queue, and nothing but
// loc_track_note_report_sent() (called only once an actual publish
// succeeds) ever consumes anything permanently.
static void loc_on_uplink_window(void)
{
    s_lock();
    bool on = s_track_enabled;
    s_unlock();
    if (!on) {
        return;
    }
    int64_t now_us = esp_timer_get_time();

    s_lock();
    bool gnss_pending = s_track_gnss_result.pending;
    loc_track_gnss_result_t gr = s_track_gnss_result;
    bool gate_ok = loc_track_gate_ok(&s_track, now_us);
    s_unlock();

    if (gnss_pending) {
        if (!gate_ok) {
            return; // still inside the P1 120s floor; try again next window
        }
        const loc_cell_t *cellp = gr.have_cell ? &gr.cell : NULL;
        publish_loc_answer(NULL, gr.success, gr.lat, gr.lon, gr.have_acc, gr.acc_m, gr.fix_ts,
                            LOC_SRC_GNSS, /*cached=*/false, cellp, "gnss");
        s_lock();
        s_track_gnss_result.pending = false;
        loc_track_note_report_sent(&s_track, now_us, LOC_REPORT_GNSS, gr.success);
        s_save();
        s_unlock();
        return;
    }

    s_lock();
    loc_report_t due = loc_track_tick(&s_track, now_us);
    s_unlock();
    const char *why = report_why_str(due);
    if (!why) {
        return;
    }

    // Every tracking report other than `gnss` is a cell-only envelope
    // (loc:null, err:"no_fix") -- §2.1's report table, "cell only (with
    // why)". Force a fresh AT+SQNMONI first (§4 item 2: catches a `+CEREG`
    // change lost in light sleep) -- free, this RRC window is already open.
    net_force_cell_refresh();
    loc_cell_t cellbuf;
    const loc_cell_t *cellp = get_cell_snapshot(&cellbuf) ? &cellbuf : NULL;
    publish_loc_answer(NULL, /*success=*/false, 0, 0, false, 0, 0, LOC_SRC_CELL, /*cached=*/false,
                        cellp, why);
    s_lock();
    loc_track_note_report_sent(&s_track, now_us, due, false);
    s_save();
    s_unlock();
}

// ---------------------------------------------------------------------------
// Init / debug hook.
// ---------------------------------------------------------------------------

void loc_init(void)
{
    s_lock();
    loc_policy_init(&s_policy);
    s_unlock();

    if (!net_gnss_config()) {
        ESP_LOGI(TAG, "gnss config failed at init; location will keep answering from cache/no_fix "
                      "(this task's own fail-open rule)");
    }
    net_set_cell_change_cb(loc_on_cell_change);
    net_set_uplink_window_cb(loc_on_uplink_window); // LOCATION_TRACKING_DESIGN.md §4, task F2/F3

    // accel.c fails open on its own (absent chip -> logs once, returns
    // false); loc_track_init() below needs that same fact ("no LIS3DH ->
    // cell-only motion signal", §1) -- so this must run before it.
    bool have_lis3dh = accel_init();

    s_lock();
    loc_track_init(&s_track, have_lis3dh);
    s_track_enabled = LOC_TRACK_DEFAULT_ENABLED;
    s_move_gnss_s = LOC_MOVE_GNSS_DEFAULT_S;
    s_unlock();

    ESP_LOGI(TAG, "location ready: loc_min_s=%u loc_period_s=%u battery_floor=%dmV loctrack=%s "
                  "locmove=%us",
             (unsigned) loc_get_min_s(), (unsigned) loc_get_period_s(), LOC_BATTERY_FLOOR_MV,
             LOC_TRACK_DEFAULT_ENABLED ? "on" : "off", (unsigned) LOC_MOVE_GNSS_DEFAULT_S);
}

bool loc_debug_run(uint32_t seconds)
{
    if (seconds == 0 || seconds > 120) {
        ESP_LOGI(TAG, "gnsstest: seconds must be 1..120");
        return false;
    }

    if (modes_coverage_owns_radio()) {
        // Radio-ownership rule (coverage.h's own module comment, this task):
        // the debug console is the one realistic way to race the duty
        // cycle's own radio-off window (a real loc_req cannot arrive without
        // MQTT, which is down for the whole window) -- refuse outright
        // rather than fight it for the radio.
        ESP_LOGI(TAG, "gnsstest: coverage duty cycle owns the radio right now (deliberately off) "
                      "-- refusing to start an attempt");
        return false;
    }

    s_lock();
    bool busy = s_policy.attempt_in_progress;
    s_unlock();
    if (busy) {
        ESP_LOGI(TAG, "gnsstest: an attempt is already in progress, not starting a second one");
        return false;
    }

    net_mqtt_status_t before;
    net_get_mqtt_status(&before);
    ESP_LOGI(TAG, "gnsstest: starting one %us attempt (bypassing backoff/battery floor); "
                  "mqtt_connected(before)=%d",
             (unsigned) seconds, (int) before.mqtt_connected);

    s_debug_mode = true; // published by begin_attempt()'s own s_lock/s_unlock below
    s_debug_done = false;
    begin_attempt(seconds, /*extendable=*/false);

    // Block THIS task (never modes_run()'s — see loc.h's own doc comment).
    // modes_run()'s ordinary loc_service() call, every wake-and-drain
    // iteration, is what actually drives the attempt forward; this loop
    // only *waits*, polling s_policy.attempt_in_progress (the same
    // cross-task flag loc_on_request()'s in-flight check uses) rather than
    // running the state machine itself -- two tasks stepping the same
    // non-reentrant phase machine at once would race.
    int64_t deadline_us = esp_timer_get_time() + (int64_t) (seconds + 30) * 1000000; // generous overall cap
    for (;;) {
        s_lock();
        bool done = !s_policy.attempt_in_progress;
        s_unlock();
        if (done) {
            break;
        }
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGI(TAG, "gnsstest: gave up waiting for modes_run() to finish the attempt");
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    s_debug_mode = false;

    net_mqtt_status_t after;
    net_get_mqtt_status(&after);
    ESP_LOGI(TAG, "gnsstest: done. sats=%u confidence=%.1f mqtt_connected(after)=%d",
             (unsigned) s_last_event.sat_count, s_last_event.confidence, (int) after.mqtt_connected);
    return s_debug_done;
}

#endif /* ESP_PLATFORM */

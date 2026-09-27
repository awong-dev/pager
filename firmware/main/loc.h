/* loc.h — on-demand location (docs/V02_DESIGN.md §5, docs/PROTOCOL.md
 * §3.2/§13) plus background location tracking
 * (docs/LOCATION_TRACKING_DESIGN.md, tasks F1-F5).
 *
 * Split the same way lock.c/auth.c/msg.c already are: everything above the
 * `#ifdef ESP_PLATFORM` banner is pure C, no ESP-IDF dependency, driven by an
 * injected clock (`int64_t now_us`, `esp_timer_get_time()`-shaped but never
 * called directly from here) so firmware/host/test_loc.c can run the whole
 * backoff/trigger/cache/queue/tracking policy without a device. It owns:
 *
 *   - the growing backoff (5 min doubling to a 12 h cap, reset by success),
 *   - the 10-minute floor a trigger reset leaves on top of it,
 *   - the cell-change (10 min) and sustained-motion (>=60s spanning a 3 min
 *     window) trigger classifiers,
 *   - the "one attempt in flight, late requests share its answer" queue,
 *   - the battery floor (3.3V) and confidence-acceptance (<=100) gates,
 *   - the `/loc` CBOR encoder (`loc_build_cbor()`), byte-compatible with
 *     `relay/app/wirecbor.py` (see firmware/host/test_loc.c for the exact
 *     `relay/.venv/bin/python` command used to cross-check it),
 *   - the `kind:"loc_req"` request parser, and
 *   - (LOCATION_TRACKING_DESIGN.md, task F1) the STILL/MOVING classifier,
 *     recent-cell ring, unsolicited report scheduler and GNSS-while-moving
 *     schedule (`loc_track_t`/`loc_track_*()`) -- a second, independent rate
 *     limiter layered alongside the on-demand one above, not sharing its
 *     state (§2.3: "the move schedule is its own rate limit").
 *
 * Below the banner: the device-only orchestration — the phased (never-
 * blocks-the-caller-for-more-than-one-step) GNSS attempt state machine
 * driven by loc_service() from modes_run()'s own loop, the accelerometer/
 * cell-change trigger entry points, the `loctrack`/`locmove` runtime flags,
 * and the `gnsstest` debug hook. All modem/GNSS access goes through net.h's
 * small facade (net_gnss_*()/net_radio_*()) — this file never touches
 * WalterModem directly, same rule every other main module follows (net.h's
 * own module comment).
 *
 * Route to the radio: in place only (attached, eDRX gap) — the vendor
 * `gnss*` API (`AT+LPGNSSCFG/FIXPROG/ASSISTANCE`,
 * `walter-modem/src/proto/WalterGNSS.cpp`). LOCATION_TRACKING_DESIGN.md §7/
 * F5 deleted the earlier deliberate-CFUN=4 fallback route (disconnect MQTT,
 * fix, re-attach) an owner decision replaced with a simpler rule: any GNSS
 * failure — refusal, timeout, no fix, or over-confidence — just sends a
 * cell report and stops, for both scheduled and on-demand attempts alike.
 * The RTC byte that used to remember which route last worked
 * (`loc_rtc_t.route`) is kept as `_reserved` rather than removed (see its
 * own comment) so this needed no RTC layout-version bump.
 */
#ifndef LOC_H
#define LOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing / schedule constants (docs/V02_DESIGN.md §5 — the numbers here
 * amend docs/PROTOCOL.md §13.3's fixed 120s `loc_min_s`: this is an owner
 * decision recorded in V02_DESIGN.md, PROTOCOL.md's own text still needs a
 * matching edit, which is out of this task's Files list — flagged in the
 * report, not applied here since docs/ is another agent's concurrent work).
 * --------------------------------------------------------------------- */

#define LOC_ATTEMPT_S 20u        /* normal per-attempt GNSS-wait budget */
#define LOC_FIRST_ATTEMPT_S 40u  /* cold boot / just-refreshed-assistance budget */

#define LOC_BACKOFF_MIN_S 300u    /* 5 min, first step after a failed attempt */
#define LOC_BACKOFF_MAX_S 43200u  /* 12 h cap */

#define LOC_TRIGGER_FLOOR_S 600u  /* 10 min floor since the last attempt, after a trigger reset */
#define LOC_CELL_DEBOUNCE_S 600u  /* 10 min: ignore cell changes more often than this */
#define LOC_MOTION_WINDOW_S 180u  /* 3 min trailing window for the motion classifier */
#define LOC_MOTION_SUSTAIN_S 60u  /* interrupts must span at least this long inside the window */

#define LOC_BATTERY_FLOOR_MV 3300 /* LiFePO4, docs/V02_DESIGN.md §5 */
#define LOC_FIX_CONFIDENCE_MAX 100.0 /* vendor demo threshold, docs/V02_DESIGN.md §5 */

/* This task: sentinel `batt_mv` meaning "no real reading yet" (modes.c's
 * modes_batt_mv_known() is false -- e.g. AT+SQNVMON hasn't returned a
 * plausible value this boot, found on hardware running on USB power).
 * loc_battery_ok() treats this exact value as passing the floor, same as any
 * other unknown-quantity gate in this codebase fails open rather than
 * blocking a GNSS attempt on a placeholder number. Deliberately the most
 * extreme value an `int` can hold, nowhere near any real millivolt reading,
 * so it can never be confused with one; callers must pass this constant
 * itself, not merely "some very negative number". */
#define LOC_BATTERY_UNKNOWN_MV INT32_MIN

#define LOC_STATUS_MIN_S 600u    /* reported /status loc_min_s: the trigger floor above */
#define LOC_STATUS_PERIOD_S 0u   /* reported /status loc_period_s: periodic fixes stay off */
#define LOC_STATUS_PERIOD_TRACKING_S 3600u /* reported /status loc_period_s while `loctrack` is on
                                             * (LOCATION_TRACKING_DESIGN.md §5 P2) */

#define LOC_ID_MAX 17     /* "l_" + 8 hex + NUL, PROTOCOL.md §1 */
#define LOC_MAX_QUEUED 4  /* loc_req ids sharing one in-flight attempt's answer */
#define LOC_CELL_KEY_MAX 24 /* "lac:ci" opaque string, net.cpp's own field widths */
#define LOC_MOTION_RING 16  /* motion-event timestamps kept for the 3-min span check */

/* ---------------------------------------------------------------------
 * Location tracking (docs/LOCATION_TRACKING_DESIGN.md, tasks F1/F3/F4) --
 * the STILL/MOVING classifier, recent-cell ring, report scheduler and
 * GNSS-while-moving schedule. Pure, host-tested (firmware/host/test_loc.c),
 * layered ON TOP OF the on-demand loc_req mechanism above, not replacing it:
 * loc_policy_t/loc_on_request()/loc_trigger_*() are unchanged and keep
 * answering `loc_req` exactly as before (§2.3 "Web request" — its own
 * backoff/floor/battery-floor gate). loc_track_t below is the new
 * always-ticking background tracker, gated at the device layer by the
 * runtime `loctrack on|off` flag (loc.c's device section, F3).
 * --------------------------------------------------------------------- */

#define LOC_CELL_RING_N 4              /* §1: distinct recent cells remembered */
#define LOC_CELL_RING_WINDOW_S 3600u    /* §1: "seen in the last 60 min" */
#define LOC_NEWCELL_WINDOW_S 900u       /* §1: "2 new cells within 15 min" -> MOVING */
#define LOC_NEWCELL_COUNT 2
#define LOC_STILL_MOTION_GAP_S 300u     /* §1 LOC_STILL_S: no accel edge for this long... */
#define LOC_STILL_CELL_GAP_S 900u       /* ...AND no new cell for this long -> STILL */

#define LOC_HOURLY_CELL_S 3300u         /* §2.1: hourly cell report gate, "still" */
#define LOC_CELL_REPORT_SERVING_S 60u   /* §2.1: a cell must serve this long before it is reported */
#define LOC_MOVE_REFRESH_S 600u         /* §2.1: "move" report, no /loc for this long while MOVING */
#define LOC_MOVE_GNSS_DEFAULT_S 600u    /* §2.2 LOC_MOVE_GNSS_S: `locmove` runtime default, 0=off */
#define LOC_MOVE_GNSS_EDGE_WINDOW_S 60u /* §2.2: needs an accel edge this recent... */
#define LOC_MOVE_GNSS_CELL_WINDOW_S 600u /* ...or (no LIS3DH) a new cell this recent */
#define LOC_GNSS_ASSIST_MAX_AGE_S 7200u /* §2.2: mandatory refresh at most every 2h */
#define LOC_GNSS_DAILY_CAP 48           /* §2.2: stuck-INT1 backstop */
#define LOC_GNSS_DAY_S 86400u

/* P1 (server-architect review, 26 Sep 2026): a 120s minimum gap applies to
 * EVERY unsolicited /loc report, whatever its `why` (cell/gnss/stop/still/
 * move) -- not just the cell-change row's own 120s in the design doc's §2.1
 * table. A report that falls due earlier than that waits for the gate
 * (loc_track_tick()'s own `pending_report` latch, below); it is never
 * dropped. */
#define LOC_UNSOLICITED_MIN_GAP_S 120u

/* PROTOCOL.md §13.3 item 2's 60s bound on a web (loc_req-driven) answer,
 * amended by the same review: time spent on an assistance-data refresh that
 * ran counts inside this bound too (loc_web_gnss_budget_s(), below). */
#define LOC_WEB_REQUEST_BOUND_S 60u

/* Owner has not yet decided whether tracking defaults on or off (it is a
 * privacy question, server-architect review 26 Sep 2026) -- ON for now.
 * One named constant so the eventual decision is a one-line flip; loc.c's
 * device section reads this exactly once, at loc_init(), into the runtime
 * `loctrack` flag `locmove`/`gnsstest` already sit alongside. */
#define LOC_TRACK_DEFAULT_ENABLED true

/* PROTOCOL.md §13.2 `cell` (this task): attach `cell` to a *cached* (not
 * no_fix) /loc answer once the fix behind it is this old. This is a product
 * decision on top of §13.2, which only requires `cell` on a no_fix answer;
 * it is not itself a protocol minimum. */
#define LOC_CELL_STALE_FIX_S 600u

/* Where a cached/answered fix came from (PROTOCOL.md §10 `loc.src`: gnss/cell). */
typedef enum {
    LOC_SRC_GNSS = 0,
    LOC_SRC_CELL = 1,
} loc_src_t;

/* RTC-resident sub-struct (docs/PROTOCOL.md §9.3's table row) — modes.c
 * embeds this in its own pager_rtc_t (same pattern as lock_rtc_t/auth_rtc_t)
 * and owns the storage/magic/CRC/locking; loc.c only reads/writes through
 * the pointer loc_bind() hands over. Deliberately just one byte: the
 * backoff/cache/queue/tracking policy below is RAM-only by design
 * (PROTOCOL.md §9.2's rule — losing it to a reset does not make the relay's
 * view of delivery wrong, and §13.3's own "cached:true ... from this power
 * session" wording requires it to reset on a reboot). Padded to 8 bytes for
 * alignment, same convention lock_rtc_t documents.
 *
 * LOCATION_TRACKING_DESIGN.md §7/F5: this byte used to hold the route (in
 * place vs. CFUN=4) that most recently produced a fix, so a device that had
 * already learned CFUN=4 works on this modem/carrier never re-spent half of
 * every attempt budget re-trying the in-place route first. Route 2 (CFUN=4)
 * is deleted (§7 of that doc — GNSS is in-place only now), so nothing reads
 * or writes this byte any more. It stays `_reserved`, not removed: dropping
 * it would shrink pager_rtc_t and force a layout-version bump (modes.c's own
 * comment on PAGER_RTC_MAGIC), which re-initialises the WHOLE struct on the
 * first boot after the update — including the auth counter and lock state,
 * neither of which this task has any business resetting. Reclaim it at
 * whatever future layout bump happens for another reason. */
typedef struct {
    uint8_t _reserved;
    uint8_t _pad[7];
} loc_rtc_t;

/* ---------------------------------------------------------------------
 * Motion classifier ring (pure, host-tested): "sustained motion" =
 * interrupts spanning >= LOC_MOTION_SUSTAIN_S within a trailing
 * LOC_MOTION_WINDOW_S window (V02_DESIGN.md §5 trigger 2). A small FIFO of
 * event timestamps, oldest evicted on every insert; span = newest - oldest
 * once evicted to the window. RAM-only (part of loc_policy_t below).
 * --------------------------------------------------------------------- */
typedef struct {
    int64_t events_us[LOC_MOTION_RING];
    int n;
} loc_motion_ring_t;

/* ---------------------------------------------------------------------
 * The whole policy state, RAM-resident (see the loc_rtc_t comment above for
 * why this is not in RTC). One instance, owned by loc.c's device-only
 * section in normal operation; firmware/host/test_loc.c owns its own for
 * each test.
 * --------------------------------------------------------------------- */
typedef struct {
    uint32_t backoff_s;               /* current backoff; 0 = none/expired */
    int64_t last_attempt_us;          /* monotonic; 0 = never attempted this session */
    int64_t trigger_floor_until_us;   /* 0 = no floor active; else an absolute deadline */
    bool attempt_in_progress;
    bool long_budget_next;            /* next/current attempt should get LOC_FIRST_ATTEMPT_S */

    bool have_fix;                    /* a fix exists from this power session */
    double fix_lat, fix_lon;
    bool fix_have_acc;
    int32_t fix_acc_m;
    int64_t fix_ts_epoch_s;
    uint8_t fix_src;                  /* loc_src_t */

    bool have_cell_key;
    char cell_key[LOC_CELL_KEY_MAX];
    bool have_last_cell_trigger;
    int64_t last_cell_trigger_us;

    loc_motion_ring_t motion;

    char queued_ids[LOC_MAX_QUEUED][LOC_ID_MAX];
    int queued_count;
} loc_policy_t;

/* Serving-cell snapshot for the `cell` sub-map (PROTOCOL.md §13.2, key 49,
 * this task) — a pure, host-testable value type so loc_build_cbor() below
 * stays host-testable; the ESP-IDF-side mapping from net.h's
 * net_cell_info_t (the modem-backed cache) lives in loc.c's own device
 * section, never here. `mcc`/`mnc` are NUL-terminated ASCII digit strings,
 * written to the wire verbatim (leading zeros kept) — loc_build_cbor() does
 * not pad or trim them; the caller is responsible for their shape (net.cpp's
 * net_get_cell_info() already produces the right widths). */
typedef struct {
    char mcc[4];     /* 3 digits + NUL */
    char mnc[4];     /* 2-3 digits + NUL */
    uint32_t tac;    /* 0..65535 */
    uint32_t ci;     /* 0..268435455 (28 bits) */
    bool have_rsrp;
    int32_t rsrp;    /* dBm, -156..-30, when have_rsrp */
} loc_cell_t;

/* What the caller of loc_on_request() must do next. */
typedef enum {
    LOC_ANSWER_NONE = 0,        /* nothing yet: request queued behind an in-flight attempt */
    LOC_ANSWER_CACHED,          /* answer now, without powering GNSS (backoff/battery gate) */
    LOC_ANSWER_START_ATTEMPT,   /* start a GNSS attempt; this request is already queued too */
} loc_decision_t;

/* ---------------------------------------------------------------------
 * Pure functions — no ESP-IDF dependency, host-tested by
 * firmware/host/test_loc.c.
 * --------------------------------------------------------------------- */

void loc_policy_init(loc_policy_t *p);

/* 5,10,20,40,80,160,320,640,720 min (V02_DESIGN.md §5's own worked sequence);
 * `current` = 0 means "no backoff yet" -> LOC_BACKOFF_MIN_S. Pure, no state. */
uint32_t loc_next_backoff_s(uint32_t current_s);

bool loc_battery_ok(int batt_mv);
bool loc_fix_confidence_ok(double confidence_m);

/* Core rate-limit decision (PROTOCOL.md §13.3, V02_DESIGN.md §5). Call once
 * per accepted loc_req. `req_id` is copied into the queue (LOC_ANSWER_NONE
 * or LOC_ANSWER_START_ATTEMPT) or ignored (LOC_ANSWER_CACHED, which needs no
 * queue slot — it is answered immediately by the caller). No I/O. */
loc_decision_t loc_on_request(loc_policy_t *p, int64_t now_us, int batt_mv, const char *req_id);

/* LOC_FIRST_ATTEMPT_S if `p->long_budget_next`, else LOC_ATTEMPT_S. */
uint32_t loc_attempt_budget_s(const loc_policy_t *p);

/* Forces the *next* attempt (or, if one is already in progress, lets the
 * caller extend the one under way — see loc.c's run_one_attempt()) onto the
 * long budget. Called after a cold boot (loc_policy_init()) and whenever an
 * assistance-data refresh actually ran. */
void loc_policy_request_long_budget(loc_policy_t *p);

/* Ends the in-flight attempt: resets `attempt_in_progress`/`long_budget_next`/
 * the trigger floor (consumed — its only job was delaying this attempt),
 * and on success resets the backoff to zero and stores the new cached fix;
 * on failure advances the backoff via loc_next_backoff_s(). Does not touch
 * the request queue — drain it with loc_take_queued_id() afterward. */
void loc_on_attempt_done(loc_policy_t *p, bool success, double lat, double lon, bool have_acc,
                          int32_t acc_m, int64_t fix_ts_epoch_s, uint8_t src);

/* Pops one queued request id (order unspecified — every queued id gets the
 * same answer). Returns false (out untouched) once the queue is empty. */
bool loc_take_queued_id(loc_policy_t *p, char *out, size_t cap);

/* Reads the cached fix, if any (`p->have_fix`). Returns false (out params
 * untouched) if there is none yet this power session. */
bool loc_get_cached(const loc_policy_t *p, double *lat, double *lon, bool *have_acc,
                    int32_t *acc_m, int64_t *fix_ts_epoch_s, uint8_t *src);

/* Trigger 1 (cell/tracking-area change), debounced to at most once per
 * LOC_CELL_DEBOUNCE_S. `cell_key` is an opaque, caller-defined identity
 * string (net.cpp's own device-side wrapper builds it as "lac:ci"); the
 * first ever call just learns the starting key (never a "change"). Returns
 * true iff this call actually reset the backoff (for logging only — the
 * reset itself, and the 10-minute floor it leaves via
 * `p->trigger_floor_until_us = last_attempt_us + LOC_TRIGGER_FLOOR_S`, are
 * already applied by the time this returns). */
bool loc_trigger_cell_change(loc_policy_t *p, int64_t now_us, const char *cell_key);

/* Trigger 2 (sustained motion): feeds one motion-interrupt timestamp into
 * the classifier and, iff the >=60s-within-3min condition newly holds,
 * applies the same reset-plus-floor as loc_trigger_cell_change() and clears
 * the ring (so a continuing stream of interrupts needs another full 60s
 * span before firing again, rather than re-triggering on every interrupt).
 * Returns true iff this call fired the reset. */
bool loc_trigger_motion_event(loc_policy_t *p, int64_t now_us);

/* Builds the unsigned `/loc` envelope body (PROTOCOL.md §13.2, keymap §10):
 * `v,id,ts,loc,req[,cached][,err][,cell]`, ascending key order except `cell`
 * (key 49) which — like `n`/`sig` — is written where PROTOCOL.md §13.2's own
 * field table lists it (right before `n`), not where its numeric key would
 * otherwise sort it; only `sig` (appended by the caller) has a HARD
 * last-place rule (§3.1). When `signed_env` an `n` field is sized into the
 * map header's pair count but appended as the LAST body field so the caller
 * can immediately follow with auth_sign() (docs/DEVICE_PLAN.md §2.4's "no
 * re-serialisation" rule; same convention modes.c's build_status_cbor()/
 * book.c's book_request() use — the header count includes both `n` and the
 * not-yet-written `sig` pair).
 *
 * Exactly one of (`have_fix` true) or (`err` non-NULL) must hold — §13.2:
 * `err` only when `loc` is null. `have_acc`/`src_cell` are the two optional
 * `loc` sub-fields (`acc`, `src`); omitted (not just zero/"gnss") when
 * false, matching the wire's own "absent = unknown/default" rule. `req`
 * NULL writes `req:null` (an unsolicited fix — never actually produced by
 * this task, since `loc_period_s` stays 0, but kept general). `cached` is
 * written only when true (default false, PROTOCOL.md §13.2). `cell` (this
 * task): NULL omits the sub-map entirely (no reading available, or the
 * caller decided not to attach one); non-NULL writes `mcc`/`mnc`/`tac`/`ci`
 * always, `rsrp` only when `cell->have_rsrp`. A malformed `cell` (`mcc` not
 * exactly 3 digits, or `mnc` not 2-3 digits) is silently treated as NULL —
 * the same "absent, not a reason to drop the envelope" tolerance §13.2 asks
 * of the *relay's* parser, applied defensively on the encode side too.
 *
 * `why` (LOCATION_TRACKING_DESIGN.md §2.1/§5, key 60, P3): the tracking
 * report reason (`"still"|"cell"|"move"|"gnss"|"stop"`), NULL to omit the
 * key entirely (every on-demand loc_req answer omits it — `why` only ever
 * accompanies an unsolicited, req:null report). Written right after `cell`
 * and before `n`/`sig`, matching the design doc's own envelope example
 * ("{v, id, ts, loc:null, req:null, err:"no_fix", cell:{…}, why, n, sig}"),
 * not where key 60's numeric value would otherwise sort it — same
 * field-table-order exception `cell` (key 49) already documents above.
 *
 * Returns false (out_len untouched) on a buffer overflow or a
 * have_fix/err contract violation — never partial output. */
bool loc_build_cbor(uint8_t *out, size_t cap, size_t *out_len, bool signed_env, uint64_t n,
                     const char *id, int64_t ts, bool have_fix, double lat, double lon,
                     bool have_acc, int32_t acc_m, int64_t fix_ts, bool src_cell, const char *req,
                     bool cached, const char *err, const loc_cell_t *cell, const char *why);

/* Parses a `/down` envelope already reduced to `count` map pairs the same
 * way msg.c's/lock.c's own MK_ and CFGK_ readers do (`sig_pair_present` stands
 * in for `ident_get_flags() & IDENT_FLAG_REQ_SIG`, same host-testability
 * convention lock_parse_cfg() documents). `buf` MUST already have passed
 * auth_verify() when signed — the trailing `sig` pair trimmed, the map
 * header's own declared pair count left untouched (docs/DEVICE_PLAN.md
 * §2.4's "no re-serialisation" rule; `sig_pair_present` tells this function
 * to account for that one extra declared-but-absent pair).
 *
 * Returns false if `buf` does not decode as a well-formed `kind:"loc_req"`
 * envelope carrying a non-empty `id` — covering both "not loc_req at all"
 * and "loc_req but malformed", deliberately conflated exactly like
 * lock_parse_cfg()'s own doc comment explains: either way the caller
 * (loc_ingest_req_cbor()) falls through to the normal ingest path. On
 * success, `out_id` is NUL-terminated into `out_id_cap` bytes (truncated
 * away silently if it does not fit — same convention lock_parse_cfg() uses,
 * this id is only ever echoed back in `/loc`'s `req` field, never rendered). */
bool loc_parse_req_cbor(const uint8_t *buf, uint16_t len, bool sig_pair_present, char *out_id,
                        size_t out_id_cap);

/* ---------------------------------------------------------------------
 * Location tracking types (LOCATION_TRACKING_DESIGN.md §1/§2, task F1).
 * --------------------------------------------------------------------- */

typedef enum {
    LOC_MSTATE_STILL = 0,
    LOC_MSTATE_MOVING = 1,
} loc_mstate_t;

/* One slot of the "distinct cells seen in the last 60 min" ring (§1). A
 * duplicate key touches its existing slot's timestamp rather than growing a
 * second entry; a genuinely new key evicts the least-recently-seen slot once
 * all LOC_CELL_RING_N are in use. */
typedef struct {
    char key[LOC_CELL_KEY_MAX];
    int64_t last_seen_us;
    bool used;
} loc_cell_ring_slot_t;

typedef struct {
    loc_cell_ring_slot_t slot[LOC_CELL_RING_N];
} loc_cell_ring_t;

/* "2 new cells within 15 min" (§1) is the same shape as the sustained-motion
 * classifier (loc_motion_ring_t) but only ever needs to hold
 * LOC_NEWCELL_COUNT-ish entries; a fixed 4-deep ring is generous headroom
 * without reusing LOC_MOTION_RING's much larger 16. */
typedef struct {
    int64_t events_us[4];
    int n;
} loc_newcell_ring_t;

/* What loc_track_tick() decided is due to publish this call, if anything
 * (LOCATION_TRACKING_DESIGN.md §2.1's report table). GNSS-while-moving
 * outcomes are NOT decided here -- see loc_track_gnss_due() below; the
 * device layer drives that attempt itself and reports its own result
 * through loc_track_note_report_sent(..., LOC_REPORT_GNSS, ...). */
typedef enum {
    LOC_REPORT_NONE = 0,
    LOC_REPORT_STILL,   /* why:"still" -- hourly cell fix, stationary */
    LOC_REPORT_CELL,    /* why:"cell"  -- a new serving cell, debounced */
    LOC_REPORT_MOVE,    /* why:"move"  -- periodic refresh while moving */
    LOC_REPORT_GNSS,    /* why:"gnss"  -- a scheduled in-motion GNSS attempt's own outcome */
    LOC_REPORT_STOP,    /* why:"stop"  -- MOVING -> STILL, only when it says something new */
} loc_report_t;

/* The whole location-tracking state, RAM-resident (see loc_rtc_t's own
 * comment for why: PROTOCOL.md §9.2 wants this reset by a reboot, not
 * carried across one). One instance, owned by loc.c's device-only section
 * in normal operation; firmware/host/test_loc.c owns its own per test.
 * Deliberately a SEPARATE struct from loc_policy_t above: the two rate
 * limiters do not share state (LOCATION_TRACKING_DESIGN.md §2.3 "the move
 * schedule is its own rate limit") -- a `loc_req` (loc_policy_t's own
 * backoff/floor) and the background tracker below can each be mid-gate
 * without the other ever seeing it. */
typedef struct {
    bool have_lis3dh; /* false: cell-change is the ONLY motion signal (§1) */
    loc_mstate_t state;

    loc_cell_ring_t cell_ring;
    loc_newcell_ring_t newcell_events;
    loc_motion_ring_t motion; /* own instance -- NOT shared with loc_policy_t's */
    int64_t last_motion_edge_us; /* every accel edge, 0 = none yet this session */
    int64_t last_new_cell_us;    /* every "new" cell (ring-miss), 0 = none yet */
    int64_t moving_since_us;     /* when the current MOVING episode began */

    bool have_cur_cell;
    char cur_cell_key[LOC_CELL_KEY_MAX];        /* most recently observed serving cell */
    int64_t cur_cell_since_us;                  /* when it started serving */
    bool have_last_reported_cell;
    char last_reported_cell_key[LOC_CELL_KEY_MAX]; /* cell attached to the last report of any kind */
    bool gnss_published_this_episode;           /* a `why:gnss` fix went out since MOVING began */

    int64_t last_loc_us;         /* last unsolicited /loc of ANY kind, 0 = none this session */
    loc_report_t pending_report; /* latched by the P1 120s gate (loc_track_tick()) -- never dropped */

    uint32_t gnss_backoff_s;              /* §2.3: doubling backoff, own to the move schedule */
    int64_t gnss_last_attempt_us;         /* 0 = no scheduled attempt this episode yet */
    int64_t gnss_last_assist_refresh_us;  /* 0 = never refreshed this power session */
    int64_t gnss_attempt_ring_us[LOC_GNSS_DAILY_CAP]; /* rolling 24h attempt log, oldest-first */
    int gnss_attempt_ring_n;
} loc_track_t;

/* ---------------------------------------------------------------------
 * Location tracking, pure functions (task F1; T0's own bullet list: STILL/
 * MOVING transitions incl. no-LIS3DH, the report scheduler, the GNSS
 * schedule, the daily cap, the transition-only backoff reset, the 2h
 * assistance rule).
 * --------------------------------------------------------------------- */

void loc_track_init(loc_track_t *t, bool have_lis3dh);
loc_mstate_t loc_track_state(const loc_track_t *t);

/* Motion-edge input: accel.c's own drained/refractory-throttled INT1 event,
 * same cadence loc_on_motion_event() already receives. Always records the
 * edge (`last_motion_edge_us`, used by the MOVING->STILL check and the GNSS
 * edge-tail guard below) regardless of state or LIS3DH presence; only feeds
 * the >=60s-within-3min sustained classifier, and only that classifier's own
 * newly-true edge fires a STILL->MOVING transition, when `have_lis3dh`.
 * Returns true iff this call caused STILL->MOVING (backoff/floor reset --
 * §1's "transition-only" rule). */
bool loc_track_on_motion(loc_track_t *t, int64_t now_us);

/* Cell-observation input: call once per genuine serving-cell change (same
 * de-duplicated cadence net_set_cell_change_cb()'s URC already provides to
 * loc_on_cell_change()) -- never on an unchanged read. Always updates
 * `cur_cell_key`/`cur_cell_since_us` (feeds the report scheduler's own
 * serving-duration debounce); a cell not in the 60-min ring is also "new"
 * (feeds `last_new_cell_us` and the 15-min/2-count classifier) EXCEPT the
 * very first cell this power session, which only seeds the ring -- same
 * "a starting point is not an observed change" carve-out
 * loc_trigger_cell_change() documents for the on-demand mechanism. Returns
 * true iff this call caused STILL->MOVING. Works identically with or
 * without a LIS3DH -- this is the ONLY motion signal when one is absent
 * (§1: "Without a LIS3DH, only the cell rule applies"). */
bool loc_track_on_cell(loc_track_t *t, int64_t now_us, const char *cell_key);

/* True iff an unsolicited /loc may publish right now (P1, server-architect
 * review 26 Sep 2026: a 120s floor between ANY two unsolicited reports,
 * whatever their `why`). Exposed separately from loc_track_tick() so the
 * device layer can apply the same gate to a scheduled GNSS attempt's own
 * outcome, which loc_track_tick() does not decide (see loc_report_t's own
 * comment). */
bool loc_track_gate_ok(const loc_track_t *t, int64_t now_us);

/* Call once per loc_service() iteration while `loctrack` is on. Runs the
 * MOVING->STILL check (§1) and the report scheduler (§2.1): "stop" outranks
 * "cell", which outranks the periodic "still"/"move" (mutually exclusive by
 * state, so they never actually compete). Whatever is due is then run
 * through the P1 120s gate above; a report that is due but gated is LATCHED
 * (`pending_report`) and re-offered on every later call -- superseded by
 * anything higher-priority that becomes due in the meantime -- until the
 * gate opens, never silently dropped. Returns LOC_REPORT_NONE when nothing
 * is due or the one thing that is due is still gated. The caller MUST call
 * loc_track_note_report_sent() once it has actually published a non-NONE
 * result (a publish failure should just leave `last_loc_us` alone -- the
 * next tick recomputes and tries again). */
loc_report_t loc_track_tick(loc_track_t *t, int64_t now_us);

/* Records that `which` was just published (or, for LOC_REPORT_GNSS, that
 * the device layer's own gate-wait -- see loc_track_gate_ok() -- just
 * cleared and it published the attempt's outcome). Updates `last_loc_us`
 * (the P1 gate), `last_reported_cell_key` (the "cell differs" stop/cell-
 * change tests) and, when `which == LOC_REPORT_GNSS && gnss_fix`, latches
 * `gnss_published_this_episode` (the stop report's own "or a GNSS fix was
 * published this episode" clause). */
void loc_track_note_report_sent(loc_track_t *t, int64_t now_us, loc_report_t which, bool gnss_fix);

/* §2.2's schedule: due once MOVING, `move_gnss_s` (the `locmove` runtime
 * tunable, 0=off) has elapsed since the later of "MOVING began" or "the last
 * scheduled attempt", AND at least as long as the current backoff, AND
 * (§2.2's own exit-tail guard) there has been a recent accel edge
 * (`have_lis3dh`) or recent new cell (no LIS3DH) -- so the tail of an
 * episode that is really over does not spend one more attempt. Does not
 * check the daily cap or assistance freshness itself -- see
 * loc_track_gnss_cap_reached()/loc_track_gnss_assist_due() below; the
 * device layer applies all three before actually starting an attempt. */
bool loc_track_gnss_due(const loc_track_t *t, int64_t now_us, uint32_t move_gnss_s);

/* §2.2 "mandatory... refreshed at most every 2h": true if never refreshed
 * this power session, or the last refresh is >=2h old. */
bool loc_track_gnss_assist_due(const loc_track_t *t, int64_t now_us);
void loc_track_gnss_assist_refreshed(loc_track_t *t, int64_t now_us);

/* §2.2 "Daily cap: 48 scheduled attempts" -- a rolling 24h window (not a
 * calendar day: this is a stuck-INT1 backstop, not a billing boundary),
 * implemented as a 48-deep ring of attempt timestamps; ages out entries
 * >=24h old as a side effect, same eviction style loc_trigger_motion_event()
 * uses for its own ring. */
bool loc_track_gnss_cap_reached(loc_track_t *t, int64_t now_us);

/* Records that a scheduled attempt is starting NOW (pushes the daily-cap
 * ring, sets `gnss_last_attempt_us` -- the schedule's own "since the last
 * attempt" clock). Call only once loc_track_gnss_due() and
 * loc_track_gnss_cap_reached() have both been checked. */
void loc_track_gnss_attempt_started(loc_track_t *t, int64_t now_us);

/* §2.3 "A failure steps the backoff 5->10->20->40 min; a success resets
 * it" -- via loc_next_backoff_s(), the SAME doubling sequence loc_policy_t's
 * own on-demand backoff uses, applied to `gnss_backoff_s` instead (§2.3
 * "the move schedule is its own rate limit" -- a separate counter, not a
 * separate formula). An assistance-refresh failure counts as a failure
 * here too (§2.2: "counts as a failure") -- the device layer calls this
 * with success=false without ever having called
 * loc_track_gnss_attempt_started() for that cycle in that case. */
void loc_track_gnss_attempt_done(loc_track_t *t, bool success);

/* PROTOCOL.md §13.3 item 2's 60s bound on a web (loc_req-driven) answer,
 * amended by the server-architect review (26 Sep 2026): time spent on an
 * assistance refresh that ran counts inside this bound too. Pure
 * arithmetic: min(base_budget_s, 60 - assist_elapsed_s), floored at 0 (a
 * refresh that alone took >=60s leaves no time for a GNSS wait at all --
 * the caller must then fail the attempt without ever starting the GNSS
 * wait phase). */
uint32_t loc_web_gnss_budget_s(uint32_t base_budget_s, uint32_t assist_elapsed_s);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * Device wiring — needs ESP-IDF (RTC struct, NVS-free but esp_timer/
 * FreeRTOS/net.h/auth.h/ident.h), same `#ifdef ESP_PLATFORM` split every
 * other main module uses.
 * --------------------------------------------------------------------- */
#include "auth.h" /* auth_rtc_t — loc_bind() below, same reason book.h/msg.h include it */

typedef void (*loc_rtc_lock_fn)(void);
typedef void (*loc_rtc_unlock_fn)(void);
typedef void (*loc_rtc_save_fn)(void); /* must be called with the lock already held */
typedef void (*loc_epoch_wrap_fn)(void);

/* Same handoff pattern as msg_bind_rtc()/lock_bind_rtc()/book_bind(): modes.c
 * owns `rtc`'s storage/magic/CRC and the auth_rtc_t `/up,/status,/loc`
 * counter (`g_rtc.auth`, shared with every other signed publish), and hands
 * both pointers plus the cross-task lock/unlock/save trio and the epoch-wrap
 * callback over here. Call once, before loc_init(). */
void loc_bind(loc_rtc_t *rtc, auth_rtc_t *auth_rtc, loc_rtc_lock_fn lock, loc_rtc_unlock_fn unlock,
              loc_rtc_save_fn save, loc_epoch_wrap_fn on_wrap);

/* Probes the LIS3DH (accel.c) and configures GNSS + the cell-change/GNSS
 * event plumbing (net.h). Call once from modes_boot(), after net_init() (so
 * the modem exists to configure) and after ui_init() (so accel.c's shared
 * I2C bus, installed there, already exists — accel.c must not install the
 * I2C driver a second time). Marks the in-memory policy as "cold boot":
 * the first attempt this power session gets the LOC_FIRST_ATTEMPT_S budget
 * (V02_DESIGN.md §5: "no ephemeris in the receiver yet"). Never fails
 * outward — a GNSS/accel bring-up problem is logged and location then
 * always answers from cache/`no_fix` (this task's own "must not break the
 * receive path" rule); nothing here can block boot. */
void loc_init(void);

/* `kind:"loc_req"` intercept (docs/PROTOCOL.md §3.2/§13, V02_DESIGN.md §5).
 * Called from modes.c's on_incoming_message(), in the same slot as
 * lock_ingest_cfg_cbor()/book_ingest_cbor() — after auth_verify(), before
 * msg_ingest_down_cbor(). Returns true iff `buf` decoded as a loc_req and
 * was fully handled here (queued behind an in-flight attempt, answered at
 * once from cache/backoff, or accepted to start a new attempt); false means
 * "not loc_req (or malformed) — fall through", same convention every other
 * intercept in this codebase uses. Deliberately mirrors the `cfg`/`book`
 * intercepts' replay-window handling exactly: like them, this function does
 * NOT call auth_accept_down_n() itself (PROTOCOL.md §3.2's own text: a
 * redelivered loc_req is suppressed by the rate limit reaching the same
 * cached/no-op outcome on its own, not by a separate replay check) — never
 * a thread entry, never shown/read-acked, works identically locked or
 * unlocked, active or asleep, and never changes mode or wakes the display. */
bool loc_ingest_req_cbor(const uint8_t *buf, uint16_t len);

/* True while a GNSS attempt (real loc_req or `gnsstest`) is in flight — the
 * one fact coverage.c's duty-cycle policy needs from this module for its own
 * radio-ownership rule (coverage.h's own module comment): the duty cycle
 * must never take the radio while this is true. Cheap, lock-guarded read of
 * the same flag loc_on_request()/loc_debug_run() already use for "one
 * attempt at a time" (this task). */
bool loc_attempt_in_progress(void);

/* One step of the GNSS attempt state machine, called every modes_run() loop
 * iteration regardless of whether an attempt is in progress (a few RAM
 * reads when idle — see loc.c's own phase-machine comment for exactly how
 * little work one call does: never the whole attempt, never a multi-second
 * GNSS wait in one call). Publishes `/loc` for every queued request once a
 * result is available. No return value: everything it does is either
 * logged or, on completion, published. */
void loc_service(void);

/* /status fields this module owns (PROTOCOL.md §5.1, §10 keys 27/28/43, and
 * LOCATION_TRACKING_DESIGN.md §5 P3 key 61). Plain reads of already-resident
 * policy state, no AT round trip of their own.
 *
 * loc_get_period_s() (key 27, P2): 0 when `loctrack` is off (today's
 * behaviour — periodic fixes parked); LOC_STATUS_PERIOD_TRACKING_S (3600)
 * while it is on, "the maximum gap between unsolicited reports while
 * connected" — a nominal, rounded-up figure for the wire, distinct from the
 * internal LOC_HOURLY_CELL_S=3300 gate that actually decides when a `still`
 * report fires.
 * loc_get_move_gnss_s() (key 61, this task, below): the current `locmove`
 * runtime tunable (LOC_MOVE_GNSS_DEFAULT_S by default; 0 = GNSS-while-moving
 * off), reported regardless of the `loctrack` flag -- it is just the
 * configured interval, same convention loc_get_min_s() already uses for the
 * (always active) on-demand trigger floor. */
uint32_t loc_get_min_s(void);
uint32_t loc_get_period_s(void);
uint32_t loc_get_backoff_remaining_s(void);

/* `loctrack on|off` (main.c console command, this task's own test plan, §9):
 * gates the whole background tracker (loc_track_tick()/loc_track_on_*()/
 * loc_track_gnss_due() and everything they drive) so the behaviour can be
 * A/B'd on one flash. Defaults to LOC_TRACK_DEFAULT_ENABLED (loc_init()).
 * Flipping it off does not clear loc_track_t's state (a later `on` resumes
 * from wherever the classifier/scheduler already was, not a fresh boot) --
 * only whether loc_service() drives it at all. RAM-only: this is a bench/
 * debug knob, not something that needs to survive a reset. */
void loc_set_track_enabled(bool enabled);
bool loc_get_track_enabled(void);

/* `locmove <s>` (main.c console command): the GNSS-while-moving interval
 * (LOC_MOVE_GNSS_S in the design doc), 0 = off. Takes effect on the next
 * loc_track_gnss_due() check; does not reset gnss_backoff_s/
 * gnss_last_attempt_us. RAM-only, same reasoning as loc_set_track_enabled(). */
void loc_set_move_gnss_s(uint32_t seconds);
uint32_t loc_get_move_gnss_s(void);

/* Trigger entry points — both do a little RAM/RTC work only (the pure
 * loc_trigger_*() calls above, under the bound lock) and never touch the
 * modem or publish anything themselves; loc_service() is what actually
 * starts an attempt, and only ever in response to a loc_req, never from a
 * trigger alone (V02_DESIGN.md §5: "they never start an attempt by
 * themselves"). */

/* Called from accel.c's accel_poll() (modes_run()'s own task, never an ISR —
 * the LIS3DH's INT1 is only ever read as a polled register, see accel.c's
 * module comment) once per drained INT1 event. */
void loc_on_motion_event(void);

/* `gnsstest <seconds>` (main.c, PAGER_DEBUG_NO_LIGHT_SLEEP builds only):
 * arms exactly one attempt through the same route/state-machine code a real
 * `loc_req` uses, bypassing the backoff/battery-floor gate (but NOT the "one
 * attempt at a time" rule — returns false immediately, without starting
 * anything, if a request-driven attempt is already running, and vice
 * versa). modes_run()'s own loc_service() call is still what actually
 * drives the attempt forward, exactly as for a real request — this function
 * itself only blocks the CALLING task (the debug console's own, never
 * modes_run()'s) until the attempt finishes or `seconds` elapses, polling
 * for completion rather than stepping the state machine itself (two tasks
 * driving the same non-reentrant phase machine at once would race). Prints
 * (via the caller's own log level) whether the modem accepted the fix
 * request, time to fix/timeout, confidence and satellite count, and what
 * happened to the MQTT session (net_get_mqtt_status() before/after).
 * Updates the same cached fix / backoff state a real attempt would (so a
 * successful `gnsstest` run also answers the next real `loc_req` from
 * cache) but never publishes `/loc` (there is no requester). LOCATION_
 * TRACKING_DESIGN.md F5: route 2 (CFUN=4) is gone, so this always runs the
 * one remaining (in-place) route. */
bool loc_debug_run(uint32_t seconds);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* LOC_H */

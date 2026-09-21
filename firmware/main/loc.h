/* loc.h — on-demand location (docs/V02_DESIGN.md §5, docs/V02_DESIGN.md §5,
 * docs/PROTOCOL.md §3.2/§13).
 *
 * Split the same way lock.c/auth.c/msg.c already are: everything above the
 * `#ifdef ESP_PLATFORM` banner is pure C, no ESP-IDF dependency, driven by an
 * injected clock (`int64_t now_us`, `esp_timer_get_time()`-shaped but never
 * called directly from here) so firmware/host/test_loc.c can run the whole
 * backoff/trigger/cache/queue policy without a device. It owns:
 *
 *   - the growing backoff (5 min doubling to a 12 h cap, reset by success),
 *   - the 10-minute floor a trigger reset leaves on top of it,
 *   - the cell-change (10 min) and sustained-motion (>=60s spanning a 3 min
 *     window) trigger classifiers,
 *   - the "one attempt in flight, late requests share its answer" queue,
 *   - the battery floor (3.3V) and confidence-acceptance (<=100) gates,
 *   - the `/loc` CBOR encoder (`loc_build_cbor()`), byte-compatible with
 *     `relay/app/wirecbor.py` (see firmware/host/test_loc.c for the exact
 *     `relay/.venv/bin/python` command used to cross-check it), and
 *   - the `kind:"loc_req"` request parser.
 *
 * Below the banner: the device-only orchestration — the RTC-resident route
 * hint, the phased (never-blocks-the-caller-for-more-than-one-step) GNSS
 * attempt state machine driven by loc_service() from modes_run()'s own loop,
 * the accelerometer/cell-change trigger entry points, and the `gnsstest`
 * debug hook. All modem/GNSS access goes through net.h's small facade
 * (net_gnss_*()/net_radio_*()) — this file never touches WalterModem
 * directly, same rule every other main module follows (net.h's own module
 * comment).
 *
 * Route to the radio (V02_DESIGN.md §5): try "in place" (attached, eDRX gap)
 * first; if the modem refuses (LTE_CONCURRENCY), fall back to a deliberate
 * CFUN=4 (NO_RF) window — disconnect MQTT, fix, re-attach, let modes_run()'s
 * own F1/F3 retry reconnect. Whichever route actually produced a result is
 * remembered in RTC (`loc_rtc_t.route`) so a device that has already learned
 * route 1 never works on this modem/carrier combination stops wasting half
 * of every attempt budget re-trying it. This is a one-byte RTC addition
 * (docs/PROTOCOL.md §9.3); see modes.c's own layout-version comment for the
 * bump this required.
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

#define LOC_STATUS_MIN_S 600u    /* reported /status loc_min_s: the trigger floor above */
#define LOC_STATUS_PERIOD_S 0u   /* reported /status loc_period_s: periodic fixes stay off */

#define LOC_ID_MAX 17     /* "l_" + 8 hex + NUL, PROTOCOL.md §1 */
#define LOC_MAX_QUEUED 4  /* loc_req ids sharing one in-flight attempt's answer */
#define LOC_CELL_KEY_MAX 24 /* "lac:ci" opaque string, net.cpp's own field widths */
#define LOC_MOTION_RING 16  /* motion-event timestamps kept for the 3-min span check */

/* Where a cached/answered fix came from (PROTOCOL.md §10 `loc.src`: gnss/cell). */
typedef enum {
    LOC_SRC_GNSS = 0,
    LOC_SRC_CELL = 1,
} loc_src_t;

/* Route to the radio that most recently produced a result — RTC-resident,
 * one byte (see loc_rtc_t below). LOC_ROUTE_UNKNOWN means "try in place
 * first", the every-attempt default until one route is proven. */
typedef enum {
    LOC_ROUTE_UNKNOWN = 0,
    LOC_ROUTE_INPLACE = 1,
    LOC_ROUTE_CFUN4 = 2,
} loc_route_t;

/* RTC-resident sub-struct (docs/PROTOCOL.md §9.3's table row) — modes.c
 * embeds this in its own pager_rtc_t (same pattern as lock_rtc_t/auth_rtc_t)
 * and owns the storage/magic/CRC/locking; loc.c only reads/writes through
 * the pointer loc_bind() hands over. Deliberately just the route byte: the
 * backoff/cache/queue policy below is RAM-only by design (PROTOCOL.md §9.2's
 * rule — losing it to a reset does not make the relay's view of delivery
 * wrong, and §13.3's own "cached:true ... from this power session" wording
 * requires it to reset on a reboot). Padded to 8 bytes for alignment, same
 * convention lock_rtc_t documents. */
typedef struct {
    uint8_t route; /* loc_route_t */
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
 * `v,id,ts,loc,req[,cached][,err]`, ascending key order, and — when
 * `signed_env` — an `n` field sized into the map header's pair count but
 * appended as the LAST body field so the caller can immediately follow with
 * auth_sign() (docs/DEVICE_PLAN.md §2.4's "no re-serialisation" rule; same
 * convention modes.c's build_status_cbor()/book.c's book_request() use —
 * the header count includes both `n` and the not-yet-written `sig` pair).
 *
 * Exactly one of (`have_fix` true) or (`err` non-NULL) must hold — §13.2:
 * `err` only when `loc` is null. `have_acc`/`src_cell` are the two optional
 * `loc` sub-fields (`acc`, `src`); omitted (not just zero/"gnss") when
 * false, matching the wire's own "absent = unknown/default" rule. `req`
 * NULL writes `req:null` (an unsolicited fix — never actually produced by
 * this task, since `loc_period_s` stays 0, but kept general). `cached` is
 * written only when true (default false, PROTOCOL.md §13.2).
 *
 * Returns false (out_len untouched) on a buffer overflow or a
 * have_fix/err contract violation — never partial output. */
bool loc_build_cbor(uint8_t *out, size_t cap, size_t *out_len, bool signed_env, uint64_t n,
                     const char *id, int64_t ts, bool have_fix, double lat, double lon,
                     bool have_acc, int32_t acc_m, int64_t fix_ts, bool src_cell, const char *req,
                     bool cached, const char *err);

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

/* One step of the GNSS attempt state machine, called every modes_run() loop
 * iteration regardless of whether an attempt is in progress (a few RAM
 * reads when idle — see loc.c's own phase-machine comment for exactly how
 * little work one call does: never the whole attempt, never a multi-second
 * GNSS wait in one call). Publishes `/loc` for every queued request once a
 * result is available. No return value: everything it does is either
 * logged or, on completion, published. */
void loc_service(void);

/* /status fields this module owns (PROTOCOL.md §5.1, §10 keys 27/28, and
 * V02_DESIGN.md §7 key 43). Plain reads of already-resident policy state,
 * no AT round trip of their own. */
uint32_t loc_get_min_s(void);
uint32_t loc_get_period_s(void);
uint32_t loc_get_backoff_remaining_s(void);

/* Trigger entry points — both do a little RAM/RTC work only (the pure
 * loc_trigger_*() calls above, under the bound lock) and never touch the
 * modem or publish anything themselves; loc_service() is what actually
 * starts an attempt, and only ever in response to a loc_req, never from a
 * trigger alone (V02_DESIGN.md §5: "they never start an attempt by
 * themselves"). */

/* Called from net.cpp's network event handler (via net_set_cell_change_cb())
 * with the raw, already-deduplicated-against-the-immediately-previous-value
 * "lac:ci" key; net.cpp's own module comment documents why that
 * de-duplication happens there rather than here. Keep this fast: it can run
 * on WalterModem's _eventProcessingTask, same rule as every other event
 * handoff in this codebase. */
void loc_on_cell_change(const char *cell_key);

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
 * (via the caller's own log level) the
 * route taken, whether the modem accepted the fix request, time to
 * fix/timeout, confidence and satellite count, and what happened to the
 * MQTT session (net_get_mqtt_status() before/after). Updates the same
 * cached fix / backoff / route-hint state a real attempt would (so a
 * successful `gnsstest` run also answers the next real `loc_req` from
 * cache) but never publishes `/loc` (there is no requester). */
bool loc_debug_run(uint32_t seconds);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* LOC_H */

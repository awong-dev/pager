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

bool loc_trigger_motion_event(loc_policy_t *p, int64_t now_us)
{
    loc_motion_ring_t *r = &p->motion;
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

    // Sustained motion confirmed. Clear the ring so a continuing stream of
    // interrupts needs a fresh 60s span before firing again, rather than
    // re-triggering (and re-resetting an already-zero backoff) on every
    // subsequent interrupt for as long as the child keeps moving.
    r->n = 0;
    apply_trigger_reset_locked(p);
    return true;
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
// `loc` sub-map (docs/PROTOCOL.md §10 "Sub-map keys").
#define LOCSUB_LAT 0
#define LOCSUB_LON 1
#define LOCSUB_ACC 2
#define LOCSUB_FIXTS 3
#define LOCSUB_SRC 4

bool loc_build_cbor(uint8_t *out, size_t cap, size_t *out_len, bool signed_env, uint64_t n,
                     const char *id, int64_t ts, bool have_fix, double lat, double lon,
                     bool have_acc, int32_t acc_m, int64_t fix_ts, bool src_cell, const char *req,
                     bool cached, const char *err)
{
    if (!out || !out_len || !id) {
        return false;
    }
    if (have_fix == (err != NULL && err[0] != '\0')) {
        // §13.2: `err` present iff `loc` is null — these two must disagree.
        return false;
    }

    uint32_t nfields = 3; // v, id, ts
    nfields += 1;         // loc (map or null)
    nfields += 1;         // req (tstr or null)
    if (cached) {
        nfields += 1;
    }
    if (!have_fix) {
        nfields += 1; // err
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

// ---------------------------------------------------------------------------
// /status fields (docs/PROTOCOL.md §5.1, V02_DESIGN.md §5/§7).
// ---------------------------------------------------------------------------

uint32_t loc_get_min_s(void) { return LOC_STATUS_MIN_S; }
uint32_t loc_get_period_s(void) { return LOC_STATUS_PERIOD_S; }

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

// ---------------------------------------------------------------------------
// /loc publish (docs/PROTOCOL.md §13.1/§13.2, §14 signing).
// ---------------------------------------------------------------------------

// req_id may be NULL for an unsolicited fix (never actually produced by this
// task — loc_period_s stays 0 — kept general per loc_build_cbor()'s own
// comment). QoS 1 iff req_id is non-NULL, PROTOCOL.md §13.1.
static void publish_loc_answer(const char *req_id, bool success, double lat, double lon,
                                bool have_acc, int32_t acc_m, int64_t fix_ts, uint8_t src,
                                bool cached)
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
                        acc_m, fix_ts, src_cell, req_id, cached, success ? NULL : "no_fix")) {
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
        ESP_LOGI(TAG, "/loc %s published: req=%s %s cached=%d", id, req_id ? req_id : "(none)",
                 success ? "fix" : "no_fix", (int) cached);
    } else {
        ESP_LOGI(TAG, "/loc publish failed for req=%s", req_id ? req_id : "(none)");
    }
}

// Drains every queued request id and gives each its own /loc, all carrying
// the one shared result just obtained (PROTOCOL.md §13.3 item 3).
static void drain_queue_and_publish(bool success, double lat, double lon, bool have_acc,
                                     int32_t acc_m, int64_t fix_ts, uint8_t src)
{
    char id[LOC_ID_MAX];
    s_lock();
    bool have = loc_take_queued_id(&s_policy, id, sizeof(id));
    s_unlock();
    while (have) {
        publish_loc_answer(id, success, lat, lon, have_acc, acc_m, fix_ts, src, false);
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

    int batt_mv = modes_get_batt_mv();
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
    case LOC_ANSWER_CACHED:
        ESP_LOGI(TAG, "loc_req %s answered from %s (batt=%dmV) without powering GNSS", id,
                 have_cached ? "cache" : "no_fix", batt_mv);
        publish_loc_answer(id, have_cached, lat, lon, have_acc, acc_m, fix_ts, src, have_cached);
        break;
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

void loc_on_cell_change(const char *cell_key)
{
    int64_t now_us = esp_timer_get_time();
    s_lock();
    bool reset = loc_trigger_cell_change(&s_policy, now_us, cell_key);
    s_save();
    s_unlock();
    if (reset) {
        ESP_LOGI(TAG, "cell change (%s): backoff reset, 10min floor since last attempt applies",
                 cell_key ? cell_key : "?");
    }
}

void loc_on_motion_event(void)
{
    int64_t now_us = esp_timer_get_time();
    s_lock();
    bool reset = loc_trigger_motion_event(&s_policy, now_us);
    s_save();
    s_unlock();
    if (reset) {
        ESP_LOGI(TAG, "sustained motion: backoff reset, 10min floor since last attempt applies");
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
    LOC_PH_ROUTE1_START,
    LOC_PH_ROUTE1_WAIT,
    LOC_PH_ROUTE2_TEARDOWN,
    LOC_PH_ROUTE2_START,
    LOC_PH_ROUTE2_WAIT,
    LOC_PH_ROUTE2_RADIO_ON,
    LOC_PH_ROUTE2_REATTACH_WAIT,
    LOC_PH_DONE,
} loc_phase_t;

// Re-attach after route 2's NO_RF window should be much cheaper than a
// cold-boot attach (PDP/APN/SIM already negotiated, only re-registration is
// needed) — UNVERIFIED, so this cap is deliberately generous rather than
// tuned; look for "re-attach after CFUN=4" in the log to see the real figure.
#define LOC_REATTACH_CAP_S 90u

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
static loc_phase_t s_phase = LOC_PH_IDLE;
static uint32_t s_budget_s = LOC_ATTEMPT_S;
static bool s_budget_extendable = true; // false only for loc_debug_run()'s explicit `seconds`
static int64_t s_phase_deadline_us = 0;
static int64_t s_attempt_start_us = 0;
static net_gnss_event_t s_last_event;
static bool s_route1_tried = false;
static loc_route_t s_route_used = LOC_ROUTE_UNKNOWN;

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

static loc_phase_t get_phase(void)
{
    s_lock();
    loc_phase_t ph = s_phase;
    s_unlock();
    return ph;
}

static loc_route_t rtc_get_route(void)
{
    s_lock();
    loc_route_t r = s_rtc ? (loc_route_t) s_rtc->route : LOC_ROUTE_UNKNOWN;
    s_unlock();
    return r;
}

static void rtc_set_route(loc_route_t r)
{
    s_lock();
    if (s_rtc && s_rtc->route != (uint8_t) r) {
        s_rtc->route = (uint8_t) r;
        s_save();
    }
    s_unlock();
}

// Called from either the MQTT event task (a real loc_req, via
// loc_ingest_req_cbor()) or the debug console task (loc_debug_run()) --
// never from modes_run()'s task. Sets s_policy.attempt_in_progress under the
// same lock as the phase kickoff so the two flags can never disagree about
// whether an attempt is running (loc_debug_run()'s busy-check and a real
// loc_req's loc_on_request() both consult attempt_in_progress, so a gnsstest
// run and a real request-driven attempt can never overlap either).
static void begin_attempt(uint32_t budget_s, bool extendable)
{
    s_attempt_start_us = esp_timer_get_time();
    s_budget_s = budget_s;
    s_budget_extendable = extendable;
    s_route1_tried = false;
    s_route_used = LOC_ROUTE_UNKNOWN;
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

    ESP_LOGI(TAG, "gnss attempt done: %s route=%d sats=%u confidence=%.1f elapsed=%llds",
             success ? "FIX" : "no_fix", (int) s_route_used, (unsigned) s_last_event.sat_count,
             s_last_event.confidence,
             (long long) ((esp_timer_get_time() - s_attempt_start_us) / 1000000));

    if (s_route_used != LOC_ROUTE_UNKNOWN) {
        rtc_set_route(s_route_used);
    }

    // Both branches update the cache/backoff (a real fix is a real fix even
    // when gnsstest found it) -- only whether there is a queue to drain
    // differs. s_debug_done is set INSIDE this same locked section, before
    // s_unlock(), so the mutex's release/acquire pairing guarantees
    // loc_debug_run()'s completion-wait (which polls attempt_in_progress
    // under the same lock) never observes "not in progress" before also
    // being able to observe s_debug_done's final value.
    s_lock();
    loc_on_attempt_done(&s_policy, success, lat, lon, have_acc, acc_m, fix_ts, LOC_SRC_GNSS);
    if (s_debug_mode) {
        s_debug_done = true;
    }
    s_save();
    s_unlock();

    if (!s_debug_mode) {
        drain_queue_and_publish(success, lat, lon, have_acc, acc_m, fix_ts, LOC_SRC_GNSS);
    }
    s_phase = LOC_PH_IDLE; // same-task write; the only cross-task readers use attempt_in_progress
}

void loc_service(void)
{
    switch (get_phase()) {
    case LOC_PH_IDLE:
        return; // nothing to do — the common case, every wake cycle

    case LOC_PH_ASSIST_CHECK: {
        int32_t stale_s = 0;
        bool ok = net_gnss_assistance_due(&stale_s);
        if (ok && stale_s > 0) {
            ESP_LOGI(TAG, "gnss assistance fresh for another %lds, skipping refresh", (long) stale_s);
            s_phase = LOC_PH_ROUTE1_START;
        } else {
            ESP_LOGI(TAG, "gnss assistance %s due", ok ? "is" : "status unknown, assuming");
            s_phase = LOC_PH_ASSIST_UPDATE;
        }
        break;
    }

    case LOC_PH_ASSIST_UPDATE: {
        // Needs LTE — must happen before any detach (route 2). Bounded but
        // UNVERIFIED duration (net_gnss_update_assistance()'s own comment);
        // one loc_service() call may therefore take a few seconds here,
        // same tolerated class as F4's checkComm() retries elsewhere in
        // modes_run()'s loop.
        if (net_gnss_update_assistance() && s_budget_extendable) {
            s_budget_s = LOC_FIRST_ATTEMPT_S; // "no ephemeris in the receiver yet"
        }
        s_phase = LOC_PH_ROUTE1_START;
        break;
    }

    case LOC_PH_ROUTE1_START: {
        net_gnss_config(); // idempotent, persists across reboots per the vendor doc
        loc_route_t remembered = rtc_get_route();
        if (remembered == LOC_ROUTE_CFUN4) {
            // Already learned route 1 does not work on this modem/carrier —
            // don't spend attempt budget re-proving it every time.
            ESP_LOGI(TAG, "route 1 (in place) previously refused; going straight to route 2");
            s_phase = LOC_PH_ROUTE2_TEARDOWN;
            break;
        }
        s_route1_tried = true;
        if (!net_gnss_start_fix()) {
            s_phase = LOC_PH_ROUTE2_TEARDOWN; // modem refused synchronously
            break;
        }
        s_phase_deadline_us = esp_timer_get_time() + (int64_t) s_budget_s * 1000000;
        s_phase = LOC_PH_ROUTE1_WAIT;
        break;
    }

    case LOC_PH_ROUTE1_WAIT: {
        net_gnss_event_t ev;
        if (net_gnss_poll_event(&ev)) {
            if (ev.kind == NET_GNSS_EVT_REFUSED) {
                ESP_LOGI(TAG, "route 1 (in place) refused mid-wait; falling back to route 2");
                s_phase = LOC_PH_ROUTE2_TEARDOWN;
                break;
            }
            s_last_event = ev;
            s_route_used = LOC_ROUTE_INPLACE;
            finish_attempt(ev.kind == NET_GNSS_EVT_FIX && loc_fix_confidence_ok(ev.confidence));
            break;
        }
        if (esp_timer_get_time() >= s_phase_deadline_us) {
            net_gnss_cancel();
            s_route_used = LOC_ROUTE_INPLACE; // it was allowed to run — just no sky
            finish_attempt(false);
        }
        break; // still waiting; try again next loc_service() call
    }

    case LOC_PH_ROUTE2_TEARDOWN: {
        // Deliberate session loss (V02_DESIGN.md §5) — must not trip
        // handle_mqtt_loss()'s backoff, the connect watchdog, or the F4
        // health check while this window is open.
        modes_set_loc_suppress(true);
        net_session_down();
        if (!net_radio_off()) {
            ESP_LOGI(TAG, "route 2: setOpState(NO_RF) failed; abandoning this attempt");
            modes_set_loc_suppress(false);
            s_route_used = LOC_ROUTE_UNKNOWN;
            finish_attempt(false);
            break;
        }
        s_phase = LOC_PH_ROUTE2_START;
        break;
    }

    case LOC_PH_ROUTE2_START: {
        if (!net_gnss_start_fix()) {
            ESP_LOGI(TAG, "route 2: gnss refused a fix even with the radio off (unexpected)");
            s_phase = LOC_PH_ROUTE2_RADIO_ON; // still have to restore the radio
            break;
        }
        s_phase_deadline_us = esp_timer_get_time() + (int64_t) s_budget_s * 1000000;
        s_phase = LOC_PH_ROUTE2_WAIT;
        break;
    }

    case LOC_PH_ROUTE2_WAIT: {
        net_gnss_event_t ev;
        if (net_gnss_poll_event(&ev)) {
            if (ev.kind != NET_GNSS_EVT_REFUSED) {
                s_last_event = ev;
            }
            s_phase = LOC_PH_ROUTE2_RADIO_ON;
            break;
        }
        if (esp_timer_get_time() >= s_phase_deadline_us) {
            net_gnss_cancel();
            s_phase = LOC_PH_ROUTE2_RADIO_ON;
        }
        break;
    }

    case LOC_PH_ROUTE2_RADIO_ON: {
        if (!net_radio_on()) {
            ESP_LOGI(TAG, "route 2: setOpState(FULL) failed; will keep trying next cycle");
            break; // stay in this phase — must not leave the radio off forever
        }
        s_phase_deadline_us = esp_timer_get_time() + (int64_t) LOC_REATTACH_CAP_S * 1000000;
        s_phase = LOC_PH_ROUTE2_REATTACH_WAIT;
        break;
    }

    case LOC_PH_ROUTE2_REATTACH_WAIT: {
        bool attached = net_is_attached();
        bool timed_out = esp_timer_get_time() >= s_phase_deadline_us;
        if (attached || timed_out) {
            ESP_LOGI(TAG, "route 2: re-attach %s after CFUN=4 window (%llds)",
                     attached ? "succeeded" : "timed out",
                     (long long) ((esp_timer_get_time() - (s_phase_deadline_us -
                                                            (int64_t) LOC_REATTACH_CAP_S * 1000000)) /
                                   1000000));
            modes_set_loc_suppress(false); // modes_run()'s own F1/F3 retry reconnects MQTT from here
            s_route_used = attached ? LOC_ROUTE_CFUN4 : LOC_ROUTE_UNKNOWN;
            finish_attempt(s_last_event.kind == NET_GNSS_EVT_FIX &&
                          loc_fix_confidence_ok(s_last_event.confidence));
        }
        break;
    }

    case LOC_PH_DONE:
    default:
        s_phase = LOC_PH_IDLE;
        break;
    }
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

    // accel.c fails open on its own (absent chip -> logs once, returns
    // false) — loc.c does not need to branch on the result, motion just
    // never triggers if it is missing.
    accel_init();

    ESP_LOGI(TAG, "location ready: loc_min_s=%u loc_period_s=%u battery_floor=%dmV",
             (unsigned) loc_get_min_s(), (unsigned) loc_get_period_s(), LOC_BATTERY_FLOOR_MV);
}

bool loc_debug_run(uint32_t seconds)
{
    if (seconds == 0 || seconds > 120) {
        ESP_LOGI(TAG, "gnsstest: seconds must be 1..120");
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
    ESP_LOGI(TAG, "gnsstest: done. route=%d sats=%u confidence=%.1f mqtt_connected(after)=%d",
             (int) s_route_used, (unsigned) s_last_event.sat_count, s_last_event.confidence,
             (int) after.mqtt_connected);
    return s_debug_done;
}

#endif /* ESP_PLATFORM */

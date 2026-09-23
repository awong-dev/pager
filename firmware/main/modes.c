// modes.c — mode state machine, RTC memory contract, wake-and-drain loop,
// button/key event dispatch.
//
// Authority: docs/PROTOCOL.md §4.1 (ack rules), §5.4 (status cadence), §8
// (wake sources), §9 (RTC memory), §11 (mode funnel).
//
// modes.c is the only place that touches the RTC struct and the only place
// that changes `mode` (funnelled through set_mode(), per §11). It drains
// input.c's event queue and wires msg.c/ui.c together; input.c itself owns
// the button GPIO/FSM and the CardKB key decode table (F6.2,
// docs/DEVICE_PLAN.md §5.3).
//
// All power-effect comments are PENDING_HW.

#include "modes.h"
#include "msg.h"
#include "net.h"
#include "watchdog.h"
#include "ui.h"

// F6.2 (docs/DEVICE_PLAN.md §5.3): CardKB decode + button FSM (+BTN_STUCK)
// + the UI-awake window + one input event queue, moved out of this file
// and ui.c respectively. ime.h is included (but not yet called from
// anywhere - see its own module comment) purely so idf.py build actually
// compiles the header; F6.3 wires the first real text field to it.
#include "input.h"
#include "ime.h"

// F3.6 (docs/PROTOCOL.md §14, §10 keymap; docs/DEVICE_PLAN.md §2.5/§2.7):
// signing, the CBOR /status codec, and the auth_rtc_t sub-struct embedded
// below.
#include "auth.h"
#include "cbor.h"
#include "ident.h"

// F6.5 (docs/DEVICE_PLAN.md §5.8): the passcode lock — RTC fields and the
// restart->locked and defer-shown-while-locked wiring below. Its `cfg.lock`
// sub-map handler is now reached through cfg.c's dispatcher (below), not
// called directly from this file (v0.2 §4.4: a single `cfg` push can also
// carry `ca`, cfg.c is what decodes the envelope once and hands each
// sub-map to its own owner).
#include "lock.h"

// v0.2 §4.4 (docs/V02_DESIGN.md, docs/V02_DESIGN.md §4): the `cfg` envelope
// dispatcher (cfg.c) and CA trust state/fallback/two-phase-apply (catrust.c)
// — the `cfg` interception ahead of msg_ingest_down_cbor() (same slot the
// old direct lock_ingest_cfg_cbor() call used to occupy), the `/status`
// `tls`/`ca_fp` fields below, and catrust_service() driven from this file's
// own wake-and-drain loop, alongside loc_service()/accel_poll().
#include "cfg.h"
#include "catrust.h"

// F7.1 (docs/DEVICE_PLAN.md §4.3): book.c's NVS-backed address book — the
// `kind:"book"` dispatch ahead of msg_ingest_down_cbor() (alongside lock.c's
// `cfg` dispatch, same interception pattern), and the real `bv` for
// build_status_cbor() below. book.c has no RTC sub-struct of its own (see
// book.h's module comment) — it only needs the EXISTING g_rtc.auth binding,
// wired via book_bind() in modes_boot().
#include "book.h"

// v0.2 §5 (docs/V02_DESIGN.md, docs/PROTOCOL.md §3.2/§13): loc.c's
// `kind:"loc_req"` dispatch ahead of msg_ingest_down_cbor() (same
// interception slot as lock.c/book.c above), its own tiny RTC-resident route
// hint (loc_rtc_t, embedded below), and its `/status` fields
// (loc_min_s/loc_period_s/loc_backoff_s). accel.c's LIS3DH poll is driven
// from modes_run()'s loop alongside input_poll()/ui_poll_keyboard().
#include "accel.h"
#include "loc.h"

// Owner request, 2026-09-20: coverage.c's own duty-cycle policy (pure,
// host-tested, see coverage.h's own module comment) — this file is the only
// caller of net_radio_off()/net_radio_on()/net_session_down() on its behalf,
// same "policy decides, modes.c/net.c act" split loc.c's route 2 established.
#include "coverage.h"

// v0.2 §6 (docs/V02_DESIGN.md, docs/PROTOCOL.md §3.6): sms.c's device-direct
// SMS allow-list/audit/send-receive state machine, driven from this file's
// own wake-and-drain loop (sms_service(), alongside loc_service()/
// accel_poll()/catrust_service() above) and from cfg.c's `cfg.sms`
// dispatch. No RTC sub-struct of its own (allow-list + audit queue both
// live in NVS, per this task's own "prefer NVS/RAM" instruction) — sms_bind()
// only hands over the shared cross-task mutex, same two-argument pattern
// catrust_bind() uses.
#include "sms.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_crc.h"
#include "esp_sleep.h"
#include "nvs.h"
#include "esp_private/esp_clk.h" /* esp_clk_rtc_time() */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "modes";

// ---------------------------------------------------------------------------
// Compile-time constants. PAGER_WAKE_INTERVAL_* / PAGER_ACTIVE_IDLE_TIMEOUT_S
// are deliberately named to match PROTOCOL.md §8.2 so they can be retuned
// against a real current trace without touching logic.
// ---------------------------------------------------------------------------

#define PAGER_WAKE_INTERVAL_SLEEP_MS 5000u  // T=5s sleep mode, PROTOCOL.md §8.2
#define PAGER_WAKE_INTERVAL_ACTIVE_MS 2000u // T=2s active mode
// pump_blocked no longer keys on ui_awake/btn_busy/btn_stuck (see its doc
// comment above), so msg_pump() now runs on every loop iteration once
// connected, not just once per wake-and-drain cycle. On the 2s/5s wake
// cadence this limit never binds; on the 100ms UI-awake busy-poll cadence
// it caps the pump rate at 5 Hz instead of one AT transaction per 100ms.
#define PAGER_PUMP_MIN_INTERVAL_US (200 * 1000)
// Owner request, 2026-09-20: nothing to receive while unregistered (no MQTT
// session at all) -- sleep mode's own wake interval can lengthen well past
// the registered T=5s without costing any latency that matters, since there
// is nothing to poll for. Still keeps the button (ext0) wake via net_sleep()
// unchanged. UNVERIFIED exact current saving (see coverage.h's own estimate
// block); 30s is a conservative middle ground, not a measured optimum.
#define PAGER_WAKE_INTERVAL_UNREGISTERED_MS 30000u
// How long the pager stays awake, with RTS asserted, after each timer wake.
// Measured on hardware 2026-09-21 (`sleeptest`, GM02SP LR8.2.1.0): while RTS is
// deasserted the modem HOLDS its URCs (nothing is lost), but with 50 ms awake
// it never hands them over: pages sent to a sleeping pager were not received
// at all, across a whole boot. With 150 ms they were delivered during real
// light sleep (35 s and 136 s after sending, against a 20.48 s eDRX cycle, so
// 150 is probably marginal). 200 ms until it has been bisected and the long
// tail explained. Power effect: 200 ms per 5 s wake = 4% awake, against the
// 1% the design assumed; this is now the dominant term of the sleep budget.
// UNVERIFIED: the minimum, and whether an AT poke right after the wake would
// let it be shorter (docs/ROADMAP.md).
#define PAGER_POST_WAKE_YIELD_MS 200u
#define PAGER_ACTIVE_IDLE_TIMEOUT_S (10 * 60) // 10 min, firmware/README.md
#define PAGER_STATUS_HEARTBEAT_S 3600u         // §5.4(d)
#define PAGER_CHECKCOMM_EVERY_N_WAKES 60u      // F4: ~5 min at T=5s
#define PAGER_MODEM_RESET_MIN_INTERVAL_US ((int64_t) 10 * 60 * 1000000) // F4 rate limit
#define PAGER_FW_VERSION "0.1.0"

// F6.2: the button FSM itself (short/long/BTN_STUCK, debounce, long-press
// threshold) moved to input.c; this is only the outer loop's own polling
// granularity while input_button_busy() is true (needs to be frequent
// enough for input.c's debounce/hold-duration timing to resolve well - see
// input.c's PAGER_BTN_DEBOUNCE_MS/PAGER_BTN_LONG_PRESS_MS).
#define PAGER_BTN_POLL_MS 20u

// F1/F3 backoff schedule: 5s, 15s, 60s, 300s, then steady at 300s.
static const uint32_t k_backoff_s[] = { 5, 15, 60, 300 };
#define PAGER_BACKOFF_STEPS (sizeof(k_backoff_s) / sizeof(k_backoff_s[0]))

// ---------------------------------------------------------------------------
// RTC memory contract (PROTOCOL.md §9). The dedup/ack/
// reply/unread state lives in msg_rtc_t (msg.h), embedded here as a
// nested `msg` field; modes.c remains the sole owner of the enclosing
// struct, its magic/crc32 pair and rtc_save() (§9.3), and hands msg.c a
// typed pointer plus lock/unlock/save callbacks via msg_bind_rtc().
//
// The measured 7003-byte walter-modem RTC footprint (see
// sdkconfig.defaults) leaves only ~1184 of the 8192-byte RTC_SLOW region
// for this struct, far short of what storing full message bodies here would
// need. §9.3 resolves that by keeping full bodies out of RTC entirely (they
// live in msg.c's RAM-resident s_thread, §9.5) and only mirroring the
// single newest-unread message plus small id-only queues in RTC.
// ---------------------------------------------------------------------------

#define PAGER_ID_MAX_LEN 17 // 16 chars + NUL, PROTOCOL.md §1/§3.1

// magic encodes both validity and a layout version tag (per CLAUDE.md's RTC
// convention: "a version tag and a CRC"). Bump it on any layout change: a
// stale-but-CRC-valid struct read across an incompatible change would
// otherwise decode as garbage.
// F3.6: bumped 2 -> 3 for the new `auth` (auth_rtc_t, +12 B) field below,
// docs/DEVICE_PLAN.md §2.7's "RTC changes".
// F6.4: bumped 3 -> 4 — msg_rtc_t's pending_up/unread sub-structs dropped
// their inline bodies (moved to NVS namespace `msgq`, docs/PROTOCOL.md
// §9.2-§9.4) and pending_up/unread gained/lost fields; a stale layout-3
// struct would otherwise decode msg_pending_up_t.to as garbage bytes that
// used to be the middle of a body.
// F6.5: bumped 4 -> 5 — new `lock` field (lock_rtc_t, +16 B, docs/PROTOCOL.md
// §9.3's table row); a stale layout-4 struct has no such field at all, so
// reading it unversioned would decode 16 bytes of whatever used to be past
// the end of the old struct as `locked`/`fail_count`/`backoff_until_us`.
// v0.2 §5: bumped 5 -> 6 — new `loc` field (loc_rtc_t, +8 B, PROTOCOL.md
// §9.3's table row): the single route-to-the-radio byte loc.c remembers
// (V02_DESIGN.md §5). Everything else location-related is deliberately
// RAM-only (loc.h's own module comment), so this is the whole addition.
#define PAGER_RTC_MAGIC 0x50475236u // "PGR" + layout version 6

typedef enum {
    PAGER_MODE_SLEEP = 0,
    PAGER_MODE_ACTIVE = 1,
} pager_mode_t;

typedef enum {
    MODE_REASON_BOOT = 0,
    MODE_REASON_BUTTON,
    MODE_REASON_INCOMING_MSG,
    MODE_REASON_IDLE_TIMEOUT,
} pager_mode_reason_t;

typedef struct {
    uint32_t magic;
    uint32_t crc32;

    uint32_t boot_count;
    char session_id[12]; // "s_" + 8 hex + NUL, PROTOCOL.md §1

    uint8_t mode; // pager_mode_t
    // This is a MONOTONIC deadline (esp_timer_get_time()
    // microseconds), not wall-clock epoch seconds. Comparing against
    // approx_epoch() would not work: it reads 0 until the network
    // clock arrives (§3.5) — combined with the "!=0" guard on the exit
    // check, that meant a device with no clock yet never left active mode
    // (stuck at 7-17mA instead of 1.8-2.1mA), and then the window could
    // slam shut instantly once epoch time first became available and
    // happened to already exceed the stale deadline computed against 0.
    // A monotonic clock has no "not available yet" state, so this class of
    // bug cannot recur here.
    int64_t active_until_us;

    uint32_t status_pub_count;
    int64_t last_status_epoch;

    msg_rtc_t msg; // PROTOCOL.md §9.3 — owned in layout by modes.c, in
                    // behaviour by msg.c via msg_bind_rtc().

    uint32_t mqtt_memfull_count; // §8.4 M6, cumulative
    uint32_t oversize_drop_count; // F6, cumulative

    uint32_t modem_resets;         // F4, cumulative
    int64_t last_modem_reset_us;   // esp_timer_get_time() at last reset, for the 10min rate limit

    uint32_t attach_fail_cycles; // F1
    uint32_t wake_cycle_count;   // drives the "every 60 wakes" F4 check

    auth_rtc_t auth; // PROTOCOL.md §14.2/§9.3, DEVICE_PLAN.md §2.5/§2.7 —
                      // owned in layout by modes.c, in behaviour by auth.c
                      // via the pointer msg_bind_auth() hands msg.c and the
                      // direct &g_rtc.auth uses in this file's own /status
                      // publish path below.

    lock_rtc_t lock; // PROTOCOL.md §9.3, DEVICE_PLAN.md §5.8 — owned in
                      // layout by modes.c, in behaviour by lock.c via the
                      // pointer lock_bind_rtc() hands it in modes_boot().

    loc_rtc_t loc; // v0.2 §5 (docs/V02_DESIGN.md, PROTOCOL.md §9.3) — owned
                    // in layout by modes.c, in behaviour by loc.c via the
                    // pointer loc_bind() hands it in modes_boot().
} pager_rtc_t;

// F6.5: sizeof(pager_rtc_t) was 512 bytes (see firmware/build/school_pager.map's
// `.rtc.data.0` entry for esp-idf/main/libmain.a(modes.c.obj), PROTOCOL.md
// §9.1's own methodology) before this change.
// v0.2 §5: +8 B for the new `loc` field (loc_rtc_t) -> 520 bytes, 664 B
// under the 1184-byte budget below. modes_boot()'s own ESP_LOGI of
// sizeof(g_rtc) is the authoritative figure — see the report this task
// ships with for the number it actually printed.
_Static_assert(sizeof(pager_rtc_t) <= 1184,
               "pager_rtc_t exceeds the measured 1184-byte RTC_SLOW budget "
               "left after walter-modem's own ~7003 bytes (PROTOCOL.md §9.1)");

RTC_DATA_ATTR static pager_rtc_t g_rtc;

// Guards g_rtc against the race between the wake-and-drain loop below and
// the MQTT MESSAGE callback (WalterModem's _eventProcessingTask), and is
// reused (msg.h's header comment explains why) to guard msg.c's RAM-resident
// thread/composer state too. Statically allocated: no heap use after init.
static StaticSemaphore_t s_rtc_mutex_buf;
static SemaphoreHandle_t s_rtc_mutex;

static void rtc_lock(void) { xSemaphoreTake(s_rtc_mutex, portMAX_DELAY); }
static void rtc_unlock(void) { xSemaphoreGive(s_rtc_mutex); }

static bool s_was_mqtt_connected = false;
static bool s_ui_awake_prev = false; // F6.3: edge-detects input_awake() for ui_wake_status_refresh()

// Bench bug fix (typing on the CardKB dropped ~every other character):
// modes_run()'s render call below (ui_render()) used to fire on every loop
// iteration a key event was drained, and each one blocks the task for
// disp_partial_refresh()'s ~455ms BUSY wait (disp.c) - ui_poll_keyboard()
// (called once per iteration, before this drain) could not run again until
// that wait returned, and the CardKB only holds the single most recent
// unread key, so a key typed mid-refresh was lost outright even with
// disp_busy_idle_hook() now polling during the wait (that fix stops the
// *loss*; this one cuts down how often the *long block* happens at all).
// modes_run()'s event-drain switch below (INPUT_EVT_KEY case) calls
// key_render_note() instead of rendering immediately; the render decision
// block further down calls key_render_due() to decide whether this
// iteration's render is allowed to fire. RAM-only, modes_run()'s task only
// - no cross-task lock needed (same reasoning s_ui_awake_prev above uses).
static bool s_key_render_pending = false;
static int64_t s_key_render_first_us = 0;
static int64_t s_key_render_deadline_us = 0;

// Called from the INPUT_EVT_KEY case below on every key event. First key of
// a burst: deadline = now+250ms. Every further key pushes the deadline back
// out to now+250ms, but never past first_key_time+1000ms, so a sustained
// fast typist still gets a render at least once a second rather than
// starving it indefinitely.
static void key_render_note(int64_t now_us)
{
    if (!s_key_render_pending) {
        s_key_render_pending = true;
        s_key_render_first_us = now_us;
        s_key_render_deadline_us = now_us + 250000;
        return;
    }
    int64_t candidate = now_us + 250000;
    int64_t cap = s_key_render_first_us + 1000000;
    s_key_render_deadline_us = (candidate < cap) ? candidate : cap;
}

// True if a key-triggered render is due (deadline passed) or there is none
// outstanding at all - i.e. this iteration's render is allowed to proceed.
// Clears the pending flag as a side effect once it lets a render through,
// so the caller's own render call is what "pays off" the debounce.
static bool key_render_due(int64_t now_us)
{
    if (!s_key_render_pending) {
        return true;
    }
    if (now_us < s_key_render_deadline_us) {
        return false;
    }
    s_key_render_pending = false;
    return true;
}

// v0.2 §9.5/§7 key 50: per-MQTT-session counter within this boot, incremented
// in the rising-edge block below on every session start (including a
// modem-initiated resume the firmware repaired, `st.session_restart_edge`)
// before publish_status_online() runs, so the session's first online status
// already carries the new value. RAM-only by design (§9.5 doesn't need it to
// survive a reset: a reset starts a new boot and the relay's `session` id
// changes too, which already forces the re-publish this counter is for).
// No modem/sleep-state effect of its own — a plain counter read/write.
static uint32_t s_mqtt_link_counter = 0;

// v0.2 bug fix #3 (docs/V02_DESIGN.md §2.3): connect watchdog. mqttConnect()
// can "wedge silently" -- neither CONNECTED nor DISCONNECTED ever fires
// (GOTCHAS.md documents this engine doing exactly that for the
// original `setup` hang). s_connect_attempt_us records when the outstanding
// attempt was issued (0 = none outstanding) so modes_run() can notice 60s
// of silence and force the issue instead of retrying (or doing nothing)
// forever. s_connect_watchdog_count is consecutive silent timeouts, reset by
// any real CONNECTED/DISCONNECTED event. Both RAM-only, same reasoning as
// s_was_mqtt_connected above: this design never deep sleeps, and a watchdog
// timeout mid-boot after a real reset just becomes an ordinary F3 backoff
// retry, nothing needs to survive a reset here.
static int64_t s_connect_attempt_us = 0;
static uint32_t s_connect_watchdog_count = 0;
#define PAGER_CONNECT_WATCHDOG_US ((int64_t) 60 * 1000000) // §2.3: 60s

// Rate limit for msg_pump(), now that pump_blocked no longer keys on
// ui_awake/btn_busy/btn_stuck (see that variable's doc comment): 0 means "no
// call yet, run immediately". RAM-only, same reasoning as
// s_connect_attempt_us above.
static int64_t s_next_pump_us = 0;

// v0.2 §5: see modes.h's own modes_set_loc_suppress() doc comment. RAM-only,
// same reasoning as s_connect_attempt_us above (never survives, or needs to
// survive, a reset).
static volatile bool s_loc_suppress = false;

void modes_set_loc_suppress(bool suppress)
{
    if (suppress != s_loc_suppress) {
        ESP_LOGI(TAG, "location %s the ordinary MQTT reconnect/F4 health-check machinery",
                 suppress ? "suppressing" : "releasing");
    }
    s_loc_suppress = suppress;
}

// v0.2 §4.4: catrust.c's own independent suppression window, same reasoning
// and same two call sites as s_loc_suppress above — see modes.h's own doc
// comment.
static volatile bool s_ca_apply_suppress = false;

void modes_set_ca_apply_suppress(bool suppress)
{
    if (suppress != s_ca_apply_suppress) {
        ESP_LOGI(TAG, "CA apply %s the ordinary MQTT reconnect/F4 health-check machinery",
                 suppress ? "suppressing" : "releasing");
    }
    s_ca_apply_suppress = suppress;
}

// Owner request, 2026-09-20: coverage.c's duty-cycle policy state + the one
// RAM flag that mirrors "the radio is deliberately off right now" for every
// other module to check (modes_coverage_owns_radio(), modes.h). RAM-only,
// same reasoning as s_loc_suppress/s_ca_apply_suppress above: this design
// never deep sleeps, so nothing here needs to survive a reset — a reboot
// mid-cycle just restarts the policy from GRACE_S, the same conservative
// default coverage_policy_init() gives a cold boot anyway.
static coverage_policy_t s_coverage;
static volatile bool s_coverage_owns_radio = false;

bool modes_coverage_owns_radio(void) { return s_coverage_owns_radio; }

void modes_note_motion_reset(void) { coverage_on_motion(&s_coverage); }

void modes_coverage_debug_print(void)
{
    bool registered = (net_unregistered_for_s() == 0);
    uint32_t dark_s = net_unregistered_for_s();
    uint32_t remaining_s = coverage_phase_remaining_s(&s_coverage, esp_timer_get_time());
    const char *phase_str = (s_coverage.phase == COVERAGE_PHASE_OFF)
                                 ? "OFF (radio deliberately off)"
                             : (s_coverage.phase == COVERAGE_PHASE_SEARCH)
                                 ? "SEARCH (radio full, hunting)"
                                 : "NONE (registered, or still inside the grace window)";
    printf("coverage: registered=%d dark_for=%us phase=%s off_step=%u/3 (%us) next_action_in=%us "
           "owns_radio=%d last_dark=%us cycles=%u\n",
           (int) registered, (unsigned) dark_s, phase_str, (unsigned) coverage_off_step_index(&s_coverage),
           (unsigned) s_coverage.off_period_s, (unsigned) remaining_s, (int) s_coverage_owns_radio,
           (unsigned) coverage_last_dark_s(&s_coverage), (unsigned) s_coverage.cycles);
}

// ---------------------------------------------------------------------------
// RTC helpers
// ---------------------------------------------------------------------------

static uint32_t rtc_compute_crc(void)
{
    // CRC over everything except the magic+crc32 header itself.
    const uint8_t *start = (const uint8_t *) &g_rtc.boot_count;
    size_t len = sizeof(g_rtc) - offsetof(pager_rtc_t, boot_count);
    return esp_rom_crc32_le(0, start, len);
}

static bool rtc_is_valid(void)
{
    return g_rtc.magic == PAGER_RTC_MAGIC && g_rtc.crc32 == rtc_compute_crc();
}

static void rtc_save(void)
{
    g_rtc.magic = PAGER_RTC_MAGIC;
    g_rtc.crc32 = rtc_compute_crc();
}

static void gen_session_id(char *out, size_t out_size)
{
    // "s_" + 8 lowercase hex, PROTOCOL.md §1.
    snprintf(out, out_size, "s_%08x", (unsigned) esp_random());
}

static void rtc_cold_init(void)
{
    memset(&g_rtc, 0, sizeof(g_rtc));
    g_rtc.boot_count = 1;
    gen_session_id(g_rtc.session_id, sizeof(g_rtc.session_id));
    g_rtc.mode = (uint8_t) PAGER_MODE_SLEEP;
    rtc_save();
}

// Best-effort wall clock; returns 0 if the network clock was never obtained
// (PROTOCOL.md §3.5 - the caller must then treat ts as 0, not retry SNTP).
static int64_t approx_epoch(void)
{
    int64_t e;
    if (net_get_clock(&e)) {
        return e;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// F3.6 (docs/PROTOCOL.md §14/§2.7): NVS persistence for the /up,/status,/loc
// counter's epoch half. Called only when auth_next_up_n() reports `up_lo`
// just wrapped (~once per 1M signed publishes) — never on the hot path.
// ---------------------------------------------------------------------------

static void on_auth_epoch_wrap(void)
{
    // Power effect: one NVS (flash) write. No modem or sleep-state effect.
    // ident_t has no per-field setter, so this snapshots every getter into a
    // local copy, bumps n_epoch, and writes the whole struct back via
    // ident_store() (same contract setup.c's own first-time write uses).
    static ident_t snap; /* static: 4 kB+ struct, too big for this task's stack (see ident_load()) */
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.dev_id, ident_get_dev_id(), sizeof(snap.dev_id) - 1);
    strncpy(snap.mqtt_pw, ident_get_mqtt_pw(), sizeof(snap.mqtt_pw) - 1);
    memcpy(snap.kdev, ident_get_kdev(), sizeof(snap.kdev));
    strncpy(snap.host, ident_get_host(), sizeof(snap.host) - 1);
    snap.port = ident_get_port();
    strncpy(snap.ca, ident_get_ca(), sizeof(snap.ca) - 1);
    snap.ca_len = ident_get_ca_len();
    strncpy(snap.apn, ident_get_apn(), sizeof(snap.apn) - 1);
    snap.flags = ident_get_flags();
    strncpy(snap.label, ident_get_label(), sizeof(snap.label) - 1);
    memcpy(snap.ca_hash, ident_get_ca_hash(), sizeof(snap.ca_hash));
    snap.n_epoch = (uint16_t) (ident_get_n_epoch() + 1);
    snap.claimed = ident_get_claimed();

    if (!ident_store(&snap)) {
        ESP_LOGI(TAG, "failed to persist n_epoch=%u after up_lo wrap (§2.5) - "
                      "next boot's replay window may see a gap",
                 (unsigned) snap.n_epoch);
    } else {
        ESP_LOGI(TAG, "n_epoch bumped to %u after up_lo wrap (§14.2)", (unsigned) snap.n_epoch);
    }
}

// ---------------------------------------------------------------------------
// Status (/status) publishing - PROTOCOL.md §5, §14.
// ---------------------------------------------------------------------------

// PROTOCOL.md §10 envelope keymap — the subset this file writes for /status.
#define STK_V 0
#define STK_TS 2
#define STK_N 12
#define STK_BV 14
#define STK_STATE 21
#define STK_MODE 22
#define STK_BATT_MV 23
#define STK_RSSI 24
#define STK_SESSION 25
#define STK_FW 26
#define STK_LOC_PERIOD_S 27
#define STK_LOC_MIN_S 28
#define STK_LOC_BACKOFF_S 43 // v0.2 §7
#define STK_TLS 39           // v0.2 §4.3/§7: "unpinned"/"pinned"/"broken"
#define STK_CA_FP 42         // v0.2 §4.3/§7: absent when unpinned
#define STK_SMS_LOST 48      // v0.2 §6/§7: sms_log audit entries dropped for lack of NVS space
#define STK_LINK 50           // v0.2 §9.5/§7: MQTT-session generation within this boot

// PROTOCOL.md §5.1: batt_mv must be in [2000, 4500] when state:"online".
#define PAGER_BATT_MV_MIN 2000
#define PAGER_BATT_MV_MAX 4500

#define PAGER_RSSI_UNSET (-1000) // outside net_get_rssi()'s valid [-113,-51] range

// Last known-good battery/RSSI readings, so a single failed AT command
// doesn't block a /status publish. Plain static (not RTC_DATA_ATTR): this
// design never deep sleeps, only light sleeps, and light sleep retains
// ordinary RAM (same reasoning net.cpp's own module-static state uses).
// Reset to the fallback below only on a real reboot.
static int s_last_batt_mv = 0;              // 0 = no good reading yet this boot
static int s_last_rssi_dbm = PAGER_RSSI_UNSET; // unset = no good reading yet this boot

// `v` MUST be non-negative for cbor_w_uint(); dBm readings are not, so
// /status's `rssi` field needs the signed form.
static bool cbor_w_int(cbor_w_t *w, uint32_t key, int64_t v)
{
    return (v < 0) ? cbor_w_nint(w, key, v) : cbor_w_uint(w, key, (uint64_t) v);
}

// F6.3 (docs/DEVICE_PLAN.md §5.4): factored out of build_status_cbor() so
// the status bar's UI-wake refresh (ui_wake_status_refresh(), below) shares
// the exact same fetch-with-fallback logic instead of duplicating it - one
// AT round trip each, no RRC of its own beyond net_get_battery_mv()/
// net_get_rssi()'s own documented cost (net.h).
static int refresh_batt_mv(void)
{
    int batt_mv;
    if (!net_get_battery_mv(&batt_mv) || batt_mv < PAGER_BATT_MV_MIN ||
        batt_mv > PAGER_BATT_MV_MAX) {
        batt_mv = (s_last_batt_mv != 0) ? s_last_batt_mv : 3300;
    } else {
        s_last_batt_mv = batt_mv;
    }
    return batt_mv;
}

static int refresh_rssi_dbm(void)
{
    int rssi_dbm;
    if (!net_get_rssi(&rssi_dbm)) {
        rssi_dbm = (s_last_rssi_dbm != PAGER_RSSI_UNSET) ? s_last_rssi_dbm : -113;
    } else {
        s_last_rssi_dbm = rssi_dbm;
    }
    return rssi_dbm;
}

// F6.3 (docs/DEVICE_PLAN.md §5.4/§5.7): "Read at every /status publish...
// and at every UI wake" / "Battery... at every UI wake and hourly" - this
// is the one new AT round trip per UI wake §5.7 budgets (~100ms at 40mA).
// Called from modes_run() on the sleep->awake edge of input_awake(). The
// "hourly" half rides the existing maybe_publish_heartbeat() cadence
// instead of a second timer (build_status_cbor() below calls the same two
// refresh_*() functions on every /status publish, including the ~hourly
// heartbeat) - a separate hourly-only timer would just re-read the same
// cache for no reason whenever a network clock is available; the one gap
// this leaves is a device with *both* no UI activity *and* no network
// clock for a long stretch (heartbeat suppressed, PROTOCOL.md §3.5),
// accepted as a minor cosmetic staleness rather than a second always-on
// timer for that edge case.
static void ui_wake_status_refresh(void)
{
    refresh_batt_mv();
    refresh_rssi_dbm();
}

// F6.3: status-bar/Device-screen getters (modes.h) - plain cache reads, no
// AT round trip of their own; see modes.h's own doc comment.
int modes_get_rssi_dbm(void) { return (s_last_rssi_dbm != PAGER_RSSI_UNSET) ? s_last_rssi_dbm : -113; }
int modes_get_batt_mv(void) { return (s_last_batt_mv != 0) ? s_last_batt_mv : 3300; }
// This task: s_last_batt_mv stays 0 until net_get_battery_mv() (AT+SQNVMON)
// returns something inside [PAGER_BATT_MV_MIN, PAGER_BATT_MV_MAX] -- see
// refresh_batt_mv() above. modes_get_batt_mv()'s 3300 fallback above is a
// placeholder, not a reading; this accessor is how a caller (loc.c's
// battery floor) tells the two apart instead of trusting the coincidence
// that the placeholder equals LOC_BATTERY_FLOOR_MV exactly.
bool modes_batt_mv_known(void) { return s_last_batt_mv != 0; }
const char *modes_get_fw_version(void) { return PAGER_FW_VERSION; }
const char *modes_get_session_id(void) { return g_rtc.session_id; }
uint32_t modes_get_memfull_count(void) { return g_rtc.mqtt_memfull_count; }
uint32_t modes_get_oversize_drop_count(void) { return g_rtc.oversize_drop_count; }
uint32_t modes_get_modem_resets(void) { return g_rtc.modem_resets; }

// F3.6: builds the CBOR /status envelope (docs/PROTOCOL.md §2.4/§10), adding
// `rssi` (now published every time, §5.1) and `bv` (book version; F7.1:
// book_get_bv() — 0 until the first `book` has ever been applied, which is
// exactly what makes §4.3's "relay sees `bv` lower than `bookVersion` ->
// push again" rule self-heal a factory reset or a never-provisioned
// device), then signs it with auth_sign() when ident's IDENT_FLAG_REQ_SIG
// is set. No modem or sleep-state effect of its own beyond the
// net_get_battery_mv()/net_get_rssi() AT round trips already documented at
// their call sites (book_get_bv() is a plain RAM read, no NVS I/O).
static bool build_status_cbor(uint8_t *out, size_t cap, size_t *out_len, const char *state)
{
    int64_t ts = approx_epoch();
    const char *mode_str = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE) ? "active" : "sleep";

    // batt_mv/rssi_dbm: F6.3 factored these out into refresh_batt_mv()/
    // refresh_rssi_dbm() (above) so the status bar's UI-wake refresh could
    // share the exact fetch-with-fallback logic instead of duplicating it —
    // net.cpp's AT+SQNVMON/AT+CSQ reads, falling back to the last
    // known-good value and finally to a fixed placeholder, never blocking
    // or failing this publish.
    int batt_mv = refresh_batt_mv();
    int rssi_dbm = refresh_rssi_dbm();

    bool signed_env = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;

    // v0.2 §4.3: `tls` is always present (one of unpinned/pinned/broken);
    // `ca_fp` only when a CA is actually pinned (PINNED or BROKEN) — "absent
    // when unpinned". Both are plain reads of already-resident state (ident's
    // cached ca_len/ca_hash and catrust.c's own RAM-cached broken flag), no
    // AT round trip or NVS I/O of their own.
    const char *tls_str = catrust_state_name(catrust_get_state());
    char ca_fp[17];
    bool have_ca_fp = catrust_get_ca_fp(ca_fp);

    // v0.2 §5/§7: +3 for loc_period_s/loc_min_s/loc_backoff_s (loc.c's own
    // getters — plain reads of already-resident policy state, no AT round
    // trip of their own beyond what batt_mv/rssi above already cost).
    uint32_t nfields = 9 + 3 + 1 + 1 + 1; // + tls, + sms_lost, + link; v,state,mode,batt_mv,rssi,
                                          // session,ts,fw,bv,loc_period_s,loc_min_s,loc_backoff_s,
                                          // tls,sms_lost,link
    if (have_ca_fp) {
        nfields += 1;
    }
    if (signed_env) {
        nfields += 2; // n (written below) + sig (appended by auth_sign())
    }

    cbor_w_t w;
    cbor_w_init(&w, out, cap);
    cbor_w_map(&w, nfields);
    cbor_w_uint(&w, STK_V, 1);
    cbor_w_tstr(&w, STK_STATE, state, strlen(state));
    cbor_w_tstr(&w, STK_MODE, mode_str, strlen(mode_str));
    cbor_w_uint(&w, STK_BATT_MV, (uint64_t) batt_mv);
    cbor_w_int(&w, STK_RSSI, rssi_dbm);
    cbor_w_tstr(&w, STK_SESSION, g_rtc.session_id, strlen(g_rtc.session_id));
    cbor_w_uint(&w, STK_TS, (uint64_t) ts);
    cbor_w_tstr(&w, STK_FW, PAGER_FW_VERSION, strlen(PAGER_FW_VERSION));
    cbor_w_uint(&w, STK_BV, book_get_bv()); // §4.3: book version, F7.1
    cbor_w_uint(&w, STK_LOC_PERIOD_S, loc_get_period_s()); // v0.2 §5: always 0, periodic fixes parked
    cbor_w_uint(&w, STK_LOC_MIN_S, loc_get_min_s());        // v0.2 §5: the 10-minute trigger floor
    cbor_w_uint(&w, STK_LOC_BACKOFF_S, loc_get_backoff_remaining_s()); // v0.2 §7 key 43
    cbor_w_tstr(&w, STK_TLS, tls_str, strlen(tls_str));                // v0.2 §4.3
    if (have_ca_fp) {
        cbor_w_tstr(&w, STK_CA_FP, ca_fp, strlen(ca_fp));              // v0.2 §4.3
    }
    cbor_w_uint(&w, STK_SMS_LOST, sms_get_lost_count());               // v0.2 §6/§7 key 48
    // v0.2 §9.5/§7 key 50: always present because build_status_cbor() is
    // only ever reached with mqtt_connected already true (every caller
    // guards on it, and the rising-edge block below increments the counter
    // before its own call), so s_mqtt_link_counter is always >= 1 here.
    cbor_w_uint(&w, STK_LINK, s_mqtt_link_counter);

    if (!signed_env) {
        *out_len = w.len;
        return !w.err;
    }

    // §14.2: n for /up,/status,/loc; RTC-resident in g_rtc.auth (this file
    // owns that storage directly, unlike msg.c which goes through the
    // msg_bind_auth() pointer).
    bool wrapped = false;
    rtc_lock();
    uint64_t n = auth_next_up_n(&g_rtc.auth, ident_get_n_epoch(), &wrapped);
    rtc_save();
    rtc_unlock();
    cbor_w_uint(&w, STK_N, n);
    if (w.err) {
        return false;
    }
    if (wrapped) {
        on_auth_epoch_wrap();
    }

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/status", net_get_device_id());
    size_t len = w.len;
    if (!auth_sign(topic, out, &len, cap)) {
        return false;
    }
    *out_len = len;
    return true;
}

static void publish_status_online(void)
{
    uint8_t buf[256];
    size_t len = 0;
    if (!build_status_cbor(buf, sizeof(buf), &len, "online")) {
        ESP_LOGI(TAG, "status CBOR build failed (buffer too small or auth_sign failed)");
        return;
    }
    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/status", net_get_device_id());

    if (net_publish_raw(topic, buf, (uint16_t) len, 1)) {
        rtc_lock();
        g_rtc.status_pub_count++;
        g_rtc.last_status_epoch = approx_epoch();
        rtc_unlock();
        ESP_LOGI(TAG, "published /status online (mode=%s)",
                 g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE ? "active" : "sleep");
    } else {
        ESP_LOGI(TAG, "publish /status online failed");
    }
}

// F6.3 (docs/DEVICE_PLAN.md §5.5): Device screen's "Re-sync address book"
// bullet — "publishes a /status now (it carries bv), which is the sync
// trigger." Refuses (returns false, no publish) if the session is not
// currently connected, same guard set_mode() already applies before
// calling publish_status_online() on a real mode edge.
bool modes_publish_status_now(void)
{
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    if (!st.mqtt_connected) {
        return false;
    }
    publish_status_online();
    return true;
}

static void maybe_publish_heartbeat(void)
{
    // §5.4(d): heartbeat rides an ordinary poll wake, at most once per
    // 3600s - not an independent timer (that would cost an extra radio
    // wake for nothing, per §5.4's own rationale).
    int64_t now = approx_epoch();
    if (now == 0) {
        return; // no clock yet; don't spam ts:0 heartbeats
    }
    if (now - g_rtc.last_status_epoch < (int64_t) PAGER_STATUS_HEARTBEAT_S) {
        return;
    }
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    if (!st.mqtt_connected) {
        return;
    }
    publish_status_online();
}

// ---------------------------------------------------------------------------
// set_mode() — the single transition funnel required by PROTOCOL.md §11.
// ---------------------------------------------------------------------------

static const char *mode_name(pager_mode_t m)
{
    return (m == PAGER_MODE_ACTIVE) ? "active" : "sleep";
}

static void set_mode(pager_mode_t new_mode, pager_mode_reason_t reason)
{
    rtc_lock();
    bool changed = ((pager_mode_t) g_rtc.mode != new_mode);
    if (changed) {
        ESP_LOGI(TAG, "mode: %s -> %s (reason=%d)", mode_name((pager_mode_t) g_rtc.mode),
                 mode_name(new_mode), (int) reason);
        g_rtc.mode = (uint8_t) new_mode;
    }
    if (new_mode == PAGER_MODE_ACTIVE) {
        // Refresh the deadline on every call while active, not only on the
        // sleep->active edge: an early return on "mode unchanged" would
        // measure the 10 minutes from mode *entry* rather than from the
        // last activity, and firmware/README.md requires the latter. modes_note_activity() below is the lightweight path
        // for this; set_mode() also refreshes it here so a fresh incoming
        // message or button press extends the window even though it also
        // happens to already be active.
        g_rtc.active_until_us = esp_timer_get_time() + (int64_t) PAGER_ACTIVE_IDLE_TIMEOUT_S * 1000000;
    }
    rtc_save();
    rtc_unlock();

    if (!changed) {
        return;
    }

    // §5.4(b): publish on every mode change, but only if the session is
    // actually usable - otherwise this just becomes a queued publish that
    // races the next connect's own online announcement.
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    if (st.mqtt_connected) {
        publish_status_online();
    }
    // F6.3: no direct render here any more. README R5's fix funnels every
    // render through modes_run()'s own task (ui_render(), called once per
    // wake-and-drain iteration while input_awake()) instead of whichever
    // task happened to call set_mode() (this function runs on both the
    // event task, via handle_ingest_result(), and modes_run()'s task, via
    // the button dispatch below). docs/DEVICE_PLAN.md §5.4's status bar
    // also dropped the old "active"/"sleep" word entirely, so a mode edge
    // alone no longer has anything of its own to repaint — whatever event
    // caused this edge (button, incoming message) already drives its own
    // render through render_pending/ui_render() below.
}

void modes_note_activity(void)
{
    rtc_lock();
    bool active = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE);
    if (active) {
        g_rtc.active_until_us = esp_timer_get_time() + (int64_t) PAGER_ACTIVE_IDLE_TIMEOUT_S * 1000000;
        rtc_save();
    }
    rtc_unlock();
}

bool modes_is_active(void)
{
    rtc_lock();
    bool active = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE);
    rtc_unlock();
    return active;
}

// ---------------------------------------------------------------------------
// F6.3 (docs/DEVICE_PLAN.md §5.3/§5.4, firmware/README.md R5): the incoming-
// message render moves off WalterModem's _eventProcessingTask and onto
// modes_run()'s own task. handle_ingest_result() below (still running on
// the event task) only records what to render; modes_run() drains it via
// service_render_pending(), which calls ui_incoming() (only ui.c entry
// point allowed to render off the render_pending path, since it must run
// synchronously with the msg_mark_shown() that follows) and marks `shown`
// strictly after that render returns — closing R5 and R7 together: shown
// is never claimed before BUSY deasserts, and the event task never blocks
// on the panel.
// ---------------------------------------------------------------------------
typedef struct {
    bool pending;
    bool was_asleep;
    char id[MSG_ID_MAX];
    char from[MSG_FROM_MAX];
} render_pending_t;

// RAM-only (not RTC_DATA_ATTR): a cross-task handoff flag, not state that
// needs to survive a reset — this design never deep sleeps (only light
// sleeps, which retain ordinary RAM), same reasoning s_was_mqtt_connected
// above uses. Guarded by s_rtc_mutex (the same "the only cross-task
// primitive in this design" lock msg.h's own module comment describes).
static render_pending_t s_render_pending;

static void render_pending_set(const char *id, const char *from, bool was_asleep)
{
    rtc_lock();
    s_render_pending.pending = true;
    s_render_pending.was_asleep = was_asleep;
    strncpy(s_render_pending.id, id, sizeof(s_render_pending.id) - 1);
    s_render_pending.id[sizeof(s_render_pending.id) - 1] = '\0';
    strncpy(s_render_pending.from, from, sizeof(s_render_pending.from) - 1);
    s_render_pending.from[sizeof(s_render_pending.from) - 1] = '\0';
    rtc_unlock();
}

// Called only from modes_run()'s own task. Copies out and clears the
// pending flag under the lock, then renders *outside* the lock — ui_incoming()
// can take tens to hundreds of ms (disp_wait_busy()), and holding
// s_rtc_mutex across that would block the event task's next ingest (and
// every other g_rtc access) for no reason; disp.c's own mutex (F6.1,
// closes README R4) is what actually serialises the panel.
static void service_render_pending(void)
{
    rtc_lock();
    if (!s_render_pending.pending) {
        rtc_unlock();
        return;
    }
    render_pending_t p = s_render_pending;
    s_render_pending.pending = false;
    rtc_unlock();

    bool displayed = ui_incoming(p.from, p.was_asleep);
    if (displayed) {
        // §4: "after the e-paper refresh completes (BUSY deasserted), never
        // before" - ui_incoming() only returns true once that render has
        // already happened.
        //
        // v0.2 bug fix #1 (docs/V02_DESIGN.md §2.1): this used to be
        // msg_mark_shown(p.id), acking only the single id this render_pending_t
        // slot remembered. render_pending_set() is a single slot: a burst of
        // messages arriving faster than this task drains it collapses down to
        // "render the last one", and every earlier MSG_ACK_UNSHOWN message in
        // that burst was never acked at all (p.id was already overwritten).
        // ui_incoming() shows the current state of the whole thread, not just
        // `p.from`, so once it renders, every still-UNSHOWN down message is
        // fair to ack in one pass. msg_mark_shown()'s new queued-before-
        // advanced contract (msg.h) means anything that does not fit this
        // pass's MSG_PENDING_ACKS_MAX (8) queue stays UNSHOWN and is retried
        // on the next successful render instead of being lost.
        msg_mark_all_unshown();
    }
    // displayed == false: §5.5's "incoming while typing/on another screen"
    // toast-only path - the message stays MSG_ACK_UNSHOWN on purpose.
    // PROTOCOL.md §4.1 rule 2's "read without a prior shown promotes and
    // back-fills" is what makes that safe once the student actually opens
    // the chat later (scr_chat.c calls msg_mark_read() directly there,
    // never msg_mark_shown() first, for exactly this reason - see its own
    // comment).
}

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
// ---------------------------------------------------------------------------
// Debug build only: `sleeptest <minutes>` (main.c). The debug build never
// light-sleeps, which is exactly why it cannot answer the design's biggest
// open question (PROTOCOL.md section 8.3, M5): does a page arrive while the ESP32
// light-sleeps with RTS deasserted, waking only every few seconds? This opens
// a timed window in which the normal sleep decision applies, records what
// happened in RAM (the USB log is dead while asleep), and prints a report
// when the window closes and the log is alive again.
// ---------------------------------------------------------------------------
#include "esp_sleep.h"

typedef struct {
    int32_t t_s;     // seconds since the window opened
    char kind;       // 'M' new message, 'D' duplicate, 'X' malformed, 'C' connected, 'L' session lost
    int32_t a;       // 'M'/'D': seconds between the relay stamping it and the pager parsing it
    char id[12];
    bool after_window; // task 3: noted during the post-close grace, not the window itself
} st_event_t;

#define ST_EVENTS_MAX 48
static st_event_t s_st_events[ST_EVENTS_MAX];
static volatile uint32_t s_st_n_events = 0;
static volatile int64_t s_st_start_us = 0;
static volatile int64_t s_st_until_us = 0;
static volatile bool s_st_report_due = false;
// Task 3 (how long must the pager stay awake after a wake to receive a held
// URC): 0 = use the build's normal PAGER_POST_WAKE_YIELD_MS / active-or-sleep
// interval; sleeptest <minutes> <yield_ms> <interval_ms> overrides both for
// the window's duration.
static uint32_t s_st_yield_ms = 0;
static uint32_t s_st_interval_ms = 0;
// 0 until the window closes; then window-close + ST_GRACE_S, so events that
// arrive right after closure (the modem catching up once the pager stays
// awake) are still recorded, and the deliberate restart waits for them and
// their acks instead of cutting them off.
static volatile int64_t s_st_grace_until_us = 0;
#define ST_GRACE_S 25
static uint32_t s_st_sleeps = 0, s_st_wake_timer = 0, s_st_wake_other = 0;
// Where the awake time goes, per wake: cumulative microseconds per loop segment.
#define ST_SEG_N 7
static const char *const k_st_seg_name[ST_SEG_N] = { "post-wake yield", "input+ui+render", "mqtt status/retry",
                                                    "msg_pump+health", "accel+loc", "sms", "catrust+heartbeat+rtc_save" };
static int64_t s_st_seg_us[ST_SEG_N];
static int64_t s_st_seg_max_us[ST_SEG_N];
static int64_t s_st_mark_us = 0;
// Raw per-cycle timestamps for the first cycles: esp_timer before and after
// net_sleep(), and the RTC's own clock across the same call, to tell real
// sleep from time spent entering/leaving it.
#define ST_CYC_MAX 10
typedef struct { int64_t before_us, after_us, rtc_before_us, rtc_after_us; } st_cycle_t;
static st_cycle_t s_st_cyc[ST_CYC_MAX];
static uint32_t s_st_ncyc = 0;
#define ST_MARK_BEGIN() do { s_st_mark_us = esp_timer_get_time(); } while (0)
#define ST_MARK(i)                                                                                  \
    do {                                                                                            \
        if (sleeptest_active_flag()) {                                                              \
            int64_t _n = esp_timer_get_time();                                                      \
            int64_t _d = _n - s_st_mark_us;                                                         \
            s_st_seg_us[i] += _d;                                                                   \
            if (_d > s_st_seg_max_us[i]) s_st_seg_max_us[i] = _d;                                   \
            s_st_mark_us = _n;                                                                      \
        }                                                                                           \
    } while (0)
static bool sleeptest_active_flag(void);
static int64_t s_st_asleep_us = 0;

static bool sleeptest_active(void);
static bool sleeptest_active_flag(void) { return sleeptest_active(); }

static bool sleeptest_active(void)
{
    return s_st_until_us != 0 && esp_timer_get_time() < s_st_until_us;
}

// True during the window itself AND during the ST_GRACE_S after it closes
// (once armed by modes_run() -- see the !sleeptest_active() block there).
// sleeptest_note() uses this instead of sleeptest_active() so a URC that
// only arrives once the pager stays fully awake after closure is still
// recorded, not silently dropped.
static bool sleeptest_recording(void)
{
    if (s_st_grace_until_us != 0) {
        return esp_timer_get_time() < s_st_grace_until_us;
    }
    return sleeptest_active();
}

static void sleeptest_note(char kind, int32_t a, const char *id)
{
    if (!sleeptest_recording() || s_st_n_events >= ST_EVENTS_MAX) {
        return;
    }
    st_event_t *e = &s_st_events[s_st_n_events];
    e->t_s = (int32_t) ((esp_timer_get_time() - s_st_start_us) / 1000000);
    e->kind = kind;
    e->a = a;
    e->after_window = !sleeptest_active();
    snprintf(e->id, sizeof(e->id), "%s", id ? id : "");
    s_st_n_events = s_st_n_events + 1;
}

void modes_debug_sleeptest_start(uint32_t minutes, uint32_t yield_ms_override, uint32_t interval_ms_override)
{
    s_st_n_events = 0;
    s_st_sleeps = s_st_wake_timer = s_st_wake_other = 0;
    s_st_asleep_us = 0;
    s_st_ncyc = 0;
    s_st_mark_us = esp_timer_get_time();
    memset(s_st_seg_us, 0, sizeof(s_st_seg_us));
    memset(s_st_seg_max_us, 0, sizeof(s_st_seg_max_us));
    s_st_start_us = esp_timer_get_time();
    s_st_until_us = s_st_start_us + (int64_t) minutes * 60 * 1000000;
    s_st_grace_until_us = 0;
    s_st_yield_ms = yield_ms_override;
    s_st_interval_ms = interval_ms_override;
    s_st_report_due = true;
    // The AT trace is hundreds of lines a minute, and a USB console with no
    // host listening (the port dies in light sleep) can stall each write;
    // that would stretch the short wake window this test is measuring.
    esp_log_level_set("WalterModem", ESP_LOG_WARN);
    ESP_LOGI(TAG,
             "sleeptest: light sleep ENABLED for %u min (yield=%u ms, interval=%u ms, "
             "0=build default). The USB log goes quiet now. Send pages; a report prints "
             "%u s after the window closes.",
             (unsigned) minutes, (unsigned) yield_ms_override, (unsigned) interval_ms_override,
             (unsigned) ST_GRACE_S);
}

// The report as text, so it can be kept in NVS: after a sleep window the USB
// port often does not re-enumerate until the pager is reset, and a reset
// would otherwise lose everything that was recorded.
static char s_st_text[1800];

static void st_appendf(size_t *n, const char *fmt, ...)
{
    if (*n >= sizeof(s_st_text) - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(s_st_text + *n, sizeof(s_st_text) - *n, fmt, ap);
    va_end(ap);
    if (w > 0) {
        *n += (size_t) w;
        if (*n > sizeof(s_st_text) - 1) {
            *n = sizeof(s_st_text) - 1;
        }
    }
}

static void sleeptest_save(void)
{
    nvs_handle_t h;
    if (nvs_open("dbg", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_str(h, "sleeprep", s_st_text);
    nvs_commit(h);
    nvs_close(h);
}

void modes_debug_sleeptest_print_saved(void)
{
    nvs_handle_t h;
    if (nvs_open("dbg", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_st_text);
    if (nvs_get_str(h, "sleeprep", s_st_text, &len) == ESP_OK && s_st_text[0]) {
        ESP_LOGI(TAG, "saved sleeptest report from before this boot:\n%s", s_st_text);
    }
    nvs_close(h);
}

void modes_debug_sleeptest_report(void)
{
    if (s_st_start_us == 0) {
        ESP_LOGI(TAG, "sleeptest: no window has been run since boot");
        modes_debug_sleeptest_print_saved();
        return;
    }
    size_t n = 0;
    int64_t now = esp_timer_get_time();
    int64_t end = (now < s_st_until_us) ? now : s_st_until_us;
    int64_t span_us = end - s_st_start_us;
    st_appendf(&n, "window %lld s%s, yield=%u ms interval=%u ms (0=build default), %u light sleeps, "
                    "asleep %lld s (%d%%), wakes: %u timer / %u other\n",
               (long long) (span_us / 1000000), (now < s_st_until_us) ? " (still open)" : "",
               (unsigned) s_st_yield_ms, (unsigned) s_st_interval_ms, (unsigned) s_st_sleeps,
               (long long) (s_st_asleep_us / 1000000),
               span_us > 0 ? (int) (s_st_asleep_us * 100 / span_us) : 0, (unsigned) s_st_wake_timer,
               (unsigned) s_st_wake_other);
    for (int i = 0; i < ST_SEG_N && s_st_sleeps > 0; i++) {
        st_appendf(&n, "  awake in %-28s avg %5lld ms/wake, max %5lld ms\n", k_st_seg_name[i],
                   (long long) (s_st_seg_us[i] / 1000 / s_st_sleeps),
                   (long long) (s_st_seg_max_us[i] / 1000));
    }
    for (uint32_t i = 0; i < s_st_ncyc; i++) {
        const st_cycle_t *c = &s_st_cyc[i];
        st_appendf(&n, "  cycle %u: net_sleep %lld ms by esp_timer, %lld ms by the RTC; awake before it %lld ms\n",
                   (unsigned) i, (long long) ((c->after_us - c->before_us) / 1000),
                   (long long) ((c->rtc_after_us - c->rtc_before_us) / 1000),
                   i ? (long long) ((c->before_us - s_st_cyc[i - 1].after_us) / 1000) : 0LL);
    }
    uint32_t ne = s_st_n_events;
    for (uint32_t i = 0; i < ne; i++) {
        const st_event_t *e = &s_st_events[i];
        switch (e->kind) {
        case 'M':
            st_appendf(&n, "  +%4d s  page %s received, %d s after the relay stamped it%s\n", (int) e->t_s,
                       e->id, (int) e->a, e->after_window ? "  (after the window closed)" : "");
            break;
        case 'D':
            st_appendf(&n, "  +%4d s  duplicate of %s (a re-publish)%s\n", (int) e->t_s, e->id,
                       e->after_window ? "  (after the window closed)" : "");
            break;
        case 'X':
            st_appendf(&n, "  +%4d s  malformed /down dropped\n", (int) e->t_s);
            break;
        case 'C':
            st_appendf(&n, "  +%4d s  MQTT session (re)connected\n", (int) e->t_s);
            break;
        case 'L':
            st_appendf(&n, "  +%4d s  MQTT session LOST (rc=%d)\n", (int) e->t_s, (int) e->a);
            break;
        default:
            break;
        }
    }
    if (ne == 0) {
        st_appendf(&n, "  no events: nothing was received during the window or the %u s after it closed\n",
                   (unsigned) ST_GRACE_S);
    }
    ESP_LOGI(TAG, "sleeptest report:\n%s", s_st_text);
    if (now >= s_st_until_us) {
        sleeptest_save(); // power effect: one NVS write, debug build only
    }
}
#endif // PAGER_DEBUG_NO_LIGHT_SLEEP

// ---------------------------------------------------------------------------
// Incoming message hook — wired to msg.c's ingest/dedup/ack state machine.
// ---------------------------------------------------------------------------

// v0.2 §6 (device-direct SMS, sms.c): factored out of handle_ingest_result()'s
// MSG_INGEST_NEW branch below so sms.c's own inbound-SMS path (an
// allow-listed sender, msg_insert_sms_in() already run) can alert exactly
// the same way a real `/down` message does, including the lock-screen rule,
// without duplicating this logic or (worse) sms.c reaching into modes.c's
// static render_pending_set()/set_mode() directly. See modes.h's own doc
// comment for the full contract.
void modes_alert_incoming(const char *id, const char *from)
{
    // Captured BEFORE set_mode(ACTIVE, ...) below flips it — this is the
    // "was the device asleep" input ui_incoming()'s steal-the-screen policy
    // needs (docs/DEVICE_PLAN.md §5.5). Unlocked read of g_rtc.mode, same as
    // several other spots in this file already do.
    bool was_asleep = (g_rtc.mode == (uint8_t) PAGER_MODE_SLEEP);
    set_mode(PAGER_MODE_ACTIVE, MODE_REASON_INCOMING_MSG);
    // F6.5 (docs/DEVICE_PLAN.md §5.8): "no `shown` is published" for a body
    // received while locked — the message is already inserted into msg.c's
    // s_thread by the caller regardless; skipping render_pending_set() here
    // is what keeps it that way, rather than relying on ui_incoming()'s own
    // screen-top check (was_asleep alone would otherwise force the "steal"
    // branch and push Chat right over the Locked screen — see
    // ui_incoming()'s `steal = was_asleep || ...`). msg_mark_all_unshown()
    // (scr_lock.c, on a successful unlock) is what eventually surfaces
    // these; for sms.c's own callers that call is a no-op for this id
    // (ack_state is already MSG_ACK_READ, never MSG_ACK_UNSHOWN) but still
    // correctly surfaces the render on unlock via ui_incoming()'s own
    // "whole thread" redraw.
    if (id && id[0] != '\0' && !lock_is_locked()) {
        render_pending_set(id, from ? from : "", was_asleep);
    }
}

static void handle_ingest_result(msg_ingest_t r, const msg_t *out, uint16_t len,
                                  const uint8_t *body)
{
    switch (r) {
    case MSG_INGEST_NEW: {
        // Copy the id/from BEFORE handing off. `out` points into msg.c's
        // s_thread ring, which thread_insert_locked() memmoves on every
        // insert; a reply submitted from modes_run()'s task while this is
        // in flight would shift s_thread[0] and make out->id the reply's
        // "u_..." id, acking a message the relay has never heard of (§4.1
        // rule 3) and leaving the real down message un-acked forever.
        char id[MSG_ID_MAX] = "";
        char from[MSG_FROM_MAX] = "";
        if (out) {
            strncpy(id, out->id, sizeof(id) - 1);
            strncpy(from, out->from, sizeof(from) - 1);
        }
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        {
            int64_t now_s = 0;
            int32_t lat = -1;
            if (out && net_get_clock(&now_s) && out->ts > 0) {
                lat = (int32_t) (now_s - out->ts);
            }
            sleeptest_note('M', lat, id);
        }
#endif
        modes_alert_incoming(id, from);
        break;
    }

    case MSG_INGEST_DUPLICATE: {
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        sleeptest_note('D', 0, out ? out->id : msg_last_ingest_id());
#endif
        // §4.1 rule 7: re-ack only, MUST NOT re-render/re-alert/re-enter
        // active mode. *out is NULL if the entry already scrolled out of
        // the 10-deep RAM thread (routine: the 16-deep dedup ring is
        // intentionally wider) - fall back to the id msg.c just parsed.
        // F6.5: while locked, a duplicate re-delivery of a body this device
        // never displayed (§5.8) must not be re-acked either — same rule as
        // the MSG_INGEST_NEW branch above, just for the relay's retry of a
        // message it never got a `shown` for the first time either.
        const char *id = out ? out->id : msg_last_ingest_id();
        if (id[0] != '\0' && !lock_is_locked()) {
            if (out && out->ack_state == MSG_ACK_READ) {
                msg_mark_read(id);
            } else {
                msg_mark_shown(id);
            }
        }
        break;
    }

    case MSG_INGEST_MALFORMED:
    default:
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        sleeptest_note('X', 0, "");
#endif
        // §3.4: log, count (already counted by whichever of
        // msg_ingest_down_cbor()/msg_count_malformed() rejected it), do not
        // ack, do not render, do not reboot.
        // v0.2 bug fix #6 (docs/V02_DESIGN.md §2.6): this was ESP_LOGD, which
        // meant a run of malformed drops was invisible unless debug logging
        // was already on — "cost an evening" on real hardware. INFO plus the
        // length and first byte gives enough to tell "truncated CBOR" from
        // "not CBOR at all" without turning on full chatter. No power/modem
        // effect: logging only.
        ESP_LOGI(TAG, "malformed down message dropped (%u bytes, first byte 0x%02x)",
                 (unsigned) len, (unsigned) ((len > 0 && body) ? body[0] : 0));
        break;
    }
}

// F3.6 (docs/PROTOCOL.md §14.3/§14.4), F6.4 (docs/DEVICE_PLAN.md H14 "CBOR
// for devices"): every device decodes CBOR now — the JSON/cJSON RX path
// that used to cover req_sig==0 devices is gone (cJSON dropped from
// CMakeLists.txt entirely). auth_verify() still only runs when ident's
// IDENT_FLAG_REQ_SIG is set; msg_ingest_down_cbor() itself only requires
// the `n` replay counter (and checks the replay window) in that same case
// — see its own doc comment in msg.h. No modem or sleep-state effect: pure
// computation on the already-received buffer.
static void on_incoming_message(const char *topic, const char *body, uint16_t len)
{
    bool req_sig = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    size_t vlen = len;

    if (req_sig) {
        // `body` points at net.cpp's own static RX buffer (s_mqtt_rx_buf):
        // mutable memory exposed through this callback's const-qualified
        // parameter. Nothing else touches it while this callback runs
        // (net.cpp's s_handler_busy guard covers exactly this window), so
        // trimming the trailing `sig` suffix in place here is safe.
        if (!auth_verify(topic, (uint8_t *) (uintptr_t) body, &vlen)) {
            msg_count_malformed();
            ESP_LOGD(TAG,
                     "signature verify failed on %s (%u bytes) - dropped, not acked (§3.4/§14.3)",
                     topic, (unsigned) len);
            return;
        }
    }

    // F6.5/v0.2 §4.4 (docs/PROTOCOL.md §3.2/§5.8, docs/V02_DESIGN.md §4.4):
    // `cfg` is not a content message — it has neither `from` nor `body`,
    // which msg_ingest_down_cbor() below requires — so it must be
    // intercepted here, before that call, exactly like auth_verify() already
    // gates what reaches it. cfg.c's cfg_ingest_cbor() decodes the envelope
    // once and dispatches whichever of `lock`/`ca` sub-maps it carries (a
    // single push can carry both) to lock.c/catrust.c respectively. Returns
    // true only for a well-formed `kind:"cfg"` envelope; false covers both
    // "not cfg" and "cfg but malformed", both of which correctly fall
    // through to the normal path below (msg.c's own MSG_INGEST_MALFORMED
    // handling covers the latter).
    if (cfg_ingest_cbor((const uint8_t *) body, (uint16_t) vlen)) {
        return;
    }

    // F7.1 (docs/DEVICE_PLAN.md §4.3, docs/PROTOCOL.md §3.2): `book` is the
    // other non-content down kind — same "neither `from` nor `body`" reason
    // `cfg` is intercepted above, before msg_ingest_down_cbor() ever sees it.
    // Returns true only for a well-formed `kind:"book"` envelope (applied +
    // acked already); false covers both "not book" and "book but
    // malformed", both of which correctly fall through to the normal path
    // below.
    if (book_ingest_cbor((const uint8_t *) body, (uint16_t) vlen)) {
        return;
    }

    // v0.2 §5 (docs/PROTOCOL.md §3.2/§13, docs/V02_DESIGN.md §5): `loc_req`
    // is the third non-content down kind — same "neither `from` nor `body`"
    // reason `cfg`/`book` are intercepted above, before
    // msg_ingest_down_cbor() ever sees it. Never a thread entry, never
    // shown/read-acked, answered regardless of lock state or mode
    // (loc_ingest_req_cbor() itself never touches ui.c/set_mode() — see
    // loc.h's own doc comment). Returns true only for a well-formed
    // `kind:"loc_req"` envelope (queued/answered/started already); false
    // covers both "not loc_req" and "loc_req but malformed", both of which
    // correctly fall through to the normal path below.
    if (loc_ingest_req_cbor((const uint8_t *) body, (uint16_t) vlen)) {
        return;
    }

    const msg_t *out = NULL;
    msg_ingest_t r = msg_ingest_down_cbor((const uint8_t *) body, (uint16_t) vlen, &out);
    handle_ingest_result(r, out, (uint16_t) vlen, (const uint8_t *) body);
}

// ---------------------------------------------------------------------------
// F1/F3: MQTT session backoff
// ---------------------------------------------------------------------------

static void schedule_backoff(uint32_t *backoff_index, int64_t *next_retry_us)
{
    uint32_t idx = *backoff_index;
    uint32_t delay_s = k_backoff_s[idx < PAGER_BACKOFF_STEPS ? idx : PAGER_BACKOFF_STEPS - 1];
    *next_retry_us = esp_timer_get_time() + (int64_t) delay_s * 1000000;
    if (idx < PAGER_BACKOFF_STEPS) {
        (*backoff_index)++;
    }
}

static void pin_to_steady_backoff(uint32_t *backoff_index, int64_t *next_retry_us)
{
    *backoff_index = PAGER_BACKOFF_STEPS; // steady 300s from here on
    *next_retry_us = esp_timer_get_time() + (int64_t) k_backoff_s[PAGER_BACKOFF_STEPS - 1] * 1000000;
}

static void handle_mqtt_loss(const net_mqtt_status_t *st, uint32_t *backoff_index,
                              int64_t *next_retry_us)
{
    switch (st->last_class) {
    case NET_MQTT_RC_PERMANENT:
        ESP_LOGI(TAG, "MQTT permanently refused (rc=%d): CONN_REFUSED/AUTH/ACL_DENIED - "
                      "steady 300s backoff, check broker credentials/ACLs",
                 st->last_rc);
        pin_to_steady_backoff(backoff_index, next_retry_us);
        break;
    case NET_MQTT_RC_TLS_FAIL: {
        // v0.2 §4.2 (docs/V02_DESIGN.md, docs/V02_DESIGN.md §4.2): a
        // pinned device gets one more validated attempt before falling back
        // to unvalidated (state -> broken) — "no CA problem may ever stop
        // pages arriving" (§0), so both of those cases use the ordinary
        // backoff schedule, not the steady 300s this branch used
        // unconditionally before. The steady 300s reasoning ("provisioning
        // bug") only still holds where there is nothing left to fall back to:
        // already unpinned, or already broken and still failing TLS.
        if (catrust_apply_in_progress()) {
            // A two-phase CA-push apply's own scratch-slot trial failing is a
            // different event from the pinned production CA breaking —
            // catrust_service() (called later this same iteration) resolves
            // it directly; conflating the two would let a bad *pushed* CA
            // incorrectly mark the still-working, currently-pinned CA broken.
            schedule_backoff(backoff_index, next_retry_us);
            ESP_LOGI(TAG, "TLS handshake failed during a CA apply trial (rc=%d) - "
                          "catrust_service() will resolve it",
                     st->last_rc);
            break;
        }
        catrust_tls_action_t action = catrust_on_mqtt_tls_fail();
        switch (action) {
        case CATRUST_TLS_RETRY_VALIDATED:
            schedule_backoff(backoff_index, next_retry_us);
            ESP_LOGI(TAG, "TLS handshake failed while pinned (rc=%d): one more validated attempt "
                          "before falling back",
                     st->last_rc);
            break;
        case CATRUST_TLS_FALL_BACK:
            schedule_backoff(backoff_index, next_retry_us);
            ESP_LOGI(TAG, "TLS handshake failed twice while pinned (rc=%d): falling back to "
                          "unvalidated MQTT (state -> broken)",
                     st->last_rc);
            break;
        case CATRUST_TLS_STEADY:
        default:
            pin_to_steady_backoff(backoff_index, next_retry_us);
            ESP_LOGI(TAG, "TLS handshake failed (rc=%d), already unpinned or broken - steady 300s "
                          "backoff, nothing left to fall back to",
                     st->last_rc);
            break;
        }
        break;
    }
    case NET_MQTT_RC_TRANSIENT:
    default:
        schedule_backoff(backoff_index, next_retry_us);
        ESP_LOGI(TAG, "MQTT session lost (rc=%d), transient - retrying with backoff", st->last_rc);
        break;
    }
    net_ack_disconnect_edge();
}

// ---------------------------------------------------------------------------
// F4: modem health check / recovery, shared with the §2.3 connect watchdog
// below (both are "the modem engine looks wedged" triggers and must share
// one rate limit, not each get their own 1-per-10-min budget).
// ---------------------------------------------------------------------------

// Records that net_session_up() was just called, so the watchdog below knows
// when to start counting; `ok` is net_session_up()'s own return value (only
// "the AT command was queued", not "it connected" -- that is exactly what
// the watchdog is for). Power effect: none of its own, bookkeeping only.
static void note_session_up_attempt(bool ok)
{
    s_connect_attempt_us = ok ? esp_timer_get_time() : 0;
}

// Shared F4 recovery action: rate-limited (1 per PAGER_MODEM_RESET_MIN_INTERVAL_US,
// "a wedged modem being reset in a loop is a battery fire") full modem reset
// via net_recover_modem(). `reason` is a human-readable trigger name for the
// log line only. Returns true if a reset was actually attempted (regardless
// of whether net_recover_modem() itself succeeded), so the caller knows
// whether to also try net_session_up() again.
static bool rate_limited_modem_recover(const char *reason)
{
    int64_t now_us = esp_timer_get_time();
    if (g_rtc.last_modem_reset_us != 0 &&
        (now_us - g_rtc.last_modem_reset_us) < PAGER_MODEM_RESET_MIN_INTERVAL_US) {
        ESP_LOGI(TAG, "%s but reset is rate-limited (last reset %lld s ago) - not resetting again yet",
                 reason, (long long) ((now_us - g_rtc.last_modem_reset_us) / 1000000));
        return false;
    }

    ESP_LOGI(TAG, "%s; resetting (F4)", reason);
    rtc_lock();
    g_rtc.modem_resets++;
    g_rtc.last_modem_reset_us = now_us;
    rtc_save();
    rtc_unlock();

    if (!net_recover_modem()) {
        ESP_LOGI(TAG, "modem recovery failed");
    }
    return true;
}

#define PAGER_NO_NETWORK_RESET_S (30u * 60u)

static void run_modem_health_check(void)
{
    watchdog_kick(WD_HEALTH);
    for (int attempt = 0; attempt < 3; attempt++) {
        if (net_check()) {
            uint32_t dark_s = net_unregistered_for_s();
            if (dark_s >= PAGER_NO_NETWORK_RESET_S && !s_loc_suppress && !s_ca_apply_suppress &&
                rate_limited_modem_recover("no network for 30 min")) {
                note_session_up_attempt(net_session_up());
            }
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    // net_check() now means "the modem answers". Out of coverage is normal and
    // resets nothing (net.cpp, "Coverage loss and recovery"). The long stop
    // below is for a modem that answers but has been unable to register for
    // so long that a radio restart is worth its cost; the 10-minute rate
    // limit still applies.
    if (rate_limited_modem_recover("modem unresponsive after 3 retries over 3s")) {
        note_session_up_attempt(net_session_up());
    }
}

// ---------------------------------------------------------------------------
// F6.2 (docs/DEVICE_PLAN.md §5.3): the button GPIO, its short/long/
// BTN_STUCK FSM, and the debounce/hold timing constants all moved to
// input.c (firmware/README.md R2's BTN_STUCK fix spec lives there now).
//
// F6.3: the button *semantics* (what short/long actually do) moved again,
// from this file's own button_action_short()/button_action_long() (now
// removed) into ui_on_button_short()/ui_on_button_long() (ui.c) — those are
// screen-stack-aware (docs/DEVICE_PLAN.md §5.5: "long = Home from
// anywhere", short = open the newest unread chat), which this file has no
// business knowing about any more. modes_run()'s event-drain loop below
// just forwards the two resolved event types.
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// F6.5 (docs/DEVICE_PLAN.md §5.8): keeps the screen stack in sync with
// lock.c's `locked` state. Runs only on modes_run()'s own task (called from
// modes_run() below), which is what makes it safe to mutate the screen stack
// here even though lock_is_locked() can flip true/false from a completely
// different task (lock_check_autolock() below, same task, is the common
// case; but lock_ingest_cfg_cbor()'s admin `clear` runs on the MQTT event
// task and must NOT touch ui_push()/ui_pop() itself for exactly that
// reason — this per-iteration poll is what picks that edge up safely
// instead). The successful-unlock-by-passcode path (scr_lock.c's on_key())
// already handles its own transition directly (same task, same iteration),
// so this function's `!locked && showing` branch only ever fires for the
// admin-clear-while-locked edge.
// ---------------------------------------------------------------------------
static void lock_screen_sync(void)
{
    bool locked = lock_is_locked();
    bool showing = (ui_top() == &g_scr_lock);
    if (locked && !showing) {
        ui_push(&g_scr_lock);
    } else if (!locked && showing) {
        ui_go_home();
    }
}

// ---------------------------------------------------------------------------
// Public entry points
// ---------------------------------------------------------------------------

void modes_boot(void)
{
    s_rtc_mutex = xSemaphoreCreateMutexStatic(&s_rtc_mutex_buf);

    bool was_valid = rtc_is_valid();
    if (was_valid) {
        // Survived a watchdog/software reset (not EN/brownout/power-cycle,
        // which lose RTC memory per PROTOCOL.md §9) - keep session_id and
        // any pending queues rather than silently dropping a student's
        // reply or ack.
        g_rtc.boot_count++;
        ESP_LOGI(TAG, "recovered RTC state across reset (boot_count=%u, session=%s)",
                 (unsigned) g_rtc.boot_count, g_rtc.session_id);
    } else {
        rtc_cold_init();
        ESP_LOGI(TAG, "cold boot, new session=%s", g_rtc.session_id);
        // docs/DEVICE_PLAN.md section 2.5: `n = (epoch << 20) | lo`. `lo` lives in RTC
        // memory, which a cold boot just lost, so it restarts at 0; the epoch
        // "increments only on a cold boot or when lo wraps". Without this
        // bump every cold boot re-issued n values the relay had already
        // accepted and the relay dropped everything as a replay -- including
        // the very first publish after setup (n=0 against the relay's initial
        // upN=0). Found live 2026-09-20. One NVS write per cold boot.
        on_auth_epoch_wrap();
    }
    rtc_save();

    ESP_LOGI(TAG, "pager_rtc_t size = %u bytes (of the 1184-byte budget left "
                  "after walter-modem's own ~7003 bytes, PROTOCOL.md §9.1)",
             (unsigned) sizeof(g_rtc));

    msg_bind_rtc(&g_rtc.msg, rtc_lock, rtc_unlock, rtc_save);
    msg_init(was_valid);

    // F3.6 (docs/PROTOCOL.md §14, §2.7): K_dev copy + the auth_rtc_t binding
    // msg.c's publish/ingest paths use. No modem or sleep-state effect:
    // ident_get_kdev() is a read of the already-loaded ident_t (main.c calls
    // ident_load() before modes_boot()), auth_init() only copies into its
    // own static storage.
    auth_init(ident_get_kdev());
    msg_bind_auth(&g_rtc.auth, on_auth_epoch_wrap);

    // F6.5 (docs/DEVICE_PLAN.md §5.8): lock.c's RTC binding + NVS load. Must
    // run before ui_init()/ui_render_boot() below so a locked device never
    // paints Home even for one frame at boot (see the ui_push() right after
    // ui_init() below). No modem or sleep-state effect: NVS reads + one
    // possible RTC write (forcing `locked` when a passcode is configured) —
    // see lock_init()'s own doc comment (lock.h) for the full reasoning.
    lock_bind_rtc(&g_rtc.lock, rtc_lock, rtc_unlock, rtc_save);
    lock_init(was_valid);

    // v0.2 §4.1 (CA trust): NVS-only, no RTC sub-struct of its own (see
    // catrust.h's own module comment) — catrust_bind() only hands over the
    // cross-task mutex (the same pattern lock_bind_rtc()/lock_init() above
    // uses their own RTC pointer for). No modem or sleep-state effect: a
    // couple of NVS reads.
    catrust_bind(rtc_lock, rtc_unlock);
    catrust_init();

    // F7.1 (docs/DEVICE_PLAN.md §4.3): book.c has no RTC sub-struct of its
    // own (book.h's module comment) — book_bind() hands it the EXISTING
    // g_rtc.auth plus the same cross-task lock/unlock/save trio and
    // epoch-wrap hook msg_bind_auth() above already uses, since a
    // `contact_req` shares the one strictly-increasing /up,/status,/loc
    // counter with every other signed publish. book_init() then loads NVS
    // namespace "book" (§4.3's one-blob storage) into book.c's own RAM
    // cache. No modem or sleep-state effect: a handful of NVS reads only.
    book_bind(&g_rtc.auth, rtc_lock, rtc_unlock, rtc_save, on_auth_epoch_wrap);
    book_init();

    input_init(); // power effect: GPIO config + static queue alloc only

    if (!ui_init()) {
        ESP_LOGI(TAG, "display init failed; continuing headless (network/replies/acks unaffected)");
    } else {
        // Boot splash (scr_greeting.c) — pushed under a possible Locked
        // screen below, so a locked device still always shows Locked first
        // (revealed once unlocked) rather than this ever bypassing it.
        scr_greeting_set_mode(GREETING_HELLO);
        ui_push(&g_scr_greeting);
        if (lock_is_locked()) {
            // §5.8: "Reached by ... any restart while a passcode is set."
            // Pushed here, before ui_render_boot()'s own forced full
            // refresh, so that refresh already paints Locked rather than
            // Home (ui_init() itself always establishes [Home] first).
            ui_push(&g_scr_lock);
        }
        ui_render_boot(); // initial Home/Locked screen; disp_init() primes the cadence counter to force a full refresh
    }

    net_set_msg_cb(on_incoming_message);

    watchdog_kick(WD_NET_INIT);
    if (!net_init()) {
        ESP_LOGI(TAG, "net_init() failed at boot; will retry from the wake loop (F1)");
        rtc_lock();
        g_rtc.attach_fail_cycles++;
        rtc_unlock();
    } else {
        rtc_lock();
        g_rtc.attach_fail_cycles = 0;
        rtc_unlock();
        bool up_ok = net_session_up();
        note_session_up_attempt(up_ok); // §2.3: arms the connect watchdog
        if (!up_ok) {
            ESP_LOGI(TAG, "net_session_up() failed at boot; will retry per F3 backoff");
        }
    }

    // v0.2 §5 (docs/V02_DESIGN.md): loc.c's RTC route-hint binding + GNSS/
    // accelerometer bring-up. Runs after ui_init() (accel.c shares the I2C
    // bus ui_init()'s i2c_kb_init() already installed — see accel.h's own
    // module comment) and after net_init() (net_gnss_config() needs the
    // modem to exist; harmless, already-logged failure either way if
    // net_init() itself failed above — location then simply always answers
    // from cache/no_fix, this task's own fail-open rule). No paging-path
    // effect either way.
    loc_bind(&g_rtc.loc, &g_rtc.auth, rtc_lock, rtc_unlock, rtc_save, on_auth_epoch_wrap);
    loc_init();

    // Owner request, 2026-09-20: coverage.c's duty-cycle policy starts
    // inactive (registered, or not-yet-known-unregistered) — a cold boot is
    // exactly the conservative default it already gives on a policy reset.
    coverage_policy_init(&s_coverage);

    // v0.2 §6 (device-direct SMS): runs after net_init() (sms_init() calls
    // net_sms_config(), which needs the modem to exist) — a modem/SIM that
    // rejects SMS setup entirely (§0/§6's own flag: a data-only SIM may not
    // carry SMS at all) logs once at INFO and disables the feature for this
    // boot; nothing here can block or fail boot. No RTC sub-struct of sms.c's
    // own (sms.h's own module comment) — sms_bind() hands over the EXISTING
    // g_rtc.auth (a signed sms_log shares the one /up,/status,/loc counter)
    // plus the shared cross-task mutex, same pattern book_bind()/loc_bind()
    // already use.
    sms_bind(&g_rtc.auth, rtc_lock, rtc_unlock, rtc_save, on_auth_epoch_wrap);
    sms_init();

    rtc_lock();
    g_rtc.mode = (uint8_t) PAGER_MODE_SLEEP; // firmware/README.md: boot in sleep mode
    rtc_save();
    rtc_unlock();

    ESP_LOGI(TAG, "boot complete, entering sleep mode");
}

// Recorded on modes_run()'s very first line below; modes_on_run_task()
// (modes.h) compares against it. RAM-only, never RTC_DATA_ATTR — a task
// handle from this boot is meaningless after a reset, and this design never
// deep sleeps anyway (see s_render_pending's own comment on that).
static TaskHandle_t s_modes_run_task = NULL;

bool modes_on_run_task(void)
{
    return s_modes_run_task != NULL && xTaskGetCurrentTaskHandle() == s_modes_run_task;
}

void modes_run(void)
{
    s_modes_run_task = xTaskGetCurrentTaskHandle();
    uint32_t backoff_index = 0;
    int64_t next_session_retry_us = 0; // 0 = retry as soon as we notice we're down

    watchdog_loop_begin();
    for (;;) {
        watchdog_kick(WD_LOOP_TOP);
        uint32_t interval_ms = (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE)
                                    ? PAGER_WAKE_INTERVAL_ACTIVE_MS
                                    : PAGER_WAKE_INTERVAL_SLEEP_MS;
        // Owner request, 2026-09-20: lengthen the sleep-mode wake interval
        // while unregistered (net_unregistered_for_s(): a plain RAM read,
        // not an AT round trip). Active mode is left alone -- a person is
        // interacting with the pager right then, regardless of coverage.
        if (g_rtc.mode != (uint8_t) PAGER_MODE_ACTIVE && net_unregistered_for_s() > 0) {
            interval_ms = PAGER_WAKE_INTERVAL_UNREGISTERED_MS;
        }
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        // Task 3: sleeptest <minutes> <yield_ms> <interval_ms> overrides the
        // wake interval for the window's duration only.
        if (sleeptest_active() && s_st_interval_ms) {
            interval_ms = s_st_interval_ms;
        }
#endif

        // F5: a non-zero MEMORY_FULL count is direct evidence the drain
        // cycle is falling behind. Shorten T defensively rather than let
        // messages keep piling up in the modem's own buffer.
        uint32_t memfull_delta = net_take_memfull_delta();
        if (memfull_delta > 0) {
            rtc_lock();
            g_rtc.mqtt_memfull_count += memfull_delta;
            uint32_t cumulative = g_rtc.mqtt_memfull_count;
            rtc_unlock();
            ESP_LOGI(TAG, "MQTT MEMORY_FULL x%u this cycle (cumulative %u) - shortening wake interval",
                     (unsigned) memfull_delta, (unsigned) cumulative);
            interval_ms = interval_ms / 2;
            if (interval_ms < 1000) {
                interval_ms = 1000;
            }
        }

        uint32_t oversize_delta = net_take_oversize_delta();
        if (oversize_delta > 0) {
            rtc_lock();
            g_rtc.oversize_drop_count += oversize_delta;
            rtc_unlock();
            ESP_LOGI(TAG, "%u oversize MQTT payload(s) dropped this cycle (F6)",
                     (unsigned) oversize_delta);
        }

        // Skip net_sleep() entirely whenever input_button_busy() (a held
        // button must not re-enter a level-triggered light sleep it would
        // just immediately exit again), OR input_button_stuck() (same
        // reason, still true for BTN_STUCK - net_sleep() (net.cpp) arms
        // ext0 wake at level 0, so it would return immediately over and
        // over for as long as the button stays down; fixing that needs the
        // net_sleep() ext0-level-1 variant firmware/README.md R2 specifies,
        // which touches net.cpp and is out of this task's Files list - see
        // input.h's input_button_stuck() doc comment), OR input_awake()
        // (docs/DEVICE_PLAN.md §5.3's 30s UI-awake window, armed by the
        // last key/button event - F6.3 dropped the separate "composer
        // open" carve-out the pre-F6.3 code had here: every screen's text
        // entry now keeps this window armed via input_feed_key() on each
        // keystroke, same as button events already do, so input_awake()
        // alone covers it), OR net_modem_busy(). On that last one: the
        // MQTT event handler runs at priority 4 against this task's
        // priority 1, so it hands the CPU back here every time it blocks;
        // without this term net_sleep() deasserts RTS in the middle of the
        // modem's response to mqttReceive() (see net.h). Costs ~1.5s of
        // 40mA busy-polling per incoming message (~0.02 mAh, ~0.4 mAh/day
        // at 20 msgs/day, estimate) and buys back a per-message
        // message-loss window. OR (v0.2 M2, 22 Sep outage) net_connect_in_flight():
        // the outage's own root cause was issuing a CONNECT and light-sleeping
        // 110ms later, deasserting RTS while the TLS handshake/CONNACK was
        // still in flight, which is how the CONNECTED event got lost forever.
        // Bounded by M1's own 30s connect timeout (net_connect_guard.h), so a
        // lost CONNACK cannot pin the device awake indefinitely -- see
        // net_connect_in_flight()'s own doc comment (net.h) for why this
        // feeds skip_sleep only, not pump_blocked.
        bool btn_busy = input_button_busy();
        bool btn_stuck = input_button_stuck();
        bool ui_awake = input_awake();
        bool skip_sleep = btn_busy || btn_stuck || ui_awake || net_modem_busy() || net_connect_in_flight();
        // pump_blocked keys ONLY on net_modem_busy() (the UART/RTS interlock
        // against the MQTT event handler, see the comment above on
        // net_modem_busy()) -- NOT on btn_busy/btn_stuck/ui_awake, and NOT on
        // net_connect_in_flight() either (v0.2 M2): those four are legitimate
        // reasons to skip net_sleep(), but none of them are reasons to
        // withhold a reply/ack publish. For net_connect_in_flight() specifically:
        // the session is not up yet during that window anyway, so there is
        // nothing meaningful for msg_pump() to publish against it (a stale
        // attempt just fails cheaply, same as it does today for any other
        // disconnected stretch) -- withholding it would only delay a publish
        // that becomes possible again the moment CONNECTED/SUBSCRIBED lands,
        // for no benefit. A 30s input_awake() window after every keystroke
        // used to gate msg_pump() off entirely, which is what let a typed
        // reply sit unsent for up to 116s waiting for that window (and the
        // UI-awake busy-poll cadence) to expire. See the rate limit at the
        // msg_pump() call site below for how the resulting faster cadence is
        // kept bounded.
        bool pump_blocked = net_modem_busy();
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        // Debug builds only (see main/CMakeLists.txt): behave as if the UI
        // were permanently awake -- no light sleep, RTS held asserted, log alive.
        // pump_blocked deliberately keeps the pre-override value: forcing it
        // true too meant msg_pump() never ran, so this build never published
        // a single ack and the web app sat on "sent" forever (found live).
        // ... except inside a `sleeptest` window, where the real decision stands.
        if (!sleeptest_active()) {
            skip_sleep = true;
            if (s_st_until_us != 0 && s_st_grace_until_us == 0) {
                // The window just closed. Task 3: stay fully awake (no more
                // light sleep - skip_sleep is already true above) for
                // ST_GRACE_S instead of restarting immediately, so a URC the
                // modem was still holding has time to arrive and be
                // processed, and msg_pump() below (still called every loop,
                // ~100ms cadence, pump_blocked keeps its pre-override value)
                // has time to publish its ack before the restart that
                // recovers the USB port. Turn the AT trace back on now,
                // not just before the report, so this catch-up is visible
                // live too.
                s_st_grace_until_us = esp_timer_get_time() + (int64_t) ST_GRACE_S * 1000000;
                esp_log_level_set("WalterModem", ESP_LOG_DEBUG);
            }
            if (s_st_report_due && s_st_grace_until_us != 0 && esp_timer_get_time() >= s_st_grace_until_us) {
                s_st_report_due = false;
                vTaskDelay(pdMS_TO_TICKS(4000)); // let the host re-enumerate the USB port
                modes_debug_sleeptest_report();
                // Proof of life that does not depend on USB: the relay logs this webhook.
                modes_publish_status_now();
                // The USB port often does not re-enumerate after light sleep.
                // A restart always brings it back, and the report (saved to
                // NVS above) is printed at boot. Unattended testing needs this.
                vTaskDelay(pdMS_TO_TICKS(3000));
                watchdog_hard_reset(); // not esp_restart(): that leaves a dead USB port dead
            }
        }
#endif
        if (!skip_sleep) {
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
            int64_t st_t0 = esp_timer_get_time();
            int64_t st_rtc0 = (int64_t) esp_clk_rtc_time();
#endif
            watchdog_kick(WD_SLEEP_ENTER);
            net_sleep(interval_ms);
            watchdog_kick(WD_SLEEP_EXIT);
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
            if (s_st_ncyc < ST_CYC_MAX) {
                s_st_cyc[s_st_ncyc].before_us = st_t0;
                s_st_cyc[s_st_ncyc].after_us = esp_timer_get_time();
                s_st_cyc[s_st_ncyc].rtc_before_us = st_rtc0;
                s_st_cyc[s_st_ncyc].rtc_after_us = (int64_t) esp_clk_rtc_time();
                s_st_ncyc++;
            }
            s_st_asleep_us += esp_timer_get_time() - st_t0;
            ST_MARK_BEGIN();
            s_st_sleeps++;
            if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER) {
                s_st_wake_timer++;
            } else {
                s_st_wake_other++;
            }
#endif
            // L4/F7: the event task ticks at 10ms + settles for 10ms; give
            // it >=30ms of awake time before looking at anything it may
            // have produced.
            uint32_t yield_ms = PAGER_POST_WAKE_YIELD_MS;
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
            // Task 3: sleeptest <minutes> <yield_ms> overrides this for the
            // window's duration only, to find the minimum stay-awake time
            // that lets a held URC actually reach the ESP32.
            if (sleeptest_active() && s_st_yield_ms) {
                yield_ms = s_st_yield_ms;
            }
#endif
            vTaskDelay(pdMS_TO_TICKS(yield_ms));
            assert(yield_ms >= 30); // F7, debug builds only
        } else if (btn_busy) {
            vTaskDelay(pdMS_TO_TICKS(PAGER_BTN_POLL_MS)); // button FSM debounce/timing granularity
        } else if (btn_stuck) {
            // BTN_STUCK: nothing left to time-measure (no more short/long
            // resolution while held), so the fine 20ms cadence buys
            // nothing - poll at the sleep-mode wake interval's own
            // granularity instead, cheaper than PAGER_BTN_POLL_MS without
            // touching net_sleep() (see the skip_sleep comment above).
            vTaskDelay(pdMS_TO_TICKS(PAGER_WAKE_INTERVAL_SLEEP_MS));
        } else {
            // ui_awake (or a busy/stuck button): docs/DEVICE_PLAN.md §5.3,
            // CardKB polled at 100ms, no light-sleep.
            vTaskDelay(pdMS_TO_TICKS(100));
        }

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        ST_MARK(0);
#endif
        watchdog_kick(WD_INPUT_UI);
        input_poll(); // power effect: one GPIO read (button FSM step) - see input.h

        // F6.3: CardKB read moved to ui.c (ui_poll_keyboard(), see its own
        // doc comment) - polled here, before the event-drain loop below, so
        // any key it decodes this same iteration is available to drain
        // immediately rather than waiting one more iteration. Gated on
        // input_awake() (not skip_sleep/btn_busy/btn_stuck): there is no
        // screen to type into unless the UI is awake, and reading I2C while
        // asleep would cost a transaction for nothing.
        bool ui_awake_now = input_awake();
        // Bench finding (21 Sep): with no button wired, a keystroke is the only
        // way to wake the UI, and the keyboard was read only while awake. Poll
        // it on every loop iteration instead: awake, that is the 100 ms
        // cadence as before; asleep, one I2C read per wake-and-drain cycle
        // (~100 us every 5 s), so a key pressed while "sleeping" wakes the UI
        // within one cycle if the CardKB holds it until read (README M13).
        ui_poll_keyboard(); // power effect: one I2C read - see ui.h

        // Set when this iteration drains a button event: button events keep
        // their existing immediate render (see the render decision block
        // below) - only INPUT_EVT_KEY goes through key_render_note()'s
        // deadline instead.
        bool btn_event_this_iter = false;

        input_event_t ievt;
        while (input_get_event(&ievt)) {
            modes_note_activity(); // any resolved key/button event counts as activity
            // F6.5 (docs/DEVICE_PLAN.md §5.8): "lock_check_autolock(now)
            // called on every input event". No modem effect; an RTC write
            // only on the (rare) edge that actually locks.
            lock_check_autolock(esp_timer_get_time());
            switch (ievt.type) {
            case INPUT_EVT_BTN_DOWN:
                // firmware/README.md: button press enters active mode.
                set_mode(PAGER_MODE_ACTIVE, MODE_REASON_BUTTON);
                btn_event_this_iter = true;
                break;
            case INPUT_EVT_BTN_SHORT:
                ui_on_button_short(); // docs/DEVICE_PLAN.md §5.5 (ui.c)
                btn_event_this_iter = true;
                break;
            case INPUT_EVT_BTN_LONG:
                ui_on_button_long();
                btn_event_this_iter = true;
                break;
            case INPUT_EVT_KEY:
                ui_dispatch_key(ievt.key); // routed to the top screen's on_key() (ui.c)
                // Coalesce: defer the render instead of letting the
                // unconditional call below fire this same iteration - see
                // key_render_note()'s own comment.
                key_render_note(esp_timer_get_time());
                break;
            }
        }

        // F6.3 (docs/DEVICE_PLAN.md §5.4/§5.7): one AT round trip each for
        // rssi/batt, only on the sleep->awake edge - see
        // ui_wake_status_refresh()'s own comment for why there is no
        // separate hourly timer here. The awake->sleep edge is the "UI-awake
        // window lapses" moment §5.4 defers the cadence's full refresh to
        // (ui_on_awake_lapse(), ui.c/ui.h's own comment) - the two edges are
        // detected together since both compare against the same
        // s_ui_awake_prev.
        bool ui_awake_edge_in = ui_awake_now && !s_ui_awake_prev;
        bool ui_awake_edge_out = !ui_awake_now && s_ui_awake_prev;
        s_ui_awake_prev = ui_awake_now;
        if (ui_awake_edge_in) {
            ui_wake_status_refresh();
            // F6.5: "...and every UI wake" — the other half of
            // lock_check_autolock()'s call-site contract (docs/DEVICE_PLAN.md
            // §5.8), covering a UI wake with no fresh input event (should not
            // normally happen — input.c's own window arms on the same events
            // that got us here — but keeps the check honest either way).
            lock_check_autolock(esp_timer_get_time());
        }

        // F6.3/README R5: renders (if a message arrived) on this task, then
        // marks `shown` - see service_render_pending()'s own comment.
        watchdog_kick(WD_RENDER);
        service_render_pending();

        // F6.5: pushes/pops the Locked screen to match lock.c's current
        // state (see lock_screen_sync()'s own comment), and drains any
        // toast lock.c queued from the MQTT event task (currently only the
        // admin `cfg` `clear` toast) — both before this iteration's own
        // render below, same discipline service_render_pending() already
        // uses for cross-task handoffs.
        lock_screen_sync();
        char lock_toast[40];
        if (lock_take_toast(lock_toast, sizeof(lock_toast))) {
            ui_show_toast(lock_toast);
        }

        // Owner decision, 22 Sep evening ("stay on chat unless it's
        // explicitly locked"): the awake->asleep edge used to push
        // scr_greeting.c's "sleeping" mode over whatever screen was open
        // (the chat, typically), replacing it until the next keystroke.
        // That push (and its matching asleep->awake pop) is deliberately
        // gone now — the top screen simply stays put across the edge.
        // lock_screen_sync() above is unchanged and still covers the
        // locked case (Locked always wins over whatever was open);
        // ui_on_awake_lapse() below is still called at this same edge and
        // is still where the cadence-driven full refresh lands (ui.c's own
        // comment). scr_greeting.c itself is untouched and still used for
        // the boot-time HELLO splash (modes_boot()).

        // Coalesce key-triggered renders (bug fix, see s_key_render_pending's
        // own comment): a button event this iteration always renders
        // immediately, same as before this fix (and pays off any
        // outstanding key debounce too - the render it triggers covers
        // whatever the keys already did to the framebuffer); otherwise this
        // iteration's render only proceeds once key_render_due() says the
        // 250ms/1000ms-capped deadline has passed (or there was never a key
        // debounce outstanding at all - the ordinary idle-iteration case,
        // unchanged). Both only consulted while ui_awake_now, matching this
        // block's pre-fix "only while awake" gating.
        bool render_now = false;
        if (ui_awake_now) {
            if (btn_event_this_iter) {
                render_now = true;
                s_key_render_pending = false;
            } else {
                render_now = key_render_due(esp_timer_get_time());
            }
        }
        if (render_now) {
            // The screen stack's own render, reflecting whatever
            // ui_dispatch_key()/ui_on_button_*() above just did. Always a
            // partial (never the cadence's full refresh - see ui_render()'s
            // doc comment) and cheap when nothing changed
            // (disp_partial_refresh()'s own "no-op if nothing changed"
            // contract).
            ui_render();
        } else if (ui_awake_edge_out) {
            // docs/DEVICE_PLAN.md §5.4: the moment the UI-awake window
            // lapses is where a due full refresh is allowed to land.
            ui_on_awake_lapse();
        }

        rtc_lock();
        g_rtc.wake_cycle_count++;
        uint32_t wake_cycle_count = g_rtc.wake_cycle_count;
        rtc_unlock();

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        ST_MARK(1);
#endif
        watchdog_kick(WD_MQTT);
        net_mqtt_status_t st;
        net_get_mqtt_status(&st);

        // Owner request, 2026-09-20: coverage.c's duty-cycle policy, one step
        // per iteration (same "never more than one small step per call"
        // discipline loc_service()/catrust_service() use). Deliberately
        // called BEFORE net_take_registered_edge() below so coverage_step()
        // observes the same "just registered" transition and can latch how
        // long the pager was dark (coverage_last_dark_s()) before its own
        // internal bookkeeping resets.
        coverage_action_t cov_action =
            coverage_step(&s_coverage, esp_timer_get_time(), net_unregistered_for_s() == 0,
                          loc_attempt_in_progress());
        switch (cov_action) {
        case COVERAGE_ACTION_ENTER_OFF:
            ESP_LOGI(TAG,
                     "coverage: no network for %u s - radio off for %u s (step %u) to save "
                     "battery (power effect: LTE radio off, no MQTT connect attempts until the "
                     "next search window)",
                     (unsigned) net_unregistered_for_s(), (unsigned) s_coverage.off_period_s,
                     (unsigned) coverage_off_step_index(&s_coverage));
            if (st.mqtt_connected) {
                net_session_down();
                net_get_mqtt_status(&st);
            }
            if (net_radio_off()) {
                s_coverage_owns_radio = true;
            } else {
                ESP_LOGI(TAG, "coverage: net_radio_off() failed - staying in normal (registered-"
                              "search) mode this cycle, will retry next wake");
            }
            break;
        case COVERAGE_ACTION_ENTER_SEARCH:
            ESP_LOGI(TAG,
                     "coverage: radio back on, searching up to %u s before going dark again "
                     "(power effect: LTE radio FULL, normal MQTT reconnect resumes)",
                     (unsigned) COVERAGE_SEARCH_S);
            s_coverage_owns_radio = false;
            if (!net_radio_on()) {
                ESP_LOGI(TAG, "coverage: net_radio_on() failed - will retry next wake");
            }
            break;
        case COVERAGE_ACTION_NONE:
        default:
            break;
        }

        if (net_take_registered_edge() && !s_loc_suppress && !s_ca_apply_suppress) {
            // Coverage is back. Retry at once instead of waiting out a backoff
            // that grew while there was no network. And a session that was
            // "connected" across the gap cannot be trusted: on this modem the
            // TCP connection under it can be dead with no event until the MQTT
            // keepalive expires, up to 45 minutes of silently missed pages
            // (Sequans forum thread 400). Tear it down and reconnect: one TLS
            // handshake (~5 kB) per coverage loss.
            ESP_LOGI(TAG, "network coverage regained - dark for %u s - reconnecting the MQTT session now",
                     (unsigned) coverage_last_dark_s(&s_coverage));
            if (st.mqtt_connected) {
                net_session_down();
                net_get_mqtt_status(&st);
            }
            backoff_index = 0;
            next_session_retry_us = 0;
        }

        if ((st.mqtt_connected && !s_was_mqtt_connected) || st.session_restart_edge) {
            // v0.2 §9.4 step 4 / §9.5: a session_restart_edge is a
            // modem-initiated silent resume (§9.1 item 2) that
            // net_service_session()'s raw re-SUBSCRIBE has just repaired --
            // mqtt_connected never went false for it, so it needs its own
            // trigger into this same re-announce block (the relay never saw
            // a disconnect either, so it never re-published anything on its
            // own - §9.1 item 3/4).
            if (st.session_restart_edge) {
                net_ack_session_restart_edge();
            }
            // v0.2 §9.5/§7 key 50: a new MQTT session within this boot,
            // whether a fresh connect or a repaired silent resume -- bump
            // before publish_status_online() below so the session's first
            // online status already carries the new `link` value. No modem/
            // sleep-state effect: a RAM counter increment only.
            s_mqtt_link_counter++;
            // Edge: session just became usable. §5.4a - drives the relay's
            // re-publish of unacked messages (§5.3).
            // v0.2 §4.2: clears the TLS-fail retry streak and, if this was a
            // validated reconnect attempted while broken, heals state back
            // to pinned. No modem/sleep-state effect: RAM/NVS bookkeeping
            // only (ident_set_tls_broken() is a single NVS write, at most).
            // MUST run before the status publish below: found on hardware,
            // the other order reported `tls:"broken"` 70 ms before healing to
            // pinned, so the relay logged SECURITY tls-broken for a session
            // that had just validated, and kept showing it until the next
            // heartbeat an hour later.
            catrust_on_mqtt_connected();
            publish_status_online();
            // This task (V02_DESIGN.md §5 / PROTOCOL.md §13.3 item 2, "the
            // device always answers a loc_req it accepted"): a GNSS attempt
            // that finished while the session was down (route 2's CFUN=4
            // window losing the race with re-attach, found on hardware --
            // build/bench-logs/07-locreq-cont.log's "/loc publish failed")
            // leaves loc.c holding one queued answer instead of dropping it.
            // Flush it now that the session is usable again -- after the two
            // calls above, same ordering reason catrust_on_mqtt_connected()'s
            // own comment gives (publish the state that is actually current).
            loc_flush_pending_answer();
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
            sleeptest_note('C', 0, "");
#endif
        }
        s_was_mqtt_connected = st.mqtt_connected;

        if (st.mqtt_connected || st.disconnect_edge) {
            // v0.2 bug fix #3 (docs/V02_DESIGN.md §2.3): a real CONNECTED or
            // DISCONNECTED event proves the engine answered at all, i.e. it
            // is not the "silently wedged" failure the watchdog below
            // exists for - clear it.
            s_connect_attempt_us = 0;
            s_connect_watchdog_count = 0;
        }

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        if (st.disconnect_edge) {
            sleeptest_note('L', st.last_rc, "");
        }
#endif
        if (s_coverage_owns_radio) {
            // Owner request, 2026-09-20: the duty cycle deliberately owns the
            // radio right now (NO_RF) — no MQTT connect attempts at all, not
            // even logging one per retry, and no backoff bookkeeping. Just
            // silently absorb whatever async DISCONNECTED our own
            // net_session_down() above produced (net_ack_disconnect_edge()
            // is otherwise only called from inside handle_mqtt_loss(), which
            // this branch deliberately never reaches).
            if (st.disconnect_edge) {
                net_ack_disconnect_edge();
            }
        } else if (st.disconnect_edge) {
            handle_mqtt_loss(&st, &backoff_index, &next_session_retry_us);
        } else if (!st.mqtt_connected && s_connect_attempt_us != 0 &&
                   (esp_timer_get_time() - s_connect_attempt_us) >= PAGER_CONNECT_WATCHDOG_US) {
            // v0.2 bug fix #3 (docs/V02_DESIGN.md §2.3): mqttConnect() has
            // produced neither CONNECTED nor DISCONNECTED within 60s - the
            // "wedge silently" failure mode GOTCHAS.md's forum-thread
            // note warns this engine has. Left alone, the branch below would
            // just re-issue mqttConnect() every wake cycle with no backoff
            // at all (net_session_up() returning true only means the AT
            // command was queued, never that it actually connected) -
            // hammering the modem. Force a clean disconnect, count it as a
            // transient failure so F1/F3 backoff applies, and escalate to a
            // full modem reset after 3 in a row.
            s_connect_attempt_us = 0;
            s_connect_watchdog_count++;
            ESP_LOGI(TAG,
                     "MQTT connect watchdog: no CONNECTED/DISCONNECTED within 60s (count=%u) - "
                     "disconnecting and backing off",
                     (unsigned) s_connect_watchdog_count);
            net_session_down(); // power effect: one AT command (mqttDisconnect), no RRC of its own
            if (s_connect_watchdog_count >= 3) {
                s_connect_watchdog_count = 0;
                if (rate_limited_modem_recover("MQTT connect watchdog fired 3x in a row")) {
                    note_session_up_attempt(net_session_up());
                }
            } else {
                schedule_backoff(&backoff_index, &next_session_retry_us);
            }
        } else if (!st.mqtt_connected) {
            // v0.2 §5/§4.4: skip entirely while location's route 2 (CFUN=4
            // window) or a CA two-phase apply's own scratch-slot reconnect
            // trial owns the session on purpose — see
            // modes_set_loc_suppress()'s/modes_set_ca_apply_suppress()'s own
            // doc comments. Once either releases its flag,
            // next_session_retry_us is normally already in the past (the
            // session was healthy, backoff_index==0, right up until it was
            // torn down), so this reconnects on the very next iteration with
            // no special-casing needed here.
            //
            // v0.2 bug fix, 22 Sep evening (phaseO-recover2.log lines 36-49):
            // net_session_up() returning true only means mqttConnect() was
            // QUEUED, not connected. Without the net_connect_in_flight()
            // term below, this branch re-entered on the very next loop
            // iteration (110ms later, before st.mqtt_connected could
            // possibly have gone true yet) and issued a second
            // AT+SQNSMQTTCONNECT while the first was still outstanding,
            // which the modem refused with +CME ERROR: 4.
            // net_connect_in_flight() (net.cpp's connect guard, M1/M2) stays
            // true until CONNECTED/SUBSCRIBED arrives or M1's 30s timeout
            // fires, so this branch simply does not run again until one of
            // those happens.
            if (!s_loc_suppress && !s_ca_apply_suppress && esp_timer_get_time() >= next_session_retry_us &&
                !net_connect_in_flight()) {
                // v0.2 §4.2: no-op unless currently `broken` — decides
                // validated vs. unvalidated for this attempt (the
                // cold-boot/24h revalidation window) and reconfigures
                // profile 2 accordingly before the connect below.
                catrust_before_reconnect();
                ESP_LOGI(TAG, "retrying MQTT session (backoff idx=%u)", (unsigned) backoff_index);
                bool up_ok = net_session_up();
                note_session_up_attempt(up_ok); // §2.3: arms the connect watchdog
                if (!up_ok) {
                    // v0.2 M3 (phaseO-recover.log): net_session_up() itself
                    // can keep failing at mqttConfig()/mqttConnect() if the
                    // modem's MQTT client is stuck up. net.cpp's own
                    // net_session_down() calls (net_service_session()'s
                    // liveness-dead / M1 connect-timeout branches, and the
                    // defensive disconnect net_session_up() now does on its
                    // own configure/connect failure) are the normal fix; if
                    // three attempts in a row still fail here regardless,
                    // treat it the same as the existing 60s connect
                    // watchdog's own 3x escalation and reach for a full
                    // modem reset rather than backing off forever.
                    if (net_connect_fail_streak_maxed() &&
                        rate_limited_modem_recover(
                            "net_session_up() failed 3x in a row (modem MQTT client possibly stuck)")) {
                        note_session_up_attempt(net_session_up());
                    } else {
                        schedule_backoff(&backoff_index, &next_session_retry_us);
                    }
                }
            }
        } else {
            // Healthy connection: reset backoff so a future loss starts
            // from 5s again rather than wherever it left off.
            backoff_index = 0;
        }

        // Part B: at most one publish per pump call, only while connected
        // and not net_modem_busy() (pump_blocked, see its doc comment -- no
        // longer gated on the composer/button FSM's ui_awake/btn_busy/
        // btn_stuck), and rate-limited to PAGER_PUMP_MIN_INTERVAL_US so the
        // 100ms UI-awake busy-poll cadence can't turn into an AT transaction
        // every 100ms - PROTOCOL.md §9.5's rationale against turning a 50ms
        // wake into a multi-second one.
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        ST_MARK(2);
#endif
        // v0.2 §9.4: idle-uplink liveness ping / silent-resume repair, once
        // per wake-and-drain iteration. Same three suppressions the
        // reconnect path above already honours (modes_set_loc_suppress()'s/
        // modes_set_ca_apply_suppress()'s own doc comments, and the coverage
        // duty-cycle reasoning at s_coverage_owns_radio above): none of them
        // want an extra AT transaction landing while they deliberately own
        // the radio/session.
        if (!s_coverage_owns_radio && !s_loc_suppress && !s_ca_apply_suppress) {
            net_service_session();
        }

        watchdog_kick(WD_PUMP);
        if (!pump_blocked && st.mqtt_connected && esp_timer_get_time() >= s_next_pump_us) {
            msg_pump();
            s_next_pump_us = esp_timer_get_time() + PAGER_PUMP_MIN_INTERVAL_US;
        }

        // F4: checkComm() every 60 wake cycles in sleep mode. v0.2 §5/§4.4:
        // skipped during location's route-2 window or a CA apply's own
        // reconnect trial (see modes_set_loc_suppress()'s/
        // modes_set_ca_apply_suppress()'s own doc comments) — net_check()
        // would read NO_RF, or a mid-trial disconnected state, as "modem
        // unresponsive" and force a real, unwanted modem reset. Same
        // reasoning for s_coverage_owns_radio (owner request, 2026-09-20):
        // NO_RF is deliberate here too, and the 30-minute "no network" reset
        // this health check owns must not fire while the duty cycle is the
        // one keeping it dark on purpose.
        if (!s_loc_suppress && !s_ca_apply_suppress && !s_coverage_owns_radio &&
            g_rtc.mode == (uint8_t) PAGER_MODE_SLEEP &&
            (wake_cycle_count % PAGER_CHECKCOMM_EVERY_N_WAKES) == 0) {
            run_modem_health_check();
        }

        // v0.2 §5: accelerometer poll (unconditional, same discipline
        // input_poll() uses — a no-op read if accel_init() never found the
        // chip) and one GNSS attempt-state-machine step (a no-op if no
        // attempt is in progress). Neither blocks for more than one small,
        // bounded piece of work — see accel.h/loc.h's own doc comments.
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        ST_MARK(3);
#endif
        watchdog_kick(WD_LOC);
        accel_poll();
        loc_service();

        // v0.2 §6 (device-direct SMS, sms.c): one bounded step of the
        // boot-drain scan / `+CMTI` drain / pending-send state machine (a
        // no-op read if nothing is pending), same "never the whole thing in
        // one call, unconditional every iteration" discipline as
        // accel_poll()/loc_service() above. Deliberately NOT gated on
        // `st.mqtt_connected`/pump_blocked: docs/V02_DESIGN.md §6's whole
        // point is that SMS send/receive works without the relay — only the
        // sms_log audit publish (msg_pump()'s own tail call into sms.c,
        // above) needs the MQTT session up.
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        ST_MARK(4);
#endif
        watchdog_kick(WD_SMS);
        sms_service();

        // v0.2 §4.4: one step of the pending cfg.ca request / two-phase
        // apply state machine (a no-op read if nothing is pending or in
        // progress) — same "never the whole thing in one call" discipline
        // loc_service() documents. `st` is THIS iteration's own snapshot
        // (see catrust_service()'s own doc comment for why it must be the
        // same one handle_mqtt_loss()/the reconnect branch above already
        // saw, not a fresh net_get_mqtt_status() call).
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        ST_MARK(5);
#endif
        watchdog_kick(WD_CATRUST);
        catrust_service(&st);

        maybe_publish_heartbeat();

        // Wake source #4: active -> sleep after 10 min idle. Part A bug #1
        // fix: compare against the monotonic deadline, not wall-clock
        // epoch (see active_until_us's field comment above).
        if (g_rtc.mode == (uint8_t) PAGER_MODE_ACTIVE) {
            if (esp_timer_get_time() >= g_rtc.active_until_us) {
                set_mode(PAGER_MODE_SLEEP, MODE_REASON_IDLE_TIMEOUT);
            }
        }

        watchdog_kick(WD_SAVE);
        rtc_lock();
        rtc_save();
        rtc_unlock();
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
        ST_MARK(6);
#endif
    }
}

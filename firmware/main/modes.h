/* modes.h — mode state machine + RTC memory contract entry points.
 *
 * modes.c owns the RTC struct (docs/PROTOCOL.md §9), the single set_mode()
 * transition funnel (§11), the button short/long press state machine
 * the wake-and-drain loop (§8).
 */
#ifndef MODES_H
#define MODES_H

#include <stdbool.h>
#include <stdint.h>

/* One-time entry point. Call exactly once from app_main(), before
 * modes_run(). Validates/initialises the RTC struct, brings up the modem
 * and MQTT session (net_init()/net_session_up()), initialises msg.c/ui.c,
 * and leaves the device in sleep mode (firmware/README.md: "Boot in sleep
 * mode"). */
void modes_boot(void);

/* The wake-and-drain loop (PROTOCOL.md §8). Never returns. */
void modes_run(void) __attribute__((noreturn));

/* True iff the calling task is modes_run()'s own task — the task every
 * disp_*_refresh() entry point runs on (ui_render()/ui_on_awake_lapse()/
 * service_render_pending(), all called only from modes_run()'s loop, see
 * their own comments). False before modes_run() has recorded its task
 * handle (e.g. during modes_boot()'s own disp_refresh_cadence() call, on
 * app_main()'s task, before modes_run() starts — no keyboard-loss risk
 * there since nothing is typed yet). Used by disp.h's disp_busy_idle_hook()
 * strong definition (ui.c) so a refresh's BUSY wait only ever touches the
 * CardKB's I2C bus from the task that owns it. */
bool modes_on_run_task(void);

/* Unconditionally bumps the active-mode idle deadline (Part A bug #2: the
 * deadline must be refreshed by activity while already active, not only set
 * once on the sleep->active edge). No-op while in sleep mode. Called from
 * modes_run()'s input-event drain loop for every resolved input.c event
 * (F6.3: this replaced the old per-keypress ui_poll_keys() call site — see
 * input.h's own event queue). */
void modes_note_activity(void);

/* ---------------------------------------------------------------------
 * F6.3 (docs/DEVICE_PLAN.md §5.4/§5.5, §5.7): read-only getters for the
 * status bar and the Device screen. modes.c remains the sole owner of the
 * *when-to-read* policy for the AT-call-backed ones (rssi/batt — see
 * modes.c's ui_wake_status_refresh(), triggered on the input_awake() edge
 * and folded into the existing heartbeat cadence, not called from ui.c
 * itself) — these getters only ever return the already-cached value, no
 * new AT round trip of their own. All are plain reads of already-resident
 * state: no modem or sleep-state effect.
 * --------------------------------------------------------------------- */
int modes_get_rssi_dbm(void);
int modes_get_batt_mv(void);

/* TASK_clock.md: true while the attentive window is open (a key, button, or
 * ext0/ext1 wake within PAGER_ATTENTIVE_S = 120s of "now") - the status
 * bar's "in use" test for its live HH:MM clock (ui.c's draw_status_bar()/
 * ui_clock_due()). Deliberately the wider 120s window, not input.c's 30s
 * input_awake() UI-awake window the render cadence itself gates off of -
 * see this function's own definition (modes.c) for why that is safe (the
 * rail hold task keeps the display powered for the whole 120s). Plain RAM
 * read: no modem or sleep-state effect. */
bool modes_in_use(void);

/* True once at least one good AT+SQNVMON reading has been taken this boot.
 * modes_get_batt_mv() returns a hardcoded 3300 mV placeholder before that
 * (or if every reading since boot has been out of range) purely so the UI
 * battery icon and the /status `batt_mv` field (PROTOCOL.md §5.1's
 * [2000,4500] range requirement) always have *something* numeric to show —
 * it is NOT a real reading. loc.c's battery floor (V02_DESIGN.md §5, exactly
 * 3300 mV) must not treat that placeholder as a real "at the floor" reading:
 * callers that gate on the battery floor must check this first (this task,
 * found on hardware bench-logs/08-locreq2.log logging "batt=3300mV" while on
 * USB power with AT+SQNVMON returning an out-of-range value). */
bool modes_batt_mv_known(void);
const char *modes_get_fw_version(void);
const char *modes_get_session_id(void);
uint32_t modes_get_memfull_count(void);
uint32_t modes_get_oversize_drop_count(void);
uint32_t modes_get_modem_resets(void);

/* Device screen's "Re-sync address book" (docs/DEVICE_PLAN.md §5.5): "publishes
 * a /status now (it carries bv), which is the sync trigger." Returns false
 * (no publish attempted) if the MQTT session is not currently connected.
 * Power effect: one MQTT publish over the already-open session — see
 * publish_status_online()'s own comment for the breakdown; no RRC of its
 * own beyond what net_publish_raw() already costs when the modem was idle. */
bool modes_publish_status_now(void);

/* Debug build only (PAGER_DEBUG_NO_LIGHT_SLEEP): open a timed window in which
 * the pager really light-sleeps, and print what was received during it.
 * yield_ms_override/interval_ms_override: 0 = use the normal
 * PAGER_POST_WAKE_YIELD_MS / active-or-sleep interval; otherwise override
 * both for the duration of the window (main.c's `sleeptest <minutes>
 * [yield_ms] [interval_ms]`, task 3's "how long must the pager stay awake to
 * receive a held URC" question).
 * probe_wait_ms_override: S18 (docs/SLEEP_URC_DESIGN.md §10), 0 = use
 * PAGER_PROBE_WAIT_MS; otherwise the bound on how long each wake holds RTS
 * asserted waiting for the drain probe's answer. Still subject to
 * PAGER_PROBE_WAIT_MIN_INTERVAL_MS, so it only has an effect when
 * interval_ms_override is also long -- pass both together.
 * wake0_ms: docs/SLEEP_PAGE_LOSS_BRIEF.md §6 item F, 0..2000, 0 = pin
 * untouched (today's behaviour); otherwise IO46/LTE_WAKE0 (pins.h's
 * PAGER_PIN_WAKE0) is pulsed high for wake0_ms then low on every wake,
 * right after flow control is restored and before the probe's wake bytes.
 * Also arms the flight recorder (flightrec.h) for the duration of the
 * window. */
void modes_debug_sleeptest_start(uint32_t minutes, uint32_t yield_ms_override,
                                 uint32_t interval_ms_override, uint32_t probe_wait_ms_override,
                                 uint32_t wake0_ms);
void modes_debug_sleeptest_report(void);

/* v0.2 §5 (location, loc.c): route 2's deliberate CFUN=4 window tears the
 * MQTT session down and takes the radio off on purpose. While `suppress` is
 * true, modes_run()'s own reconnect-retry loop and the F4 modem-health
 * check are both skipped entirely (neither would make sense mid-window: the
 * reconnect loop would race loc.c's own re-attach, and net_check() reading
 * NO_RF as "modem unresponsive" would trigger a real, unwanted modem reset).
 * loc.c is the only caller: set true right before net_session_down()+
 * net_radio_off(), false right after net_is_attached() confirms (or times
 * out on) the re-attach — modes_run()'s ordinary F1/F3 logic then reconnects
 * MQTT itself on its very next iteration, with no special-casing needed
 * there. No modem/sleep-state effect of its own: a single RAM flag. */
void modes_set_loc_suppress(bool suppress);

/* v0.2 §4.4 (CA trust, catrust.c): the two-phase apply's own deliberate
 * session teardown/scratch-slot reconnect trial — same reasoning and same
 * two call sites' worth of gating as modes_set_loc_suppress() above (the
 * ordinary reconnect-retry loop and the F4 modem-health check), just a
 * second, independent flag rather than reusing loc.c's (each module owns
 * its own suppression window; both can be OR'd together where modes_run()
 * checks them, since only one is ever expected active at a time in
 * practice but neither needs to know about the other). catrust.c is the
 * only caller: true right before net_session_down() (the scratch-slot
 * trial connect), false once catrust_service() has committed or rolled the
 * trial back. No modem/sleep-state effect of its own: a single RAM flag. */
void modes_set_ca_apply_suppress(bool suppress);

/* Owner request, 2026-09-20 (coverage.c's duty-cycle policy): true whenever
 * the pager has deliberately switched the radio off to save battery while
 * out of coverage (coverage_owns_radio(), driven from modes_run()'s own
 * loop). loc.c checks this before starting ANY GNSS attempt, real or
 * `gnsstest` — coverage.h's own module comment states the ownership rule
 * both directions: this policy never takes the radio while a location
 * attempt is in flight (loc_attempt_in_progress()), and a location attempt
 * never starts while this policy owns it. Read-only; modes.c is the only
 * writer (via coverage_step()'s own return value, never set directly). */
bool modes_coverage_owns_radio(void);

/* Owner request, 2026-09-20: loc.c's own sustained-motion trigger
 * (loc_on_motion_event(), fired by its accelerometer classifier) also resets
 * coverage.c's off-period backoff to its first step — moving is when
 * coverage changes. loc.c is the only caller, exactly once per
 * motion-triggered backoff reset (never once per raw accelerometer
 * interrupt). No modem/sleep-state effect of its own: RAM bookkeeping only. */
void modes_note_motion_reset(void);

/* Debug console (`coverage`, main.c, PAGER_DEBUG_NO_LIGHT_SLEEP builds
 * only): prints the duty-cycle policy's current state -- registered?, dark
 * for Ns, current phase, off-period step/duration, seconds until the next
 * action, and whether it currently owns the radio. Plain reads of
 * already-resident state, no AT round trip, no modem/sleep-state effect. */
void modes_coverage_debug_print(void);

/* v0.2 §6 (device-direct SMS, sms.c): alerts exactly like an incoming page
 * (docs/V02_DESIGN.md §6) for an inbound SMS from an allow-listed sender
 * that `msg_insert_sms_in()` has already put in the RAM thread — the same
 * set_mode(ACTIVE)+render_pending_set() pair handle_ingest_result()'s
 * MSG_INGEST_NEW branch uses for a real `/down` message, respecting the
 * exact same lock-screen rule (`lock_is_locked()`: never steals the screen
 * out from under Locked; the message is already inserted into the thread
 * regardless and is picked up on unlock, same as a page received while
 * locked). Unlike a real `/down` message this never queues a `shown` ack
 * (there is nothing to ack — `id` is sms.c's own synthetic "x_..." id, never
 * a relay-issued one). `from` is the SMS contact's display name. Call from
 * sms.c's own task (modes_run(), via sms_service()) only — never from an
 * event/URC callback. No modem effect of its own; may flip the mode to
 * active (same power effect set_mode() already documents) and, if not
 * locked, trigger the next render_pending drain to wake the panel. */
void modes_alert_incoming(const char *id, const char *from);

#endif /* MODES_H */

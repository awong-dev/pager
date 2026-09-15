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

/* Unconditionally bumps the active-mode idle deadline (Part A bug #2: the
 * deadline must be refreshed by activity while already active, not only set
 * once on the sleep->active edge). No-op while in sleep mode. Called from
 * modes_run()'s input-event drain loop for every resolved input.c event
 * (F6.3: this replaced the old per-keypress ui_poll_keys() call site — see
 * input.h's own event queue). */
void modes_note_activity(void);

/* Read-only mode getter. Does not participate in the
 * set_mode() funnel — it only reads. */
bool modes_is_active(void);

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

#endif /* MODES_H */

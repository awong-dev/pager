/* rail.h — the gated peripheral rail: two enables driven together by
 * rail_on()/rail_off(), the eInk Friend's ENA pin (PAGER_PIN_DISP_VCC_EN,
 * pins.h, active-high), which powers the display, and Walter's own switched
 * 3V3-OUT (PAGER_PIN_3V3_EN, IO0, active-low), which powers the CardKB.
 * Rewired 3 Oct 2026 (owner): previously ENA alone gated both the display
 * and (via the Friend's own 3V3 output pin) the CardKB, and IO0 fed the
 * LIS3DH permanently; now the CardKB moved to its own IO0 gate and the
 * LIS3DH moved off Walter entirely, onto the power board's always-on "3V"
 * rail, so this module never touches it.
 *
 * docs/ROADMAP.md "Design needed: input and display power gating", option 2
 * (owner decision, 24 Sep 10:30 pm PDT: gate the rail off outside the
 * attentive window — reverses the 24-Sep-earlier "hold the rail through
 * sleep" stopgap now that the IO8 wake button, not the keyboard, is the
 * always-on way to wake the pager). This module is the single place that
 * decides/drives either gate; modes.c only calls rail_on()/rail_off() at
 * the sleep-entry/wake-path call sites its own comments describe, it never
 * touches PAGER_PIN_3V3_EN or PAGER_PIN_DISP_VCC_EN directly.
 *
 * TASK_ui_round2.md Do #4 (lazy rail, owner feedback 25 Sep 2:30 am PDT):
 * the rail comes on for exactly four reasons, none of them "every wake
 * regardless of whether there is anything to draw" any more:
 *   (a) at boot — rail_init() below, called once from main.c.
 *   (b) on a wake whose cause is EXT1 (shared by the IO8 button and the
 *       LIS3DH motion interrupt, 3 Oct 2026 rewiring) — modes.c's wake
 *       path, right after net_sleep() returns, calls ui_ensure_powered()
 *       (ui.h) whenever esp_sleep_get_wakeup_cause() is EXT1.
 *   (c) whenever a render is about to happen — ui_ensure_powered() (ui.h)
 *       itself, called from the top of ui_render() and ui_incoming()'s own
 *       synchronous render path (and so, transitively, from every caller of
 *       either). This is what lets a
 *       plain timer wake with nothing queued to draw leave the rail OFF: the
 *       rail only comes up lazily, at the moment something actually needs to
 *       be painted, not preemptively on the wake that happened to notice it.
 *   (d) inside the attentive window — modes.c's own sleep-entry decision
 *       (`if (attentive) rail_on(); else rail_off();`, unchanged by this
 *       task), so a sustained typing/scrolling session never reboots the
 *       CardKB mid-session.
 * rail_on() itself is a no-op, including no change to rail_restored_us(), if
 * the rail is already on — so (b)/(c)/(d) overlapping in the same iteration
 * (the common case: an EXT1 wake immediately followed by a render) costs
 * exactly one rail edge, not three.
 *
 * device-only (GPIO), not built on the host.
 */
#ifndef RAIL_H
#define RAIL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time setup: configures PAGER_PIN_3V3_EN (CardKB supply gate, since the
 * 3 Oct 2026 rewiring) and PAGER_PIN_DISP_VCC_EN (ENA, the display gate) as
 * plain GPIO outputs and turns both ON. Must run before either downstream
 * peripheral (display, CardKB) is touched — same ordering board_power_init()
 * (main.c) used to require; the LIS3DH is on the power board's always-on
 * "3V" rail and needs no such ordering. Also excludes both pads from
 * ESP-IDF's sleep GPIO isolation (gpio_sleep_sel_dis()) so they hold
 * whichever level rail_on()/rail_off() last drove through every light
 * sleep, instead of floating and letting the board pull-ups switch them
 * (the 24 Sep finding this module fixes properly instead of papering over
 * with a permanent hold). Counts as a rail-restore edge (rail_restored_us())
 * the same as a post-sleep rail_on(): the CardKB MCU is powering up cold
 * here too and needs the same settle time before its first read. Power
 * effect: turns both gated rails on for the rest of this boot, until the
 * first rail_off().
 */
void rail_init(void);

/* Drives PAGER_PIN_DISP_VCC_EN high and PAGER_PIN_3V3_EN low (both gates
 * on). A no-op, including no change to rail_restored_us(), if the rail is
 * already on — so calling this every wake during the attentive window
 * (modes.c) does not re-arm ui.c's post-restore keyboard guard on every 1 s
 * cycle. Also returns the CardKB I2C bus (IO10/IO9) from rail_off()'s
 * bus-release hold back to I2C mode (ui_kb_bus_restore(), ui.h). Power
 * effect: powers the display and CardKB (not the LIS3DH — it is on the
 * power board's always-on "3V" rail, never gated by either pin).
 */
void rail_on(void);

/* Drives PAGER_PIN_DISP_VCC_EN low and PAGER_PIN_3V3_EN high (both gates
 * off). A no-op if the rail is already off. Before the drive, releases the
 * CardKB I2C bus (ui_kb_bus_release(), ui.h) so the ESP32 side of SDA/SCL is
 * not left driven high into the unpowered CardKB MCU's I/O protection
 * diodes while it is meant to be off (owner, 24 Sep 11:15 pm PDT). Power
 * effect: powers down the display and CardKB (not the LIS3DH — it is not on
 * either gate) — this is the whole point of the gate (ends their current
 * draw for the sleep about to be entered). The CardKB MCU loses power and
 * reboots on the next rail_on(); the display's panel RAM is lost (modes.c
 * calls disp_note_power_loss() on the matching wake, which restores it from
 * disp.c's own shadow copy of the last frame so the next refresh can still
 * be a partial, per disp.h).
 */
void rail_off(void);

bool rail_is_on(void);

/* esp_timer_get_time() timestamp of the most recent rail-restore edge: the
 * last rail_init() or off->on rail_on() transition. 0 only before
 * rail_init() has run. ui.c's ui_poll_keyboard() withholds CardKB reads
 * until PAGER_KB_BOOT_GUARD_MS after this, since the CardKB MCU needs time
 * to boot after its power returns.
 */
int64_t rail_restored_us(void);

/* Round 9: the universal post-rail-on settle delay rail_on() waits before
 * touching any downstream peripheral (rail.c's own comment on s_settle_ms
 * has the full rationale/sourcing). Default 15ms; the setter exists so a
 * bench sweep (15/20/30/50ms) runs on one flash. Compiles in a release
 * build too (no #ifdef at the call site needed), even though only debug
 * console commands call the setter today. */
void rail_debug_set_settle_ms(uint32_t ms);
uint32_t rail_debug_get_settle_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* RAIL_H */

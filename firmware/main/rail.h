/* rail.h — the board's 3V3 peripheral rail (display, CardKB, LIS3DH),
 * PAGER_PIN_3V3_EN (pins.h), active-low.
 *
 * docs/ROADMAP.md "Design needed: input and display power gating", option 2
 * (owner decision, 24 Sep 10:30 pm PDT: gate the rail off outside the
 * attentive window — reverses the 24-Sep-earlier "hold the rail through
 * sleep" stopgap now that the IO1 wake button, not the keyboard, is the
 * always-on way to wake the pager). This module is the single place that
 * decides/drives the rail level; modes.c only calls rail_on()/rail_off() at
 * the sleep-entry/wake-path call sites its own comments describe, it never
 * touches PAGER_PIN_3V3_EN directly.
 *
 * TASK_ui_round2.md Do #4 (lazy rail, owner feedback 25 Sep 2:30 am PDT):
 * the rail comes on for exactly four reasons, none of them "every wake
 * regardless of whether there is anything to draw" any more:
 *   (a) at boot — rail_init() below, called once from main.c.
 *   (b) on a wake whose cause is EXT0 (the IO1 button) or EXT1 (the LIS3DH
 *       motion interrupt) — modes.c's wake path, right after net_sleep()
 *       returns, calls ui_ensure_powered() (ui.h) when
 *       esp_sleep_get_wakeup_cause() is one of those two.
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
 * (the common case: an EXT0 wake immediately followed by a render) costs
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

/* One-time setup: configures PAGER_PIN_3V3_EN as a plain GPIO output and
 * turns the rail ON. Must run before any peripheral downstream of it
 * (display, CardKB, LIS3DH) is touched — same ordering board_power_init()
 * (main.c) used to require. Also excludes the pad from ESP-IDF's sleep GPIO
 * isolation (gpio_sleep_sel_dis()) so it holds whichever level rail_on()/
 * rail_off() last drove through every light sleep, instead of floating and
 * letting the board pull-up switch it (the 24 Sep finding this module
 * fixes properly instead of papering over with a permanent hold). Counts
 * as a rail-restore edge (rail_restored_us()) the same as a post-sleep
 * rail_on(): the CardKB MCU is powering up cold here too and needs the same
 * settle time before its first read. Power effect: turns the 3V3 peripheral
 * rail on for the rest of this boot, until the first rail_off().
 */
void rail_init(void);

/* Drives PAGER_PIN_3V3_EN low (rail on). A no-op, including no change to
 * rail_restored_us(), if the rail is already on — so calling this every
 * wake during the attentive window (modes.c) does not re-arm ui.c's
 * post-restore keyboard guard on every 1 s cycle. Also returns the CardKB
 * I2C bus (IO8/IO9) from rail_off()'s bus-release hold back to I2C mode
 * (ui_kb_bus_restore(), ui.h). Power effect: powers the display, CardKB,
 * and LIS3DH.
 */
void rail_on(void);

/* Drives PAGER_PIN_3V3_EN high (rail off). A no-op if the rail is already
 * off. Before the drive, releases the CardKB I2C bus (ui_kb_bus_release(),
 * ui.h) so the SDA/SCL pull-ups — tied to the always-on 3V3, not this
 * gated rail — do not phantom-power the CardKB MCU through its I/O
 * protection diodes while it is meant to be off (owner, 24 Sep 11:15 pm
 * PDT). Power effect: powers down the display, CardKB, and LIS3DH — this is
 * the whole point of the gate (ends their current draw for the sleep about
 * to be entered). The CardKB MCU loses power and reboots on the next
 * rail_on(); the display's panel RAM is lost (modes.c calls
 * disp_note_power_loss() on the matching wake, which restores it from disp.c's
 * own shadow copy of the last frame so the next refresh can still be a
 * partial, per disp.h).
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

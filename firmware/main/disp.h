/* disp.h — SSD1680 e-paper transport: BUSY handling, partial/full refresh,
 * the 20-partial cadence. Split out of ui.c by docs/DEVICE_TASKS.md F6.1
 * (moved, not rewritten — the command sequence and timings below are
 * carried forward byte-for-byte from the pre-split driver).
 *
 * Authority: docs/PROTOCOL.md §6 (command sequence — the 0x22 parameter
 * values 0xF7/0xFF are called out there as "inferred, not verified from
 * the datasheet PDF"; carried forward unchanged, same caveat), §9.5 (frame
 * buffer sizing). firmware/README.md (every-20th-partial full refresh,
 * this project's explicit requirement over the component author's "~10"
 * suggestion). docs/DEVICE_PLAN.md §5.3 ("one mutex in disp.c" — every
 * public entry point below takes it, so a future caller off the render
 * task (e.g. an event-task toast) cannot interleave a partial SPI
 * transaction with a full one; today only ui.c's single render path calls
 * these, so the mutex is currently uncontended, but it closes README R4
 * ahead of F6.3's screen stack adding more call sites).
 *
 * gfx.h/gfx.c own the framebuffer this module draws from (gfx_fb_native_row());
 * this module owns only the SPI/GPIO transport and the shadow plane it
 * diffs against for partial refresh.
 *
 * All timing/current/visual claims here are PENDING_HW.
 */
#ifndef DISP_H
#define DISP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the panel: VCC gate on, hardware reset, SSD1680 init sequence.
 * Power effect: turns the panel's VCC rail on (PAGER_PIN_DISP_VCC_EN) for
 * the duration of init. Returns false if BUSY never deasserts (15s
 * timeout, one retry) — the caller MUST keep running headless in that case
 * (PROTOCOL.md: no pager function may be gated on the display). Forces a
 * full refresh on the first disp_refresh_cadence() call after a successful
 * init. */
bool disp_init(void);

/* True once a BUSY timeout has persisted through a reset+re-init retry;
 * every refresh call below becomes a no-op once this is true, and the
 * device runs headless for the rest of this boot (network/replies/acks
 * unaffected). */
bool disp_is_dead(void);

/* Sends gfx.c's whole current framebuffer and marks the shadow plane
 * clean. Power effect: ~2-4s of panel refresh current (README M7/M14,
 * PENDING_HW) — the most expensive display operation; callers should
 * prefer disp_refresh_cadence()/disp_partial_refresh() and reserve this
 * for init/composer-close per the pre-split ui.c's existing call sites. */
void disp_full_refresh(void);

/* Diffs gfx.c's framebuffer against the shadow plane and sends only the
 * changed row band (a no-op if nothing changed). Power effect: ~0.3-0.8s
 * of panel refresh current (README M7/M14, PENDING_HW), the "cheap"
 * refresh. Does not consume/reset the 20-partial cadence counter — see
 * disp_refresh_cadence(). */
void disp_partial_refresh(void);

/* Every 20th call does a disp_full_refresh() instead and resets the
 * counter (firmware/README.md's explicit override of the component
 * author's "~10" suggestion); otherwise disp_partial_refresh(). This is
 * the entry point ui.c's normal render path uses; disp_full_refresh()/
 * disp_partial_refresh() above are for the two call sites (init,
 * composer-close/open) that must force one or the other regardless of the
 * counter. */
void disp_refresh_cadence(void);

/* Weak hook, called every ~10ms from inside disp_wait_busy_fb()'s BUSY-wait
 * loops (both the poll-until-low loop and the fixed-wait fallback path) for
 * as long as a refresh keeps the panel/task busy (up to ~3.5s on the full-
 * refresh fallback). Default definition (disp.c) is empty. modes.c/ui.c's
 * bug fix (CardKB losing keystrokes typed during a partial refresh's ~455ms
 * BUSY wait, since ui_poll_keyboard() otherwise only runs once per
 * modes_run() loop iteration) provides the strong definition, which polls
 * the CardKB while it's safe to do so. disp.c intentionally does NOT
 * include ui.h — this hook is the layering seam that lets a UI-level poll
 * happen without disp.c knowing anything about the UI. Runs on whichever
 * task called the refresh (disp_lock() is already held, so no two refreshes
 * ever call it concurrently); the strong definition is responsible for its
 * own task-safety check before touching shared I2C/input state. */
void disp_busy_idle_hook(void);

/* Bench A/B for the garbled-bands fix: false = pre-fix behaviour (only the
 * previous-image plane is re-synced after a partial), true = also re-write the
 * new-image plane, as the vendor reference does. Default true (CONFIRMED on
 * hardware: 13 consecutive adjacent-band partials all correct, real typing
 * stays clean); false deliberately reproduces the pre-fix bug and is kept so
 * a future session can re-confirm the fix on the bench in about 40 seconds
 * via `disptest again 0` / `disptest seq`. The `disptest` console command
 * flips it so one flash can test both. */
void disp_set_partial_write_again(bool on);
bool disp_partial_write_again(void);

/* Number of native rows where gfx.c's framebuffer differs from the shadow
 * plane, i.e. what the next partial refresh would send. 0 right after any
 * successful refresh. Bench diagnostic. */
int disp_dirty_rows(void);

/* Partials since the last full refresh (the 20-partial cadence counter). */
uint32_t disp_partial_count(void);

#ifdef __cplusplus
}
#endif

#endif /* DISP_H */

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

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the panel: VCC gate on, hardware reset, SSD1680 init sequence.
 * Power effect: turns the panel's VCC rail on (PAGER_PIN_DISP_VCC_EN) for
 * the duration of init; disp_shutdown() gates it back off. Returns false
 * if BUSY never deasserts (15s timeout, one retry) — the caller MUST keep
 * running headless in that case (PROTOCOL.md: no pager function may be
 * gated on the display). Forces a full refresh on the first
 * disp_refresh_cadence() call after a successful init. */
bool disp_init(void);

/* Sleep the panel (0x10) and power the VCC gate off. Power effect: this is
 * the single largest display-side saving in this driver — PROTOCOL.md
 * §8.4: "Display (gated off via IO15) ~0 mA". Safe to call even if
 * disp_init() failed or the display was marked dead. */
void disp_shutdown(void);

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

#ifdef __cplusplus
}
#endif

#endif /* DISP_H */

/* accel.h — LIS3DH accelerometer driver (I2C 0x18, INT1 on pins.h's
 * PAGER_PIN_LIS3DH_INT1 / IO16), docs/V02_DESIGN.md §5 trigger 2 (sustained
 * motion).
 *
 * Device-only (ESP-IDF I2C driver, no host-testable part of its own — the
 * motion *classifier* that decides "sustained" from a stream of interrupt
 * timestamps lives in loc.c/loc.h, pure and host-tested, per this task's own
 * instruction; accel.c only ever calls loc_on_motion_event() once per
 * drained INT1 edge).
 *
 * Bus ownership: owner decision, 26 Sep 2026 -- the LIS3DH has its own I2C
 * bus, I2C_NUM_1 on PAGER_PIN_ACCEL_SDA/SCL (pins.h), separate from the
 * CardKB's I2C_NUM_0, and is powered directly from the battery via the
 * breakout's own regulator, not the gated 3V3 rail (rail.h). This module
 * installs and owns that I2C_NUM_1 driver itself (accel_init(), once at
 * boot) -- it does not depend on ui.c's i2c_kb_init() or on ui_init() having
 * run first, and nothing in rail.c/ui.c ever touches this bus or its pins.
 *
 * UNVERIFIED, very likely absent on the owner's bench unit (V02_DESIGN.md
 * §5: "Probe the chip at boot; if it is absent (it may not be wired yet) log
 * once and run without it" — the task brief calls this "the path that will
 * actually run first"). Look for "LIS3DH not found" (absent, expected) vs.
 * "LIS3DH found" (present) in the boot log.
 */
#ifndef ACCEL_H
#define ACCEL_H

#include <stdbool.h>
#include <stdint.h>
#ifdef ESP_PLATFORM
#include "esp_err.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* A1 (docs/DEVICE_NEXT_TASKS.md): default refractory window, seconds. See
 * accel_edge_wanted() below for what it gates. `acceltest refr <seconds>`
 * (A2) overrides this at runtime; 0 disables the guard entirely (reproduces
 * the pre-A1 wake storm on purpose, for bench comparison). */
#define ACCEL_REFRACTORY_S 20

/* Pure, host-tested (firmware/host/test_accel_rate.c) -- no ESP-IDF
 * dependency, the split sms.c/loc.c use, so this one function lives above
 * accel.c's own `#ifdef ESP_PLATFORM` banner.
 *
 * CTRL_REG5's LIR_INT1 latches INT1 until INT1_SRC is read, and ext1 is
 * armed ESP_EXT1_WAKEUP_ANY_HIGH on that same pin -- so while the pager is
 * moving, every LIS3DH sample above threshold (up to 25/s at the configured
 * 25 Hz ODR) would otherwise end light sleep. The 60s-within-3min motion
 * classifier (loc_trigger_motion_event()) needs only two edges at least 60s
 * apart, so throttling reported edges to at most one per `refractory_us`
 * costs it nothing while cutting the wake rate by roughly two orders of
 * magnitude.
 *
 * Returns true if the INT1 edge at `now_us` should be reported to the
 * classifier, given the timestamp of the last *reported* edge
 * (`last_reported_us`, 0 = none yet) and the refractory window in
 * microseconds (0 = no throttling -- every edge is wanted). A negative/
 * backwards clock (now_us < last_reported_us -- never expected from this
 * hardware's monotonic esp_timer_get_time(), but must never wedge per the
 * task brief) is treated as "wanted" rather than permanently blocking every
 * future edge: fail open, the same discipline this codebase uses everywhere
 * else a clock reading feeds a gate. */
bool accel_edge_wanted(int64_t now_us, int64_t last_reported_us, int64_t refractory_us);

/* Shake-to-wake classifier (docs/SHAKE_WAKE_DESIGN.md D5/D6), pure and
 * host-tested like accel_edge_wanted(). A "chain" is a run of accel_poll()
 * observations with LIS3DH generator 2 (IA2) set. Defaults for the cfg: */
#define ACCEL_SHAKE_N           6
#define ACCEL_SHAKE_GAP_MS      500 /* bench 6 Oct 2026: a shake's events arrive 60-250 ms apart, the onset pause can exceed 250 */
#define ACCEL_SHAKE_SPAN_MS     300 /* bench 6 Oct 2026: a fast six-sample burst spans ~260 ms */
#define ACCEL_SHAKE_COOLDOWN_MS 3000
#define ACCEL_SHAKE_HOLDOFF_MS  0 /* bench 6 Oct 2026: a holdoff discarded the shake that followed a rejected onset; the n/span rule alone rejects taps and steps */

typedef struct {
    uint8_t n_min;
    uint16_t gap_ms, span_ms;
    uint32_t cooldown_ms, holdoff_ms;
} accel_shake_cfg_t;

typedef struct {
    uint16_t n;
    int64_t first_us, last_us, cooldown_until_us, holdoff_until_us;
    uint16_t out_n;
    uint32_t out_span_ms;
} accel_shake_t; /* zero-init = idle */

typedef enum {
    ACCEL_SHAKE_IDLE,
    ACCEL_SHAKE_PENDING,
    ACCEL_SHAKE_FIRED,
    ACCEL_SHAKE_REJECTED
} accel_shake_verdict_t;

/* Feeds one observation (`ia2` = generator 2 was set in this poll) at
 * `now_us`. Evaluated in this order:
 *  (a) Backwards clock (s->n && now_us < s->last_us): reset the chain
 *      (n=0) and return IDLE. If cooldown_until_us or holdoff_until_us lies
 *      more than its own duration ahead of now, clear it. This is the
 *      never-wedge rule.
 *  (b) If s->n > 0 and now - last > gap: set out_n=n and
 *      out_span_ms=(last-first)/1000, n=0, holdoff_until=now+holdoff and
 *      return REJECTED.
 *  (c) If now < cooldown_until or now < holdoff_until: return IDLE (ia2 is
 *      ignored).
 *  (d) If ia2: when n==0 set first=now; then n++ and last=now. If
 *      n >= n_min and now-first >= span: set out_n/out_span_ms, n=0,
 *      cooldown_until=now+cooldown and return FIRED.
 *  (e) Return PENDING if n>0, else IDLE.
 * out_n / out_span_ms are valid only after FIRED or REJECTED. No ESP-IDF
 * calls, no side effects beyond *s. */
accel_shake_verdict_t accel_shake_step(accel_shake_t *s, const accel_shake_cfg_t *c, int64_t now_us, bool ia2);

/* True while the device's shake chain has n>0, i.e. the main loop should
 * keep polling at the fast cadence instead of light-sleeping. Device state
 * (accel.c); false if the chip is absent. Power effect: none by itself. */
bool accel_shake_pending(void);

/* Probes WHO_AM_I (register 0x0F, expected 0x33). On success, configures
 * low-power 25 Hz ODR at +-4 g with high-pass-filtered motion (generator 1)
 * and shake-candidate (generator 2) interrupts, both routed to INT1
 * (docs/SHAKE_WAKE_DESIGN.md) and calls
 * net_enable_accel_wake() so IO16 becomes a light-sleep wake source. On
 * failure (no/wrong response — the expected case if the chip is not wired),
 * logs once at INFO and returns false; every other accel.c/loc.c function
 * then simply never has anything to report, which is this task's own
 * required fail-open behaviour. Call once, from modes_boot(); installs the
 * I2C_NUM_1 driver on its own bus first (see the module comment above --
 * independent of ui_init()/i2c_kb_init(), no ordering requirement either
 * way). Power effect: a
 * handful of I2C transactions at init, then ~a few uA continuous per the
 * LIS3DH's own low-power-mode datasheet figure (PENDING_HW, not measured on
 * this board). */
bool accel_init(void);

/* Reads INT1_SRC (0x31) and INT2_SRC (0x35) once each -- the reads clear the
 * LIS3DH's latched interrupts (CTRL_REG5 LIR_INT1/LIR_INT2). No-op (returns
 * false) if accel_init() did not find the chip. Call once per modes_run()
 * loop iteration, unconditionally, never from an ISR. On a generator 1
 * (motion) edge, consults accel_edge_wanted() and calls loc_on_motion_event()
 * exactly once per *wanted* edge, then clears CTRL_REG3 I1_IA1 at the sensor
 * for the refractory window (the ext1 bit stays armed so IA2 can always
 * wake). Generator 2 observations feed accel_shake_step(). Returns true
 * exactly once per intentional shake (FIRED); the caller turns that into a
 * wake gesture (input_note_shake_wake()). Power effect: two I2C reads, same
 * class as ui_poll_keyboard()'s CardKB read, plus at most one CTRL_REG3
 * write when the wanted IA1/IA2 routing changes. */
bool accel_poll(void);

/* ---------------------------------------------------------------------
 * Debug-only accessors for main.c's `acceltest` console command
 * (PAGER_DEBUG_NO_LIGHT_SLEEP builds only, docs/DEVICE_NEXT_TASKS.md A2).
 * main.c must not touch I2C or the LIS3DH's registers directly -- every
 * register read/write `acceltest` needs goes through one of these. Device-
 * only (defined in accel.c's own `#ifdef ESP_PLATFORM` section; not part
 * of the host build, same as accel_init()/accel_poll() above). Runs on the
 * caller's task (the console task) -- the IDF I2C driver is per-port
 * mutexed, so concurrent ui_poll_keyboard()/accel_poll() from modes_run()'s
 * task is safe.
 * --------------------------------------------------------------------- */

typedef struct {
    bool present;             /* WHO_AM_I answered 0x33 just now */
    uint8_t who_am_i;
    uint8_t ctrl_reg1, ctrl_reg2, ctrl_reg3, ctrl_reg4, ctrl_reg5;
    uint8_t int1_cfg, int1_ths, int1_duration, int1_src;
    uint8_t int2_cfg, int2_ths, int2_duration, int2_src;
    uint32_t ths_mg;           /* int1_ths * THS LSB for the CTRL_REG4 FS bits (16/32/62/186 mg) */
    uint32_t ths2_mg;          /* int2_ths * the same LSB */
    bool shake_enabled;
    uint8_t shake_n;           /* the s_shake_cfg fields */
    uint16_t shake_gap_ms, shake_span_ms;
    uint32_t shake_cooldown_ms, shake_holdoff_ms;
    uint32_t shake_candidates, shake_rejected, shake_fired;
    int64_t refractory_us;     /* current accel_edge_wanted() window, 0 = off */
    uint32_t edges_reported;   /* wanted edges passed to loc_on_motion_event() since boot */
    uint32_t ext1_wakes;       /* net_get_ext1_wakes() at the moment of this call */
} accel_debug_status_t;

/* Re-probes WHO_AM_I every call (so a chip wired after boot is picked up
 * without a reboot). If it now answers and accel_init() had not previously
 * configured it, runs the same configuration accel_init() would have and
 * sets *out_newly_configured -- otherwise leaves the running configuration
 * (including any `acceltest ths`/`dur` tuning) untouched. Fills *out with
 * every field `acceltest` prints. Returns false (with *out zeroed except
 * .present=false) if the chip still does not answer. Power effect: a
 * handful of I2C transactions, same class as accel_init()/accel_poll(). */
bool accel_debug_status(accel_debug_status_t *out, bool *out_newly_configured);

/* One live sample: OUT_X_L..OUT_Z_H (0x28, auto-increment bit 0x80) plus
 * INT1_SRC and INT2_SRC, converted to mg from the current CTRL_REG4 FS bits
 * (CTRL_REG1's LPen=1 selects the LIS3DH's 8-bit low-power resolution,
 * left-justified in the high byte of each axis: value = (raw >> 8) * LSB,
 * LSB 16/32/62/186 mg for +-2/4/8/16 g). Returns false (chip absent)
 * without touching the outputs. Power effect: three I2C transactions
 * (6-byte OUT burst + the two SRC reads), same class as accel_poll(). */
bool accel_debug_sample(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg, uint8_t *int1_src,
                        uint8_t *int2_src);

/* Sample period in ms for the current CTRL_REG1 ODR bits (1->1000, 2->100,
 * 3->40, 4->20, 5->10, else 100), so `acceltest samples` paces at the real
 * ODR. Power effect: one I2C read. */
uint32_t accel_debug_sample_period_ms(void);

/* Writes INT1_THS (0-127) and reads it back into *readback. Returns false
 * (readback unchanged) if the chip is absent or either I2C transaction
 * fails. Power effect: two I2C transactions. */
bool accel_debug_set_ths(uint8_t ths, uint8_t *readback);

/* Writes INT1_DURATION (0-127) and reads it back into *readback. Same
 * contract/power effect as accel_debug_set_ths(). */
bool accel_debug_set_dur(uint8_t dur, uint8_t *readback);

/* Writes INT2_THS / INT2_DURATION (0-127) and reads back into *readback.
 * Same contract/power effect as accel_debug_set_ths(). */
bool accel_debug_set_ths2(uint8_t ths, uint8_t *readback);
bool accel_debug_set_dur2(uint8_t dur, uint8_t *readback);

/* Enables/disables the shake feature: off takes IA2 off the pin (CTRL_REG3
 * on the next accel_poll()) and keeps the classifier idle. Resets the chain
 * and the holdoff either way. Power effect: none by itself. */
void accel_debug_set_shake(bool enabled);

/* Replaces the classifier cfg (n_min, gap, span, holdoff in seconds) and
 * resets the chain and holdoff. Power effect: none by itself. */
void accel_debug_set_shake_cfg(uint8_t n, uint16_t gap_ms, uint16_t span_ms, uint32_t holdoff_s);

/* Writes CTRL_REG1 and CTRL_REG4 and reads both back; true only if both
 * read-backs equal the written values. Power effect: four I2C transactions;
 * the ODR change alters the sensor's own current draw (about 3-4 uA at
 * 10-25 Hz LP). */
bool accel_debug_set_cfg(uint8_t ctrl1, uint8_t ctrl4);

/* Sets the refractory window accel_poll() passes to accel_edge_wanted(),
 * in seconds (0 = off, reproducing the pre-A1 wake storm on purpose).
 * Takes effect on the next accel_poll() call; does not require the chip
 * to be present. Power effect: none by itself. */
void accel_debug_set_refractory_s(uint32_t seconds);

#ifdef ESP_PLATFORM
/* 5 Oct 2026: esp_err_t of the most recent LIS3DH I2C read (ESP_OK, ESP_FAIL = NACK/no
 * device, ESP_ERR_TIMEOUT = bus held) -- the console's wire-vs-wedge triage. */
esp_err_t accel_debug_last_err(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* ACCEL_H */

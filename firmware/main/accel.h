/* accel.h — LIS3DH accelerometer driver (I2C 0x18, INT1 on pins.h's
 * PAGER_PIN_LIS3DH_INT1 / IO2), docs/V02_DESIGN.md §5 trigger 2 (sustained
 * motion).
 *
 * Device-only (ESP-IDF I2C driver, no host-testable part of its own — the
 * motion *classifier* that decides "sustained" from a stream of interrupt
 * timestamps lives in loc.c/loc.h, pure and host-tested, per this task's own
 * instruction; accel.c only ever calls loc_on_motion_event() once per
 * drained INT1 edge).
 *
 * Bus ownership: this module does NOT install the I2C driver. ui.c's
 * i2c_kb_init() (called from ui_init(), which modes_boot() always calls
 * before accel_init()) already installs I2C_NUM_0 for the CardKB at
 * PAGER_PIN_KB_SDA/SCL, 100 kHz — the LIS3DH shares that same bus (separate
 * 7-bit address, 0x18, no conflict). accel_init() MUST run after ui_init().
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
 * moving, every LIS3DH sample above threshold (up to 10/s at the configured
 * 10 Hz ODR) would otherwise end light sleep. The 60s-within-3min motion
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

/* Probes WHO_AM_I (register 0x0F, expected 0x33). On success, configures
 * low-power 10 Hz ODR with a high-pass-filtered INT1 motion interrupt
 * (thresholds UNVERIFIED/tunable — see accel.c) and calls
 * net_enable_accel_wake() so IO2 becomes a light-sleep wake source. On
 * failure (no/wrong response — the expected case if the chip is not wired),
 * logs once at INFO and returns false; every other accel.c/loc.c function
 * then simply never has anything to report, which is this task's own
 * required fail-open behaviour. Call once, from modes_boot(), after
 * ui_init() (see the module comment above for why). Power effect: a
 * handful of I2C transactions at init, then ~a few uA continuous per the
 * LIS3DH's own low-power-mode datasheet figure (PENDING_HW, not measured on
 * this board). */
bool accel_init(void);

/* Reads INT1_SRC (register 0x31) once — the read itself clears the LIS3DH's
 * latched interrupt (CTRL_REG5 LIR_INT1). No-op if accel_init() did not find
 * the chip. Call once per modes_run() loop iteration, unconditionally, same
 * polling discipline input_poll()/ui_poll_keyboard() already use — never
 * from an ISR (this chip's INT1 is only ever read as a polled register, not
 * hooked to a GPIO interrupt handler; IO2's only "interrupt" role is as the
 * ext1 light-sleep wake source net_enable_accel_wake()/net_set_accel_wake()
 * arms). On an asserted interrupt, consults accel_edge_wanted() and calls
 * loc_on_motion_event() exactly once per *wanted* edge (A1: unwanted edges,
 * inside the refractory window, are drained from INT1_SRC -- clearing the
 * latch -- but never reported to the classifier). Power effect: one I2C
 * transaction, same class as ui_poll_keyboard()'s CardKB read, PLUS -- on a
 * wanted edge, while the refractory window is non-zero -- one
 * net_set_accel_wake(false) that disarms the ext1 light-sleep wake source
 * for that window (cutting the ~10 Hz wake storm down to at most 1
 * wake/refractory-window while the pager is being carried), and later one
 * net_set_accel_wake(true) re-arming it once the window has elapsed. */
void accel_poll(void);

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
    uint32_t ths_mg;           /* int1_ths * 16 mg, at +-2g (accel.c's own INT1_THS comment) */
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
 * INT1_SRC, converted to mg using the same 16 mg/LSB, +-2g scale as
 * INT1_THS (CTRL_REG1's LPen=1 selects the LIS3DH's 8-bit low-power
 * resolution, left-justified in the high byte of each axis -- UNVERIFIED
 * against real hardware, like every other threshold in this file, but
 * good enough for a human eyeballing "is this reading sane"). Returns
 * false (chip absent) without touching x_mg / y_mg / z_mg / int1_src.
 * Power effect: two I2C transactions (6-byte OUT burst + INT1_SRC),
 * same class as accel_poll(). */
bool accel_debug_sample(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg, uint8_t *int1_src);

/* Writes INT1_THS (0-127) and reads it back into *readback. Returns false
 * (readback unchanged) if the chip is absent or either I2C transaction
 * fails. Power effect: two I2C transactions. */
bool accel_debug_set_ths(uint8_t ths, uint8_t *readback);

/* Writes INT1_DURATION (0-127) and reads it back into *readback. Same
 * contract/power effect as accel_debug_set_ths(). */
bool accel_debug_set_dur(uint8_t dur, uint8_t *readback);

/* Sets the refractory window accel_poll() passes to accel_edge_wanted(),
 * in seconds (0 = off, reproducing the pre-A1 wake storm on purpose).
 * Takes effect on the next accel_poll() call; does not require the chip
 * to be present. Power effect: none by itself. */
void accel_debug_set_refractory_s(uint32_t seconds);

#ifdef __cplusplus
}
#endif

#endif /* ACCEL_H */

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

#ifdef __cplusplus
extern "C" {
#endif

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

/* True iff accel_init() found the chip. */
bool accel_present(void);

/* Reads INT1_SRC (register 0x31) once — the read itself clears the LIS3DH's
 * latched interrupt (CTRL_REG5 LIR_INT1). No-op if accel_present() is
 * false. Call once per modes_run() loop iteration, unconditionally, same
 * polling discipline input_poll()/ui_poll_keyboard() already use — never
 * from an ISR (this chip's INT1 is only ever read as a polled register, not
 * hooked to a GPIO interrupt handler; IO2's only "interrupt" role is as the
 * ext1 light-sleep wake source net_enable_accel_wake() arms). On an
 * asserted interrupt, calls loc_on_motion_event() exactly once. Power
 * effect: one I2C transaction, same class as ui_poll_keyboard()'s CardKB
 * read. */
void accel_poll(void);

#ifdef __cplusplus
}
#endif

#endif /* ACCEL_H */

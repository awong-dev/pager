/* haptic.h -- DRV2605L LRA driver at 0x5A on the CardKB bus (I2C_NUM_0), carrier v0.2 only
 * (docs/CARRIER_V02_FIRMWARE_DESIGN.md §3.2). Rail-gated: it needs the 3V3_AWK rail, so every call
 * returns false when rail_is_on() is false. Bus access holds ui_kb_bus_lock() (the CardKB bus mutex).
 *
 * Threading: calls run on the caller's task (modes loop for alerts, console task for `haptic`); the
 * post-play standby is an esp_timer one-shot (esp_timer task) that takes the same bus lock.
 */
#ifndef HAPTIC_H
#define HAPTIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAPTIC_NEW_MSG = 1000, /* strong click, 150 ms pause, strong click */
    HAPTIC_TICK = 24,      /* sharp tick */
} haptic_effect_t;         /* any other value 1..123 is passed through as a library effect number */

/* Plays an effect. False on the breadboard, rail off, or bus/chip error.
 * Power effect: motor current for the effect (~100 ms); chip returns to standby ~0.6 s later. */
bool haptic_play(int effect);

/* Forces auto-calibration, saves the result to NVS, and fills the report line. */
bool haptic_calibrate(char *report, unsigned report_len);

/* Console `haptic cfg`: rated_mv (Vrms mV), clamp_mv (peak mV), f_hz; persisted, applied on next init. */
bool haptic_set_cfg(uint32_t rated_mv, uint32_t clamp_mv, uint32_t f_hz);

/* One-line status (config, calibration, last result). */
void haptic_status(char *out, unsigned len);

#ifdef __cplusplus
}
#endif

#endif /* HAPTIC_H */

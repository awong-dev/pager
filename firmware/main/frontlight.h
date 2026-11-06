/* frontlight.h -- panel front light on IO12 (LEDC, carrier v0.2 only; design §3.3). No-ops on the
 * breadboard. The button (IO4) lives in input.c (INPUT_EVT_LIGHT); the light-sleep wake is armed in
 * net.cpp.
 *
 * Threading: all calls are serialised by a mutex; the idle-off one-shot runs on the esp_timer task
 * and only touches LEDC. frontlight_activity() is called from whichever task pushes an input event.
 */
#ifndef FRONTLIGHT_H
#define FRONTLIGHT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FRONTLIGHT_IDLE_S 20
#define FRONTLIGHT_DEFAULT_PCT 60

/* Power effect: LED on = tens of mA from the 3V3_AWK/battery rail; off = 0. */
void frontlight_set(bool on);
void frontlight_off(void);   /* sets off; also called by rail_off() and before light sleep */
void frontlight_toggle(void);
bool frontlight_is_on(void);

/* Brightness percent (1-100), persisted in NVS board/fl_duty; applied at once if lit. */
void frontlight_set_pct(uint8_t pct);
uint8_t frontlight_get_pct(void);

/* Restarts the 20 s idle-off timer if the light is on (any key / button event). */
void frontlight_activity(void);

#ifdef __cplusplus
}
#endif

#endif /* FRONTLIGHT_H */

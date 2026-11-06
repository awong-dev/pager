/* extled.h -- external LED on IO8 (LEDC hardware fades, carrier v0.2 only; design §3.4). No-ops on the
 * breadboard (IO8 is untouched there).
 *
 * Threading: a 6 s periodic esp_timer plus a one-shot step timer, both on the esp_timer task, run the
 * breath pattern; they only touch LEDC. extled_pause()/extled_resume() bracket light sleep (net.cpp,
 * modes loop task). extled_set_unread() is called from the UI render path.
 */
#ifndef EXTLED_H
#define EXTLED_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the 6 s breath timer (idempotent). Power effect: ~25 % duty LED pulses while awake only. */
void extled_start(void);

/* Stops timers, cancels any fade and drives the pin low. Call before esp_light_sleep_start().
 * Power effect: LED dark. */
void extled_pause(void);

/* Restarts the 6 s timer after a wake. */
void extled_resume(void);

/* Unread messages present -> double breath instead of single. The one place the UI reports unread. */
void extled_set_unread(bool unread);

/* Console: one immediate pattern (0 = single, 1 = double); on/off hold steady (debug). */
void extled_pulse(bool dbl);
void extled_hold(bool on);

#ifdef __cplusplus
}
#endif

#endif /* EXTLED_H */

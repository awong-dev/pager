// watchdog.h — so a stalled pager recovers by itself, and says where it stalled.
//
// Three layers:
//  1. The hardware RTC watchdog (180 s). It runs from the RTC's own clock, so
//     it keeps counting during light sleep and fires even if FreeRTOS is dead
//     or the chip never wakes. ESP-IDF's light-sleep code leaves an RTC
//     watchdog alone when the application enabled it (sleep_modes.c, "If WDT
//     was enabled in the user code, then do not change it here").
//  2. The task watchdog on the main loop (sdkconfig: 60 s, the maximum; panic = reboot),
//     for a stall while awake with the scheduler still running.
//  3. A breadcrumb in reset-surviving RTC memory: the last stage the main loop
//     reached. After a watchdog reset, boot logs the reset reason and that
//     stage, and counts the reset.
// Power effect: none worth measuring (two register writes and one RAM write
// per loop pass).
#ifndef WATCHDOG_H
#define WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WD_BOOT = 1,
    WD_NET_INIT,
    WD_LOOP_TOP,
    WD_SLEEP_ENTER,
    WD_SLEEP_EXIT,
    WD_INPUT_UI,
    WD_RENDER,
    WD_MQTT,
    WD_PUMP,
    WD_HEALTH,
    WD_LOC,
    WD_SMS,
    WD_CATRUST,
    WD_SAVE,
    WD_DELIBERATE_RESTART,
} wd_stage_t;

/* Call once, early in app_main(): logs why the chip last reset and where the
 * main loop was, then arms the RTC watchdog. */
void watchdog_boot(void);
/* Call from the main loop's task before its loop: subscribes it to the task watchdog. */
void watchdog_loop_begin(void);
/* Feed both watchdogs and leave a breadcrumb. */
void watchdog_kick(wd_stage_t stage);
/* For long waits off the main loop's beaten path (network attach at boot). */
void watchdog_feed(void);
/* Watchdog or panic resets since power-on, for /status or the device screen. */
uint32_t watchdog_reset_count(void);
const char *watchdog_stage_name(uint32_t stage);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_H */

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

#include <stdbool.h>
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

/* True iff watchdog_boot() classified the last reset as a crash worth
 * flagging to the user at boot: a panic, a task/RTC watchdog, or a
 * brownout. False for a power-on, an external/USB (esptool) reset, and this
 * firmware's own deliberate restarts — esp_restart() (reports ESP_RST_SW,
 * never in the "abnormal" set below) and watchdog_hard_reset() (reports a
 * watchdog reset, but excluded via the WD_DELIBERATE_RESTART stage marker
 * watchdog_boot() checks before classifying). Valid only after
 * watchdog_boot() has run. No modem or sleep-state effect: reads a static
 * set at boot. */
bool watchdog_last_reset_was_crash(void);

/* Human-readable reason for the last reset (e.g. "PANIC (crash)", "TASK
 * WATCHDOG") — the same string watchdog_boot()'s own boot log line uses.
 * Valid only after watchdog_boot() has run. */
const char *watchdog_last_reset_reason_str(void);
/* Call from the main loop's task before its loop: subscribes it to the task watchdog. */
void watchdog_loop_begin(void);
/* Feed both watchdogs and leave a breadcrumb. */
void watchdog_kick(wd_stage_t stage);
/* For long waits off the main loop's beaten path (network attach at boot). */
void watchdog_feed(void);
/* Reset the whole chip through the RTC watchdog, peripherals included. Unlike
 * esp_restart() (a CPU reset), this also resets the USB-Serial-JTAG block:
 * after light sleep the USB port was seen to stay dead for hours across
 * esp_restart(). Does not return. */
void watchdog_hard_reset(void);

/* PAGER PATCH 1.10: strong override of the walter-modem component's weak
 * `walter_modem_block_tick()` (declared extern "C" there so the C++ component
 * links against this C definition). The component calls this once a second
 * from inside its own untimed synchronous command wait, so a slow command
 * still feeds both watchdogs instead of holding the queue in silence.
 * Arithmetic for why this is needed at all: the command queue retries a
 * timed-out command up to 3 times (CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS = 30 s
 * each in this project's sdkconfig), so one slow command (AT+COPS=0 during a
 * network search) can hold the queue for up to 3 * 30 s = 90 s, which is
 * already past the 60 s task watchdog timeout (the IDF maximum) even before
 * the next queued command (net_check()'s health-check AT) gets a turn.
 *
 * S6 (docs/SLEEP_URC_DESIGN.md §6 "Watchdog arithmetic"): task WDT 60 s,
 * RTC WDT 180 s, WD_MODEM_BLOCK_BUDGET_MS 95 s (watchdog.c) reset per
 * *stage* by watchdog_kick(). One stalled command at the 30 s/3-attempt
 * default = 90 s, fits inside the 95 s budget (survives). Two stalled
 * commands in one stage do not (180 s): feeding stops at 95 s and the task
 * watchdog reboots ~60 s later -- the likely explanation of the uncaptured
 * phaseAA reboot. Patch 1.13's 10 s/2-attempt timeout for the MQTT publish/
 * subscribe/disconnect/config commands brings one such stall down to 20 s,
 * so four of them fit in one stage (80 s) inside the same 95 s budget.
 * This function also logs (ESP_LOGI, once per stage) the stage name and,
 * best-effort, which command was running long, the first time a block
 * reaches 5 s -- so a subsequent reboot is attributable to a specific stage/
 * command instead of just "the watchdog fired somewhere". No watchdog
 * constant changed here; the arithmetic above is why none needed to. */
void walter_modem_block_tick(void);

#ifdef __cplusplus
}
#endif

#endif /* WATCHDOG_H */

// watchdog.c — see watchdog.h.
#include "watchdog.h"

#include <stdbool.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"
#include "net.h" // S6: net_get_pager_counters() for the 5s-stall attribution log below

static const char *TAG = "watchdog";
static TaskHandle_t s_loop_task = NULL; // the one task subscribed to the task watchdog

#define WD_MAGIC 0x57444f47u /* "WDOG" */
#define WD_RTC_TIMEOUT_MS 180000u
/* PAGER PATCH 1.10: bigger than the worst case a wedged synchronous AT
 * command wait can legitimately take (3 retries * 30 s
 * CONFIG_WALTER_MODEM_CMD_TIMEOUT_MS = 90 s), smaller than the 180 s RTC
 * watchdog, so a command that is merely slow gets fed through and a command
 * that is genuinely wedged still trips the task watchdog with the
 * "modem health check" breadcrumb intact rather than running forever. */
#define WD_MODEM_BLOCK_BUDGET_MS 95000u
static int64_t s_block_start_us;
// S6 (docs/SLEEP_URC_DESIGN.md §6 "Watchdog arithmetic"): edge-triggered so
// the 5s-stall log below fires once per stage/command block, not once a
// second for the rest of a long stall. Reset alongside s_block_start_us.
static bool s_block_5s_logged;

// RTC_NOINIT: survives software, panic and watchdog resets; garbage after a
// power-on, hence the magic.
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_stage;
static RTC_NOINIT_ATTR uint32_t s_resets;

// Ordinary (non-RTC) statics: the boot crash classification, computed once
// by watchdog_boot() and read back by main.c to decide whether to raise the
// status-bar crash indicator (ui.c). This-boot-only, like every other
// ordinary static in this file -- nothing here needs to survive a reset.
static bool s_last_abnormal = false;
static const char *s_last_reason_str = "unknown";
// Raw values behind the two strings above, for the /status crash-diagnostic
// keys (modes.c STK_RST/STK_STAGE/STK_ABN): so a crash can be read off the
// relay when the USB port stays dead after a watchdog/panic reset (it only
// re-enumerates after a power cycle). This-boot-only, computed once by
// watchdog_boot(), same as s_last_abnormal/s_last_reason_str above.
static int s_last_reset_reason = 0;      // esp_reset_reason_t of THIS boot
static uint32_t s_last_reset_stage = 0;  // previous boot's breadcrumb; 0 if none (no valid RTC breadcrumb)

static const char *watchdog_stage_name(uint32_t stage)
{
    static const char *const k_names[] = {
        "?", "boot", "network init", "loop top", "entering light sleep", "just woke from light sleep",
        "input/ui", "render", "mqtt status/retry", "message pump", "modem health check", "location",
        "sms", "ca trust", "saving state", "deliberate restart",
    };
    return (stage < sizeof(k_names) / sizeof(k_names[0])) ? k_names[stage] : "?";
}

static const char *reason_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_EXT: return "external pin";
    case ESP_RST_SW: return "software restart";
    case ESP_RST_PANIC: return "PANIC (crash)";
    case ESP_RST_INT_WDT: return "INTERRUPT WATCHDOG";
    case ESP_RST_TASK_WDT: return "TASK WATCHDOG";
    case ESP_RST_WDT: return "RTC/other WATCHDOG";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_USB: return "USB";
    default: return "unknown";
    }
}

void watchdog_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool valid = (s_magic == WD_MAGIC);
    bool abnormal = (r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
                     r == ESP_RST_WDT || r == ESP_RST_BROWNOUT);
    if (valid && s_stage == (uint32_t) WD_DELIBERATE_RESTART) {
        abnormal = false; // watchdog_hard_reset(): a watchdog reset on purpose
    }
    if (!valid) {
        s_magic = WD_MAGIC;
        s_resets = 0;
        s_stage = WD_BOOT;
    } else if (abnormal) {
        s_resets++;
    }
    if (abnormal && valid) {
        ESP_LOGW(TAG, "RESET REASON: %s. The main loop's last stage was: %s. Abnormal resets since power-on: %u",
                 reason_name(r), watchdog_stage_name(s_stage), (unsigned) s_resets);
    } else {
        ESP_LOGI(TAG, "reset reason: %s%s%s", reason_name(r), valid ? "; last stage: " : "",
                 valid ? watchdog_stage_name(s_stage) : "");
    }
    // Classification for main.c's boot crash indicator (ui.c's status-bar
    // icon) -- captured before s_stage resets to WD_BOOT below, though only
    // `abnormal`/`r` (not s_stage) feed it.
    s_last_abnormal = abnormal;
    s_last_reason_str = reason_name(r);
    s_last_reset_reason = (int) r;
    s_last_reset_stage = valid ? s_stage : 0;
    s_stage = WD_BOOT;

    // Arm the RTC watchdog through the HAL (the rtc_wdt.h convenience API is
    // not built for this target): one stage, whole-chip reset, RTC included,
    // so a wedged peripheral is reset too. The timeout is in RTC slow-clock
    // ticks.
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    uint32_t ticks = (uint32_t) ((uint64_t) WD_RTC_TIMEOUT_MS * rtc_clk_slow_freq_get_hz() / 1000ULL);
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    ESP_LOGI(TAG, "RTC watchdog armed (%u s); task watchdog %d s", (unsigned) (WD_RTC_TIMEOUT_MS / 1000),
             CONFIG_ESP_TASK_WDT_TIMEOUT_S);
}

bool watchdog_last_reset_was_crash(void) { return s_last_abnormal; }
const char *watchdog_last_reset_reason_str(void) { return s_last_reason_str; }

// Raw esp_reset_reason_t of THIS boot's reset. No modem or sleep-state
// effect: reads a static set at boot. Valid only after watchdog_boot().
int watchdog_last_reset_reason(void) { return s_last_reset_reason; }

// Stage index (watchdog_stage_name()'s table) the main loop reached in the
// *previous* boot, per the RTC breadcrumb; 0 if there was none (first boot,
// or the RTC breadcrumb was not valid). No modem or sleep-state effect:
// reads a static set at boot. Valid only after watchdog_boot().
int watchdog_last_reset_stage(void) { return (int) s_last_reset_stage; }

// Abnormal (panic/watchdog/brownout) reset count since power-on -- the same
// RTC counter the "Abnormal resets since power-on" boot log line prints. No
// modem or sleep-state effect: reads an RTC_NOINIT value set at boot. Valid
// only after watchdog_boot().
unsigned watchdog_abnormal_reset_count(void) { return (unsigned) s_resets; }

void watchdog_loop_begin(void)
{
    esp_err_t err = esp_task_wdt_add(NULL);
    if (err == ESP_OK || err == ESP_ERR_INVALID_ARG) {
        s_loop_task = xTaskGetCurrentTaskHandle();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_ARG) {
        ESP_LOGI(TAG, "esp_task_wdt_add failed: 0x%x (RTC watchdog still covers the loop)", err);
    }
}

// The RTC watchdog feed only; factored out so walter_modem_block_tick() can
// feed the RTC watchdog without also touching the task watchdog subscription
// check below (that check is duplicated there, not called through here,
// since it needs its own gate on the block budget).
static void rtc_feed(void)
{
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
}

void watchdog_feed(void)
{
    rtc_feed();
    // Only from the subscribed task: from any other task the call fails and
    // logs an error every time.
    if (s_loop_task != NULL && xTaskGetCurrentTaskHandle() == s_loop_task) {
        esp_task_wdt_reset();
    }
}

void watchdog_kick(wd_stage_t stage)
{
    s_stage = (uint32_t) stage;
    s_block_start_us = 0; // PAGER PATCH 1.10: fresh block budget for this stage
    s_block_5s_logged = false; // S6: fresh stage, allow the 5s-stall log to fire again
    watchdog_feed();
}

// PAGER PATCH 1.10: called about once a second from inside the walter-modem
// component's untimed synchronous command wait (WalterDefines.h
// _returnAfterReply(), WalterModem.cpp _waitCmdResult() and
// getNetworkRegState()). Power effect: none worth measuring, same as
// watchdog_feed() (two register writes and one RAM read per call); it does
// not itself keep the modem or radio active, it only keeps both watchdogs
// fed while a command already in flight is waited on.
void walter_modem_block_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (s_block_start_us == 0) {
        s_block_start_us = now;
    }
    int64_t blocked_us = now - s_block_start_us;
    // S6 (docs/SLEEP_URC_DESIGN.md §6 "Watchdog arithmetic"): so the next
    // reboot is attributable, not just "the task watchdog fired somewhere".
    // Names the stage (this file's own breadcrumb) and, best-effort, which
    // command S3's per-command stall snapshot last saw running long -- that
    // snapshot is written by a different task (_cmdProcessingTask) and may
    // belong to an earlier command in the same stage if several ran, but it
    // is the only command-level attribution this component exposes.
    if (!s_block_5s_logged && blocked_us >= 5 * 1000000LL) {
        s_block_5s_logged = true;
        net_pager_counters_t pc = net_get_pager_counters();
        if (pc.stall_cmd[0] != '\0') {
            ESP_LOGI(TAG, "stage '%s' blocked >= 5s on a modem command, last seen: \"%s\" (%u ms so far)",
                     watchdog_stage_name(s_stage), pc.stall_cmd, (unsigned) pc.stall_elapsed_ms);
        } else {
            ESP_LOGI(TAG, "stage '%s' blocked >= 5s on a modem command (no command name available yet)",
                     watchdog_stage_name(s_stage));
        }
    }
    if ((uint64_t) blocked_us < (uint64_t) WD_MODEM_BLOCK_BUDGET_MS * 1000ULL) {
        rtc_feed();
        if (s_loop_task != NULL && xTaskGetCurrentTaskHandle() == s_loop_task) {
            esp_task_wdt_reset();
        }
    }
    // else: past the budget, feed nothing — a genuinely wedged modem still
    // trips the task watchdog (~155 s in: 95 s budget here plus however long
    // the task watchdog's own timeout takes from the last real feed) with
    // the "modem health check" breadcrumb intact.
}

void watchdog_hard_reset(void)
{
    s_stage = (uint32_t) WD_DELIBERATE_RESTART;
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    uint32_t ticks = (uint32_t) ((uint64_t) 200 * rtc_clk_slow_freq_get_hz() / 1000ULL); // 200 ms
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_init(&ctx, WDT_RWDT, 0, false);
    wdt_hal_config_stage(&ctx, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    for (;;) {
        // wait for the watchdog
    }
}

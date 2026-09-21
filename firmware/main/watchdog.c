// watchdog.c — see watchdog.h.
#include "watchdog.h"

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/wdt_hal.h"
#include "soc/rtc.h"

static const char *TAG = "watchdog";
static TaskHandle_t s_loop_task = NULL; // the one task subscribed to the task watchdog

#define WD_MAGIC 0x57444f47u /* "WDOG" */
#define WD_RTC_TIMEOUT_MS 180000u

// RTC_NOINIT: survives software, panic and watchdog resets; garbage after a
// power-on, hence the magic.
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_stage;
static RTC_NOINIT_ATTR uint32_t s_resets;

const char *watchdog_stage_name(uint32_t stage)
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

void watchdog_feed(void)
{
    wdt_hal_context_t ctx = RWDT_HAL_CONTEXT_DEFAULT();
    wdt_hal_write_protect_disable(&ctx);
    wdt_hal_feed(&ctx);
    wdt_hal_write_protect_enable(&ctx);
    // Only from the subscribed task: from any other task the call fails and
    // logs an error every time.
    if (s_loop_task != NULL && xTaskGetCurrentTaskHandle() == s_loop_task) {
        esp_task_wdt_reset();
    }
}

void watchdog_kick(wd_stage_t stage)
{
    s_stage = (uint32_t) stage;
    watchdog_feed();
}

uint32_t watchdog_reset_count(void) { return (s_magic == WD_MAGIC) ? s_resets : 0; }

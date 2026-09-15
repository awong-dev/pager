#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ident.h"
#include "modes.h"

static const char *TAG = "school_pager";

void app_main(void)
{
    ESP_LOGI(TAG, "school_pager boot");

    // NVS init only; no modem/radio access, no power effect beyond the
    // flash read/erase-and-retry below (DEVICE_PLAN.md §3.4/§2.7).
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    if (!ident_load()) {
        // No valid identity in NVS. DEVICE_TASKS.md F3.1: until F3.5
        // (Setup mode) exists, halt here instead of running the normal
        // boot path with no broker credentials. Power effect: the modem
        // never leaves reset, so current stays at the CPU-idle-loop floor
        // (PENDING_HW — no device attached to this build session).
        ESP_LOGI(TAG, "IDENT missing");
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "IDENT %s sig=%d claimed=%d", ident_get_dev_id(),
             (ident_get_flags() & IDENT_FLAG_REQ_SIG) ? 1 : 0, ident_get_claimed() ? 1 : 0);

    // modes_boot() decides cold-boot vs. reset-recovery internally by
    // validating the RTC struct (PROTOCOL.md §9); it is the only wake-cause
    // dispatch that happens at app_main() entry, because this design never
    // deep sleeps (§8.3) - a light-sleep wake never re-runs app_main() at
    // all, it just resumes inside modes_run()'s loop.
    modes_boot();
    modes_run(); // never returns
}

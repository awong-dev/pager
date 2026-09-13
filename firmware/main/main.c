#include "esp_log.h"
#include "modes.h"

static const char *TAG = "school_pager";

void app_main(void)
{
    ESP_LOGI(TAG, "school_pager boot");

    // modes_boot() decides cold-boot vs. reset-recovery internally by
    // validating the RTC struct (PROTOCOL.md §9); it is the only wake-cause
    // dispatch that happens at app_main() entry, because this design never
    // deep sleeps (§8.3) - a light-sleep wake never re-runs app_main() at
    // all, it just resumes inside modes_run()'s loop.
    modes_boot();
    modes_run(); // never returns
}

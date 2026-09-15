#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ident.h"
#include "modes.h"
#include "setup.h"

static const char *TAG = "school_pager";

/* docs/DEVICE_TASKS.md F3.5: `setup <code>` over the USB serial console.
 * argtable3-free by design — the setup code itself contains a space
 * (" @ "), so this just re-joins every argv past argv[0] with single spaces,
 * which recovers the original string whether or not the caller quoted it
 * (docs/DEVICE_PLAN.md §3.1's `format_code()` never emits runs of more than
 * one space). */
#define SETUP_CMD_LINE_MAX 128

static int cmd_setup(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: setup <code>\n");
        return 1;
    }

    char code[SETUP_CMD_LINE_MAX];
    size_t len = 0;
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            if (len + 1 >= sizeof(code)) {
                printf("setup code too long\n");
                return 1;
            }
            code[len++] = ' ';
        }
        size_t alen = strlen(argv[i]);
        if (len + alen >= sizeof(code)) {
            printf("setup code too long\n");
            return 1;
        }
        memcpy(code + len, argv[i], alen);
        len += alen;
    }
    code[len] = '\0';

    // setup_run() esp_restart()s on success and never returns here; a false
    // return means it already logged/toasted one of the four
    // DEVICE_PLAN.md §3.2 error strings and the person may retry.
    if (!setup_run(code)) {
        printf("setup failed; see the SETUP log line above, retry with a fresh code if needed\n");
        return 1;
    }
    return 0; // unreachable: setup_run() only returns by not returning (esp_restart())
}

// No modem/radio access of its own; starts the USB-serial REPL task that
// waits for a person to type `setup <code>` (docs/DEVICE_PLAN.md §3.2 step
// 5's Setup mode). Power effect: none beyond the idle CPU/UART-RX floor
// until a command is typed — everything that actually touches the modem
// happens inside setup_run() (setup.c), not here.
static void start_setup_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "pager>";

    esp_console_dev_uart_config_t uart_config = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&uart_config, &repl_config, &repl));

    esp_console_register_help_command();

    const esp_console_cmd_t setup_cmd = {
        .command = "setup",
        .help = "setup <code> -- docs/DEVICE_PLAN.md section 3.2 bootstrap fetch from a typed setup code",
        .hint = NULL,
        .func = &cmd_setup,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&setup_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

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
        // No valid identity in NVS: docs/DEVICE_TASKS.md F3.5, Setup mode.
        // The modem is never touched here — start_setup_console() only
        // starts the USB-serial REPL task; setup_run() (setup.c) is the one
        // thing that ever brings the modem up, once a person types
        // `setup <code>`. Power effect: current stays at the CPU-idle-loop
        // floor until that happens.
        ESP_LOGI(TAG, "IDENT missing; starting Setup mode console");
        start_setup_console();
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

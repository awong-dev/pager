#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"

#include <stdio.h>

#include "ident.h"
#include "modes.h"
#include "net.h"
#include "pins.h"
#include "setup.h"
#include "ui.h"

static const char *TAG = "school_pager";

// Board 3V3 peripheral rail is off by default on power-up; see pins.h's
// PAGER_PIN_3V3_EN comment. Must run before any peripheral (display, CardKB,
// LIS3DH) is touched. GPIO0 is a boot strapping pin but is safe to
// reconfigure as a plain output here -- strapping is sampled only during
// the reset/boot sequence, which has already completed by app_main().
static void board_power_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PAGER_PIN_3V3_EN,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    gpio_set_level(PAGER_PIN_3V3_EN, 0); // active-low: enable the rail
}

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

// TEMPORARY diagnostic command: `nettest <host> <port>` dials a plain TCP
// socket (no TLS) via net_check_tcp() -- see that function's own doc
// comment in net.h for why. Remove once the broker-connect issue is
// root-caused.
static int cmd_nettest(int argc, char **argv)
{
    esp_log_level_set("WalterModem", ESP_LOG_DEBUG); // raw AT TX:/RX: trace
    if (argc < 3 || argc > 4) {
        printf("usage: nettest <host> <port> [udp|tls]\n");
        return 1;
    }
    long port = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535) {
        printf("bad port\n");
        return 1;
    }
    bool udp = (argc == 4) && (strcmp(argv[3], "udp") == 0);
    bool tls = (argc == 4) && (strcmp(argv[3], "tls") == 0);
    bool ok = net_check_tcp(argv[1], (uint16_t) port, udp, tls);
    printf("nettest: %s\n", ok ? "CONNECTED" : "FAILED (see log above)");
    return ok ? 0 : 1;
}

// TEMPORARY diagnostic command: `mqtttest <host> <port>` issues a real
// AT+SQNSMQTTCONNECT (TLS, VALIDATION_NONE, dummy credentials) via
// net_check_mqtt() -- see net.h. Remove together with nettest.
static int cmd_mqtttest(int argc, char **argv)
{
    esp_log_level_set("WalterModem", ESP_LOG_DEBUG); // raw AT TX:/RX: trace
    if (argc < 3 || argc > 4) {
        printf("usage: mqtttest <host> <port> [ca|noneca|emptyca]\n");
        return 1;
    }
    long port = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535) {
        printf("bad port\n");
        return 1;
    }
    int tls_mode = 0;
    if (argc == 4 && strcmp(argv[3], "ca") == 0) tls_mode = 1;
    if (argc == 4 && strcmp(argv[3], "noneca") == 0) tls_mode = 2;
    if (argc == 4 && strcmp(argv[3], "emptyca") == 0) tls_mode = 3;
    bool ok = net_check_mqtt(argv[1], (uint16_t) port, tls_mode);
    printf("mqtttest: %s\n", ok ? "CONNECTED" : "NOT CONNECTED (see log above)");
    return ok ? 0 : 1;
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
    // Default (4096) overflows: confirmed on real hardware -- `setup <code>`
    // crashed (Guru Meditation LoadProhibited, corrupted-pointer signature
    // several frames deep in vfprintf/xRingbufferSend) on the very first
    // ESP_LOGI call inside ui_init()->gfx_init(), before setup_run() even
    // reaches its own mbedtls AES-GCM/HKDF and MQTT work, which use
    // meaningfully more stack still.
    repl_config.task_stack_size = 16384;

    // esp_console_new_repl_uart() binds to UART0's own RX/TX pins, a
    // physically separate peripheral from the native USB-Serial/JTAG port
    // this board's single USB connector actually exposes (confirmed on
    // real hardware: typing `setup <code>` over the flashing/monitor cable
    // reached nowhere -- the REPL's stdin was listening on unconnected
    // UART0 pins the whole time). USB-Serial/JTAG is this project's
    // sdkconfig CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG, already used
    // for boot/log output; using it here too makes the console reachable
    // over the same cable used to flash and monitor.
    esp_console_dev_usb_serial_jtag_config_t usb_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl));

    esp_console_register_help_command();

    const esp_console_cmd_t setup_cmd = {
        .command = "setup",
        .help = "setup <code> -- docs/DEVICE_PLAN.md section 3.2 bootstrap fetch from a typed setup code",
        .hint = NULL,
        .func = &cmd_setup,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&setup_cmd));

    const esp_console_cmd_t nettest_cmd = {
        .command = "nettest",
        .help = "nettest <host> <port> -- TEMPORARY: plain TCP (no TLS) connectivity probe",
        .hint = NULL,
        .func = &cmd_nettest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nettest_cmd));

    const esp_console_cmd_t mqtttest_cmd = {
        .command = "mqtttest",
        .help = "mqtttest <host> <port> -- TEMPORARY: modem MQTT-engine TLS connect probe",
        .hint = NULL,
        .func = &cmd_mqtttest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mqtttest_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
// v0.2 §2.7 (docs/V02_DESIGN.md item 7): the same USB-serial REPL Setup mode
// uses (start_setup_console()), but reachable in normal (provisioned) mode
// too -- PAGER_DEBUG_NO_LIGHT_SLEEP builds only -- so `nettest`/`mqtttest`
// diagnostics can run against a provisioned pager's real SIM/APN without
// erasing its identity to get back into Setup mode first. Deliberately does
// NOT register `setup`: re-running it here would overwrite the very
// identity being diagnosed.
//
// Caveat, debug-build-only and worth stating plainly: nettest/mqtttest both
// call net_bootstrap_attach() (net.cpp), which is documented as safe to
// share process-global state with the production session (s_down_topic,
// the MQTT event handler registration) only because bootstrap and
// production "never run in the same power cycle" outside of this REPL. Once
// this REPL exists in normal mode, that stops being strictly true: running
// either command while the production MQTT session is up re-attaches the
// modem (setOpState(NO_RF) then FULL) and, for mqtttest, repoints
// s_down_topic at a test topic, which will disrupt (and, for mqtttest,
// mis-subscribe) live paging until the next full reconnect. Acceptable for
// a debug build that already carries CMakeLists.txt's "never ship a build
// made this way" warning and is only ever run by a person deliberately
// diagnosing a bench unit, not for anything that reaches a real pager in
// the field.
//
// Own task (esp_console_start_repl() spawns one internally and returns;
// this function does not block app_main()), 16 kB stack -- same overflow
// finding start_setup_console() documents (ui_init()/gfx_init()'s first
// ESP_LOGI call overflowed the default 4 kB stack).
//
// `gnsstest <seconds>`, `cafetch <url> <sha256hex>`, `smstest <number>
// <text>` are named in docs/V02_DESIGN.md §2.7 alongside this REPL but each
// depends on a feature this task does not implement (§5 location/GNSS, §4.4
// CA fetch, §6 SMS respectively) -- left for the tasks that add those
// features; only the REPL itself plus the pre-existing nettest/mqtttest are
// added here.
//
// Power effect: none beyond the idle CPU/UART-RX floor until a command is
// typed, same as start_setup_console(); nettest/mqtttest's own modem use is
// documented at their definitions above.
static void start_normal_console(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "pager>";
    repl_config.task_stack_size = 16384;

    esp_console_dev_usb_serial_jtag_config_t usb_config =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&usb_config, &repl_config, &repl));

    esp_console_register_help_command();

    const esp_console_cmd_t nettest_cmd = {
        .command = "nettest",
        .help = "nettest <host> <port> -- TEMPORARY: plain TCP (no TLS) connectivity probe",
        .hint = NULL,
        .func = &cmd_nettest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&nettest_cmd));

    const esp_console_cmd_t mqtttest_cmd = {
        .command = "mqtttest",
        .help = "mqtttest <host> <port> -- TEMPORARY: modem MQTT-engine TLS connect probe",
        .hint = NULL,
        .func = &cmd_mqtttest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&mqtttest_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "debug console REPL started in normal mode (PAGER_DEBUG_NO_LIGHT_SLEEP)");
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "school_pager boot");

    board_power_init(); // 3V3 peripheral rail on -- must precede any display/I2C use

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
        // Reuses scr_greeting.c (the same "Hi ...!" banner shown post-setup)
        // as a generic pre-provisioning splash, with a status footer that
        // walks booting -> sim missing/found -> shutting down -- added at
        // the user's request so a device sitting on a shelf with no SIM
        // fitted yet shows *something* legible instead of the noise-pattern
        // blank shadow plane a never-painted panel shows.
        //
        // The SIM check runs unconditionally, once, every boot (not gated
        // behind a console command) -- the user's explicit choice over
        // waiting for a person to notice and type something first. It is a
        // single net_check_sim() call, never retried: also the user's
        // explicit choice over a periodic recheck loop, since nothing about
        // a missing SIM changes without a person physically opening the
        // device, and this design never deep sleeps (§8.3) to wake back up
        // for one anyway.
        //
        // start_setup_console() still always runs afterward regardless of
        // the SIM result: `setup <code>` is the one and only way this device
        // ever leaves this branch (a fresh net_bootstrap_attach() inside
        // setup_run() will simply fail its own way if the SIM is genuinely
        // absent), and typing it requires nothing this check could have
        // broken.
        ESP_LOGI(TAG, "IDENT missing; starting Setup mode console");

        bool ui_ok = ui_init();
        if (ui_ok) {
            scr_greeting_set_mode(GREETING_HELLO);
            scr_greeting_set_status("booting");
            ui_push(&g_scr_greeting);
            ui_render_boot();
        }

        bool sim_ok = net_check_sim();
        ESP_LOGI(TAG, "SIM check: %s", sim_ok ? "found" : "missing");
        if (ui_ok) {
            scr_greeting_set_status(sim_ok ? "type: setup <code>" : "sim missing");
            ui_render();
        }

        if (!sim_ok) {
            // Best-effort pause so "sim missing" is actually readable before
            // the panel moves on -- not a retry wait, just legibility.
            vTaskDelay(pdMS_TO_TICKS(4000));
            if (ui_ok) {
                scr_greeting_set_status("shutting down");
                ui_render();
            }
        }

        // net_check_sim() leaves the modem parked at NO_RF (no RRC, no
        // teardown call) rather than touching it further here -- harmless,
        // since net_bootstrap_attach() (setup_run(), setup.c) unconditionally
        // re-issues setOpState(NO_RF) then setOpState(FULL) itself on the
        // way to a real attach, regardless of the opstate it finds. Power
        // effect from here: current stays at the CPU-idle-loop floor (plus
        // the modem's own NO_RF floor) until a person types `setup <code>`.
        start_setup_console();
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "IDENT %s sig=%d claimed=%d", ident_get_dev_id(),
             (ident_get_flags() & IDENT_FLAG_REQ_SIG) ? 1 : 0, ident_get_claimed() ? 1 : 0);

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
    // v0.2 §2.7: see start_normal_console()'s own comment. Started before
    // modes_boot()/modes_run() (which never returns) so the console task
    // exists for the rest of the device's life; esp_console_start_repl()
    // does not block here.
    start_normal_console();
#endif

    // modes_boot() decides cold-boot vs. reset-recovery internally by
    // validating the RTC struct (PROTOCOL.md §9); it is the only wake-cause
    // dispatch that happens at app_main() entry, because this design never
    // deep sleeps (§8.3) - a light-sleep wake never re-runs app_main() at
    // all, it just resumes inside modes_run()'s loop.
    modes_boot();
    modes_run(); // never returns
}

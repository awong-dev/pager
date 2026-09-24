#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c.h"

#include <stdio.h>

#include "accel.h"
#include "catrust.h"
#include "ident.h"
#include "watchdog.h"
#include "carrier.h"
#include "disp.h"
#include "gfx.h"
#include "input.h"
#include "loc.h"
#include "modes.h"
#include "net.h"
#include "pins.h"
#include "setup.h"
#include "sms.h"
#include "ui.h"

// docs/WIFI_TASKS.md W5: console control + wifisleeptest are debug-build
// only (both live inside start_normal_console(), itself
// PAGER_DEBUG_NO_LIGHT_SLEEP-gated, same as every other block below that
// includes these three) -- release main.c never touches a WiFi symbol.
// esp_wifi.h is only needed for wifisleeptest's own
// esp_wifi_sta_get_ap_info() probe; wifi_sta.h's own API deliberately never
// exposes a wifi_ap_record_t so ordinary callers stay decoupled from
// esp_wifi's headers.
#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
#include "wifi_sta.h"
#include "wificred.h"
#include "esp_wifi.h"
#endif

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

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
// TEMPORARY diagnostic command: `nettest <host> <port>` dials a plain TCP
// socket (no TLS) via net_check_tcp() -- see that function's own doc
// comment in net.h for why. Remove once the broker-connect issue is
// root-caused. Debug build only (PAGER_DEBUG_NO_LIGHT_SLEEP): docs/
// ROADMAP.md's "temporary diagnostics" no longer ship in the release
// binary.
static int cmd_nettest(int argc, char **argv)
{
    esp_log_level_set("WalterModem", ESP_LOG_DEBUG); // raw AT TX:/RX: trace
    if (argc < 3 || argc > 4) {
        printf("usage: nettest <host> <port> [udp|tls|<bytes>]\n");
        return 1;
    }
    long port = strtol(argv[2], NULL, 10);
    if (port <= 0 || port > 65535) {
        printf("bad port\n");
        return 1;
    }
    bool udp = (argc == 4) && (strcmp(argv[3], "udp") == 0);
    bool tls = (argc == 4) && (strcmp(argv[3], "tls") == 0);
    long pad = (argc == 4 && !udp && !tls) ? strtol(argv[3], NULL, 10) : 0;
    bool ok = (pad > 0) ? net_check_tcp_sized(argv[1], (uint16_t) port, (size_t) pad)
                        : net_check_tcp(argv[1], (uint16_t) port, udp, tls);
    printf("nettest: %s\n", ok ? "CONNECTED" : "FAILED (see log above)");
    return ok ? 0 : 1;
}

// TEMPORARY diagnostic command: `mqtttest <host> <port>` issues a real
// AT+SQNSMQTTCONNECT (TLS, VALIDATION_NONE, dummy credentials) via
// net_check_mqtt() -- see net.h. Remove together with nettest. Debug build
// only (PAGER_DEBUG_NO_LIGHT_SLEEP), same as nettest above.
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
    // RCA_SLEEP_PUBLISH.md §3 instrumentation: a natural place to glance at
    // the vendored library's four "orphaned prompt" counters (PATCHES.md
    // 1.12) alongside a manual MQTT probe.
    net_pager_counters_t pc = net_get_pager_counters();
    printf("modem counters: datatx_retx=%u prompt_orphan=%u buf_drop_queue=%u buf_drop_pool=%u\n",
           (unsigned) pc.datatx_retx, (unsigned) pc.prompt_orphan, (unsigned) pc.buf_drop_queue,
           (unsigned) pc.buf_drop_pool);
    return ok ? 0 : 1;
}

// Debug build only: raw AT passthrough.
// Debug build only: `sleeptest <minutes> [yield_ms] [interval_ms]` opens a
// window of real light sleep (modes.c); `sleeptest` alone prints the report
// again. yield_ms overrides the post-wake yield (default
// PAGER_POST_WAKE_YIELD_MS, 200); interval_ms overrides the wake interval
// (default: 2000/5000 by mode) -- task 3's "how long must the pager stay
// awake after a wake to receive a held URC" question. Either override may be
// given as 0, which means "use the build default" (the same thing omitting it
// does), so the documented `sleeptest 6 0 0` form works.
static int cmd_sleeptest(int argc, char **argv)
{
    if (argc == 1) {
        modes_debug_sleeptest_report();
        return 0;
    }
    long m = strtol(argv[1], NULL, 10);
    if (m < 1 || m > 120) {
        printf("usage: sleeptest [<minutes 1..120> [yield_ms 0|30..10000] "
               "[interval_ms 0|200..60000] [probe_wait_ms 0|100..15000]]   "
               "(0 = build default)\n");
        return 1;
    }
    long yield_ms = 0;    // 0 = use PAGER_POST_WAKE_YIELD_MS
    long interval_ms = 0; // 0 = use the normal active/sleep interval
    // 0 means "keep the build default" for both overrides, exactly as
    // modes_debug_sleeptest_start() already treats them (modes.c's
    // s_st_yield_ms/s_st_interval_ms are only applied when non-zero) and as
    // docs/RCA_SLEEP_PUBLISH.md §3's own console sequence assumes. Before
    // this, `sleeptest 6 0 0` was rejected by the range checks below and the
    // documented command line did not run at all.
    if (argc >= 3) {
        yield_ms = strtol(argv[2], NULL, 10);
        if (yield_ms != 0 && (yield_ms < 30 || yield_ms > 10000)) {
            printf("usage: sleeptest <minutes> [yield_ms 0|30..10000] [interval_ms 0|200..60000]\n");
            return 1;
        }
    }
    if (argc >= 4) {
        interval_ms = strtol(argv[3], NULL, 10);
        if (interval_ms != 0 && (interval_ms < 200 || interval_ms > 60000)) {
            printf("usage: sleeptest <minutes> [yield_ms 0|30..10000] [interval_ms 0|200..60000]\n");
            return 1;
        }
    }
    // S18 (docs/SLEEP_URC_DESIGN.md §10): the bound on how long each wake
    // holds RTS asserted waiting for the URC drain probe's answer. Only has
    // an effect together with a long interval_ms (modes.c's
    // PAGER_PROBE_WAIT_MIN_INTERVAL_MS), so the experiment is
    // `sleeptest 6 0 20000 4000`.
    long probe_wait_ms = 0; // 0 = use PAGER_PROBE_WAIT_MS
    if (argc >= 5) {
        probe_wait_ms = strtol(argv[4], NULL, 10);
        if (probe_wait_ms != 0 && (probe_wait_ms < 100 || probe_wait_ms > 15000)) {
            printf("usage: sleeptest <minutes> [yield_ms 0|30..10000] [interval_ms 0|200..60000] "
                   "[probe_wait_ms 0|100..15000]\n");
            return 1;
        }
    }
    modes_debug_sleeptest_start((uint32_t) m, (uint32_t) yield_ms, (uint32_t) interval_ms,
                                (uint32_t) probe_wait_ms);
    return 0;
}

static int cmd_at(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: at <command>   e.g. at AT+CIMI  (quote arguments containing commas or spaces)\n");
        return 1;
    }
    char line[160];
    size_t n = 0;
    for (int i = 1; i < argc && n + 1 < sizeof(line); i++) {
        n += (size_t) snprintf(line + n, sizeof(line) - n, "%s%s", (i > 1) ? " " : "", argv[i]);
    }
    esp_log_level_set("WalterModem", ESP_LOG_DEBUG);
    bool ok = net_debug_at(line);
    printf("at: %s\n", ok ? "OK" : "ERROR/timeout");
    return ok ? 0 : 1;
}

#endif

// `carrier` -- the pager-side APN choice (carrier.h). Available in Setup mode
// (it must be, the APN is needed before anything can be fetched) and in the
// debug build's normal-mode console.
//   carrier              list the presets, mark the current one
//   carrier <n>          choose preset n
//   carrier custom <apn> any other APN
static int cmd_carrier(int argc, char **argv)
{
    if (argc == 1) {
        bool is_auto = carrier_get_mode() == CARRIER_MODE_AUTO;
        int cur = is_auto ? CARRIER_PRESET_AUTO
                          : (carrier_get_apn()[0] == '\0' ? CARRIER_PRESET_BLANK
                                                          : carrier_preset_index_for(carrier_get_apn()));
        for (size_t i = 0; i < carrier_preset_count(); i++) {
            const carrier_preset_t *p = carrier_preset_at(i);
            printf(" %c %u  %-22s %s\n", ((int) i == cur) ? '*' : ' ', (unsigned) i, p->label,
                   i == CARRIER_PRESET_AUTO ? "(detect from the SIM)" : (p->apn[0] ? p->apn : "(blank)"));
        }
        if (cur < 0) {
            printf(" *    custom                 %s\n", carrier_get_apn());
        }
        if (is_auto && carrier_last_detected()[0]) {
            printf("detected: %s\n", carrier_last_detected());
        }
        printf("carrier <n> | carrier custom <apn>\n");
        return 0;
    }
    bool ok = false;
    if (argc == 3 && strcmp(argv[1], "custom") == 0) {
        ok = carrier_set_custom(argv[2]);
    } else if (argc == 2) {
        char *end = NULL;
        long n = strtol(argv[1], &end, 10);
        ok = (end && *end == '\0' && n >= 0) && carrier_select_preset((size_t) n);
    }
    if (!ok) {
        printf("carrier: not a preset number or a valid APN\n");
        return 1;
    }
    printf("carrier: %s. Used from the next attach: run `setup <code>` now, or restart.\n",
           carrier_get_label());
    return 0;
}

// `wake`: arm the UI-awake window so the CardKB is polled (a bench with no
// button). `key <text>`: feed bytes as if typed; "\n" = enter, "\e" = esc.
static int cmd_wake(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    input_arm_awake();
    printf("wake: UI awake; the keyboard is polled now\n");
    return 0;
}

static int cmd_key(int argc, char **argv)
{
    input_arm_awake();
    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            input_feed_key(' ');
        }
        for (const char *p = argv[i]; *p; p++) {
            if (p[0] == '\\' && p[1] == 'n') {
                input_feed_key(0x0D);
                p++;
            } else if (p[0] == '\\' && p[1] == 'e') {
                input_feed_key(0x1B);
                p++;
            } else {
                input_feed_key((uint8_t) *p);
            }
        }
    }
    return 0;
}

// `i2cscan`: probe every 7-bit address on the keyboard bus (IO8/IO9) and
// print who ACKs. Bench aid: tells wiring faults from a dead keyboard.
static int cmd_i2cscan(int argc, char **argv)
{
    bool swap = (argc >= 2) && (strcmp(argv[1], "swap") == 0);
    int sda = swap ? PAGER_PIN_KB_SCL : PAGER_PIN_KB_SDA;
    int scl = swap ? PAGER_PIN_KB_SDA : PAGER_PIN_KB_SCL;
    if (swap) {
        i2c_driver_delete(I2C_NUM_0);
        i2c_config_t conf = {
            .mode = I2C_MODE_MASTER,
            .sda_io_num = sda,
            .scl_io_num = scl,
            .sda_pullup_en = GPIO_PULLUP_ENABLE,
            .scl_pullup_en = GPIO_PULLUP_ENABLE,
            .master.clk_speed = 100000,
        };
        i2c_param_config(I2C_NUM_0, &conf);
        i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
    }
    int found = 0, nack = 0, timeout = 0, other = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (uint8_t) ((a << 1) | I2C_MASTER_WRITE), true);
        i2c_master_stop(cmd);
        esp_err_t err = i2c_master_cmd_begin(I2C_NUM_0, cmd, pdMS_TO_TICKS(20));
        i2c_cmd_link_delete(cmd);
        if (err == ESP_OK) {
            printf("i2cscan: device at 0x%02x%s\n", a,
                   a == PAGER_I2C_ADDR_CARDKB ? " (CardKB)" : a == PAGER_I2C_ADDR_LIS3DH ? " (LIS3DH)" : "");
            found++;
        } else if (err == ESP_FAIL) {
            nack++; // nobody answered: a normal empty address
        } else if (err == ESP_ERR_TIMEOUT) {
            timeout++; // the bus never completed: a line held low or floating
        } else {
            other++;
            if (other == 1) {
                printf("i2cscan: error %s at 0x%02x\n", esp_err_to_name(err), a);
            }
        }
    }
    printf("i2cscan: %d device(s) on SDA=IO%d SCL=IO%d; %d no-answer, %d timeout, %d other. "
           "Lines idle: SDA=%d SCL=%d (1 = pulled up, as they should be)\n",
           found, sda, scl, nack, timeout, other, gpio_get_level((gpio_num_t) sda),
           gpio_get_level((gpio_num_t) scl));
    if (timeout > 0) {
        printf("i2cscan: timeouts mean a line is stuck: check for a short, a swapped pair, or a "
               "keyboard powered from the wrong rail\n");
    }
    return 0;
}

static void register_input_cmds(void)
{
    const esp_console_cmd_t scan_cmd = {
        .command = "i2cscan",
        .help = "i2cscan -- list the devices that answer on the keyboard I2C bus",
        .hint = NULL,
        .func = &cmd_i2cscan,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&scan_cmd));
    const esp_console_cmd_t wake_cmd = {
        .command = "wake",
        .help = "wake -- arm the UI-awake window so the keyboard is polled",
        .hint = NULL,
        .func = &cmd_wake,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wake_cmd));
    const esp_console_cmd_t key_cmd = {
        .command = "key",
        .help = "key <text> -- type text as if on the keyboard (\\n enter, \\e esc)",
        .hint = NULL,
        .func = &cmd_key,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&key_cmd));
}

static void register_carrier_cmd(void)
{
    const esp_console_cmd_t carrier_cmd = {
        .command = "carrier",
        .help = "carrier [<n> | custom <apn>] -- choose the carrier APN (presets are built in)",
        .hint = NULL,
        .func = &cmd_carrier,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&carrier_cmd));
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
    register_carrier_cmd();
    register_input_cmds();

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
    // Debug build only: docs/ROADMAP.md's "temporary diagnostics" no longer
    // ship in the release binary's Setup console.
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
#endif

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
// `gnsstest <seconds>` (docs/V02_DESIGN.md §2.7/§5) and `cafetch <url>
// <sha256hex>` (§4.4, this task) are added below. `smstest <number> <text>`
// still depends on §6 (SMS), not started by this task -- left for the task
// that adds it.
//
// Power effect: none beyond the idle CPU/UART-RX floor until a command is
// typed, same as start_setup_console(); nettest/mqtttest's own modem use is
// documented at their definitions above; gnsstest's is documented at
// loc_debug_run() (loc.h).

// v0.2 §5: `gnsstest <seconds>` -- runs one location-fix attempt through the
// exact same route/state-machine code a real loc_req uses, bypassing the
// backoff/battery-floor gate. Blocks this console task (never modes_run()'s
// -- see loc_debug_run()'s own doc comment) until the attempt finishes or
// `seconds` elapses.
static int cmd_gnsstest(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: gnsstest <seconds>\n");
        return 1;
    }
    long seconds = strtol(argv[1], NULL, 10);
    if (seconds <= 0 || seconds > 120) {
        printf("seconds must be 1..120\n");
        return 1;
    }
    bool ok = loc_debug_run((uint32_t) seconds);
    printf("gnsstest: %s (see the log above for route/confidence/satellite/session detail)\n",
           ok ? "FIX" : "no fix / refused / already running - see log");
    return ok ? 0 : 1;
}
// Owner request, 2026-09-20: `coverage` -- prints the coverage duty-cycle
// policy's current state. Plain reads only, no modem/sleep-state effect.
static int cmd_coverage(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    modes_coverage_debug_print();
    return 0;
}

// v0.2 §4.4: `cafetch <url> <sha256hex>` -- runs the CA fetch only (no
// apply), via catrust_debug_cafetch()/cafetch_run_blocking(). Blocks this
// console task (never modes_run()'s -- see catrust_debug_cafetch()'s own
// doc comment) for up to the fetch's own 30s budget.
static int cmd_cafetch(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: cafetch <url> <sha256hex>\n");
        return 1;
    }
    bool ok = catrust_debug_cafetch(argv[1], argv[2]);
    printf("cafetch: %s (see the log above for bytes/http_status/elapsed/mqtt_survived detail)\n",
           ok ? "OK (hash matched)" : "FAILED - see log");
    return ok ? 0 : 1;
}

// v0.2 §6 (docs/V02_DESIGN.md, this task): `smstest <number> <text>` -- sends
// one SMS bypassing the allow-list (an arbitrary number), via
// sms_debug_send() (main/sms.c). Blocks this console task (never
// modes_run()'s) for the AT+CMGS round trip; still produces an audit entry
// (sms_debug_send()'s own doc comment: the audit trail has no debug
// carve-out).
static int cmd_smstest(int argc, char **argv)
{
    if (argc != 3) {
        printf("usage: smstest <number> <text>\n");
        return 1;
    }
    bool ok = sms_debug_send(argv[1], argv[2]);
    printf("smstest: %s (see the log above for the encoding chosen and the raw modem result)\n",
           ok ? "OK" : "FAILED - see log");
    return ok ? 0 : 1;
}

// v0.2 §6: `smslist` -- logs the allow-list and the audit queue depth
// (sms.c's own sms_debug_list() does the ESP_LOGI calls).
static int cmd_smslist(int argc, char **argv)
{
    (void) argc;
    (void) argv;
    sms_debug_list();
    printf("smslist: see the log above\n");
    return 0;
}

// A2 (docs/DEVICE_NEXT_TASKS.md): `acceltest` -- LIS3DH register/sample
// dump and runtime tuning, entirely through accel.h's accel_debug_*() API
// (this file never touches I2C or the LIS3DH's registers directly). Every
// form re-probes WHO_AM_I first (accel_debug_status()'s own contract), so
// the owner can wire the chip and start typing without a reboot -- if it
// now answers and had not been configured yet, this prints that once.
// Runs on this console task; the IDF I2C driver is per-port mutexed, so
// concurrent ui_poll_keyboard()/accel_poll() (modes_run()'s task) is safe.
// `acceltest samples 600` blocks THIS task (never modes_run()'s) for ~60s
// (100ms/sample at the LIS3DH's 10Hz ODR).
static const char *ACCELTEST_USAGE =
    "acceltest                 -- WHO_AM_I, CTRL_REG1-5, INT1_CFG/THS/DURATION/SRC, THS in mg, "
    "the refractory setting, edges reported, and ext1 wake count\n"
    "acceltest samples <n>     -- n (1..600) live x/y/z samples at 10Hz, plus INT1_SRC when IA is set\n"
    "acceltest ths <0-127>     -- write INT1_THS, echo the read-back\n"
    "acceltest dur <0-127>     -- write INT1_DURATION, echo the read-back\n"
    "acceltest refr <seconds>  -- set A1's refractory window (0 = off, reproduces the wake storm)\n";

static int cmd_acceltest(int argc, char **argv)
{
    accel_debug_status_t st;
    bool newly_configured = false;
    bool present = accel_debug_status(&st, &newly_configured);
    if (newly_configured) {
        printf("acceltest: LIS3DH now answers WHO_AM_I -- configured\n");
    }

    if (argc == 1) {
        if (!present) {
            printf("acceltest: not present\n");
            return 1;
        }
        printf("acceltest: present=1 WHO_AM_I=0x%02x\n", (unsigned) st.who_am_i);
        printf("acceltest: CTRL_REG1=0x%02x CTRL_REG2=0x%02x CTRL_REG3=0x%02x CTRL_REG4=0x%02x "
               "CTRL_REG5=0x%02x\n",
               (unsigned) st.ctrl_reg1, (unsigned) st.ctrl_reg2, (unsigned) st.ctrl_reg3,
               (unsigned) st.ctrl_reg4, (unsigned) st.ctrl_reg5);
        printf("acceltest: INT1_CFG=0x%02x INT1_THS=0x%02x (%u mg) INT1_DURATION=0x%02x "
               "INT1_SRC=0x%02x\n",
               (unsigned) st.int1_cfg, (unsigned) st.int1_ths, (unsigned) st.ths_mg,
               (unsigned) st.int1_duration, (unsigned) st.int1_src);
        printf("acceltest: refractory=%llds edges_reported=%u ext1_wakes=%u\n",
               (long long) (st.refractory_us / 1000000), (unsigned) st.edges_reported,
               (unsigned) st.ext1_wakes);
        return 0;
    }

    if (strcmp(argv[1], "samples") == 0) {
        if (argc != 3) {
            printf("usage: acceltest samples <n>\n");
            return 1;
        }
        char *end = NULL;
        long n = strtol(argv[2], &end, 10);
        if (!end || *end != '\0' || n < 1 || n > 600) {
            printf("acceltest: n must be 1..600\n");
            return 1;
        }
        if (!present) {
            printf("acceltest: not present\n");
            return 1;
        }
        for (long i = 0; i < n; i++) {
            int16_t x_mg = 0, y_mg = 0, z_mg = 0;
            uint8_t src = 0;
            if (!accel_debug_sample(&x_mg, &y_mg, &z_mg, &src)) {
                printf("acceltest: sample %ld: I2C read failed\n", i);
            } else {
                printf("acceltest: x=%d y=%d z=%d mg\n", (int) x_mg, (int) y_mg, (int) z_mg);
                if (src & 0x40) { // IA
                    printf("acceltest: INT1_SRC=0x%02x\n", (unsigned) src);
                }
            }
            vTaskDelay(pdMS_TO_TICKS(100)); // 10 Hz ODR
        }
        return 0;
    }

    if (strcmp(argv[1], "ths") == 0) {
        if (argc != 3) {
            printf("usage: acceltest ths <0-127>\n");
            return 1;
        }
        char *end = NULL;
        long v = strtol(argv[2], &end, 10);
        if (!end || *end != '\0' || v < 0 || v > 127) {
            printf("acceltest: ths must be 0..127\n");
            return 1;
        }
        uint8_t readback = 0;
        if (!accel_debug_set_ths((uint8_t) v, &readback)) {
            printf("acceltest: not present, or the write failed\n");
            return 1;
        }
        printf("acceltest: INT1_THS=0x%02x (%u mg)\n", (unsigned) readback,
               (unsigned) readback * 16);
        return 0;
    }

    if (strcmp(argv[1], "dur") == 0) {
        if (argc != 3) {
            printf("usage: acceltest dur <0-127>\n");
            return 1;
        }
        char *end = NULL;
        long v = strtol(argv[2], &end, 10);
        if (!end || *end != '\0' || v < 0 || v > 127) {
            printf("acceltest: dur must be 0..127\n");
            return 1;
        }
        uint8_t readback = 0;
        if (!accel_debug_set_dur((uint8_t) v, &readback)) {
            printf("acceltest: not present, or the write failed\n");
            return 1;
        }
        printf("acceltest: INT1_DURATION=0x%02x\n", (unsigned) readback);
        return 0;
    }

    if (strcmp(argv[1], "refr") == 0) {
        if (argc != 3) {
            printf("usage: acceltest refr <seconds>\n");
            return 1;
        }
        char *end = NULL;
        long v = strtol(argv[2], &end, 10);
        if (!end || *end != '\0' || v < 0) {
            printf("acceltest: seconds must be >= 0 (0 disables the refractory guard)\n");
            return 1;
        }
        accel_debug_set_refractory_s((uint32_t) v);
        printf("acceltest: refractory=%lds\n", v);
        return 0;
    }

    printf("%s", ACCELTEST_USAGE);
    return 1;
}

// Bench harness for the garbled-bands bug (disp.c's partial_refresh_locked()
// comment, docs task-disp-fix.md): a deterministic, keyboard-free,
// network-free reproduction. Paints a known pattern and drives
// disp_partial_refresh()/disp_full_refresh() directly instead of typing on
// the CardKB, so the bench can correlate "the band that went wrong" (a
// screen-x range) against disp.c's own "partial: native rows ..." log line
// with nothing else moving. No modem/sleep-state effect; every refresh call
// below has its own documented panel-VCC power effect at its definition in
// disp.c.
#define DISPTEST_COL_PX 8
#define DISPTEST_MAX_STEP (GFX_SCREEN_W / DISPTEST_COL_PX) /* n must be < this */
#define DISPTEST_SEQ_MAX_COUNT 36
#define DISPTEST_SEQ_MIN_MS 200
#define DISPTEST_SEQ_MAX_MS 5000

static const char *DISPTEST_USAGE =
    "disptest                     -- same as `disptest info`\n"
    "disptest info                -- print again-mode, partial count, disp_dirty_rows()\n"
    "disptest again <0|1>         -- set the partial-write-again A/B flag\n"
    "disptest bars                -- paint the baseline pattern and FULL refresh\n"
    "disptest step <n>            -- invert the 8px screen column at x=n*8 (full height), then "
    "ONE partial refresh; n must be 0..(GFX_SCREEN_W/8 - 1)\n"
    "disptest seq [n0] [n1] [ms]  -- `step` for n = n0..n1, ms apart (defaults 2 12 1500; "
    "count clamped to 36, ms clamped to 200..5000)\n"
    "disptest full                -- force one full refresh of whatever is in the framebuffer\n"
    "disptest swreset             -- fault injector: send SW reset (0x12) alone, wait BUSY, "
    "nothing else -- leaves the controller on power-on register defaults (23 Sep field failure)\n"
    "NOTE: do not use `wake` or `key` while disptest is running -- the UI render task would "
    "repaint over the test pattern.\n";

// Baseline test pattern: 8px black, 8px white, repeating across the full
// 296px screen width, full screen height. Nested gfx_set_pixel() loops --
// gfx.h has no filled-rect primitive (gfx_rect() is an outline only).
static void disptest_paint_bars(void)
{
    gfx_clear();
    for (int x0 = 0; x0 < GFX_SCREEN_W; x0 += 2 * DISPTEST_COL_PX) {
        for (int dx = 0; dx < DISPTEST_COL_PX && x0 + dx < GFX_SCREEN_W; dx++) {
            for (int y = 0; y < GFX_SCREEN_H; y++) {
                gfx_set_pixel(x0 + dx, y, true);
            }
        }
    }
}

// One `disptest step`/`seq` iteration: invert the n'th 8px screen column
// (full height) and fire ONE partial refresh. Prints the screen-x range and
// the native-row range gfx_set_pixel()'s x -> (GFX_FB_ROWS-1)-x mirror maps
// it to, so the log reads as "asked for x 16..23 => native rows 272..279"
// right next to disp.c's own "partial: native rows ..." line for the same
// request.
static void disptest_step(int n)
{
    int x = n * DISPTEST_COL_PX;
    int x_hi = x + DISPTEST_COL_PX - 1;
    int native_lo = (GFX_FB_ROWS - 1) - x_hi;
    int native_hi = (GFX_FB_ROWS - 1) - x;
    printf("disptest: step %d asked for x %d..%d => native rows %d..%d\n", n, x, x_hi, native_lo,
           native_hi);
    gfx_invert_rect(x, 0, DISPTEST_COL_PX, GFX_SCREEN_H);
    disp_partial_refresh();
}

static int cmd_disptest(int argc, char **argv)
{
    if (argc < 2 || strcmp(argv[1], "info") == 0) {
        printf("disptest: again=%d partial_count=%u dirty_rows=%d\n",
               (int) disp_partial_write_again(), (unsigned) disp_partial_count(),
               disp_dirty_rows());
        return 0;
    }
    if (strcmp(argv[1], "again") == 0) {
        if (argc != 3 || (strcmp(argv[2], "0") != 0 && strcmp(argv[2], "1") != 0)) {
            printf("usage: disptest again <0|1>\n");
            return 1;
        }
        disp_set_partial_write_again(strcmp(argv[2], "1") == 0);
        printf("disptest: again=%d\n", (int) disp_partial_write_again());
        return 0;
    }
    if (strcmp(argv[1], "bars") == 0) {
        if (argc != 2) {
            printf("usage: disptest bars\n");
            return 1;
        }
        disptest_paint_bars();
        disp_full_refresh();
        printf("disptest: bars painted, full refresh done\n");
        return 0;
    }
    if (strcmp(argv[1], "step") == 0) {
        if (argc != 3) {
            printf("usage: disptest step <n>\n");
            return 1;
        }
        char *end = NULL;
        long n = strtol(argv[2], &end, 10);
        if (!end || *end != '\0' || n < 0 || n >= DISPTEST_MAX_STEP) {
            printf("disptest: n must be 0..%d\n", DISPTEST_MAX_STEP - 1);
            printf("usage: disptest step <n>\n");
            return 1;
        }
        disptest_step((int) n);
        return 0;
    }
    if (strcmp(argv[1], "seq") == 0) {
        if (argc > 5) {
            printf("usage: disptest seq [n0] [n1] [ms]\n");
            return 1;
        }
        long n0 = 2, n1 = 12, ms = 1500;
        char *end = NULL;
        if (argc >= 3) {
            n0 = strtol(argv[2], &end, 10);
            if (!end || *end != '\0') {
                printf("usage: disptest seq [n0] [n1] [ms]\n");
                return 1;
            }
        }
        if (argc >= 4) {
            n1 = strtol(argv[3], &end, 10);
            if (!end || *end != '\0') {
                printf("usage: disptest seq [n0] [n1] [ms]\n");
                return 1;
            }
        }
        if (argc >= 5) {
            ms = strtol(argv[4], &end, 10);
            if (!end || *end != '\0') {
                printf("usage: disptest seq [n0] [n1] [ms]\n");
                return 1;
            }
        }
        if (n0 < 0 || n0 >= DISPTEST_MAX_STEP || n1 < 0 || n1 >= DISPTEST_MAX_STEP || n1 < n0) {
            printf("disptest: n0/n1 must be 0..%d with n1 >= n0\n", DISPTEST_MAX_STEP - 1);
            printf("usage: disptest seq [n0] [n1] [ms]\n");
            return 1;
        }
        if (n1 - n0 + 1 > DISPTEST_SEQ_MAX_COUNT) {
            n1 = n0 + DISPTEST_SEQ_MAX_COUNT - 1;
            printf("disptest: clamped to %d steps (n0=%ld n1=%ld)\n", DISPTEST_SEQ_MAX_COUNT, n0,
                   n1);
        }
        if (ms < DISPTEST_SEQ_MIN_MS) {
            ms = DISPTEST_SEQ_MIN_MS;
        } else if (ms > DISPTEST_SEQ_MAX_MS) {
            ms = DISPTEST_SEQ_MAX_MS;
        }
        printf("disptest: seq n=%ld..%ld, %ld ms apart\n", n0, n1, ms);
        for (long n = n0; n <= n1; n++) {
            disptest_step((int) n);
            if (n != n1) {
                vTaskDelay(pdMS_TO_TICKS(ms));
            }
        }
        return 0;
    }
    if (strcmp(argv[1], "full") == 0) {
        if (argc != 2) {
            printf("usage: disptest full\n");
            return 1;
        }
        disp_full_refresh();
        printf("disptest: full refresh done\n");
        return 0;
    }
    if (strcmp(argv[1], "swreset") == 0) {
        if (argc != 2) {
            printf("usage: disptest swreset\n");
            return 1;
        }
        disp_fault_inject_swreset();
        printf("disptest: swreset sent (SW reset only, no re-init) -- controller now on "
               "power-on register defaults\n");
        return 0;
    }
    printf("%s", DISPTEST_USAGE);
    return 1;
}

// docs/WIFI_TASKS.md W5: `wifi set|clear|on|off|status|scan`. Debug build
// only -- phase 1's manual selection policy (docs/WIFI_DESIGN.md §2/§3:
// "the console turns it on; nothing turns it on by itself") lives entirely
// here for now, not in wifi_policy.c (that module is pure/host-tested and
// has no caller yet -- see this task's own report).
#define WIFI_ON_IP_WAIT_MS 15000
#define WIFI_ON_MQTT_WAIT_MS 10000

static int wifi_do_set(int argc, char **argv)
{
    if (argc != 4) {
        printf("usage: wifi set <ssid> <psk>\n");
        return 1;
    }
    const char *ssid = argv[2];
    const char *psk = argv[3];
    size_t slen = strlen(ssid);
    size_t plen = strlen(psk);
    if (!wificred_valid_ssid(ssid, slen) || !wificred_valid_psk(psk, plen)) {
        printf("wifi set: invalid ssid (1-32 bytes, no embedded NUL) or psk (8-63 bytes)\n");
        return 1;
    }
    // Replace-wholesale (wificred.h): a console `wifi set` always carries
    // exactly the one network phase 1 uses -- any second network previously
    // stored via `cfg.wifi` is dropped, matching "the console is the
    // phase-1 switch" framing (docs/WIFI_DESIGN.md §2).
    wificred_candidate_t cand = { .ssid = ssid, .ssid_len = slen, .psk = psk, .psk_len = plen };
    wificred_set_t set;
    if (!wificred_apply_candidates(&cand, 1, &set) || !wificred_store(&set)) {
        printf("wifi set: failed to store\n");
        return 1;
    }
    printf("wifi: credentials stored for '%s' (psk <set>)\n", ssid);
    return 0;
}

static int wifi_do_on(void)
{
    if (net_xport_active() == NET_XPORT_WIFI) {
        printf("wifi: already on\n");
        return 0;
    }
    if (wificred_count() == 0) {
        printf("wifi: no credentials stored -- run `wifi set <ssid> <psk>` first\n");
        return 1;
    }
    if (catrust_get_state() != CATRUST_PINNED) {
        // Same sentence W7/W8's own 409 guard uses (docs/WIFI_DESIGN.md §5,
        // docs/WIFI_TASKS.md W8) -- kept identical so the console and the
        // web app never describe this refusal two different ways.
        printf("wifi: this device is not reporting a verified TLS connection; push a CA first\n");
        return 1;
    }
    if (!wifi_sta_start()) {
        printf("wifi: station failed to start (see log)\n");
        return 1;
    }
    if (!wifi_sta_associate()) {
        printf("wifi: association could not be queued (see log)\n");
        return 1;
    }
    int waited_ms = 0;
    while (!wifi_sta_got_ip() && waited_ms < WIFI_ON_IP_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(200)); // NEVER a tight busy-loop
        waited_ms += 200;
    }
    if (!wifi_sta_got_ip()) {
        printf("wifi: no IP after %ds (see log for association state)\n", WIFI_ON_IP_WAIT_MS / 1000);
        return 1;
    }

    net_xport_switch(NET_XPORT_WIFI);

    net_mqtt_status_t st = { 0 };
    waited_ms = 0;
    while (waited_ms < WIFI_ON_MQTT_WAIT_MS) {
        net_get_mqtt_status(&st);
        if (st.mqtt_connected) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited_ms += 200;
    }
    // Persisted regardless of whether the MQTT session reached SUBSCRIBED
    // in time -- `en` records the user's own choice (wificred.h), not
    // whether this attempt happened to succeed.
    wificred_set_enabled(true);

    if (!st.mqtt_connected) {
        printf("wifi: association/IP ok but MQTT did not reach SUBSCRIBED within %ds (see log)\n",
               WIFI_ON_MQTT_WAIT_MS / 1000);
        return 1;
    }

    int rssi = 0;
    wifi_sta_get_rssi(&rssi);
    char ip[16] = "";
    wifi_sta_get_ip(ip, sizeof(ip));
    printf("wifi: on (ip=%s rssi=%d, MQTT subscribed)\n", ip, rssi);
    // Orchestrator addendum to docs/WIFI_TASKS.md W0/W5: W0 never measured a
    // real heap watermark on this branch, so log it here instead, one INFO
    // line, now that the WiFi station + TLS + esp-mqtt session is actually
    // up -- W6's bench diffs this against modes_boot()'s own boot-time line.
    ESP_LOGI(TAG, "heap after WiFi+TLS+MQTT up: free=%u minimum_free=%u",
             (unsigned) esp_get_free_heap_size(), (unsigned) esp_get_minimum_free_heap_size());
    return 0;
}

static int wifi_do_off(void)
{
    wificred_set_enabled(false);
    if (net_xport_active() == NET_XPORT_WIFI) {
        net_xport_switch(NET_XPORT_LTE);
    }
    wifi_sta_disassociate();
    wifi_sta_stop(); // power effect: radio off, current returns to the LTE-only floor
    printf("wifi: off (LTE)\n");
    return 0;
}

static int wifi_do_status(void)
{
    wificred_net_t net;
    bool have_creds = wificred_get(0, &net);
    printf("wifi: %s, credentials %s\n", wificred_enabled() ? "enabled" : "disabled",
           have_creds ? "set" : "unset");
    if (have_creds) {
        printf("  ssid: %s\n", net.ssid); // never the psk (wificred.h's own hard rule)
    }
    printf("  associated: %s\n", wifi_sta_associated() ? "yes" : "no");
    int rssi = 0;
    if (wifi_sta_get_rssi(&rssi)) {
        printf("  rssi: %d dBm\n", rssi);
    }
    char ip[16] = "";
    printf("  ip: %s\n", wifi_sta_get_ip(ip, sizeof(ip)) ? ip : "(none)");
    int dtim = wifi_sta_get_dtim_period();
    if (dtim >= 0) {
        printf("  dtim: %d\n", dtim);
    } else {
        printf("  dtim: n/a (not exposed by esp_wifi's public API on this IDF version)\n");
    }
    printf("  transport: %s\n", net_xport_active() == NET_XPORT_WIFI ? "WIFI" : "LTE");
    return 0;
}

static int wifi_do_scan(void)
{
    if (!wifi_sta_start()) {
        printf("wifi scan: station failed to start (see log)\n");
        return 1;
    }
    wifi_sta_scan_result_t results[16];
    int n = wifi_sta_scan(results, (int) (sizeof(results) / sizeof(results[0])));
    if (n < 0) {
        printf("wifi scan: failed (see log)\n");
        return 1;
    }
    printf("wifi scan: %d network(s)\n", n);
    for (int i = 0; i < n; i++) {
        printf("  %-32s rssi=%4d ch=%2u auth=%u\n", results[i].ssid, (int) results[i].rssi,
               (unsigned) results[i].channel, (unsigned) results[i].authmode);
    }
    return 0;
}

static int cmd_wifi(int argc, char **argv)
{
    static const char *USAGE =
        "usage: wifi set <ssid> <psk> | wifi clear | wifi on | wifi off | wifi status | wifi scan\n";
    if (argc < 2) {
        printf("%s", USAGE);
        return 1;
    }
    if (strcmp(argv[1], "set") == 0) {
        return wifi_do_set(argc, argv);
    }
    if (strcmp(argv[1], "clear") == 0) {
        wificred_clear();
        printf("wifi: credentials cleared\n");
        return 0;
    }
    if (strcmp(argv[1], "on") == 0) {
        return wifi_do_on();
    }
    if (strcmp(argv[1], "off") == 0) {
        return wifi_do_off();
    }
    if (strcmp(argv[1], "status") == 0) {
        return wifi_do_status();
    }
    if (strcmp(argv[1], "scan") == 0) {
        return wifi_do_scan();
    }
    printf("%s", USAGE);
    return 1;
}

// docs/WIFI_DESIGN.md §9, docs/WIFI_TASKS.md W5's own last step (run before
// W6): does a manual esp_light_sleep_start() keep a WiFi association (and
// its MQTT session) alive on this IDF if esp_sleep_enable_wifi_wakeup() is
// armed and ESP_PD_DOMAIN_MODEM is left powered? The answer decides whether
// W9 (CONFIG_PM_ENABLE) is needed at all (§9: "if yes ... the whole
// PM_ENABLE risk evaporates"). Requires `wifi on` to have already brought a
// WiFi MQTT session up to SUBSCRIBED. This is the SECOND esp_light_sleep_start()
// call site in this tree (net.cpp's net_sleep() being the first) --
// deliberately: it is a temporary bench experiment gated the same way
// nettest/mqtttest are (PAGER_DEBUG_NO_LIGHT_SLEEP, "remove once
// root-caused"/here, once §9 is answered), not a second production sleep
// path.
static int cmd_wifisleeptest(int argc, char **argv)
{
    (void) argv;
    if (argc != 1) {
        printf("usage: wifisleeptest -- no arguments; run `wifi on` first\n");
        return 1;
    }
    if (net_xport_active() != NET_XPORT_WIFI) {
        printf("wifisleeptest: WiFi is not the active transport -- run `wifi on` first\n");
        return 1;
    }
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    if (!st.mqtt_connected) {
        printf("wifisleeptest: the WiFi MQTT session is not SUBSCRIBED yet -- check `wifi status` "
               "and retry\n");
        return 1;
    }

    modes_publish_status_now();
    vTaskDelay(pdMS_TO_TICKS(500)); // let that publish clear before the first sleep cycle

    esp_sleep_enable_wifi_wakeup();
    esp_sleep_pd_config(ESP_PD_DOMAIN_MODEM, ESP_PD_OPTION_ON);

    int survived = 0;
    for (int cycle = 1; cycle <= 10; cycle++) {
        if (cycle == 6) {
            printf("wifisleeptest: cycle 6 -- send a page from the relay now\n");
        }
        esp_sleep_enable_timer_wakeup(5ULL * 1000000ULL);
        esp_light_sleep_start();

        wifi_ap_record_t rec;
        esp_err_t ap_err = esp_wifi_sta_get_ap_info(&rec);
        net_get_mqtt_status(&st);
        printf("wifisleeptest: cycle %d: ap_info=%s rssi=%d mqtt_connected=%d\n", cycle,
               (ap_err == ESP_OK) ? "OK" : "FAIL", (ap_err == ESP_OK) ? (int) rec.rssi : 0,
               (int) st.mqtt_connected);
        if (ap_err == ESP_OK && st.mqtt_connected) {
            survived++;
        }
    }
    printf("wifisleeptest: %d/10 cycles kept the association and the MQTT session up\n", survived);
    printf("wifisleeptest: check the relay/EMQX log for whether the cycle-6 page arrived\n");
    return (survived == 10) ? 0 : 1;
}

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

    const esp_console_cmd_t sleeptest_cmd = {
        .command = "sleeptest",
        .help = "sleeptest [<minutes> [yield_ms] [interval_ms] [probe_wait_ms]] -- really light-sleep for a "
                 "while (optionally overriding the post-wake yield and the wake interval), "
                 "then report what arrived",
        .hint = NULL,
        .func = &cmd_sleeptest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&sleeptest_cmd));

    const esp_console_cmd_t at_cmd = {
        .command = "at",
        .help = "at <command> -- send one raw AT command; the reply shows in the AT trace",
        .hint = NULL,
        .func = &cmd_at,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&at_cmd));

    register_carrier_cmd();
    register_input_cmds();

    const esp_console_cmd_t gnsstest_cmd = {
        .command = "gnsstest",
        .help = "gnsstest <seconds> -- one location-fix attempt, bypassing backoff/battery floor",
        .hint = NULL,
        .func = &cmd_gnsstest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&gnsstest_cmd));

    const esp_console_cmd_t coverage_cmd = {
        .command = "coverage",
        .help = "coverage -- print the coverage duty-cycle policy's state (registered/dark-for/"
                "phase/step/next-action)",
        .hint = NULL,
        .func = &cmd_coverage,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&coverage_cmd));

    const esp_console_cmd_t cafetch_cmd = {
        .command = "cafetch",
        .help = "cafetch <url> <sha256hex> -- fetch-only CA-over-HTTPS probe (V02_DESIGN.md §4.4), no apply",
        .hint = NULL,
        .func = &cmd_cafetch,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&cafetch_cmd));

    const esp_console_cmd_t smstest_cmd = {
        .command = "smstest",
        .help = "smstest <number> <text> -- send one SMS bypassing the allow-list (V02_DESIGN.md §6)",
        .hint = NULL,
        .func = &cmd_smstest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&smstest_cmd));

    const esp_console_cmd_t smslist_cmd = {
        .command = "smslist",
        .help = "smslist -- print the SMS allow-list and audit queue depth",
        .hint = NULL,
        .func = &cmd_smslist,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&smslist_cmd));

    const esp_console_cmd_t acceltest_cmd = {
        .command = "acceltest",
        .help = "acceltest [samples <n>|ths <0-127>|dur <0-127>|refr <seconds>] -- LIS3DH "
                 "register/sample dump and runtime tuning (docs/DEVICE_NEXT_TASKS.md A2); run "
                 "`acceltest` with no args for the full usage",
        .hint = NULL,
        .func = &cmd_acceltest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&acceltest_cmd));

    const esp_console_cmd_t disptest_cmd = {
        .command = "disptest",
        .help = "disptest [info|again <0|1>|bars|step <n>|seq [n0] [n1] [ms]|full|swreset] -- "
                 "deterministic e-paper partial-refresh bench harness (docs task-disp-fix.md); "
                 "run `disptest` with no args for the full usage. NOTE: do not use `wake` or "
                 "`key` while this is running -- the UI render task would repaint over the test "
                 "pattern.",
        .hint = NULL,
        .func = &cmd_disptest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&disptest_cmd));

    const esp_console_cmd_t wifi_cmd = {
        .command = "wifi",
        .help = "wifi set <ssid> <psk> | clear | on | off | status | scan -- docs/WIFI_TASKS.md "
                 "W5: manual WiFi transport control (never turns on by itself; `wifi status` "
                 "never prints the psk)",
        .hint = NULL,
        .func = &cmd_wifi,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifi_cmd));

    const esp_console_cmd_t wifisleeptest_cmd = {
        .command = "wifisleeptest",
        .help = "wifisleeptest -- docs/WIFI_DESIGN.md §9 experiment: 10x 5s light-sleep cycles "
                 "with the WiFi association held, reporting whether it (and the MQTT session) "
                 "survive each one. Requires `wifi on` first.",
        .hint = NULL,
        .func = &cmd_wifisleeptest,
    };
    ESP_ERROR_CHECK(esp_console_cmd_register(&wifisleeptest_cmd));

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGD(TAG, "debug console REPL started in normal mode (PAGER_DEBUG_NO_LIGHT_SLEEP)");
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "school_pager boot");
    watchdog_boot(); // logs why we reset and where the loop was; arms the RTC watchdog

    // Owner request, beta feedback 2026-09-23: flag a crash/watchdog/brownout
    // reset in the status bar until the first key press (ui.c's
    // GFX_ICON_CRASH slot) -- set here, before ui_init() even runs, since the
    // flag only gates a render-time icon, not any ui.c init ordering. No
    // modem or sleep-state effect.
    if (watchdog_last_reset_was_crash()) {
        ESP_LOGI(TAG, "boot crash indicator: %s", watchdog_last_reset_reason_str());
        ui_set_crash_indicator(true);
    }

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

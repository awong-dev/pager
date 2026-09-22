#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/i2c.h"

#include <stdio.h>

#include "catrust.h"
#include "ident.h"
#include "watchdog.h"
#include "carrier.h"
#include "input.h"
#include "loc.h"
#include "modes.h"
#include "net.h"
#include "pins.h"
#include "setup.h"
#include "sms.h"
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

#ifdef PAGER_DEBUG_NO_LIGHT_SLEEP
// Debug build only: raw AT passthrough.
// Debug build only: `sleeptest <minutes> [yield_ms] [interval_ms]` opens a
// window of real light sleep (modes.c); `sleeptest` alone prints the report
// again. yield_ms overrides the post-wake yield (default
// PAGER_POST_WAKE_YIELD_MS, 50); interval_ms overrides the wake interval
// (default: 2000/5000 by mode) -- task 3's "how long must the pager stay
// awake after a wake to receive a held URC" question.
static int cmd_sleeptest(int argc, char **argv)
{
    if (argc == 1) {
        modes_debug_sleeptest_report();
        return 0;
    }
    long m = strtol(argv[1], NULL, 10);
    if (m < 1 || m > 120) {
        printf("usage: sleeptest [<minutes 1..120> [yield_ms 30..10000] [interval_ms 200..60000]]\n");
        return 1;
    }
    long yield_ms = 0;    // 0 = use PAGER_POST_WAKE_YIELD_MS
    long interval_ms = 0; // 0 = use the normal active/sleep interval
    if (argc >= 3) {
        yield_ms = strtol(argv[2], NULL, 10);
        if (yield_ms < 30 || yield_ms > 10000) {
            printf("usage: sleeptest <minutes> [yield_ms 30..10000] [interval_ms 200..60000]\n");
            return 1;
        }
    }
    if (argc >= 4) {
        interval_ms = strtol(argv[3], NULL, 10);
        if (interval_ms < 200 || interval_ms > 60000) {
            printf("usage: sleeptest <minutes> [yield_ms] [interval_ms 200..60000]\n");
            return 1;
        }
    }
    modes_debug_sleeptest_start((uint32_t) m, (uint32_t) yield_ms, (uint32_t) interval_ms);
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
        .help = "sleeptest [<minutes> [yield_ms] [interval_ms]] -- really light-sleep for a "
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

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "debug console REPL started in normal mode (PAGER_DEBUG_NO_LIGHT_SLEEP)");
}
#endif

void app_main(void)
{
    ESP_LOGI(TAG, "school_pager boot");
    watchdog_boot(); // logs why we reset and where the loop was; arms the RTC watchdog

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

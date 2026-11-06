/* test_fuel.c -- host test for main/fuel_fmt.c. No ESP-IDF. */
#include "fuel_fmt.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void line(int mv, int pct, fuel_state_t s, const char *want)
{
    char b[48];
    fuel_fmt_line(mv, pct, s, b, sizeof(b));
    CHECK(strcmp(b, want) == 0, "line %d %d %d -> \"%s\" want \"%s\"", mv, pct, (int) s, b, want);
}

int main(void)
{
    /* VCELL: 78.125 uV/LSB. 0xC000 = 49152 -> 3840 mV; 0xB000 = 45056 -> 3520 mV; 0xD1B7 -> 4.1 V-ish. */
    CHECK(fuel_mv_from_vcell(49152) == 3840, "vcell 49152 -> %d", fuel_mv_from_vcell(49152));
    CHECK(fuel_mv_from_vcell(45056) == 3520, "vcell 45056 -> %d", fuel_mv_from_vcell(45056));
    CHECK(fuel_mv_from_vcell(0) == 0, "vcell 0");
    CHECK(fuel_mv_from_vcell(65535) == 5120, "vcell max -> %d", fuel_mv_from_vcell(65535)); /* 5119.99 */
    CHECK(fuel_mv_from_vcell(7) == 1, "vcell 7 -> %d", fuel_mv_from_vcell(7));            /* 0.547 mV rounds to 1 */

    /* SOC: 1/256 % per LSB, rounded. */
    CHECK(fuel_pct_from_soc(0) == 0, "soc 0");
    CHECK(fuel_pct_from_soc(127) == 0, "soc 127");
    CHECK(fuel_pct_from_soc(128) == 1, "soc 128");
    CHECK(fuel_pct_from_soc(72 * 256) == 72, "soc 72");
    CHECK(fuel_pct_from_soc(72 * 256 + 127) == 72, "soc 72.49");
    CHECK(fuel_pct_from_soc(72 * 256 + 128) == 73, "soc 72.5");
    CHECK(fuel_pct_from_soc(100 * 256) == 100, "soc 100");
    CHECK(fuel_pct_from_soc(0xFFFF) == 100, "soc clamp");

    CHECK(fuel_crate_milli_from_raw(1) == 208, "crate +1");
    CHECK(fuel_crate_milli_from_raw(-5) == -1040, "crate -5");

    /* Charger: VBUS_DET high = plugged, STAT1 low = charging. */
    CHECK(fuel_state_from_pins(0, 1) == FUEL_ON_BATTERY, "unplugged, stat1 high");
    CHECK(fuel_state_from_pins(0, 0) == FUEL_ON_BATTERY, "unplugged, stat1 low");
    CHECK(fuel_state_from_pins(1, 0) == FUEL_CHARGING, "plugged, stat1 low");
    CHECK(fuel_state_from_pins(1, 1) == FUEL_CHARGED, "plugged, stat1 high");
    CHECK(strcmp(fuel_state_name(FUEL_CHARGING), "charging") == 0, "name charging");
    CHECK(strcmp(fuel_state_name(FUEL_CHARGED), "charged") == 0, "name charged");
    CHECK(strcmp(fuel_state_name(FUEL_ON_BATTERY), "battery") == 0, "name battery");

    /* Icon segments: >75/50/25/10. */
    CHECK(fuel_icon_segs(100, FUEL_ON_BATTERY) == 4, "seg 100");
    CHECK(fuel_icon_segs(76, FUEL_ON_BATTERY) == 4, "seg 76");
    CHECK(fuel_icon_segs(75, FUEL_ON_BATTERY) == 3, "seg 75");
    CHECK(fuel_icon_segs(51, FUEL_ON_BATTERY) == 3, "seg 51");
    CHECK(fuel_icon_segs(50, FUEL_ON_BATTERY) == 2, "seg 50");
    CHECK(fuel_icon_segs(26, FUEL_ON_BATTERY) == 2, "seg 26");
    CHECK(fuel_icon_segs(25, FUEL_ON_BATTERY) == 1, "seg 25");
    CHECK(fuel_icon_segs(11, FUEL_ON_BATTERY) == 1, "seg 11");
    CHECK(fuel_icon_segs(10, FUEL_ON_BATTERY) == 0, "seg 10");
    CHECK(fuel_icon_segs(0, FUEL_ON_BATTERY) == 0, "seg 0");
    CHECK(fuel_icon_segs(-1, FUEL_ON_BATTERY) == -1, "seg unknown");
    CHECK(fuel_icon_segs(-1, FUEL_CHARGED) == 4, "seg charged without pct");
    CHECK(fuel_icon_segs(40, FUEL_CHARGED) == 4, "seg charged");
    CHECK(fuel_icon_segs(40, FUEL_CHARGING) == 2, "seg charging shows the percent");

    /* Shutdown rule: voltage only; two consecutive low periodic reads; boot threshold 3550. */
    CHECK(!fuel_should_shutdown(3450, false, 1, false), "first low read does not shut down");
    CHECK(fuel_should_shutdown(3450, false, 2, false), "second consecutive low read shuts down");
    CHECK(fuel_should_shutdown(3300, false, 3, false), "well below, third read");
    CHECK(!fuel_should_shutdown(3451, false, 2, false), "3451 mV is above the running threshold");
    CHECK(!fuel_should_shutdown(3450, true, 5, false), "never while plugged in");
    CHECK(!fuel_should_shutdown(0, false, 5, false), "failed read (0 mV) never shuts down");
    CHECK(!fuel_should_shutdown(-1, false, 5, true), "negative mv never shuts down");
    CHECK(fuel_should_shutdown(3550, false, 0, true), "boot: 3550 shuts down, no count needed");
    CHECK(fuel_should_shutdown(3400, false, 0, true), "boot: 3400 shuts down");
    CHECK(!fuel_should_shutdown(3551, false, 0, true), "boot: 3551 does not");
    CHECK(!fuel_should_shutdown(3500, true, 0, true), "boot: plugged in never");
    CHECK(!fuel_should_shutdown(3500, false, 5, false), "running at 3500 (between thresholds) does not shut down");

    line(3870, 72, FUEL_CHARGING, "3.87 V 72% charging");
    line(3870, 72, FUEL_ON_BATTERY, "3.87 V 72%");
    line(4200, 100, FUEL_CHARGED, "4.20 V 100% charged");
    line(3870, -1, FUEL_CHARGING, "3.87 V charging");
    line(0, 50, FUEL_ON_BATTERY, "--");

    if (g_failures == 0) {
        printf("test_fuel: all passed\n");
        return 0;
    }
    printf("test_fuel: %d failure(s)\n", g_failures);
    return 1;
}

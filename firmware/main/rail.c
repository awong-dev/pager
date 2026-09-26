// rail.c — see rail.h for the policy this module implements and why.

#include "rail.h"

#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pins.h"
#include "ui.h" /* ui_kb_bus_release()/ui_kb_bus_restore(): stop back-powering the
                  * CardKB (and LIS3DH) through the I2C pull-ups while the rail is
                  * off, owner 24 Sep 11:15 pm PDT finding, see ui.h's own comment */

static bool s_rail_on = false;
static int64_t s_restored_us = 0;

// Round 9 (the "6s from tap to password: on a real sleep wake" defect):
// universal settle delay between the 3V3 rail actually coming up and this
// module touching ANY downstream peripheral (display, CardKB/I2C, LIS3DH --
// pins.h has the full list on this rail). Owner ruling, 26 Sep: mandatory,
// applied once here rather than as a per-peripheral fix, since every
// peripheral on this rail needs its own supply to have actually risen
// before its first command/read, not just the display. Datasheet minimums
// found (firmware/README.md's own PENDING_HW caveat applies -- no live
// datasheet fetch from this bench, see the round 9 report for the full
// sourcing/caveat):
//   - SSD1680 (display): "wait >=10ms after VCI up" before RST.
//   - LIS3DH (accel): datasheet-cited boot time ~5ms after power-up
//     (from training-data recollection of the electrical characteristics
//     table, NOT re-verified against a fetched PDF on this bench).
//   - CardKB (M5Stack unit, ATmega8A): the binding constraint -- factory
//     default AVR fuses (internal RC, long startup) are commonly cited at
//     14 CK + 65ms from a cold power-up; the bench's own I2C-ACK-timing
//     measurement (`kbtime`, main.c) is the authoritative number actually
//     used to pick this value, per the owner's own "measure it if the fuse
//     can't be determined" fallback.
// Starting value 15ms per owner instruction; swept upward only (15/20/30/
// 50ms) against 5 real off-sleep wakes -- see the round 9 report for which
// value was kept. rail_debug_set_settle_ms() exists so that sweep runs on
// one flash, not five separate builds.
#define PAGER_RAIL_SETTLE_MS_DEFAULT 15
static uint32_t s_settle_ms = PAGER_RAIL_SETTLE_MS_DEFAULT;

void rail_debug_set_settle_ms(uint32_t ms) { s_settle_ms = ms; }

// Round 10: CONFIG_FREERTOS_HZ=100 -- pdMS_TO_TICKS(15) is 1 tick, and
// vTaskDelay(1) is 0..10 ms depending on tick phase (random after a real
// light-sleep wake). Round up and add one tick so the settle is never
// shorter than asked.
static void rail_delay_at_least_ms(uint32_t ms)
{
    vTaskDelay((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1);
}

// Round 10: the display control lines RST/DC/CS are excluded from sleep
// isolation (disp.c disp_gpio_init()) and held HIGH through sleep, which
// back-powers the unpowered SSD1680 (and, through the panel's VCC_EN
// P-FET, the rest of the rail) via its I/O clamp diodes -- the same defect
// the I2C lines had. During a rail-off sleep only, hand these pads (plus
// MOSI/SCLK) to the sleep pad config: input with pull-down, so nothing
// sources current into the panel. The hardware switches back to the normal
// (driven / SPI) config automatically at wake; rail_on() re-excludes
// RST/DC/CS so attentive (rail-on) sleeps hold them exactly as before.
static const gpio_num_t s_disp_bus_pins[] = {
    (gpio_num_t) PAGER_PIN_DISP_RST, (gpio_num_t) PAGER_PIN_DISP_DC,
    (gpio_num_t) PAGER_PIN_DISP_CS, (gpio_num_t) PAGER_PIN_DISP_MOSI,
    (gpio_num_t) PAGER_PIN_DISP_SCK,
};

static void rail_disp_bus_release(void)
{
    for (size_t i = 0; i < sizeof(s_disp_bus_pins) / sizeof(s_disp_bus_pins[0]); i++) {
        gpio_sleep_set_direction(s_disp_bus_pins[i], GPIO_MODE_INPUT);
        gpio_sleep_set_pull_mode(s_disp_bus_pins[i], GPIO_PULLDOWN_ONLY);
        gpio_sleep_sel_en(s_disp_bus_pins[i]);
    }
}

static void rail_disp_bus_restore(void)
{
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_RST);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_DC);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_CS);
}

uint32_t rail_debug_get_settle_ms(void) { return s_settle_ms; }

void rail_init(void)
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
    // Not otherwise excluded from ESP-IDF's sleep GPIO isolation
    // (CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND) -- without this the pad
    // floats through every light sleep and the board pull-up can switch the
    // rail off regardless of what rail_on()/rail_off() last drove (the 24
    // Sep finding, docs/ROADMAP.md). rail_off() below relies on this too:
    // an OFF level must hold through sleep exactly as reliably as an ON one.
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_3V3_EN);
    s_rail_on = true;
    s_restored_us = esp_timer_get_time(); // see rail.h: counts as a restore edge
}

void rail_on(void)
{
    if (s_rail_on) {
        return; // already on: no power edge, do not re-arm the CardKB boot guard
    }
    gpio_set_level(PAGER_PIN_3V3_EN, 0); // active-low: enable the rail
    // Round 9: universal settle before touching ANYTHING downstream (see
    // s_settle_ms's own comment above) -- must come before ui_kb_bus_
    // restore() below, not after, so the I2C bus is not driven while the
    // CardKB's own supply is still rising.
    rail_delay_at_least_ms(s_settle_ms);
    // After the enable+settle, not before: the CardKB MCU is only booting
    // once the rail is actually live, and ui_kb_bus_restore() just returns
    // the bus to I2C mode (the PAGER_KB_BOOT_GUARD_MS guard, ui.c, still
    // withholds the first read on top of this). Power effect: none of its
    // own -- the rail edge above is what powers the CardKB/LIS3DH back up.
    ui_kb_bus_restore();
    rail_disp_bus_restore();
    s_rail_on = true;
    s_restored_us = esp_timer_get_time();
}

void rail_off(void)
{
    if (!s_rail_on) {
        return;
    }
    // Before dropping the rail: stop driving the I2C pull-ups' idle-high
    // level so the CardKB (and LIS3DH, which shares this bus and rail) are
    // not phantom-powered through their I/O protection diodes for the
    // sleep about to be entered (owner, 24 Sep 11:15 pm PDT). Power effect:
    // see ui_kb_bus_release()'s own comment.
    ui_kb_bus_release();
    rail_disp_bus_release();
    gpio_set_level(PAGER_PIN_3V3_EN, 1); // active-low: disable the rail
    s_rail_on = false;
}

bool rail_is_on(void) { return s_rail_on; }

int64_t rail_restored_us(void) { return s_restored_us; }

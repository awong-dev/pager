// rail.c — see rail.h for the policy this module implements and why.

#include "rail.h"

#include "driver/gpio.h"
#include "esp_timer.h"

#include "pins.h"
#include "ui.h" /* ui_kb_bus_release()/ui_kb_bus_restore(): stop back-powering the
                  * CardKB (and LIS3DH) through the I2C pull-ups while the rail is
                  * off, owner 24 Sep 11:15 pm PDT finding, see ui.h's own comment */

static bool s_rail_on = false;
static int64_t s_restored_us = 0;

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
    // After the enable, not before: the CardKB MCU is only booting once the
    // rail is actually live, and ui_kb_bus_restore() just returns the bus to
    // I2C mode (the PAGER_KB_BOOT_GUARD_MS guard, ui.c, still withholds the
    // first read). Power effect: none of its own -- the rail edge above is
    // what powers the CardKB/LIS3DH back up.
    ui_kb_bus_restore();
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
    gpio_set_level(PAGER_PIN_3V3_EN, 1); // active-low: disable the rail
    s_rail_on = false;
}

bool rail_is_on(void) { return s_rail_on; }

int64_t rail_restored_us(void) { return s_restored_us; }

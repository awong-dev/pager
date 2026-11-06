/* fuel.c -- see fuel.h. */
#include "fuel.h"

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/rtc_io.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "accel.h"
#include "board.h"
#include "pins.h"

static const char *TAG = "fuel";

#define MAX_REG_VCELL 0x02
#define MAX_REG_SOC 0x04
#define MAX_REG_MODE 0x06
#define MAX_REG_VERSION 0x08
#define MAX_REG_CRATE 0x16
#define FUEL_I2C_TO pdMS_TO_TICKS(50)

bool fuel_reg_read(uint8_t reg, uint16_t *val)
{
    uint8_t b[2] = {0, 0};
    accel_bus_lock();
    esp_err_t e = i2c_master_write_read_device(I2C_NUM_1, PAGER_I2C_ADDR_MAX17048, &reg, 1, b, 2, FUEL_I2C_TO);
    accel_bus_unlock();
    if (e != ESP_OK) {
        return false;
    }
    *val = (uint16_t) ((b[0] << 8) | b[1]); // MAX17048 registers are big-endian
    return true;
}

bool fuel_read(fuel_t *out)
{
    out->ok = false;
    uint16_t v, s, c;
    if (!fuel_reg_read(MAX_REG_VCELL, &v) || !fuel_reg_read(MAX_REG_SOC, &s) || !fuel_reg_read(MAX_REG_CRATE, &c)) {
        return false;
    }
    out->mv = fuel_mv_from_vcell(v);
    out->pct = fuel_pct_from_soc(s);
    out->crate_milli = fuel_crate_milli_from_raw((int16_t) c);
    out->ok = true;
    return true;
}

void fuel_pins(int *vbus, int *stat1, int *stat2)
{
    *vbus = gpio_get_level((gpio_num_t) PAGER_PIN_VBUS_DET);
    *stat1 = gpio_get_level((gpio_num_t) PAGER_PIN_CHG_STAT1);
    *stat2 = gpio_get_level((gpio_num_t) PAGER_PIN_CHG_STAT2);
}

fuel_state_t fuel_state(void)
{
    return fuel_state_from_pins(gpio_get_level((gpio_num_t) PAGER_PIN_VBUS_DET),
                                gpio_get_level((gpio_num_t) PAGER_PIN_CHG_STAT1));
}

bool fuel_quick_start(void)
{
    uint8_t w[3] = {MAX_REG_MODE, 0x40, 0x00}; // MODE bit 14
    accel_bus_lock();
    esp_err_t e = i2c_master_write_to_device(I2C_NUM_1, PAGER_I2C_ADDR_MAX17048, w, sizeof(w), FUEL_I2C_TO);
    accel_bus_unlock();
    return e == ESP_OK;
}

void fuel_init(void)
{
    if (!board_has_fuel_gauge()) {
        return;
    }
    // A Shutdown deep sleep woke on IO15 (ext1): IDF leaves a reset-surviving pad HOLD that makes
    // gpio_get_level() read 1 (memory ext1-rtc-pad-hold). Release it before reading the pin.
    rtc_gpio_hold_dis((gpio_num_t) PAGER_PIN_VBUS_DET);
    rtc_gpio_deinit((gpio_num_t) PAGER_PIN_VBUS_DET);
    const gpio_num_t pins[3] = {(gpio_num_t) PAGER_PIN_VBUS_DET, (gpio_num_t) PAGER_PIN_CHG_STAT1,
                                (gpio_num_t) PAGER_PIN_CHG_STAT2};
    for (int i = 0; i < 3; i++) {
        gpio_config_t g = {
            .pin_bit_mask = 1ULL << pins[i],
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&g);
    }
    uint16_t ver;
    if (fuel_reg_read(MAX_REG_VERSION, &ver)) {
        ESP_LOGI(TAG, "MAX17048 VERSION 0x%04x%s", (unsigned) ver, ((ver & 0xFFF0) == 0x0010) ? "" : " (unexpected, expected 0x001x)");
    } else {
        ESP_LOGW(TAG, "MAX17048 not answering at 0x%02x", PAGER_I2C_ADDR_MAX17048);
    }
}

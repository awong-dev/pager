/* board.c -- see board.h. Exists on both boards; decides at runtime which one this is.
 * Power effect: board_init() is one I2C read + at most one NVS write per boot. */

#include "board.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs.h"

#include "accel.h"
#include "pins.h"

static const char *TAG = "board";

#define BOARD_NVS_NS "board"
#define BOARD_WHO_BREADBOARD 0x33
#define BOARD_WHO_CARRIER 0x11

static board_kind_t s_kind = BOARD_BREADBOARD;
static board_src_t s_src = BOARD_SRC_DEFAULT;
static uint8_t s_who = 0;
static bool s_who_ok = false;

static uint8_t board_nvs_get(const char *key)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(BOARD_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, key, &v) != ESP_OK) {
            v = 0;
        }
        nvs_close(h);
    }
    return v;
}

static bool board_nvs_put(const char *key, uint8_t v)
{
    nvs_handle_t h;
    if (nvs_open(BOARD_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u8(h, key, v) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

// Power effect: one I2C read, at most one NVS write.
void board_init(void)
{
    uint8_t kind = board_nvs_get("kind");
    uint8_t last = board_nvs_get("last");
    s_who_ok = accel_probe_id(&s_who);

    if (kind == BOARD_BREADBOARD || kind == BOARD_CARRIER_V02) {
        s_kind = (board_kind_t) kind;
        s_src = BOARD_SRC_FIXED;
    } else if (s_who_ok && (s_who == BOARD_WHO_BREADBOARD || s_who == BOARD_WHO_CARRIER)) {
        s_kind = s_who == BOARD_WHO_CARRIER ? BOARD_CARRIER_V02 : BOARD_BREADBOARD;
        s_src = BOARD_SRC_AUTO;
        if (last != (uint8_t) s_kind) {
            board_nvs_put("last", (uint8_t) s_kind);
        }
    } else if (last == BOARD_BREADBOARD || last == BOARD_CARRIER_V02) {
        s_kind = (board_kind_t) last;
        s_src = BOARD_SRC_LAST;
    } else {
        s_kind = BOARD_BREADBOARD;
        s_src = BOARD_SRC_DEFAULT;
        ESP_LOGW(TAG, "accelerometer WHO_AM_I unreadable and no history: assuming breadboard");
    }
    ESP_LOGI(TAG, "board %s (%s), WHO_AM_I %s0x%02x", board_name(), board_source_name(),
             s_who_ok ? "" : "(read failed) ", (unsigned) s_who);
}

board_kind_t board_get(void) { return s_kind; }
const char *board_name(void) { return s_kind == BOARD_CARRIER_V02 ? "carrier-v0.2" : "breadboard"; }
board_src_t board_source(void) { return s_src; }

const char *board_source_name(void)
{
    switch (s_src) {
    case BOARD_SRC_FIXED: return "fixed";
    case BOARD_SRC_AUTO: return "auto";
    case BOARD_SRC_LAST: return "last";
    default: return "default";
    }
}

bool board_is_carrier(void) { return s_kind == BOARD_CARRIER_V02; }

bool board_who_am_i(uint8_t *who)
{
    if (s_who_ok) {
        *who = s_who;
    }
    return s_who_ok;
}

int board_disp_vcc_en_pin(void) { return board_is_carrier() ? -1 : PAGER_PIN_DISP_VCC_EN; }
int board_disp_srcs_pin(void) { return board_is_carrier() ? -1 : PAGER_PIN_DISP_SRCS; }
int board_disp_miso_pin(void) { return board_is_carrier() ? -1 : PAGER_PIN_DISP_MISO; }

bool board_has_haptic(void) { return board_is_carrier(); }
bool board_has_piezo(void) { return board_is_carrier(); }
bool board_has_frontlight(void) { return board_is_carrier(); }
bool board_has_ext_led(void) { return board_is_carrier(); }
bool board_has_fuel_gauge(void) { return board_is_carrier(); }

board_kind_t board_stored_kind(void) { return (board_kind_t) board_nvs_get("kind"); }

// Power effect: one NVS write; the new kind applies after a reboot.
bool board_set_kind(board_kind_t k) { return board_nvs_put("kind", (uint8_t) k); }

void board_read_charger_pins(int *vbus, int *stat1, int *stat2)
{
    const gpio_num_t p[3] = { (gpio_num_t) PAGER_PIN_VBUS_DET, (gpio_num_t) PAGER_PIN_CHG_STAT1,
                              (gpio_num_t) PAGER_PIN_CHG_STAT2 };
    int v[3];
    for (int i = 0; i < 3; i++) {
        gpio_set_direction(p[i], GPIO_MODE_INPUT);
        v[i] = gpio_get_level(p[i]);
    }
    *vbus = v[0];
    *stat1 = v[1];
    *stat2 = v[2];
}

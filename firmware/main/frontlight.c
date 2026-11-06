/* frontlight.c -- see frontlight.h. LEDC timer 1, channel 2, 2 kHz, 10-bit, on IO12. Off = duty 0,
 * channel stopped with idle level low, pin kept low through light sleep. */
#include "frontlight.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "pins.h"

static const char *TAG = "light";

#define FL_MODE LEDC_LOW_SPEED_MODE
#define FL_TIMER LEDC_TIMER_1
#define FL_CH LEDC_CHANNEL_2

static SemaphoreHandle_t s_mtx;
static StaticSemaphore_t s_mtx_buf;
static esp_timer_handle_t s_idle_timer;
static bool s_inited, s_on, s_configured;
static uint8_t s_pct = FRONTLIGHT_DEFAULT_PCT;

static void idle_cb(void *arg)
{
    (void) arg;
    frontlight_off(); /* esp_timer task */
}

static void init_once(void)
{
    if (s_inited) {
        return;
    }
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    const esp_timer_create_args_t ta = {.callback = idle_cb, .name = "fl_idle"};
    esp_timer_create(&ta, &s_idle_timer);
    nvs_handle_t h;
    if (nvs_open("board", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "fl_duty", &v) == ESP_OK && v >= 1 && v <= 100) {
            s_pct = v;
        }
        nvs_close(h);
    }
    gpio_config_t cfg = {.pin_bit_mask = 1ULL << PAGER_PIN_FL_PWM, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&cfg);
    gpio_set_level(PAGER_PIN_FL_PWM, 0);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_FL_PWM); /* stay low through light sleep */
    s_inited = true;
}

static void apply_locked(void)
{
    uint32_t duty = ((uint32_t) s_pct * 1023u) / 100u;
    if (!s_configured) {
        ledc_timer_config_t tc = {
            .speed_mode = FL_MODE,
            .duty_resolution = LEDC_TIMER_10_BIT,
            .timer_num = FL_TIMER,
            .freq_hz = 2000,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        ledc_timer_config(&tc);
        ledc_channel_config_t cc = {
            .gpio_num = PAGER_PIN_FL_PWM,
            .speed_mode = FL_MODE,
            .channel = FL_CH,
            .timer_sel = FL_TIMER,
            .duty = duty,
            .hpoint = 0,
        };
        ledc_channel_config(&cc);
        s_configured = true;
    } else {
        ledc_set_duty(FL_MODE, FL_CH, duty);
        ledc_update_duty(FL_MODE, FL_CH);
    }
}

void frontlight_set(bool on)
{
    if (!board_has_frontlight()) {
        return;
    }
    init_once();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (on) {
        apply_locked();
        esp_timer_stop(s_idle_timer);
        esp_timer_start_once(s_idle_timer, (uint64_t) FRONTLIGHT_IDLE_S * 1000000ULL);
    } else {
        esp_timer_stop(s_idle_timer);
        if (s_configured) {
            ledc_stop(FL_MODE, FL_CH, 0);
        }
        gpio_set_level(PAGER_PIN_FL_PWM, 0);
    }
    bool changed = (s_on != on);
    s_on = on;
    xSemaphoreGive(s_mtx);
    if (changed) {
        ESP_LOGI(TAG, "front light %s (%u%%)", on ? "on" : "off", (unsigned) s_pct);
    }
}

void frontlight_off(void) { frontlight_set(false); }

bool frontlight_is_on(void) { return s_inited && s_on; }

void frontlight_toggle(void) { frontlight_set(!frontlight_is_on()); }

void frontlight_set_pct(uint8_t pct)
{
    if (!board_has_frontlight() || pct < 1 || pct > 100) {
        return;
    }
    init_once();
    s_pct = pct;
    nvs_handle_t h;
    if (nvs_open("board", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "fl_duty", pct);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_on) {
        frontlight_set(true);
    }
}

uint8_t frontlight_get_pct(void) { return s_pct; }

void frontlight_activity(void)
{
    if (!s_inited || !s_on) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_on) {
        esp_timer_stop(s_idle_timer);
        esp_timer_start_once(s_idle_timer, (uint64_t) FRONTLIGHT_IDLE_S * 1000000ULL);
    }
    xSemaphoreGive(s_mtx);
}

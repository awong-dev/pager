/* extled.c -- see extled.h. LEDC timer 2, channel 3, 4 kHz 10-bit on IO8. A breath is a hardware fade
 * 0 -> 25 % over 250 ms and back to 0 over 400 ms; the second leg (and the double-breath legs) are
 * started by a one-shot esp_timer so no ISR callback has to call the fade API. */
#include "extled.h"

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pins.h"

#define EL_MODE LEDC_LOW_SPEED_MODE
#define EL_TIMER LEDC_TIMER_2
#define EL_CH LEDC_CHANNEL_3
#define EL_PERIOD_US 6000000ULL
#define EL_MAX_DUTY 256 /* 25 % of 1023 */

typedef struct {
    uint16_t duty;
    uint16_t ms;
} step_t;

static const step_t k_single[] = {{EL_MAX_DUTY, 250}, {0, 400}};
static const step_t k_double[] = {{EL_MAX_DUTY, 250}, {0, 250}, {EL_MAX_DUTY, 250}, {0, 400}};

static SemaphoreHandle_t s_mtx;
static StaticSemaphore_t s_mtx_buf;
static esp_timer_handle_t s_period, s_step;
static bool s_inited, s_configured, s_unread, s_running;
static const step_t *s_pat;
static unsigned s_pat_n, s_pat_i;

static void period_cb(void *arg);
static void step_cb(void *arg);

static void run_step_locked(void)
{
    if (s_pat_i >= s_pat_n) {
        return;
    }
    step_t st = s_pat[s_pat_i++];
    ledc_set_fade_with_time(EL_MODE, EL_CH, st.duty, st.ms);
    ledc_fade_start(EL_MODE, EL_CH, LEDC_FADE_NO_WAIT);
    if (s_pat_i < s_pat_n) {
        esp_timer_start_once(s_step, (uint64_t) st.ms * 1000ULL + 5000ULL);
    }
}

static void silence_locked(void)
{
    esp_timer_stop(s_step);
    s_pat_i = s_pat_n = 0;
    if (s_configured) {
        ledc_fade_stop(EL_MODE, EL_CH);
        ledc_stop(EL_MODE, EL_CH, 0);
    }
    gpio_set_level(PAGER_PIN_EXT_LED, 0);
}

static bool init_once(void)
{
    if (s_inited) {
        return true;
    }
    if (!board_has_ext_led()) {
        return false;
    }
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    ledc_timer_config_t tc = {
        .speed_mode = EL_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = EL_TIMER,
        .freq_hz = 4000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    ledc_channel_config_t cc = {
        .gpio_num = PAGER_PIN_EXT_LED,
        .speed_mode = EL_MODE,
        .channel = EL_CH,
        .timer_sel = EL_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&cc);
    ledc_fade_func_install(0); /* ESP_ERR_INVALID_STATE if already installed: fine */
    s_configured = true;
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_EXT_LED); /* stay low through light sleep */
    const esp_timer_create_args_t pa = {.callback = period_cb, .name = "led_t"};
    const esp_timer_create_args_t sa = {.callback = step_cb, .name = "led_s"};
    esp_timer_create(&pa, &s_period);
    esp_timer_create(&sa, &s_step);
    s_inited = true;
    return true;
}

static void start_pattern_locked(bool dbl)
{
    esp_timer_stop(s_step);
    ledc_fade_stop(EL_MODE, EL_CH);
    s_pat = dbl ? k_double : k_single;
    s_pat_n = dbl ? sizeof(k_double) / sizeof(k_double[0]) : sizeof(k_single) / sizeof(k_single[0]);
    s_pat_i = 0;
    run_step_locked();
}

static void period_cb(void *arg) /* esp_timer task */
{
    (void) arg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_running) {
        start_pattern_locked(s_unread);
    }
    xSemaphoreGive(s_mtx);
}

static void step_cb(void *arg) /* esp_timer task */
{
    (void) arg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    run_step_locked(); /* pause/silence clear the pattern, so a stale callback is a no-op */
    xSemaphoreGive(s_mtx);
}

void extled_start(void)
{
    if (!init_once()) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (!s_running) {
        s_running = true;
        esp_timer_start_periodic(s_period, EL_PERIOD_US);
    }
    xSemaphoreGive(s_mtx);
}

void extled_pause(void)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s_running = false;
    esp_timer_stop(s_period);
    silence_locked();
    xSemaphoreGive(s_mtx);
}

void extled_resume(void) { extled_start(); }

void extled_set_unread(bool unread) { s_unread = unread; }

void extled_pulse(bool dbl)
{
    if (!init_once()) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    start_pattern_locked(dbl);
    xSemaphoreGive(s_mtx);
}

void extled_hold(bool on)
{
    if (!init_once()) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_step);
    ledc_fade_stop(EL_MODE, EL_CH);
    ledc_set_duty(EL_MODE, EL_CH, on ? EL_MAX_DUTY : 0);
    ledc_update_duty(EL_MODE, EL_CH);
    xSemaphoreGive(s_mtx);
}

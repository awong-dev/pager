/* buzzer.c -- see buzzer.h. LEDC timer 0 (low-speed, 10-bit), channels 0/1 on IO6/IO7 at 50 % duty,
 * channel 1 inverted so the piezo sees the full differential swing. Silence = both channels
 * stopped and both pins detached from LEDC and driven low (no DC across the piezo). */
#include "buzzer.h"

#include <string.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pins.h"
#include "soc/gpio_sig_map.h"
#include "tunes.h"

static const char *TAG = "buzzer";

#define BZ_MODE LEDC_LOW_SPEED_MODE
#define BZ_TIMER LEDC_TIMER_0
#define BZ_CH_P LEDC_CHANNEL_0
#define BZ_CH_N LEDC_CHANNEL_1
#define BZ_DUTY 512 /* 50 % of 10 bits */

static const gpio_num_t k_pin_p = (gpio_num_t) PAGER_PIN_PIEZ_P;
static const gpio_num_t k_pin_n = (gpio_num_t) PAGER_PIN_PIEZ_N;

const buzzer_note_t buzzer_seq_new_msg[BUZZER_SEQ_NEW_MSG_N] = {
    {NOTE_E5 * 2, 60}, /* E6 */
    {0, 30},
    {NOTE_G4 * 4, 90}, /* G6 (1568 Hz) */
};

static SemaphoreHandle_t s_mtx; /* serialises play/stop/step */
static StaticSemaphore_t s_mtx_buf;
static esp_timer_handle_t s_timer;
static bool s_inited;
static bool s_attached; /* pins currently routed to LEDC */
static bool s_active;
static buzzer_note_t s_seq[BUZZER_MAX_STEPS];
static size_t s_n, s_idx;

/* Both pins low, detached from LEDC. Caller holds s_mtx (or is init). */
static void silence_locked(void)
{
    if (s_attached) {
        ledc_stop(BZ_MODE, BZ_CH_P, 0);
        ledc_stop(BZ_MODE, BZ_CH_N, 0);
        s_attached = false;
    }
    esp_rom_gpio_connect_out_signal(k_pin_p, SIG_GPIO_OUT_IDX, false, false);
    esp_rom_gpio_connect_out_signal(k_pin_n, SIG_GPIO_OUT_IDX, false, false);
    gpio_set_level(k_pin_p, 0);
    gpio_set_level(k_pin_n, 0);
}

static void attach_locked(uint32_t hz)
{
    ledc_timer_config_t tc = {
        .speed_mode = BZ_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BZ_TIMER,
        .freq_hz = hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&tc);
    ledc_channel_config_t cp = {
        .gpio_num = k_pin_p,
        .speed_mode = BZ_MODE,
        .channel = BZ_CH_P,
        .timer_sel = BZ_TIMER,
        .duty = BZ_DUTY,
        .hpoint = 0,
    };
    ledc_channel_config(&cp);
    ledc_channel_config_t cn = cp;
    cn.gpio_num = k_pin_n;
    cn.channel = BZ_CH_N;
    cn.flags.output_invert = 1;
    ledc_channel_config(&cn);
    s_attached = true;
}

static void step_locked(void);

static void timer_cb(void *arg)
{
    (void) arg;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    step_locked();
    xSemaphoreGive(s_mtx);
}

/* Starts the next step. Caller holds s_mtx. */
static void step_locked(void)
{
    if (!s_active || s_idx >= s_n) {
        silence_locked();
        s_active = false;
        return;
    }
    buzzer_note_t nt = s_seq[s_idx++];
    if (nt.hz == 0) {
        silence_locked();
    } else if (!s_attached) {
        attach_locked(nt.hz);
    } else {
        ledc_set_freq(BZ_MODE, BZ_TIMER, nt.hz);
        ledc_set_duty(BZ_MODE, BZ_CH_P, BZ_DUTY);
        ledc_set_duty(BZ_MODE, BZ_CH_N, BZ_DUTY);
        ledc_update_duty(BZ_MODE, BZ_CH_P);
        ledc_update_duty(BZ_MODE, BZ_CH_N);
    }
    esp_timer_start_once(s_timer, (uint64_t) (nt.ms ? nt.ms : 1) * 1000ULL);
}

void buzzer_init(void)
{
    if (s_inited || !board_has_piezo()) {
        return;
    }
    s_mtx = xSemaphoreCreateMutexStatic(&s_mtx_buf);
    const esp_timer_create_args_t ta = {.callback = timer_cb, .name = "buzz"};
    ESP_ERROR_CHECK(esp_timer_create(&ta, &s_timer));
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << k_pin_p) | (1ULL << k_pin_n),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&cfg);
    gpio_set_level(k_pin_p, 0);
    gpio_set_level(k_pin_n, 0);
    /* Keep the idle-low level through light sleep (sleep GPIO isolation would float them). */
    gpio_sleep_sel_dis(k_pin_p);
    gpio_sleep_sel_dis(k_pin_n);
    s_inited = true;
    ESP_LOGD(TAG, "init: IO%d/IO%d idle low", (int) k_pin_p, (int) k_pin_n);
}

bool buzzer_play(const buzzer_note_t *seq, size_t n)
{
    if (!board_has_piezo() || seq == NULL || n == 0 || n > BUZZER_MAX_STEPS) {
        return false;
    }
    buzzer_init();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_timer); /* ESP_ERR_INVALID_STATE when idle is fine */
    memcpy(s_seq, seq, n * sizeof(seq[0]));
    s_n = n;
    s_idx = 0;
    s_active = true;
    silence_locked();
    step_locked();
    xSemaphoreGive(s_mtx);
    return true;
}

bool buzzer_play_mario(size_t n)
{
    buzzer_note_t seq[TUNE_MARIO_MAX_STEPS];
    size_t steps = tune_mario_expand(n, seq, TUNE_MARIO_MAX_STEPS);
    return buzzer_play(seq, steps);
}

void buzzer_stop(void)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    esp_timer_stop(s_timer);
    s_active = false;
    silence_locked();
    xSemaphoreGive(s_mtx);
}

bool buzzer_busy(void)
{
    return s_inited && s_active;
}

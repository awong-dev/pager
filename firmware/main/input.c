/* input.c — see input.h for the module contract, split rationale and the
 * F6.2 scope note on CardKB I2C ownership.
 *
 * Authority: docs/DEVICE_PLAN.md §5.3 (input model), firmware/README.md R2
 * (BTN_STUCK fix spec). All timing/current claims here are PENDING_HW, same
 * as the button FSM they replace (modes.c's pre-F6.2 version carried the
 * same caveat).
 */
#include "input.h"

#include <string.h>

/* ---------------------------------------------------------------------
 * Key decode table (docs/DEVICE_PLAN.md §5.3). No ESP-IDF dependency —
 * compiled on the host by firmware/host/test_input.c.
 * --------------------------------------------------------------------- */

input_key_t input_decode_key(uint8_t byte)
{
    input_key_t k = INPUT_KEY_NONE_VAL;

    if (byte == 0x00) {
        return k; /* CardKB "no key" sentinel */
    }
    if (byte >= 0x20 && byte <= 0x7E) {
        k.type = INPUT_KEY_CHAR;
        k.ch = (char) byte;
        return k;
    }
    switch (byte) {
    case 0x08: k.type = INPUT_KEY_BACKSPACE; break;
    case 0x09: k.type = INPUT_KEY_TAB; break;
    case 0x0D: k.type = INPUT_KEY_ENTER; break;
    case 0x1B: k.type = INPUT_KEY_ESC; break;
    case 0xB4: k.type = INPUT_KEY_LEFT; break;
    case 0xB5: k.type = INPUT_KEY_UP; break;
    case 0xB6: k.type = INPUT_KEY_DOWN; break;
    case 0xB7: k.type = INPUT_KEY_RIGHT; break;
    default: break; /* dropped, docs/DEVICE_PLAN.md §5.3 */
    }
    return k;
}

#ifdef ESP_PLATFORM

#include "pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

static const char *TAG = "input";

/* Part C (moved from modes.c) + firmware/README.md R2's BTN_STUCK. */
#define PAGER_BTN_LONG_PRESS_MS 600u
#define PAGER_BTN_DEBOUNCE_MS 30u
#define PAGER_BTN_STUCK_MS 5000u /* R2: "~5s in BTN_HELD" */

/* docs/DEVICE_PLAN.md §5.3: "A key or button event arms a 30s timer
 * (PAGER_UI_AWAKE_S, compile-time)". */
#define PAGER_UI_AWAKE_S 30

#define INPUT_QUEUE_DEPTH 8

typedef enum {
    BTN_IDLE = 0,
    BTN_DOWN,
    BTN_HELD,
    BTN_STUCK,
} btn_state_t;

static btn_state_t s_btn_state = BTN_IDLE;
static int64_t s_btn_t0_us = 0;           /* press start (debounce resolved) */
static int64_t s_btn_held_t0_us = 0;      /* BTN_HELD entry, for the BTN_STUCK cutoff */
static int64_t s_btn_debounce_start_us = 0;

static int64_t s_awake_until_us = 0;

static StaticQueue_t s_queue_buf;
static uint8_t s_queue_storage[INPUT_QUEUE_DEPTH * sizeof(input_event_t)];
static QueueHandle_t s_queue;

static void arm_awake_window(int64_t now_us)
{
    s_awake_until_us = now_us + (int64_t) PAGER_UI_AWAKE_S * 1000000;
}

static void push_event(input_event_t evt)
{
    /* Non-blocking: a full queue (modes_run() not draining fast enough)
     * drops the event rather than blocking this FSM step from any caller
     * context (CLAUDE.md: no busy-wait loops). */
    if (xQueueSend(s_queue, &evt, 0) != pdTRUE) {
        ESP_LOGD(TAG, "input event queue full, dropping event type=%d", (int) evt.type);
    }
}

/* Button short/long/stuck FSM. Note the hazard BTN_STUCK exists to avoid: a
 * LEVEL wake (esp_sleep_enable_ext0_wakeup(..., 0)) makes
 * esp_light_sleep_start() return immediately for as long as the button is
 * held, so a held button busy-loops modes_run() at ~40mA
 * (firmware/README.md R2). input_button_busy() returning false for
 * BTN_STUCK stops modes_run()'s fine ~20ms press/hold-timing poll once
 * there is nothing left to measure; it deliberately does NOT resume
 * net_sleep() (see input_button_stuck()'s doc comment in input.h for why
 * that needs a net.cpp change outside this task's scope).
 */
static void button_fsm_step(int level, int64_t now_us)
{
    switch (s_btn_state) {
    case BTN_IDLE:
        if (level == 0) {
            if (s_btn_debounce_start_us == 0) {
                s_btn_debounce_start_us = now_us;
            } else if ((now_us - s_btn_debounce_start_us) >=
                       (int64_t) PAGER_BTN_DEBOUNCE_MS * 1000) {
                s_btn_state = BTN_DOWN;
                s_btn_t0_us = now_us;
                s_btn_debounce_start_us = 0;
                arm_awake_window(now_us);
                push_event((input_event_t) { .type = INPUT_EVT_BTN_DOWN });
            }
        } else {
            s_btn_debounce_start_us = 0;
        }
        break;

    case BTN_DOWN:
        if (level != 0) {
            /* Released before the long-press threshold: short press. */
            arm_awake_window(now_us);
            push_event((input_event_t) { .type = INPUT_EVT_BTN_SHORT });
            s_btn_state = BTN_IDLE;
        } else if ((now_us - s_btn_t0_us) >= (int64_t) PAGER_BTN_LONG_PRESS_MS * 1000) {
            /* Fire at the threshold elapsed - do not wait for release. */
            arm_awake_window(now_us);
            push_event((input_event_t) { .type = INPUT_EVT_BTN_LONG });
            s_btn_held_t0_us = now_us;
            s_btn_state = BTN_HELD;
        }
        break;

    case BTN_HELD:
        if (level != 0) {
            s_btn_state = BTN_IDLE;
        } else if ((now_us - s_btn_held_t0_us) >= (int64_t) PAGER_BTN_STUCK_MS * 1000) {
            ESP_LOGI(TAG, "button held >=%us since long-press fired; entering BTN_STUCK "
                          "(firmware/README.md R2) - dropping to the coarse poll cadence, "
                          "net_sleep() still withheld (see input_button_stuck() doc comment)",
                     (unsigned) (PAGER_BTN_STUCK_MS / 1000));
            s_btn_state = BTN_STUCK;
        }
        break;

    case BTN_STUCK:
        if (level != 0) {
            ESP_LOGI(TAG, "button released after BTN_STUCK");
            s_btn_state = BTN_IDLE;
        }
        break;
    }
}

void input_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << PAGER_PIN_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, /* active low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);

    s_queue = xQueueCreateStatic(INPUT_QUEUE_DEPTH, sizeof(input_event_t), s_queue_storage,
                                  &s_queue_buf);
}

void input_poll(void)
{
    int64_t now_us = esp_timer_get_time();
    button_fsm_step(gpio_get_level((gpio_num_t) PAGER_PIN_BUTTON), now_us);
}

void input_feed_key(uint8_t byte)
{
    input_key_t key = input_decode_key(byte);
    if (key.type == INPUT_KEY_NONE) {
        return;
    }
    arm_awake_window(esp_timer_get_time());
    push_event((input_event_t) { .type = INPUT_EVT_KEY, .key = key });
}

bool input_get_event(input_event_t *out)
{
    return xQueueReceive(s_queue, out, 0) == pdTRUE;
}

bool input_awake(void)
{
    return esp_timer_get_time() < s_awake_until_us;
}

bool input_button_busy(void)
{
    return s_btn_state == BTN_DOWN || s_btn_state == BTN_HELD;
}

bool input_button_stuck(void)
{
    return s_btn_state == BTN_STUCK;
}

#endif /* ESP_PLATFORM */

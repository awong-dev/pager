/* firmware/host/idf_stub/driver/gpio.h — minimal stand-in for ESP-IDF's
 * driver/gpio.h, just enough surface for input.c's ESP_PLATFORM section
 * (gpio_config()/gpio_get_level() on the button pin) to compile and link on
 * the host. Task 1.0 (docs/V03_TASKS.md): the host build never exercised
 * input.c's queue/awake-window code before this, so there was no coverage
 * for "N keys fed at once are all delivered" (input_feed_key() -> the
 * FreeRTOS queue -> input_get_event()) at all. Real behaviour, no mocks
 * beyond "GPIO reads always return released (level 1)" -- the button FSM
 * is not what this test suite exercises.
 */
#ifndef IDF_STUB_DRIVER_GPIO_H
#define IDF_STUB_DRIVER_GPIO_H

#include <stdint.h>

typedef int gpio_num_t;

typedef struct {
    uint64_t pin_bit_mask;
    int mode;
    int pull_up_en;
    int pull_down_en;
    int intr_type;
} gpio_config_t;

#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0

void gpio_config(const gpio_config_t *cfg);
int gpio_get_level(gpio_num_t pin);

/* Test-only: overrides the level gpio_get_level() returns (default 1,
 * released/idle, active-low), so a host test can drive input.c's button
 * FSM through input_poll() with a chosen pin state. input.c itself never
 * calls this -- device code always reads the real pin. */
void idf_stub_set_button_level(int level);

#endif

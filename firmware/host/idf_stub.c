/* firmware/host/idf_stub.c — implementations for the headers under
 * firmware/host/idf_stub/ (see idf_stub/driver/gpio.h's module comment).
 * Links against input.c
 * built with -DESP_PLATFORM so test_input.c can exercise input_init()/
 * input_feed_key()/input_get_event() -- the actual FreeRTOS queue path --
 * on the host (task 1.0, docs/V03_TASKS.md).
 */
#include "driver/gpio.h"
#include "esp_timer.h"
#include "freertos/queue.h"

#include <string.h>

void gpio_config(const gpio_config_t *cfg)
{
    (void) cfg; /* no real GPIO on the host; input.c only configures the button pin */
}

static int s_button_level = 1; /* idle (active-low, released) by default */

int gpio_get_level(gpio_num_t pin)
{
    (void) pin;
    return s_button_level;
}

void idf_stub_set_button_level(int level)
{
    s_button_level = level;
}

static int64_t s_fake_time_us = 0;

int64_t esp_timer_get_time(void)
{
    /* Every call advances a tick so two calls in the same test are never
     * bit-for-bit identical, without needing a real clock. */
    return s_fake_time_us++;
}

void idf_stub_advance_us(int64_t delta_us)
{
    s_fake_time_us += delta_us;
}

QueueHandle_t xQueueCreateStatic(UBaseType_t uxQueueLength, UBaseType_t uxItemSize,
                                  uint8_t *pucQueueStorage, StaticQueue_t *pxQueueBuffer)
{
    pxQueueBuffer->storage = pucQueueStorage;
    pxQueueBuffer->item_size = uxItemSize;
    pxQueueBuffer->capacity = uxQueueLength;
    pxQueueBuffer->head = 0;
    pxQueueBuffer->count = 0;
    return pxQueueBuffer;
}

BaseType_t xQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait)
{
    (void) xTicksToWait; /* input.c always passes 0; blocking is not implemented */
    if (xQueue->count >= xQueue->capacity) {
        return pdFALSE;
    }
    size_t tail = (xQueue->head + xQueue->count) % xQueue->capacity;
    memcpy(xQueue->storage + tail * xQueue->item_size, pvItemToQueue, xQueue->item_size);
    xQueue->count++;
    return pdTRUE;
}

BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait)
{
    (void) xTicksToWait; /* input.c always passes 0; blocking is not implemented */
    if (xQueue->count == 0) {
        return pdFALSE;
    }
    memcpy(pvBuffer, xQueue->storage + xQueue->head * xQueue->item_size, xQueue->item_size);
    xQueue->head = (xQueue->head + 1) % xQueue->capacity;
    xQueue->count--;
    return pdTRUE;
}

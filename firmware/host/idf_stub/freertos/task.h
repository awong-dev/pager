/* firmware/host/idf_stub/freertos/task.h -- stand-in for ESP-IDF's
 * freertos/task.h (see idf_stub/driver/gpio.h's module comment). input.c
 * (since 9e29d50, the kbd task wakes the main loop) blocks in
 * input_wait_event() on a task notification and push_event() raises it. The
 * host has one thread and never blocks, so: the handle is an opaque dummy,
 * xTaskNotifyGive() just counts, and ulTaskNotifyTake() consumes the count
 * and returns at once (a timeout is indistinguishable from "woke").
 */
#ifndef IDF_STUB_FREERTOS_TASK_H
#define IDF_STUB_FREERTOS_TASK_H

#include <stdint.h>

#include "freertos/queue.h"

typedef void *TaskHandle_t;

#define pdMS_TO_TICKS(ms) ((TickType_t) (ms))

TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t xTaskToNotify);
uint32_t ulTaskNotifyTake(BaseType_t xClearCountOnExit, TickType_t xTicksToWait);

#endif

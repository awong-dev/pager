/* firmware/host/idf_stub/freertos/queue.h — stand-in for ESP-IDF's
 * freertos/queue.h (see idf_stub/driver/gpio.h's module comment).
 *
 * A real, working (not mocked-to-always-succeed) fixed-capacity FIFO ring
 * buffer over the caller-supplied static storage, matching the three calls
 * input.c makes: xQueueCreateStatic() to set it up over
 * s_queue_storage/s_queue_buf, xQueueSend() with a 0 tick wait (fails
 * rather than blocking when full, same as the real API with xTicksToWait
 * == 0), and xQueueReceive() with a 0 tick wait (fails rather than
 * blocking when empty). No dynamic allocation: StaticQueue_t just tracks
 * head/count over the storage array input.c already owns statically.
 * input.c never uses a non-zero xTicksToWait, so blocking is intentionally
 * not implemented here.
 */
#ifndef IDF_STUB_FREERTOS_QUEUE_H
#define IDF_STUB_FREERTOS_QUEUE_H

#include <stddef.h>
#include <stdint.h>

#define pdTRUE 1
#define pdFALSE 0

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

typedef struct {
    uint8_t *storage;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
} StaticQueue_t;

typedef StaticQueue_t *QueueHandle_t;

QueueHandle_t xQueueCreateStatic(UBaseType_t uxQueueLength, UBaseType_t uxItemSize,
                                  uint8_t *pucQueueStorage, StaticQueue_t *pxQueueBuffer);
BaseType_t xQueueSend(QueueHandle_t xQueue, const void *pvItemToQueue, TickType_t xTicksToWait);
BaseType_t xQueueReceive(QueueHandle_t xQueue, void *pvBuffer, TickType_t xTicksToWait);

#endif

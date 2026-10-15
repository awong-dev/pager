/* firmware/host/idf_stub/freertos/FreeRTOS.h — stand-in for ESP-IDF's
 * freertos/FreeRTOS.h (see idf_stub/driver/gpio.h's module comment). The
 * real header pulls in the whole kernel API; input.c only needs the queue
 * declarations below (freertos/queue.h), so this file exists solely to
 * satisfy the #include and is otherwise empty.
 */
#ifndef IDF_STUB_FREERTOS_H
#define IDF_STUB_FREERTOS_H

/* Single-threaded host: a critical section is a no-op. */
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) ((void) (m))
#define portEXIT_CRITICAL(m) ((void) (m))
#endif

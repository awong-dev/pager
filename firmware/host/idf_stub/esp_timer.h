/* firmware/host/idf_stub/esp_timer.h — stand-in for ESP-IDF's esp_timer.h
 * (see idf_stub/driver/gpio.h's module comment). idf_stub.c implements a
 * simple monotonic counter (advanced by idf_stub_advance_us(), starting at
 * 0) instead of a real hardware timer -- input.c only ever compares two
 * calls to this against each other (the UI-awake window arithmetic), never
 * a wall-clock value.
 */
#ifndef IDF_STUB_ESP_TIMER_H
#define IDF_STUB_ESP_TIMER_H

#include <stdint.h>

int64_t esp_timer_get_time(void);

#endif

/* firmware/host/idf_stub/driver/rtc_io.h — stand-in for ESP-IDF's driver/rtc_io.h,
 * just the one call input.c's input_init() makes (5 Oct 2026: releasing the
 * button pad's RTC hold, see input.c). No-ops on the host, implemented in idf_stub.c. */
#ifndef IDF_STUB_DRIVER_RTC_IO_H
#define IDF_STUB_DRIVER_RTC_IO_H

#include "driver/gpio.h"

void rtc_gpio_hold_dis(gpio_num_t pin);

#endif

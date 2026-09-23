/* firmware/host/idf_stub/esp_log.h — stand-in for ESP-IDF's esp_log.h (see
 * idf_stub/driver/gpio.h's module comment). ESP_LOGD is chatter (per
 * CLAUDE.md logging convention) so it is compiled out entirely on the host;
 * ESP_LOGI prints, matching what a state-transition log would show.
 */
#ifndef IDF_STUB_ESP_LOG_H
#define IDF_STUB_ESP_LOG_H

#include <stdio.h>

#define ESP_LOGD(tag, fmt, ...) do { (void) (tag); } while (0)
#define ESP_LOGI(tag, fmt, ...) printf("I (%s) " fmt "\n", (tag), ##__VA_ARGS__)

#endif

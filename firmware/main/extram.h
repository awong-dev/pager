/* extram.h -- EXT_RAM_BSS_ATTR for files that also build on the host.
 *
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y (sdkconfig.defaults) puts a
 * static marked EXT_RAM_BSS_ATTR in PSRAM (.ext_ram.bss, zeroed by
 * cpu_start.c after esp_psram_init(), before any constructor or app_main).
 * Use ONLY for large, zero-initialised, task-context statics that are never
 * a DMA source/target, never touched from an ISR or with the flash cache
 * disabled, and never needed before PSRAM is up. Never on RTC_DATA/RTC_NOINIT.
 * On the host the attribute is empty.
 */
#ifndef PAGER_EXTRAM_H
#define PAGER_EXTRAM_H

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#else
#define EXT_RAM_BSS_ATTR
#endif

#endif

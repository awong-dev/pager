// astpart.h -- commit record for the `assets_ota` font partition
// (docs/ASSETS_OTA_DESIGN.md section 2). The record is 64 B, alone in the partition's last 4 KiB
// sector, written only after the whole image has been hashed and checked. Its absence (or any
// corruption) means "use the factory `assets` partition". The pure part (crc32, encode, decode)
// builds on the host; tools/astpart.py writes the same layout.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#include "esp_partition.h"
#endif

#define ASTPART_LABEL "assets_ota"
#define ASTPART_SECTOR 4096u
#define ASTPART_REC_SIZE 64u

typedef struct {
    uint32_t len;   // image length in bytes (16 <= len <= part_size - ASTPART_SECTOR)
    uint8_t sha[32];// sha256(image[0, len))
    uint8_t fv;     // assets format version (header byte 4)
} astpart_rec_t;

// zlib-compatible CRC-32 (init 0 as zlib.crc32(p); table-free bitwise, poly 0xEDB88320).
uint32_t astpart_crc32(const uint8_t *p, size_t n);
void astpart_rec_encode(const astpart_rec_t *r, uint8_t out[ASTPART_REC_SIZE]);
// True only if magic "PGAC", rec_ver 1, crc and 16 <= len <= part_size - 4096 all hold.
bool astpart_rec_decode(const uint8_t in[ASTPART_REC_SIZE], uint32_t part_size, astpart_rec_t *out);

#ifdef ESP_PLATFORM
const esp_partition_t *astpart_find(void);                           // by label; NULL = old table
bool astpart_read_rec(const esp_partition_t *p, astpart_rec_t *out); // 64 B read at size-4096
// Power effect: one 4 KiB flash sector erase (~45 ms at flash current); the partition is invalid afterwards.
esp_err_t astpart_invalidate(const esp_partition_t *p);
// Power effect: one 64 B flash program (inside one 256 B page), well under 1 ms.
esp_err_t astpart_write_rec(const esp_partition_t *p, const astpart_rec_t *r);
#endif

// astpart.c -- see astpart.h. Pure section first (host-testable), device section under ESP_PLATFORM.
#include "astpart.h"

#include <string.h>

#define REC_VER 1u

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

uint32_t astpart_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return c ^ 0xFFFFFFFFu;
}

void astpart_rec_encode(const astpart_rec_t *r, uint8_t out[ASTPART_REC_SIZE])
{
    memset(out, 0, ASTPART_REC_SIZE);
    memcpy(out, "PGAC", 4);
    put_u32(out + 4, REC_VER);
    put_u32(out + 8, r->len);
    memcpy(out + 12, r->sha, 32);
    out[44] = r->fv;
    put_u32(out + 60, astpart_crc32(out, 60));
}

bool astpart_rec_decode(const uint8_t in[ASTPART_REC_SIZE], uint32_t part_size, astpart_rec_t *out)
{
    if (memcmp(in, "PGAC", 4) != 0 || get_u32(in + 4) != REC_VER) {
        return false;
    }
    if (get_u32(in + 60) != astpart_crc32(in, 60)) {
        return false;
    }
    uint32_t len = get_u32(in + 8);
    if (part_size < ASTPART_SECTOR || len < 16u || len > part_size - ASTPART_SECTOR) {
        return false;
    }
    if (out) {
        out->len = len;
        memcpy(out->sha, in + 12, 32);
        out->fv = in[44];
    }
    return true;
}

#ifdef ESP_PLATFORM
const esp_partition_t *astpart_find(void)
{
    return esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ASTPART_LABEL);
}

bool astpart_read_rec(const esp_partition_t *p, astpart_rec_t *out)
{
    if (!p || p->size < ASTPART_SECTOR) {
        return false;
    }
    uint8_t b[ASTPART_REC_SIZE];
    if (esp_partition_read(p, p->size - ASTPART_SECTOR, b, sizeof b) != ESP_OK) {
        return false;
    }
    return astpart_rec_decode(b, p->size, out);
}

esp_err_t astpart_invalidate(const esp_partition_t *p)
{
    return esp_partition_erase_range(p, p->size - ASTPART_SECTOR, ASTPART_SECTOR);
}

esp_err_t astpart_write_rec(const esp_partition_t *p, const astpart_rec_t *r)
{
    uint8_t b[ASTPART_REC_SIZE];
    astpart_rec_encode(r, b);
    return esp_partition_write(p, p->size - ASTPART_SECTOR, b, sizeof b);
}
#endif

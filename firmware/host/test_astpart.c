/* test_astpart.c -- host test for main/astpart.c's pure section (docs/ASSETS_OTA_DESIGN.md section 2). */
#include "astpart.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

#define PSIZE 0x200000u

int main(void)
{
    CHECK(astpart_crc32((const uint8_t *) "123456789", 9) == 0xCBF43926u, "crc32 check value");
    CHECK(astpart_crc32((const uint8_t *) "", 0) == 0u, "crc32 of empty");

    astpart_rec_t r = {.len = 736237, .fv = 1};
    for (int i = 0; i < 32; i++) {
        r.sha[i] = (uint8_t) (i * 7 + 3);
    }
    uint8_t b[ASTPART_REC_SIZE];
    astpart_rec_encode(&r, b);
    CHECK(memcmp(b, "PGAC", 4) == 0 && b[4] == 1 && b[5] == 0, "magic/version bytes");
    CHECK(b[8] == (736237 & 0xFF) && b[9] == ((736237 >> 8) & 0xFF), "len little-endian");
    astpart_rec_t d;
    memset(&d, 0, sizeof d);
    CHECK(astpart_rec_decode(b, PSIZE, &d), "round trip decodes");
    CHECK(d.len == r.len && d.fv == r.fv && memcmp(d.sha, r.sha, 32) == 0, "round trip fields");

    /* every single-byte flip in 0..63 fails (bytes 45..59 are covered by the crc) */
    for (int i = 0; i < (int) ASTPART_REC_SIZE; i++) {
        uint8_t c[ASTPART_REC_SIZE];
        memcpy(c, b, sizeof c);
        c[i] ^= 0x01;
        CHECK(!astpart_rec_decode(c, PSIZE, &d), "flip at byte %d must fail", i);
    }

    /* len bounds (re-encoded so the crc is valid) */
    uint32_t lens[] = {15, 0, PSIZE - ASTPART_SECTOR + 1, PSIZE - 4095};
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        astpart_rec_t x = r;
        x.len = lens[i];
        astpart_rec_encode(&x, b);
        CHECK(!astpart_rec_decode(b, PSIZE, &d), "len %u must fail", (unsigned) lens[i]);
    }
    uint32_t ok[] = {16, PSIZE - ASTPART_SECTOR};
    for (size_t i = 0; i < sizeof ok / sizeof ok[0]; i++) {
        astpart_rec_t x = r;
        x.len = ok[i];
        astpart_rec_encode(&x, b);
        CHECK(astpart_rec_decode(b, PSIZE, &d), "len %u must pass", (unsigned) ok[i]);
    }
    /* the same record in a smaller partition is out of range */
    astpart_rec_t big = r;
    big.len = 0x100000u;
    astpart_rec_encode(&big, b);
    CHECK(!astpart_rec_decode(b, 0x100000u, &d), "len beyond a 1 MiB partition fails");
    astpart_rec_encode(&r, b);
    CHECK(astpart_rec_decode(b, PSIZE, NULL), "NULL out accepted");

    if (g_failures) {
        printf("test_astpart: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_astpart: all passed\n");
    return 0;
}

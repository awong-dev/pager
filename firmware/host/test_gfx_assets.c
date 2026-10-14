/* test_gfx_assets.c -- host test for gfx.c's pure gfx_assets_check() (docs/ASSETS_OTA_DESIGN.md
 * section 2). Uses a tiny synthetic image always, and build/images/assets-dejavu-emoji.bin when
 * present (reported as SKIP when absent). */
#include "gfx.h"

#include <stdio.h>
#include <stdlib.h>
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

static void put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = (uint8_t) (v >> (8 * i));
    }
}

/* header(16) + two empty blocks (12 and 16 px, 0 glyphs, 12 bytes each) */
static size_t synth(uint8_t *b)
{
    memset(b, 0, 40);
    memcpy(b, "PGFA", 4);
    b[4] = 1;
    memcpy(b + 5, "sc", 2);
    put32(b + 8, 40);
    b[12] = 2;
    b[16] = 12;
    put32(b + 24, 12);
    b[28] = 16;
    put32(b + 36, 12);
    return 40;
}

static void battery(const uint8_t *img, size_t n, const char *name)
{
    uint8_t *w = malloc(n);
    uint8_t ver = 0;
    memcpy(w, img, n);
    CHECK(gfx_assets_check(w, n, &ver) && ver == 1, "%s: valid v1", name);
    CHECK(gfx_assets_check(w, n, NULL), "%s: NULL ver_out ok", name);
    w[4] = 2;
    CHECK(!gfx_assets_check(w, n, &ver), "%s: version 2 rejected", name);
    w[4] = 0;
    CHECK(!gfx_assets_check(w, n, &ver), "%s: version 0 rejected", name);
    w[4] = 1;
    CHECK(!gfx_assets_check(w, n - 1, &ver), "%s: len-1 rejected", name);
    CHECK(!gfx_assets_check(w, 15, &ver), "%s: truncated header rejected", name);
    CHECK(!gfx_assets_check(w, 4, &ver), "%s: 4-byte input rejected", name);
    w[0] = 'X';
    CHECK(!gfx_assets_check(w, n, &ver), "%s: bad magic rejected", name);
    free(w);
}

int main(void)
{
    uint8_t tiny[64];
    size_t n = synth(tiny);
    battery(tiny, n, "synthetic");
    /* a total smaller than len is not a match for the strict check */
    uint8_t padded[64];
    memcpy(padded, tiny, n);
    memset(padded + n, 0xFF, 24);
    CHECK(!gfx_assets_check(padded, n + 24, NULL), "total != len rejected");

    const char *path = "../../build/images/assets-dejavu-emoji.bin";
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("SKIP real image (%s absent)\n", path);
    } else {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = malloc((size_t) sz);
        CHECK(fread(buf, 1, (size_t) sz, f) == (size_t) sz, "read");
        fclose(f);
        battery(buf, (size_t) sz, "assets-dejavu-emoji.bin");
        free(buf);
    }
    if (g_failures) {
        printf("test_gfx_assets: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_gfx_assets: all passed\n");
    return 0;
}

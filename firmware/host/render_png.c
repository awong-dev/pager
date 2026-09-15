/* render_png.c — host-only PNG renderer for gfx.c's framebuffer (task
 * F6.1's "host PNG test": docs/DEVICE_TASKS.md F6.1, docs/DEVICE_PLAN.md
 * §5.2's "Host-side render test").
 *
 * There is no way to see or photograph the real SSD1680 panel in this
 * environment (every visual claim elsewhere in this codebase is marked
 * PENDING_HW/UNVERIFIED for that reason); this is the closest available
 * substitute — the *exact* gfx.c that ships on-device, compiled for the
 * host and pointed at a real `assets.bin` (tools/mkassets.py's output),
 * rendering a fixed set of strings so the Noto rasterisation and the
 * proportional-metrics/wrap code can be eyeballed before hardware exists.
 *
 * Usage: ./render_png [assets.bin path]  (default: ../../build/assets.bin,
 * i.e. the repo-root build/ dir the Verify command's `mkassets.py` writes
 * to, resolved from firmware/host/ where this binary is built and run —
 * see Makefile's `png` target). Writes build/render_{latin,cyrillic,cjk,
 * tofu}.png (this directory, firmware/host/build/, same as the test_*
 * binaries).
 *
 * PNG output: a hand-written minimal writer (signature + IHDR + one IDAT +
 * IEND, 8-bit grayscale, filter type 0/None per scanline) rather than a
 * bespoke DEFLATE implementation — the IDAT payload is produced by a single
 * zlib compress2() call, since a PNG IDAT chunk *is* a zlib stream by
 * definition (RFC 2083 §9.3). zlib is the one dependency this file adds;
 * it ships with the OS on every platform this project's other host tooling
 * already assumes (firmware/host/Makefile's mbedtls pkg-config/brew/bare
 * fallback chain makes the same kind of assumption for test_setup).
 */
#include "gfx.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <zlib.h>

/* ---------------------------------------------------------------------
 * Minimal PNG writer. Fixed-size static buffers (no malloc): the largest
 * image this file ever produces is GFX_SCREEN_W x GFX_SCREEN_H, known at
 * compile time.
 * --------------------------------------------------------------------- */

#define PNG_RAW_LEN ((GFX_SCREEN_W + 1) * GFX_SCREEN_H) /* +1 filter-type byte/row */

static uint8_t s_png_raw[PNG_RAW_LEN];
static uint8_t s_png_comp[PNG_RAW_LEN + PNG_RAW_LEN / 8 + 128]; /* compressBound() margin */

static void put_u32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) (v >> 24);
    p[1] = (uint8_t) (v >> 16);
    p[2] = (uint8_t) (v >> 8);
    p[3] = (uint8_t) v;
}

static bool write_chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t len)
{
    uint8_t len_be[4];
    put_u32be(len_be, len);
    if (fwrite(len_be, 1, 4, f) != 4 || fwrite(type, 1, 4, f) != 4) {
        return false;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        return false;
    }
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (const Bytef *) type, 4);
    if (len > 0) {
        crc = crc32(crc, data, len);
    }
    uint8_t crc_be[4];
    put_u32be(crc_be, (uint32_t) crc);
    return fwrite(crc_be, 1, 4, f) == 4;
}

/* `gray` is width*height bytes, row-major, top-to-bottom, 0=black/255=white. */
static bool write_png_gray8(const char *path, const uint8_t *gray, int width, int height)
{
    if ((size_t) (width + 1) * (size_t) height > sizeof(s_png_raw)) {
        fprintf(stderr, "render_png: %dx%d exceeds this writer's static buffer\n", width, height);
        return false;
    }
    for (int y = 0; y < height; y++) {
        uint8_t *row = s_png_raw + (size_t) y * (width + 1);
        row[0] = 0; /* filter type: None */
        memcpy(row + 1, gray + (size_t) y * width, (size_t) width);
    }

    uLongf comp_len = sizeof(s_png_comp);
    int zerr = compress2(s_png_comp, &comp_len, s_png_raw, (uLong) (width + 1) * (uLong) height,
                          Z_BEST_COMPRESSION);
    if (zerr != Z_OK) {
        fprintf(stderr, "render_png: compress2() failed (%d)\n", zerr);
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "render_png: fopen(%s) failed\n", path);
        return false;
    }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    bool ok = fwrite(sig, 1, 8, f) == 8;

    uint8_t ihdr[13];
    put_u32be(ihdr + 0, (uint32_t) width);
    put_u32be(ihdr + 4, (uint32_t) height);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 0;  /* color type: grayscale */
    ihdr[10] = 0; /* compression method */
    ihdr[11] = 0; /* filter method */
    ihdr[12] = 0; /* interlace method */
    ok = ok && write_chunk(f, "IHDR", ihdr, sizeof(ihdr));
    ok = ok && write_chunk(f, "IDAT", s_png_comp, (uint32_t) comp_len);
    ok = ok && write_chunk(f, "IEND", NULL, 0);

    fclose(f);
    return ok;
}

/* Reads gfx.c's current framebuffer (gfx_fb_native_row(), the same
 * accessor disp.c uses) into a normal top-left-origin gray8 image. 1 = white
 * (unset), 0 = black (ink) per gfx.h's polarity note. */
static uint8_t s_gray[GFX_SCREEN_H][GFX_SCREEN_W];

static void snapshot_framebuffer(void)
{
    for (int x = 0; x < GFX_SCREEN_W; x++) {
        const uint8_t *row = gfx_fb_native_row(x);
        for (int y = 0; y < GFX_SCREEN_H; y++) {
            int bit = (row[y / 8] >> (7 - (y % 8))) & 1;
            s_gray[y][x] = bit ? 255 : 0;
        }
    }
}

/* ---------------------------------------------------------------------
 * The fixed set of test renders, docs/DEVICE_TASKS.md F6.1: "a fixed set
 * of strings (Latin, Cyrillic, a CJK sentence, a tofu case)".
 * --------------------------------------------------------------------- */

static void render_latin(void)
{
    gfx_clear();
    gfx_text(0, 0, GFX_FONT_NORMAL, "The quick brown fox jumps over 12 lazy dogs!");
    gfx_hline(0, GFX_SCREEN_W - 1, 11);
    gfx_text(0, 14, GFX_FONT_LARGE, "Noto Sans 16px ABCxyz");
    gfx_hline(0, GFX_SCREEN_W - 1, 34);
    gfx_text(0, 38, GFX_FONT_NORMAL, "Punctuation: ,.;:!?()[]{}\"'-_/\\@#$%^&*+=<>");

    char lines[8][64];
    int n = gfx_text_wrap(GFX_FONT_NORMAL,
                           "text_wrap() breaks a longer paragraph on space "
                           "boundaries so it fits the 296px body width.",
                           GFX_SCREEN_W, lines, 8);
    for (int i = 0; i < n && i >= 0; i++) {
        gfx_text(0, 50 + i * 12, GFX_FONT_NORMAL, lines[i]);
    }
}

static void render_cyrillic(void)
{
    gfx_clear();
    gfx_text(0, 0, GFX_FONT_NORMAL, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82, "
                                     "\xD0\xBC\xD0\xB8\xD1\x80! \xD0\x9F\xD0\xB5\xD0\xB9\xD0\xB4"
                                     "\xD0\xB6\xD0\xB5\xD1\x80 \xD1\x80\xD0\xB0\xD0\xB1\xD0\xBE"
                                     "\xD1\x82\xD0\xB0\xD0\xB5\xD1\x82.");
    gfx_hline(0, GFX_SCREEN_W - 1, 11);
    gfx_text(0, 14, GFX_FONT_LARGE,
             "\xD0\x9A\xD0\xB8\xD1\x80\xD0\xB8\xD0\xBB\xD0\xBB\xD0\xB8\xD1\x86\xD0\xB0 16px");
    gfx_hline(0, GFX_SCREEN_W - 1, 34);
    gfx_text(0, 38, GFX_FONT_NORMAL,
             "\xD0\x90\xD0\x91\xD0\x92\xD0\x93\xD0\x94 "
             "\xD0\xB0\xD0\xB1\xD0\xB2\xD0\xB3\xD0\xB4 0123456789");
}

static void render_cjk(void)
{
    gfx_clear();
    /* "Hello, world! The pager works normally." (sc) */
    gfx_text(0, 0, GFX_FONT_NORMAL,
             "\xE4\xBD\xA0\xE5\xA5\xBD\xEF\xBC\x8C\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x81\xE5\xAF\xBB"
             "\xE5\x91\xBC\xE6\x9C\xBA\xE5\xB7\xA5\xE4\xBD\x9C\xE6\xAD\xA3\xE5\xB8\xB8\xE3\x80\x82");
    gfx_hline(0, GFX_SCREEN_W - 1, 11);
    /* "Chinese test sentence" at 16px */
    gfx_text(0, 14, GFX_FONT_LARGE,
             "\xE4\xB8\xAD\xE6\x96\x87\xE6\xB5\x8B\xE8\xAF\x95\xE5\x8F\xA5\xE5\xAD\x90");
    gfx_hline(0, GFX_SCREEN_W - 1, 34);

    char lines[8][64];
    /* A longer sentence, to exercise gfx_text_wrap()'s per-character CJK
     * break rule (docs/DEVICE_PLAN.md §5.2) rather than the space rule. */
    int n = gfx_text_wrap(GFX_FONT_NORMAL,
                           "\xE8\xBF\x99\xE6\x98\xAF\xE4\xB8\x80\xE4\xB8\xAA\xE7\x94\xA8\xE4\xBA"
                           "\x8E\xE6\xB5\x8B\xE8\xAF\x95\xE6\x8D\xA2\xE8\xA1\x8C\xE9\x80\xBB\xE8"
                           "\xBE\x91\xE7\x9A\x84\xE8\xBE\x83\xE9\x95\xBF\xE5\x8F\xA5\xE5\xAD\x90"
                           "\xEF\xBC\x8C\xE6\xB2\xA1\xE6\x9C\x89\xE7\xA9\xBA\xE6\xA0\xBC\xE3\x80"
                           "\x82",
                           GFX_SCREEN_W, lines, 8);
    for (int i = 0; i < n && i >= 0; i++) {
        gfx_text(0, 46 + i * 12, GFX_FONT_NORMAL, lines[i]);
    }
}

static void render_tofu(void)
{
    gfx_clear();
    gfx_text(0, 0, GFX_FONT_NORMAL, "tofu case (codepoint outside every block):");
    gfx_hline(0, GFX_SCREEN_W - 1, 11);
    /* Devanagari "namaste" (U+0928 U+092E U+0938 U+094D U+0924 U+0947) --
     * outside every _COMMON_RANGES/_CJK_RANGES entry mkassets.py encodes,
     * so every codepoint here must draw as a tofu box. */
    uint32_t before = gfx_get_tofu_count();
    gfx_text(0, 20, GFX_FONT_NORMAL,
             "\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87");
    uint32_t after = gfx_get_tofu_count();

    char counter[64];
    snprintf(counter, sizeof(counter), "tofu count: %u -> %u (delta %u)", (unsigned) before,
             (unsigned) after, (unsigned) (after - before));
    gfx_text(0, 40, GFX_FONT_NORMAL, counter);
}

int main(int argc, char **argv)
{
    const char *assets_path = (argc > 1) ? argv[1] : "../../build/assets.bin";
    if (!gfx_init_from_file(assets_path)) {
        fprintf(stderr,
                "render_png: could not load '%s' -- build it first, e.g.\n"
                "  python3 ../../tools/mkassets.py --lang sc -o ../../build/assets.bin "
                "--sans-font <NotoSans.ttf> --cjk-font <NotoSansCJK.ttc>\n",
                assets_path);
        return 1;
    }

    struct {
        const char *name;
        void (*render)(void);
    } cases[] = {
        { "latin", render_latin },
        { "cyrillic", render_cyrillic },
        { "cjk", render_cjk },
        { "tofu", render_tofu },
    };

    int status = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        cases[i].render();
        snapshot_framebuffer();
        char path[128];
        snprintf(path, sizeof(path), "build/render_%s.png", cases[i].name);
        if (!write_png_gray8(path, (const uint8_t *) s_gray, GFX_SCREEN_W, GFX_SCREEN_H)) {
            status = 1;
            continue;
        }
        printf("render_png: wrote %s\n", path);
    }
    printf("render_png: total tofu count = %u\n", (unsigned) gfx_get_tofu_count());
    return status;
}

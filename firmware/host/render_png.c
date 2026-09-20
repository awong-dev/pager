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
    // gfx_set_pixel()'s native_row = (GFX_FB_ROWS - 1) - x (a real-hardware
    // fix, found live: the panel's native row 0 is wired to the opposite
    // physical edge from a direct x->native_row mapping) -- mirrored here
    // to match, or every PNG this tool writes is horizontally flipped
    // relative to gfx_set_pixel's actual write-side convention. Confirmed:
    // every existing screen_* fixture was mirrored before this fix.
    for (int x = 0; x < GFX_SCREEN_W; x++) {
        const uint8_t *row = gfx_fb_native_row((GFX_FB_ROWS - 1) - x);
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

/* ---------------------------------------------------------------------
 * docs/DEVICE_TASKS.md F6.3: "Host: render_png.c renders each screen with
 * fixture data." F6.3's actual screens (scr_home.c/scr_chat.c/scr_device.c/
 * scr_setup.c) are ESP-IDF/msg.c/modes.c/ident.h-dependent (RTC state, NVS,
 * the message ring, network getters) and are not built for the host by any
 * `#ifdef ESP_PLATFORM` split the way gfx.c/input.c/auth.c/setup.c are —
 * doing that split would be a much larger refactor of modules well outside
 * this task's Files list. So, consistent with render_latin()/render_cjk()/
 * etc. above (hand-drawn fixture strings via gfx.c's own primitives, no
 * dependency on the rest of the firmware), the four functions below draw
 * a representative mockup of each F6.3 screen directly with hardcoded
 * fixture data matching docs/DEVICE_PLAN.md §5.5's own mockups, using the
 * exact same gfx_text()/gfx_icon()/gfx_hline() primitives and the same
 * UI_STATUS_H=14/UI_BODY_TOP=15 band split ui.c's real status bar uses
 * (ui.h) — close enough to eyeball the font/layout choices those screens
 * actually make, without linking code that cannot build on the host.
 * UI_STATUS_H is 14, not the nominal-looking 12, and every footer below
 * uses UI_FOOTER_Y (GFX_SCREEN_H - 16), not "- 9": both confirmed on real
 * hardware to be the minimum needed to stop gfx_set_pixel()'s bounds-check
 * from silently clipping real glyph descenders (see ui.h's own comments on
 * both constants for the measured font-asset numbers behind them).
 * --------------------------------------------------------------------- */

#define FIXTURE_STATUS_H 14
#define FIXTURE_BODY_TOP (FIXTURE_STATUS_H + 1)
#define FIXTURE_FOOTER_Y (GFX_SCREEN_H - 16)

// ui.c's own UI_STATUS_TEXT_Y (-3): confirmed on real hardware that y=1 put
// the status text's ink visibly lower than the icons -- see ui.c's comment
// on that constant for the measured font-asset numbers behind -3.
#define FIXTURE_STATUS_TEXT_Y (-3)
#define FIXTURE_STATUS_ICON_GAP 4 /* matches ui.c's UI_STATUS_ICON_GAP */

// Matches ui.c's draw_status_bar() layout: spelled-out counts on the left,
// MQTT link / signal bars / battery right-aligned on the right (user
// request: "new: 0  unsent: 0" on the left, icons on the right, MQTT and
// bars swapped from an earlier iteration so MQTT sits leftmost of the
// three).
static void draw_fixture_status_bar(int bars, bool link_ok, int unsent, bool lock_on, int unread,
                                     int batt_segs)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "new: %d  unsent: %d", unread, unsent);
    int x = gfx_text(0, FIXTURE_STATUS_TEXT_Y, GFX_FONT_NORMAL, buf) + 3;
    if (lock_on) {
        gfx_icon(x, 0, GFX_ICON_LOCK);
    }

    int batt_x = GFX_SCREEN_W - GFX_ICON_W;
    gfx_icon(batt_x, 0, (gfx_icon_t) (GFX_ICON_BATTERY_0 + batt_segs));

    int bars_x = batt_x - GFX_ICON_W - FIXTURE_STATUS_ICON_GAP;
    gfx_icon(bars_x, 0, (gfx_icon_t) (GFX_ICON_SIGNAL_0 + bars));

    int mqtt_x = bars_x - GFX_ICON_W - FIXTURE_STATUS_ICON_GAP;
    gfx_icon(mqtt_x, 0, link_ok ? GFX_ICON_LINK_OK : GFX_ICON_LINK_X);

    gfx_hline(0, GFX_SCREEN_W - 1, FIXTURE_STATUS_H);
}

static void render_screen_home(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, true, 1, 3);

    int y = FIXTURE_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, ">");
    int x = gfx_text(10, y, GFX_FONT_NORMAL, "mom");
    x = gfx_text(x + 4, y, GFX_FONT_NORMAL, "Pickup at 3:15 by the gym");
    (void) x;
    gfx_text(GFX_SCREEN_W - gfx_text_width(GFX_FONT_NORMAL, "14:02 *"), y, GFX_FONT_NORMAL,
              "14:02 *");
    y += 12;
    gfx_hline(0, GFX_SCREEN_W - 1, y);
    y += 3;
    gfx_text(10, y, GFX_FONT_NORMAL, "New message (needs address book)");
    y += 12;
    gfx_text(10, y, GFX_FONT_NORMAL, "Address book (needs address book)");
    y += 12;
    gfx_text(0, y, GFX_FONT_NORMAL, ">");
    gfx_text(10, y, GFX_FONT_NORMAL, "Device");
    y += 12;
    gfx_text(10, y, GFX_FONT_NORMAL, "Lock now (needs passcode lock)");

    gfx_text(0, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL, "up/down move  enter open  hold=home");
}

static void render_screen_chat(void)
{
    gfx_clear();
    draw_fixture_status_bar(4, true, 0, false, 0, 3);

    struct {
        const char *who, *ts, *body, *tag;
    } rows[] = {
        { "mom", "13:58", "where are you?", NULL },
        { "you", "13:59", "library, coming now", "sent" },
        { "mom", "14:02", "Pickup at 3:15 by the gym", "NEW" },
    };
    int y = FIXTURE_BODY_TOP + 2;
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int x = gfx_text(0, y, GFX_FONT_NORMAL, rows[i].who);
        x = gfx_text(x, y, GFX_FONT_NORMAL, " ");
        x = gfx_text(x, y, GFX_FONT_NORMAL, rows[i].ts);
        x = gfx_text(x, y, GFX_FONT_NORMAL, " ");
        gfx_text(x, y, GFX_FONT_NORMAL, rows[i].body);
        if (rows[i].tag) {
            int tw = gfx_text_width(GFX_FONT_NORMAL, rows[i].tag);
            gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, rows[i].tag);
        }
        y += 14; /* 12 px glyph + 2 px leading, matches scr_chat.c */
    }
    gfx_hline(0, GFX_SCREEN_W - 1, y);
    y += 2;

    const char *counter = "9/160";
    int cw = gfx_text_width(GFX_FONT_NORMAL, counter);
    int x = gfx_text(0, y, GFX_FONT_NORMAL, "> ok coming_");
    (void) x;
    gfx_text(GFX_SCREEN_W - cw, y, GFX_FONT_NORMAL, counter);

    /* no key-hint footer, matches scr_chat.c */
}

static void render_screen_device(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, true, 0, 3);

    const char *lines[] = {
        "id pgr-0001  fw 0.2.0",
        "owner kid1  claimed yes",
        "broker mqtt.example:8883  sig on",
        "signal -93 dBm  batt 3280 mV",
        "session s_3ab91c02  book v- (not synced, needs book.c)",
        "counters memfull 0  drops 0  resets 0",
    };
    int y = FIXTURE_BODY_TOP + 2;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        gfx_text(0, y, GFX_FONT_NORMAL, lines[i]);
        y += 12;
    }
    gfx_text(0, y, GFX_FONT_NORMAL, ">");
    gfx_text(10, y, GFX_FONT_NORMAL, "Re-sync address book");
    y += 12;
    gfx_text(10, y, GFX_FONT_NORMAL, "Text size: normal");

    gfx_text(0, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL, "up/down move  enter select  esc back");
}

static void render_screen_setup(void)
{
    gfx_clear();
    draw_fixture_status_bar(0, false, 0, false, 0, 4);

    int y = FIXTURE_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, "Setup");
    y += 16;
    gfx_text(0, y, GFX_FONT_NORMAL, "type your setup code:");
    y += 12;
    gfx_text(0, y, GFX_FONT_NORMAL, "> 4S29B-K7H1P-QX3M @ mqtt.example");
    y += 16;
    gfx_text(0, y, GFX_FONT_NORMAL, "network . broker . bundle . done");

    gfx_text(0, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL, "enter submit  esc clear/back");
}

/* ---------------------------------------------------------------------
 * docs/DEVICE_TASKS.md F7.2: "make png (fixtures for each screen)" —
 * scr_pick.c/scr_book.c are book.h/ESP-IDF-dependent (book_contact_at() etc.
 * are ESP_PLATFORM-only, book.h's own module comment) and are not built for
 * the host by any #ifdef split, same reasoning the F6.3 fixtures above
 * already give for scr_home.c/scr_chat.c/scr_device.c/scr_setup.c. These
 * four functions hand-draw each of scr_pick.c's/scr_book.c's rendered
 * states with fixture data matching docs/DEVICE_PLAN.md §5.5's own mockups,
 * same gfx_text()/gfx_hline() primitives, same FIXTURE_STATUS_H/BODY_TOP
 * band split.
 * --------------------------------------------------------------------- */

static void render_screen_pick(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, false, 0, 3);

    int y = FIXTURE_BODY_TOP + 2;
    struct {
        const char *label, *tag;
        bool sel;
    } rows[] = {
        { "mom  (default)", "web", true },
        { "dad", "web", false },
        { "grandma", "sms", false },
        { "uncle bob", "pending approval", false },
        { "sam", "not approved", false },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (rows[i].sel) {
            gfx_text(0, y, GFX_FONT_NORMAL, ">");
        }
        gfx_text(10, y, GFX_FONT_NORMAL, rows[i].label);
        int tw = gfx_text_width(GFX_FONT_NORMAL, rows[i].tag);
        gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, rows[i].tag);
        y += 12;
    }

    gfx_text(0, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL, "enter choose   esc back");
}

static void render_screen_book(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, false, 0, 3);

    int y = FIXTURE_BODY_TOP + 2;
    struct {
        const char *label, *tag;
        bool sel;
    } rows[] = {
        { "gma  (grandma)", "sms", false },
        { "mom", "web", true },
        { "dad", "web", false },
        { "uncle bob", "pending approval", false },
        { "sam", "not approved", false },
        { "Add", "", false },
    };
    for (size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (rows[i].sel) {
            gfx_text(0, y, GFX_FONT_NORMAL, ">");
        }
        gfx_text(10, y, GFX_FONT_NORMAL, rows[i].label);
        if (rows[i].tag[0] != '\0') {
            int tw = gfx_text_width(GFX_FONT_NORMAL, rows[i].tag);
            gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, rows[i].tag);
        }
        y += 12;
    }

    gfx_text(0, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL, "up/down move   enter select   esc back");
}

static void render_screen_book_add(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, false, 0, 3);

    int y = FIXTURE_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, "Add contact");
    y += 14;
    gfx_text(0, y, GFX_FONT_NORMAL, "Name   Grandma_");
    y += 12;
    gfx_text(0, y, GFX_FONT_NORMAL, "Phone  +1555123");
    y += 12;
    gfx_text(8, y, GFX_FONT_NORMAL, "(or leave blank and type an @alias)");

    gfx_text(0, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL,
             "tab next field   enter send for approval   esc cancel");
}

static void render_screen_nickname(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, false, 0, 3);

    int y = FIXTURE_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, "Nickname");
    y += 14;
    gfx_text(0, y, GFX_FONT_NORMAL, "for grandma");
    y += 12;
    gfx_text(0, y, GFX_FONT_NORMAL, "> gma_");
    y += 12;
    gfx_text(0, y, GFX_FONT_NORMAL, "3/12");

    gfx_text(0, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL, "enter save   esc cancel");
}

// scr_greeting.c fixtures. Not the real screen (render_png links only
// gfx.c, not ui.c/scr_*.c -- see this file's own module comment on why)
// -- a fixed name order rather than scr_greeting.c's actual random shuffle,
// so the PNG is deterministic to review. Layout matches scr_greeting.c's
// render() by hand: centered, GFX_FONT_LARGE, wrapped to 2 lines max.
static void draw_centered_wrapped(const char *text)
{
    char wrapped[2][64];
    int n = gfx_text_wrap(GFX_FONT_LARGE, text, GFX_SCREEN_W - 16, wrapped, 2);
    int total_h = n * 20;
    int y = FIXTURE_BODY_TOP + (GFX_SCREEN_H - FIXTURE_BODY_TOP - total_h) / 2;
    for (int i = 0; i < n && i < 2; i++) {
        int w = gfx_text_width(GFX_FONT_LARGE, wrapped[i]);
        gfx_text((GFX_SCREEN_W - w) / 2, y, GFX_FONT_LARGE, wrapped[i]);
        y += 20;
    }
}

static void render_screen_greeting(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, false, 0, 3);
    draw_centered_wrapped("Hi Colin! Hi May! Hi Hannah!");
}

// main.c's pre-provisioning boot screen: same banner, plus scr_greeting.c's
// status footer (added for the no-SIM boot path) -- drawn by hand here the
// same way draw_centered_wrapped() mirrors render()'s banner half, since
// this fixture set predates scr_greeting.c existing as a live screen and
// still draws everything itself rather than calling into it.
static void draw_status_footer(const char *status)
{
    int fw = gfx_text_width(GFX_FONT_NORMAL, status);
    gfx_text((GFX_SCREEN_W - fw) / 2, FIXTURE_FOOTER_Y, GFX_FONT_NORMAL, status);
}

static void render_screen_greeting_booting(void)
{
    gfx_clear();
    draw_fixture_status_bar(0, false, 0, false, 0, 0);
    draw_centered_wrapped("Hi Colin! Hi May! Hi Hannah!");
    draw_status_footer("booting");
}

static void render_screen_greeting_sim_missing(void)
{
    gfx_clear();
    draw_fixture_status_bar(0, false, 0, false, 0, 0);
    draw_centered_wrapped("Hi Colin! Hi May! Hi Hannah!");
    draw_status_footer("sim missing");
}

static void render_screen_greeting_shutting_down(void)
{
    gfx_clear();
    draw_fixture_status_bar(0, false, 0, false, 0, 0);
    draw_centered_wrapped("Hi Colin! Hi May! Hi Hannah!");
    draw_status_footer("shutting down");
}

static void render_screen_sleeping(void)
{
    gfx_clear();
    draw_fixture_status_bar(3, true, 0, false, 0, 3);
    draw_centered_wrapped("sleeping");
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
        { "screen_home", render_screen_home },
        { "screen_chat", render_screen_chat },
        { "screen_device", render_screen_device },
        { "screen_setup", render_screen_setup },
        { "screen_pick", render_screen_pick },
        { "screen_book", render_screen_book },
        { "screen_book_add", render_screen_book_add },
        { "screen_nickname", render_screen_nickname },
        { "screen_greeting", render_screen_greeting },
        { "screen_sleeping", render_screen_sleeping },
        { "screen_greeting_booting", render_screen_greeting_booting },
        { "screen_greeting_sim_missing", render_screen_greeting_sim_missing },
        { "screen_greeting_shutting_down", render_screen_greeting_shutting_down },
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

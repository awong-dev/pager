/* gfx.h — framebuffer, UTF-8 text, and icon primitives. Split out of ui.c
 * by docs/DEVICE_TASKS.md F6.1; disp.c (the SSD1680 transport) reads the
 * framebuffer this module owns via gfx_fb_native_row().
 *
 * Authority: docs/DEVICE_PLAN.md §5.1 (296x128 1-bit landscape panel),
 * §5.2 (fonts are pre-rasterised Noto data in a dedicated `assets`
 * partition, coverage, proportional metrics, UTF-8 buffers), §5.4 (status
 * bar/icon list). docs/DEVICE_PLAN.md §9 decisions (Noto Sans + Noto Sans
 * CJK, default language `sc`) are final and not re-litigated here.
 *
 * Geometry (unchanged from the pre-split ui.c, PROTOCOL.md §9.5): native
 * SSD1680 addressing is X = 128px / 8 = 16 bytes (0..15), Y = 296 rows
 * (0..295); this module renders a landscape (296-wide x 128-tall) view by
 * choosing which native (row, bit) a given screen pixel maps to. Screen x
 * maps to the native row, screen y maps to the native byte/bit — see
 * gfx_set_pixel()'s comment in gfx.c. 1 = white (unset), 0 = black (ink),
 * matching the panel's own polarity so disp.c can DMA rows out unchanged.
 *
 * Glyph asset format (tools/mkassets.py is the encoder, gfx.c the only
 * decoder — the layout is private to this pair of files, no third reader):
 *
 *   File header (16 bytes):
 *     0   char[4]  magic "PGFA"
 *     4   u8       version (=1)
 *     5   char[3]  lang, NUL-padded ("sc"/"tc"/"jp"/"kr")
 *     8   u32 LE   total file bytes (sanity check against the actual
 *                  mmap/file size)
 *     12  u8       num size blocks (=2: 12px, 16px)
 *     13  u8[3]    reserved (0)
 *
 *   Each size block, back to back, starts with a 12-byte sub-header:
 *     0   u8       size_px (12 or 16)
 *     1   u8       baseline — pixels from the top of the text line down to
 *                  this size's shared baseline (see gfx_glyph_blit()); one
 *                  value per block, taken from the Latin face's ascender at
 *                  build time, not per-glyph (docs/DEVICE_PLAN.md §5.2 lists
 *                  the per-glyph record as exactly {w, adv, bearing, rows};
 *                  reproducing correct vertical alignment for a proportional,
 *                  multi-face (Latin + CJK) font at a fixed line height also
 *                  needs *some* per-line vertical reference, so this format
 *                  adds one baseline byte per size block rather than a
 *                  second per-glyph field).
 *     2   u8[2]    reserved (0)
 *     4   u32 LE   num_glyphs
 *     8   u32 LE   block_bytes — total size of this block including this
 *                  12-byte sub-header, so a reader can skip to the next
 *                  block without decoding this one
 *   followed by, within the block:
 *     - codepoint table: num_glyphs * u32 LE, strictly ascending (binary
 *       search key)
 *     - glyph record table: num_glyphs * 10 bytes, index-parallel to the
 *       codepoint table:
 *         0 u8   w            bitmap width, pixels
 *         1 u8   adv          advance width, pixels (pen_x += adv)
 *         2 i8   bearing_x    left-side bearing, pixels (may be negative)
 *         3 i8   bearing_y    height from this block's baseline up to the
 *                             top of the bitmap (FreeType's bitmap_top),
 *                             may be negative for descenders
 *         4 u8   rows         bitmap height, pixels
 *         5 u8   reserved (0)
 *         6 u32 LE bitmap_offset — byte offset into this block's bitmap
 *                             blob (below), NOT into the file (bytes 6-9,
 *                             so the record is 10 bytes total, not 9 --
 *                             an earlier draft of this comment miscounted)
 *     - bitmap blob: each glyph's rows, MSB-first, packed to
 *       ceil(w/8) bytes per row (FreeType FT_LOAD_TARGET_MONO's own packing),
 *       concatenated in codepoint order; bit=1 means ink.
 *
 *   A codepoint outside every block's table renders as a tofu box
 *   (gfx_icon-sized outline) and increments gfx_get_tofu_count().
 */
#ifndef GFX_H
#define GFX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Geometry — the single source of truth disp.c builds its own shadow
 * plane and SPI row loop against (formerly ui.c's FB_ROW_BYTES/FB_ROWS).
 * --------------------------------------------------------------------- */
#define GFX_FB_ROW_BYTES 16
#define GFX_FB_ROWS 296
#define GFX_SCREEN_W GFX_FB_ROWS               /* 296 — landscape width */
#define GFX_SCREEN_H (GFX_FB_ROW_BYTES * 8)    /* 128 — landscape height */

/* Text size is a Setting (normal/large), DEVICE_PLAN.md §5.2; body text
 * uses whichever is selected, status bar/footer are always GFX_FONT_NORMAL. */
typedef enum {
    GFX_FONT_NORMAL = 0, /* 12 px */
    GFX_FONT_LARGE = 1,  /* 16 px */
} gfx_font_t;

/* 12x12 status-bar/footer icon ids, docs/DEVICE_PLAN.md §5.2/§5.4. Bitmaps
 * are hand-authored for this project (not transcribed from any external
 * icon set), same licensing rationale ui.c's retired 5x7 font used to
 * state; on-glass legibility is UNVERIFIED/PENDING_HW like every other
 * visual claim in this codebase. */
typedef enum {
    GFX_ICON_SIGNAL_0 = 0,
    GFX_ICON_SIGNAL_1,
    GFX_ICON_SIGNAL_2,
    GFX_ICON_SIGNAL_3,
    GFX_ICON_SIGNAL_4,
    GFX_ICON_BATTERY_0,
    GFX_ICON_BATTERY_1,
    GFX_ICON_BATTERY_2,
    GFX_ICON_BATTERY_3,
    GFX_ICON_BATTERY_4,
    GFX_ICON_LINK_OK,
    GFX_ICON_LINK_X,
    GFX_ICON_LOCK,
    GFX_ICON_PENDING, /* clock, unsent/pending-ack marker */
    GFX_ICON_SMS,
    GFX_ICON_WEB,
    GFX_ICON_CHAT,
    /* v0.2 §4.3 (CA trust, docs/V02_DESIGN.md/docs/V02_DESIGN.md §4.3):
     * TLS trust-state indicators, drawn from code (gfx.c's own
     * primitives) exactly like every other icon here, NOT from the assets
     * partition — see gfx.c's icon_padlock() for the shape. Distinct from
     * GFX_ICON_LOCK above, which is envelope-signing's own "[lock if sig
     * on]" indicator (docs/DEVICE_PLAN.md §5.4), a different meaning that
     * happens to use a similar glyph. */
    GFX_ICON_TLS_PINNED,  /* closed padlock: CA pinned, last connect validated */
    GFX_ICON_TLS_BROKEN,  /* broken padlock: CA pinned, running unvalidated */
    GFX_ICON_COUNT,
} gfx_icon_t;
#define GFX_ICON_W 12
#define GFX_ICON_H 12

/* ---------------------------------------------------------------------
 * Asset loading.
 * --------------------------------------------------------------------- */

/* On-device: mmaps the `assets` partition (esp_partition_mmap, read-only,
 * never unmapped — the mapping lives for the process lifetime) and
 * validates the header. Power/sleep effect: none (flash is
 * memory-mapped, not actively read until a glyph is blitted; no peripheral
 * is powered by this call). Returns false (logs once) if the partition is
 * missing/corrupt/oversized for its declared total_bytes — callers must
 * keep running with every codepoint drawing as tofu, mirroring ui.c's
 * existing "headless is not fatal" contract for the display driver. */
#ifdef ESP_PLATFORM
bool gfx_init(void);
#else
/* Host build (firmware/host/render_png.c): no partition table, so the
 * caller names the assets.bin file directly. mmap()'d read-only, same
 * validation as the device path. */
bool gfx_init_from_file(const char *path);
#endif

void gfx_clear(void); /* framebuffer -> all white (0xFF bytes) */

/* ---------------------------------------------------------------------
 * Drawing primitives, docs/DEVICE_PLAN.md §5.2's list.
 * --------------------------------------------------------------------- */

/* Sets/clears one screen pixel; out-of-bounds (x,y) is a silent no-op
 * (every other primitive below is built on this, so bounds-checking here
 * is what makes text/icon calls naturally clip at the screen edge). */
void gfx_set_pixel(int x, int y, bool black);

void gfx_hline(int x0, int x1, int y);

/* Unfilled rectangle outline, 1px lines, black. */
void gfx_rect(int x, int y, int w, int h);

/* XORs every pixel in the given box — the selection-row highlight. */
void gfx_invert_rect(int x, int y, int w, int h);

void gfx_icon(int x, int y, gfx_icon_t id);

/* Draws `utf8` left-to-right starting at (x, y) — y is the TOP of the
 * text line, not the baseline (matches the pre-split ui.c's draw_text_n()
 * convention so callers didn't need to change their layout math). Glyphs
 * missing from the asset store draw as a small tofu box of the glyph's
 * `adv` width and increment gfx_get_tofu_count(). Draws are naturally
 * clipped at the screen edge by gfx_set_pixel(); there is no separate
 * "max_chars" clip parameter any more (proportional fonts have no fixed
 * char pitch to clip by). Returns the ending pen x position, so callers
 * chain fragments the same way the old code chained draw_text_n()'s
 * returned character count: `x = gfx_text(x, y, sz, a); x = gfx_text(x, y,
 * sz, b);`. */
int gfx_text(int x, int y, gfx_font_t size, const char *utf8);

/* Pixel width `utf8` would occupy if drawn with gfx_text(), without
 * drawing it (sum of each decoded codepoint's `adv`, tofu counted as its
 * own placeholder width). */
int gfx_text_width(gfx_font_t size, const char *utf8);

/* Splits `utf8` into lines no wider than max_width_px, wrapping on space
 * boundaries for scripts that have them and per-character for CJK
 * (docs/DEVICE_PLAN.md §5.2) — a codepoint >= 0x2E80 (CJK radicals and
 * above; see gfx.c) is treated as breakable on its own. Each line is
 * written as a NUL-terminated byte slice (never splitting a UTF-8
 * sequence) into out_lines[i][0..out_line_cap), i < max_lines. Returns
 * the number of lines produced (>0 even for an empty string: one empty
 * line), or -1 if the text needs more than max_lines lines (out_lines is
 * still filled up to max_lines in that case, matching the "refuse rather
 * than silently drop" spirit of PROTOCOL.md's other input caps). */
int gfx_text_wrap(gfx_font_t size, const char *utf8, int max_width_px, char out_lines[][64],
                   int max_lines);

/* Number of codepoints drawn since boot that had no glyph in either size
 * block (tofu boxes) — shown on the Device screen per §5.2. */
uint32_t gfx_get_tofu_count(void);

/* ---------------------------------------------------------------------
 * disp.c's read side: the framebuffer this module owns. `native_row` is
 * 0..GFX_FB_ROWS-1; the returned pointer is valid for GFX_FB_ROW_BYTES
 * bytes and is disp.c's own row buffer to read (never write) or diff
 * against its shadow plane.
 * --------------------------------------------------------------------- */
const uint8_t *gfx_fb_native_row(int native_row);

#ifdef __cplusplus
}
#endif

#endif /* GFX_H */

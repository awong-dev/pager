/* gfx.c — see gfx.h for the asset format, geometry and API contract.
 *
 * Authority: docs/DEVICE_PLAN.md §5.2 (fonts/coverage/metrics), §5.4 (icon
 * list). No modem or sleep-state effect: this module only ever touches
 * memory it owns and (read-only) the mmap'd `assets` partition/file.
 */
#include "gfx.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_partition.h"
static const char *TAG = "gfx";
#else
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* ---------------------------------------------------------------------
 * Framebuffer. 1 = white (unset), 0 = black (ink) — matches the SSD1680's
 * own RAM polarity so disp.c can send rows out unmodified. Screen x maps
 * to the native row, screen y maps to the native byte/bit (see
 * gfx_set_pixel below) — this is the landscape-rotation trick the
 * pre-split ui.c used, carried forward unchanged.
 * --------------------------------------------------------------------- */
static uint8_t s_fb[GFX_FB_ROWS][GFX_FB_ROW_BYTES];

void gfx_clear(void) { memset(s_fb, 0xFF, sizeof(s_fb)); }

void gfx_set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= GFX_SCREEN_W || y < 0 || y >= GFX_SCREEN_H) {
        return;
    }
    // Hardware bring-up finding (docs/DEVICE_TASKS_LOG.md): the panel's
    // native row 0 is wired to the opposite physical edge from what a
    // direct x->native_row mapping assumed, producing a horizontal mirror
    // with no vertical flip. Reversed here rather than by changing the
    // RAM Y-address direction in disp.c, so this is the single point of
    // truth for the landscape-rotation mapping.
    int native_row = (GFX_FB_ROWS - 1) - x;
    int native_byte = y / 8;
    int bit = 7 - (y % 8);
    if (black) {
        s_fb[native_row][native_byte] &= (uint8_t) ~(1u << bit);
    } else {
        s_fb[native_row][native_byte] |= (uint8_t) (1u << bit);
    }
}

const uint8_t *gfx_fb_native_row(int native_row)
{
    if (native_row < 0 || native_row >= GFX_FB_ROWS) {
        static const uint8_t zero[GFX_FB_ROW_BYTES] = { 0 };
        return zero;
    }
    return s_fb[native_row];
}

void gfx_hline(int x0, int x1, int y)
{
    if (x1 < x0) {
        int t = x0;
        x0 = x1;
        x1 = t;
    }
    for (int x = x0; x <= x1; x++) {
        gfx_set_pixel(x, y, true);
    }
}

void gfx_rect(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    for (int i = 0; i < w; i++) {
        gfx_set_pixel(x + i, y, true);
        gfx_set_pixel(x + i, y + h - 1, true);
    }
    for (int j = 0; j < h; j++) {
        gfx_set_pixel(x, y + j, true);
        gfx_set_pixel(x + w - 1, y + j, true);
    }
}

void gfx_invert_rect(int x, int y, int w, int h)
{
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < 0 || yy >= GFX_SCREEN_H) {
            continue;
        }
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < 0 || xx >= GFX_SCREEN_W) {
                continue;
            }
            int native_row = xx;
            int native_byte = yy / 8;
            int bit = 7 - (yy % 8);
            s_fb[native_row][native_byte] ^= (uint8_t) (1u << bit);
        }
    }
}

/* ---------------------------------------------------------------------
 * Asset store. See gfx.h for the on-disk/on-flash layout; this section is
 * the only reader of it.
 * --------------------------------------------------------------------- */

typedef struct {
    bool valid;
    uint8_t size_px;
    uint8_t baseline;
    uint32_t num_glyphs;
    const uint8_t *codepoints; /* num_glyphs * 4 bytes, ascending */
    const uint8_t *records;    /* num_glyphs * 10 bytes, see gfx.h */
    const uint8_t *bitmaps;    /* blob start */
} gfx_block_t;

typedef struct {
    uint8_t w;
    uint8_t adv;
    int8_t bearing_x;
    int8_t bearing_y;
    uint8_t rows;
    uint32_t bitmap_offset;
} gfx_glyph_t;

static const uint8_t *s_assets;
static size_t s_assets_len;
static gfx_block_t s_blocks[2]; /* [GFX_FONT_NORMAL], [GFX_FONT_LARGE] */
static uint32_t s_tofu_count;

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

static bool parse_assets(const uint8_t *data, size_t len)
{
    memset(s_blocks, 0, sizeof(s_blocks));
    s_assets = data;
    s_assets_len = len;

    if (len < 16 || memcmp(data, "PGFA", 4) != 0 || data[4] != 1) {
        return false;
    }
    uint32_t total = rd_u32(data + 8);
    if (total > len) {
        return false;
    }
    uint8_t num_sizes = data[12];

    size_t pos = 16;
    for (uint8_t i = 0; i < num_sizes; i++) {
        if (pos + 12 > total) {
            return false;
        }
        uint8_t size_px = data[pos];
        uint8_t baseline = data[pos + 1];
        uint32_t num_glyphs = rd_u32(data + pos + 4);
        uint32_t block_bytes = rd_u32(data + pos + 8);
        if (block_bytes < 12 || pos + block_bytes > total) {
            return false;
        }

        int slot = -1;
        if (size_px == 12) {
            slot = GFX_FONT_NORMAL;
        } else if (size_px == 16) {
            slot = GFX_FONT_LARGE;
        }
        if (slot >= 0) {
            const uint8_t *codepoints = data + pos + 12;
            const uint8_t *records = codepoints + (size_t) num_glyphs * 4;
            const uint8_t *bitmaps = records + (size_t) num_glyphs * 10;
            s_blocks[slot].valid = true;
            s_blocks[slot].size_px = size_px;
            s_blocks[slot].baseline = baseline;
            s_blocks[slot].num_glyphs = num_glyphs;
            s_blocks[slot].codepoints = codepoints;
            s_blocks[slot].records = records;
            s_blocks[slot].bitmaps = bitmaps;
        }
        pos += block_bytes;
    }
    return true;
}

#ifdef ESP_PLATFORM
bool gfx_init(void)
{
    /* Power/sleep effect: none — memory-mapping flash draws no additional
     * current and reads nothing until a glyph is blitted (docs/PROTOCOL.md
     * has no power claim over `assets`, which is idle flash the rest of
     * the time). */
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "assets");
    if (part == NULL) {
        ESP_LOGI(TAG, "no 'assets' partition found; every codepoint will draw as tofu");
        return false;
    }
    const void *mapped;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped,
                                        &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "esp_partition_mmap failed: %d; every codepoint will draw as tofu",
                 (int) err);
        return false;
    }
    if (!parse_assets((const uint8_t *) mapped, part->size)) {
        ESP_LOGI(TAG, "assets partition header invalid; every codepoint will draw as tofu");
        return false;
    }
    ESP_LOGI(TAG, "assets loaded: %u bytes, sizes 12px=%s 16px=%s", (unsigned) part->size,
             s_blocks[GFX_FONT_NORMAL].valid ? "ok" : "missing",
             s_blocks[GFX_FONT_LARGE].valid ? "ok" : "missing");
    return true;
}
#else
bool gfx_init_from_file(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "gfx_init_from_file: open(%s) failed\n", path);
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return false;
    }
    void *mapped = mmap(NULL, (size_t) st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* mapping stays valid after close() on POSIX */
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "gfx_init_from_file: mmap(%s) failed\n", path);
        return false;
    }
    if (!parse_assets((const uint8_t *) mapped, (size_t) st.st_size)) {
        fprintf(stderr, "gfx_init_from_file: %s header invalid\n", path);
        return false;
    }
    return true;
}
#endif

static bool lookup_glyph(const gfx_block_t *blk, uint32_t cp, gfx_glyph_t *out)
{
    if (!blk->valid || blk->num_glyphs == 0) {
        return false;
    }
    /* Binary search the ascending codepoint table (mkassets.py's contract). */
    uint32_t lo = 0, hi = blk->num_glyphs; /* [lo, hi) */
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        uint32_t v = rd_u32(blk->codepoints + (size_t) mid * 4);
        if (v == cp) {
            const uint8_t *r = blk->records + (size_t) mid * 10;
            out->w = r[0];
            out->adv = r[1];
            out->bearing_x = (int8_t) r[2];
            out->bearing_y = (int8_t) r[3];
            out->rows = r[4];
            out->bitmap_offset = rd_u32(r + 6);
            return true;
        }
        if (v < cp) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

/* ---------------------------------------------------------------------
 * UTF-8 decode. Malformed leading bytes and truncated sequences at the
 * end of the buffer decode as U+FFFD and advance exactly one byte, so a
 * caller looping until the NUL terminator can never stall.
 * --------------------------------------------------------------------- */
static const char *utf8_next(const char *p, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t *) p;
    uint8_t b0 = u[0];
    int extra;
    uint32_t v;
    if (b0 < 0x80) {
        *cp = b0;
        return p + 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        extra = 1;
        v = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2;
        v = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        extra = 3;
        v = b0 & 0x07;
    } else {
        *cp = 0xFFFD;
        return p + 1;
    }
    for (int i = 1; i <= extra; i++) {
        uint8_t bi = u[i];
        if (bi == 0 || (bi & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return p + 1;
        }
        v = (v << 6) | (bi & 0x3F);
    }
    *cp = v;
    return p + 1 + extra;
}

static bool is_cjk(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) ||  /* CJK radicals..Unified Ideographs */
           (cp >= 0x3000 && cp <= 0x303F) ||  /* CJK punctuation */
           (cp >= 0xAC00 && cp <= 0xD7A3) ||  /* Hangul syllables */
           (cp >= 0xFF00 && cp <= 0xFFEF);    /* halfwidth/fullwidth forms */
}

static int tofu_adv(gfx_font_t size) { return size == GFX_FONT_LARGE ? 10 : 8; }

static void draw_tofu(int pen_x, int line_top_y, gfx_font_t size)
{
    int w = tofu_adv(size) - 2;
    int h = (size == GFX_FONT_LARGE) ? 12 : 9;
    int y = line_top_y + ((size == GFX_FONT_LARGE ? 16 : 12) - h) / 2;
    gfx_rect(pen_x, y, w, h);
    s_tofu_count++;
}

static int glyph_adv(gfx_font_t size, uint32_t cp)
{
    gfx_glyph_t g;
    if (lookup_glyph(&s_blocks[size], cp, &g)) {
        return g.adv;
    }
    return tofu_adv(size);
}

static void blit_glyph(int pen_x, int line_top_y, const gfx_block_t *blk, const gfx_glyph_t *g)
{
    int row_bytes = (g->w + 7) / 8;
    const uint8_t *bmp = blk->bitmaps + g->bitmap_offset;
    int y0 = line_top_y + (blk->baseline - g->bearing_y);
    for (int r = 0; r < g->rows; r++) {
        const uint8_t *rowp = bmp + (size_t) r * row_bytes;
        for (int c = 0; c < g->w; c++) {
            int byte_i = c / 8;
            int bit_i = 7 - (c % 8);
            if ((rowp[byte_i] >> bit_i) & 1) {
                gfx_set_pixel(pen_x + g->bearing_x + c, y0 + r, true);
            }
        }
    }
}

int gfx_text(int x, int y, gfx_font_t size, const char *utf8)
{
    const gfx_block_t *blk = &s_blocks[size];
    int pen = x;
    const char *p = utf8;
    while (*p) {
        uint32_t cp;
        p = utf8_next(p, &cp);
        gfx_glyph_t g;
        if (lookup_glyph(blk, cp, &g)) {
            blit_glyph(pen, y, blk, &g);
            pen += g.adv;
        } else {
            draw_tofu(pen, y, size);
            pen += tofu_adv(size);
        }
    }
    return pen;
}

int gfx_text_width(gfx_font_t size, const char *utf8)
{
    int w = 0;
    const char *p = utf8;
    while (*p) {
        uint32_t cp;
        p = utf8_next(p, &cp);
        w += glyph_adv(size, cp);
    }
    return w;
}

int gfx_text_wrap(gfx_font_t size, const char *utf8, int max_width_px, char out_lines[][64],
                   int max_lines)
{
    const int cap = 63; /* out_lines[i] is char[64], leave room for the NUL */
    int line_count = 0;
    const char *line_start = utf8;
    const char *cur = utf8;
    int width = 0;
    /* Last valid break point within the current line: byte length from
     * line_start, pixel width up to (not including) the break, and
     * whether the break character itself (a space) must be dropped when
     * the line is cut there. */
    int have_break = 0;
    ptrdiff_t break_len = 0;
    int break_drop = 0; /* bytes to skip (the space) when resuming after the break */

    for (;;) {
        if (*cur == '\0') {
            if (line_count >= max_lines) {
                return -1;
            }
            ptrdiff_t len = cur - line_start;
            if (len > cap) {
                len = cap; /* defensive; should not happen given per-token checks below */
            }
            memcpy(out_lines[line_count], line_start, (size_t) len);
            out_lines[line_count][len] = '\0';
            line_count++;
            return line_count;
        }

        uint32_t cp;
        const char *next = utf8_next(cur, &cp);
        int cw = glyph_adv(size, cp);
        ptrdiff_t new_len = next - line_start;

        bool must_break = (width + cw > max_width_px && cur != line_start) || new_len > cap;
        if (must_break) {
            if (line_count >= max_lines) {
                return -1;
            }
            ptrdiff_t len;
            const char *resume;
            if (have_break) {
                len = break_len;
                resume = line_start + break_len + break_drop;
            } else if (cur != line_start) {
                /* No break point in this line at all (a single run wider
                 * than the line) — hard-break exactly before `cur`. */
                len = cur - line_start;
                resume = cur;
            } else {
                /* Even the very first codepoint of this line doesn't fit
                 * (a huge glyph, or max_width_px smaller than one glyph).
                 * Emit it alone anyway — dropping it would violate the
                 * "refuse rather than silently drop" contract this
                 * function's own header comment promises, and resuming at
                 * `cur` (unchanged) would never make forward progress. */
                len = new_len;
                resume = next;
            }
            if (len > cap) {
                len = cap;
            }
            memcpy(out_lines[line_count], line_start, (size_t) len);
            out_lines[line_count][len] = '\0';
            line_count++;

            line_start = resume;
            cur = resume;
            width = 0;
            have_break = 0;
            continue; /* re-evaluate *cur (could be '\0', another space, etc.) */
        }

        width += cw;
        if (cp == ' ') {
            have_break = 1;
            break_len = cur - line_start; /* line up to (not including) the space */
            break_drop = (int) (next - cur); /* drop the space's own byte(s) */
        } else if (is_cjk(cp)) {
            have_break = 1;
            break_len = next - line_start; /* line including this character */
            break_drop = 0;
        }
        cur = next;
    }
}

uint32_t gfx_get_tofu_count(void) { return s_tofu_count; }

/* ---------------------------------------------------------------------
 * Icons, docs/DEVICE_PLAN.md §5.2/§5.4. Drawn procedurally with the
 * primitives above rather than as hand-encoded bitmap tables — simpler to
 * get right without a way to render/photograph the panel in this
 * environment (PENDING_HW/UNVERIFIED like every other visual claim in
 * this codebase).
 * --------------------------------------------------------------------- */

static void icon_bars(int x, int y, int filled /* 0..4 */)
{
    for (int b = 0; b < 4; b++) {
        int bar_h = 3 + b * 2; /* 3,5,7,9 px, fits the 12px cell */
        int bx = x + b * 3;
        int by = y + (GFX_ICON_H - bar_h);
        bool on = b < filled;
        for (int yy = 0; yy < bar_h; yy++) {
            bool edge = (yy == 0 || yy == bar_h - 1);
            for (int xx = 0; xx < 2; xx++) {
                if (on || edge) {
                    gfx_set_pixel(bx + xx, by + yy, true);
                }
            }
        }
    }
}

static void icon_battery(int x, int y, int filled /* 0..4 */)
{
    gfx_rect(x, y + 2, 10, GFX_ICON_H - 4);
    gfx_set_pixel(x + 10, y + 4, true); /* nub */
    gfx_set_pixel(x + 10, y + 5, true);
    for (int s = 0; s < filled; s++) {
        int sx = x + 2 + s * 2;
        for (int yy = y + 4; yy < y + GFX_ICON_H - 4; yy++) {
            gfx_set_pixel(sx, yy, true);
        }
    }
}

void gfx_icon(int x, int y, gfx_icon_t id)
{
    switch (id) {
    case GFX_ICON_SIGNAL_0:
    case GFX_ICON_SIGNAL_1:
    case GFX_ICON_SIGNAL_2:
    case GFX_ICON_SIGNAL_3:
    case GFX_ICON_SIGNAL_4:
        icon_bars(x, y, (int) id - (int) GFX_ICON_SIGNAL_0);
        break;
    case GFX_ICON_BATTERY_0:
    case GFX_ICON_BATTERY_1:
    case GFX_ICON_BATTERY_2:
    case GFX_ICON_BATTERY_3:
    case GFX_ICON_BATTERY_4:
        icon_battery(x, y, (int) id - (int) GFX_ICON_BATTERY_0);
        break;
    case GFX_ICON_LINK_OK:
        /* checkmark */
        for (int i = 0; i < 4; i++) {
            gfx_set_pixel(x + 1 + i, y + 5 + i, true);
        }
        for (int i = 0; i < 7; i++) {
            gfx_set_pixel(x + 5 + i, y + 9 - i, true);
        }
        break;
    case GFX_ICON_LINK_X: {
        /* "no entry" sign: a circle outline (midpoint circle algorithm) plus
         * a single bottom-left-to-top-right slash through it, replacing the
         * old corner-to-corner X at the user's request -- easier to tell
         * apart from GFX_ICON_LOCK/other diagonal-heavy icons at a glance. */
        int cx = x + GFX_ICON_W / 2;
        int cy = y + GFX_ICON_H / 2;
        int r = GFX_ICON_W / 2 - 1;
        int dx = r, dy = 0, err = 0;
        while (dx >= dy) {
            gfx_set_pixel(cx + dx, cy + dy, true);
            gfx_set_pixel(cx + dy, cy + dx, true);
            gfx_set_pixel(cx - dy, cy + dx, true);
            gfx_set_pixel(cx - dx, cy + dy, true);
            gfx_set_pixel(cx - dx, cy - dy, true);
            gfx_set_pixel(cx - dy, cy - dx, true);
            gfx_set_pixel(cx + dy, cy - dx, true);
            gfx_set_pixel(cx + dx, cy - dy, true);
            dy++;
            if (err <= 0) {
                err += 2 * dy + 1;
            }
            if (err > 0) {
                dx--;
                err -= 2 * dx + 1;
            }
        }
        for (int i = -r; i <= r; i++) {
            gfx_set_pixel(cx + i, cy - i, true);
        }
        break;
    }
    case GFX_ICON_LOCK:
        gfx_rect(x + 1, y + 5, 10, 7);
        gfx_hline(x + 3, x + 8, y + 2);
        gfx_set_pixel(x + 3, y + 3, true);
        gfx_set_pixel(x + 3, y + 4, true);
        gfx_set_pixel(x + 8, y + 3, true);
        gfx_set_pixel(x + 8, y + 4, true);
        gfx_set_pixel(x + 5, y + 8, true);
        gfx_set_pixel(x + 6, y + 8, true);
        break;
    case GFX_ICON_PENDING:
        gfx_rect(x + 1, y + 1, 10, 10);
        gfx_set_pixel(x + 6, y + 6, true);
        gfx_set_pixel(x + 6, y + 4, true);
        gfx_set_pixel(x + 6, y + 5, true);
        gfx_set_pixel(x + 7, y + 6, true);
        gfx_set_pixel(x + 8, y + 6, true);
        break;
    case GFX_ICON_SMS:
        gfx_rect(x, y + 1, GFX_ICON_W, GFX_ICON_H - 3);
        for (int i = 0; i < 6; i++) {
            gfx_set_pixel(x + i, y + 1 + i, true);
            gfx_set_pixel(x + GFX_ICON_W - 1 - i, y + 1 + i, true);
        }
        break;
    case GFX_ICON_WEB:
        gfx_rect(x + 1, y + 1, 10, 10);
        gfx_hline(x + 1, x + 10, y + 6);
        for (int i = 1; i < 10; i++) {
            gfx_set_pixel(x + 6, y + i, true);
        }
        break;
    case GFX_ICON_CHAT:
        gfx_rect(x, y, GFX_ICON_W, GFX_ICON_H - 3);
        gfx_set_pixel(x + 2, y + GFX_ICON_H - 3, true);
        gfx_set_pixel(x + 3, y + GFX_ICON_H - 3, true);
        gfx_set_pixel(x + 2, y + GFX_ICON_H - 2, true);
        break;
    default:
        gfx_rect(x, y, GFX_ICON_W, GFX_ICON_H);
        break;
    }
}

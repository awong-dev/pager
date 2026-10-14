/* render_emoji.c -- host test sheet for the emoji coverage in the assets
 * image (tools/mkassets.py --emoji-font). Reuses render_png.c's PNG writer
 * and framebuffer snapshot by including it with its main() renamed.
 *
 * Usage: ./render_emoji <assets.bin> [out_prefix]
 * Writes <prefix>_text.png, <prefix>_grid12.png, <prefix>_grid16.png and
 * exits non-zero if any listed string drew a tofu box (gfx_get_tofu_count). */
#define main render_png_main
#include "render_png.c"
#undef main

#define S_TEXT "Ok \xF0\x9F\x91\x8D see you at 3 \xE2\x9D\xA4\xEF\xB8\x8F \xF0\x9F\x98\x82 " \
    "\xF0\x9F\x8E\x89 \xE2\x9C\x85 \xE2\x9D\x8C \xE2\xAD\x90 \xF0\x9F\x94\xA5 " \
    "\xF0\x9F\x99\x8F \xF0\x9F\x91\x8B \xF0\x9F\xA4\x94 \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"

/* ~60 common emoji, codepoints. */
static const uint32_t GRID[] = {
    0x1F600, 0x1F601, 0x1F602, 0x1F923, 0x1F603, 0x1F604, 0x1F605, 0x1F606, 0x1F609, 0x1F60A,
    0x1F60D, 0x1F618, 0x1F60E, 0x1F914, 0x1F610, 0x1F62E, 0x1F62D, 0x1F622, 0x1F621, 0x1F631,
    0x1F44D, 0x1F44E, 0x1F44F, 0x1F44B, 0x1F64F, 0x1F4AA, 0x1F44C, 0x270C, 0x1F91D, 0x1F64C,
    0x2764, 0x1F499, 0x1F49B, 0x1F494, 0x1F389, 0x1F381, 0x1F382, 0x1F525, 0x2728, 0x1F4AF,
    0x2705, 0x274C, 0x2B50, 0x2714, 0x2757, 0x2753, 0x26A0, 0x1F6D1, 0x1F680, 0x1F697,
    0x1F3E0, 0x1F355, 0x1F354, 0x2615, 0x1F37A, 0x1F436, 0x1F431, 0x1F31E, 0x1F319, 0x2744,
};

static int tofu_total;

static int put_cp(int x, int y, gfx_font_t f, uint32_t cp)
{
    char b[5] = { 0 };
    if (cp < 0x80) { b[0] = (char) cp; }
    else if (cp < 0x800) { b[0] = (char) (0xC0 | cp >> 6); b[1] = (char) (0x80 | (cp & 63)); }
    else if (cp < 0x10000) { b[0] = (char) (0xE0 | cp >> 12); b[1] = (char) (0x80 | ((cp >> 6) & 63)); b[2] = (char) (0x80 | (cp & 63)); }
    else { b[0] = (char) (0xF0 | cp >> 18); b[1] = (char) (0x80 | ((cp >> 12) & 63)); b[2] = (char) (0x80 | ((cp >> 6) & 63)); b[3] = (char) (0x80 | (cp & 63)); }
    return gfx_text(x, y, f, b);
}

static void check(const char *what, uint32_t before)
{
    uint32_t d = gfx_get_tofu_count() - before;
    printf("%-22s tofu=%u\n", what, (unsigned) d);
    tofu_total += (int) d;
}

static void grid(const char *path, gfx_font_t f, int pitch)
{
    gfx_clear();
    uint32_t before = gfx_get_tofu_count();
    int x = 0, y = 0;
    for (size_t i = 0; i < sizeof(GRID) / sizeof(GRID[0]); i++) {
        int w = gfx_glyph_advance(f, GRID[i]) + 2;
        if (x + w > GFX_SCREEN_W) { x = 0; y += pitch; }
        x = put_cp(x, y, f, GRID[i]) + 2;
    }
    check(path, before);
    snapshot_framebuffer();
    write_png_gray8(path, &s_gray[0][0], GFX_SCREEN_W, GFX_SCREEN_H);
}

int main(int argc, char **argv)
{
    if (argc < 2 || !gfx_init_from_file(argv[1])) { fprintf(stderr, "usage: render_emoji assets.bin [prefix]\n"); return 2; }
    const char *pre = argc > 2 ? argv[2] : "build/emoji";
    char path[256];

    gfx_clear();
    uint32_t before = gfx_get_tofu_count();
    gfx_text(0, 0, GFX_FONT_NORMAL, S_TEXT);
    gfx_hline(0, GFX_SCREEN_W - 1, 14);
    check("text 12px", before);
    before = gfx_get_tofu_count();
    gfx_text(0, 20, GFX_FONT_LARGE, S_TEXT);
    gfx_hline(0, GFX_SCREEN_W - 1, 40);
    check("text 16px", before);
    char lines[6][64];
    int n = gfx_text_wrap(GFX_FONT_NORMAL, S_TEXT " " S_TEXT, 150, lines, 6);
    for (int i = 0; i < n; i++) { gfx_text(0, 46 + i * 14, GFX_FONT_NORMAL, lines[i]); }
    gfx_text(150, 46, GFX_FONT_LARGE, "Hi \xF0\x9F\x91\x8B Mom");
    printf("text_width 12 'a\\xE2\\x9D\\xA4\\xEF\\xB8\\x8F b' = %d (heart adv %d)\n",
           gfx_text_width(GFX_FONT_NORMAL, "a\xE2\x9D\xA4\xEF\xB8\x8F" "b"),
           gfx_glyph_advance(GFX_FONT_NORMAL, 0x2764));
    snapshot_framebuffer();
    snprintf(path, sizeof path, "%s_text.png", pre);
    write_png_gray8(path, &s_gray[0][0], GFX_SCREEN_W, GFX_SCREEN_H);

    snprintf(path, sizeof path, "%s_grid12.png", pre);
    grid(path, GFX_FONT_NORMAL, 14);
    snprintf(path, sizeof path, "%s_grid16.png", pre);
    grid(path, GFX_FONT_LARGE, 18);
    printf("tofu_total=%d\n", tofu_total);
    return tofu_total ? 1 : 0;
}

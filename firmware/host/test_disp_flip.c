/* test_disp_flip.c — host test harness for main/disp_flip.c (owner request:
 * NVS `disp_flip`, 180-degree display rotation so the pager can be read
 * upside down). No ESP-IDF dependency (disp_flip.h's own module comment).
 *
 * Coverage required by the task brief:
 *  - byte bit-reversal (disp_flip_bitreverse())
 *  - a known pattern's full-frame 180-degree rotation is an involution
 *    (applying disp_flip_row()+disp_flip_src_row() twice reproduces the
 *    original frame exactly)
 *  - a partial window [first,last] maps to the mirrored window, and that
 *    map is its own inverse
 *  - the mirrored window stays 8-row-aligned when the input window already
 *    is (disp.c's PAGER_UI_PARTIAL_ROW_ALIGN invariant)
 */
#include "disp_flip.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                 \
    do {                                                  \
        if (!(cond)) {                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            g_failures++;                                 \
        }                                                  \
    } while (0)

#define ROWS 296
#define ROW_BYTES 16

/* ---------------------------------------------------------------------
 * disp_flip_bitreverse()
 * --------------------------------------------------------------------- */
static void test_bitreverse(void)
{
    CHECK(disp_flip_bitreverse(0x00) == 0x00, "0x00 reverses to 0x00");
    CHECK(disp_flip_bitreverse(0xFF) == 0xFF, "0xFF reverses to 0xFF");
    CHECK(disp_flip_bitreverse(0x01) == 0x80, "bit0 -> bit7");
    CHECK(disp_flip_bitreverse(0x80) == 0x01, "bit7 -> bit0");
    CHECK(disp_flip_bitreverse(0xA5) == 0xA5, "0xA5 (10100101) is its own mirror");
    CHECK(disp_flip_bitreverse(0x0F) == 0xF0, "low nibble -> high nibble");
    for (int v = 0; v <= 255; v++) {
        uint8_t once = disp_flip_bitreverse((uint8_t) v);
        uint8_t twice = disp_flip_bitreverse(once);
        CHECK(twice == (uint8_t) v, "bitreverse must be an involution: v=%d", v);
    }
}

/* ---------------------------------------------------------------------
 * disp_flip_row(): byte-order reversal + per-byte bit reversal, and its
 * own inverse.
 * --------------------------------------------------------------------- */
static void test_row_flip_known_pattern(void)
{
    /* Row with a distinct, recognisable bit pattern per byte so a
     * byte-order mistake (vs. a bit-order mistake) is unambiguous. */
    uint8_t in[ROW_BYTES];
    for (int i = 0; i < ROW_BYTES; i++) {
        in[i] = (uint8_t) (0x10 + i); /* 0x10, 0x11, ..., 0x1F -- all distinct */
    }
    uint8_t out[ROW_BYTES];
    disp_flip_row(in, out, ROW_BYTES);

    /* out[0] must be bitreverse(in[15]), out[15] must be bitreverse(in[0]) */
    CHECK(out[0] == disp_flip_bitreverse(in[ROW_BYTES - 1]),
          "out[0] must be bitreverse(in[last]) (byte order reversed)");
    CHECK(out[ROW_BYTES - 1] == disp_flip_bitreverse(in[0]),
          "out[last] must be bitreverse(in[0])");
    for (int i = 0; i < ROW_BYTES; i++) {
        CHECK(out[i] == disp_flip_bitreverse(in[ROW_BYTES - 1 - i]),
              "out[%d] must be bitreverse(in[%d])", i, ROW_BYTES - 1 - i);
    }

    /* Involution: flipping the output reproduces the input exactly. */
    uint8_t back[ROW_BYTES];
    disp_flip_row(out, back, ROW_BYTES);
    CHECK(memcmp(in, back, ROW_BYTES) == 0, "disp_flip_row() applied twice must be the identity");
}

/* ---------------------------------------------------------------------
 * disp_flip_src_row(): the x-axis half, and its own inverse.
 * --------------------------------------------------------------------- */
static void test_src_row(void)
{
    CHECK(disp_flip_src_row(0, ROWS) == ROWS - 1, "row 0 maps to the last row");
    CHECK(disp_flip_src_row(ROWS - 1, ROWS) == 0, "last row maps to row 0");
    CHECK(disp_flip_src_row(148, ROWS) == 147, "middle pair maps to each other (296 is even)");
    CHECK(disp_flip_src_row(147, ROWS) == 148, "...and the reverse");
    for (int r = 0; r < ROWS; r++) {
        CHECK(disp_flip_src_row(disp_flip_src_row(r, ROWS), ROWS) == r,
              "disp_flip_src_row() must be its own inverse at r=%d", r);
    }
}

/* ---------------------------------------------------------------------
 * Full-frame 180-degree rotation (every row, full width) of a known
 * asymmetric pattern is an involution: flip twice and get the original
 * frame back, bit for bit.
 * --------------------------------------------------------------------- */
static void test_full_frame_involution(void)
{
    static uint8_t frame[ROWS][ROW_BYTES];
    static uint8_t once[ROWS][ROW_BYTES];
    static uint8_t twice[ROWS][ROW_BYTES];

    /* Deterministic, asymmetric (not a palindrome in either axis) pattern:
     * byte b of row r is (r * 7 + b * 13 + 3) & 0xFF. */
    for (int r = 0; r < ROWS; r++) {
        for (int b = 0; b < ROW_BYTES; b++) {
            frame[r][b] = (uint8_t) ((r * 7 + b * 13 + 3) & 0xFF);
        }
    }

    for (int panel_row = 0; panel_row < ROWS; panel_row++) {
        int src = disp_flip_src_row(panel_row, ROWS);
        disp_flip_row(frame[src], once[panel_row], ROW_BYTES);
    }
    for (int panel_row = 0; panel_row < ROWS; panel_row++) {
        int src = disp_flip_src_row(panel_row, ROWS);
        disp_flip_row(once[src], twice[panel_row], ROW_BYTES);
    }

    CHECK(memcmp(frame, twice, sizeof(frame)) == 0,
          "full-frame 180-degree rotation applied twice must reproduce the original frame");

    /* And confirm the single rotation actually moved something (otherwise
     * the involution check above would pass vacuously for an all-zero or
     * symmetric frame -- it doesn't here, but assert it directly too). */
    CHECK(memcmp(frame, once, sizeof(frame)) != 0,
          "a single 180-degree rotation of this asymmetric pattern must differ from the original");
}

/* ---------------------------------------------------------------------
 * disp_flip_mirror_window(): partial-refresh window mapping, its own
 * inverse, and 8-row alignment preservation.
 * --------------------------------------------------------------------- */
static void test_mirror_window(void)
{
    int of, ol;

    /* Whole-panel window is a fixed point. */
    disp_flip_mirror_window(0, ROWS - 1, ROWS, &of, &ol);
    CHECK(of == 0 && ol == ROWS - 1, "the full window must map to itself");

    /* A window near the start maps to one near the end, same size. */
    disp_flip_mirror_window(0, 7, ROWS, &of, &ol);
    CHECK(of == ROWS - 8 && ol == ROWS - 1, "rows 0..7 must map to the last 8 rows");

    disp_flip_mirror_window(8, 15, ROWS, &of, &ol);
    CHECK(of == ROWS - 16 && ol == ROWS - 9, "rows 8..15 must map to rows %d..%d", ROWS - 16,
          ROWS - 9);

    /* Arbitrary single-row window ("step" column, disptest's own test
     * harness uses exactly this: an 8-aligned band). */
    disp_flip_mirror_window(100, 123, ROWS, &of, &ol);
    CHECK(of == ROWS - 1 - 123 && ol == ROWS - 1 - 100, "rows 100..123 must map correctly");

    /* Own inverse: mapping the output window back reproduces the input. */
    for (int first = 0; first < ROWS; first += 23) {
        for (int last = first; last < ROWS; last += 29) {
            int mf, ml, bf, bl;
            disp_flip_mirror_window(first, last, ROWS, &mf, &ml);
            disp_flip_mirror_window(mf, ml, ROWS, &bf, &bl);
            CHECK(bf == first && bl == last,
                  "mirror_window must be its own inverse: first=%d last=%d -> %d..%d -> %d..%d",
                  first, last, mf, ml, bf, bl);
        }
    }

    /* 8-row alignment preserved: for every 8-aligned [first,last] window
     * (first % 8 == 0, (last+1) % 8 == 0), the mirrored window must be
     * 8-aligned the same way -- disp.c's PAGER_UI_PARTIAL_ROW_ALIGN
     * invariant, which must survive mirroring (ROWS=296=8*37). */
    for (int first = 0; first < ROWS; first += 8) {
        for (int last = first + 7; last < ROWS; last += 8) {
            disp_flip_mirror_window(first, last, ROWS, &of, &ol);
            CHECK(of % 8 == 0, "mirrored first=%d must stay 8-aligned (from %d..%d)", of, first,
                  last);
            CHECK((ol + 1) % 8 == 0, "mirrored last=%d must stay 8-aligned (from %d..%d)", ol,
                  first, last);
            CHECK(ol - of == last - first, "mirrored window must be the same size");
        }
    }
}

int main(void)
{
    test_bitreverse();
    test_row_flip_known_pattern();
    test_src_row();
    test_full_frame_involution();
    test_mirror_window();

    if (g_failures == 0) {
        printf("PASS: disp_flip (180-degree rotation geometry), 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

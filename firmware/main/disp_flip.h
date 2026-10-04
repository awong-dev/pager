/* disp_flip.h — pure geometry helpers for the `disp_flip` NVS setting (owner
 * request: read the pager upside down). No ESP-IDF/gfx.h/disp.h dependency,
 * so these are host-tested directly (firmware/host/test_disp_flip.c) the
 * same way net_connect_guard.c/resub_verdict.c are.
 *
 * The on-glass image is a 296 (screen-x) x 128 (screen-y) landscape view;
 * gfx.c stores it as GFX_FB_ROWS native rows of GFX_FB_ROW_BYTES bytes each,
 * where native_row = (rows-1) - screen_x (gfx_set_pixel()'s own mirror) and,
 * within a row, bit 7 of byte 0 is screen_y=0 counting up to bit 0 of the
 * last byte at screen_y=127 (gfx.h's module comment: "native_byte = y / 8,
 * bit = 7 - y % 8").
 *
 * A 180-degree rotation of the displayed image is screen (x,y) -> (295-x,
 * 127-y) — BOTH axes. Flipping x alone (just reversing row order) or y
 * alone (just reversing bits within each row) would mirror-image the text
 * instead of rotating it; only doing both reads right upside down. In
 * native-row terms:
 *   - the x half is exactly "read native rows back to front"
 *     (disp_flip_src_row()/disp_flip_mirror_window() below), since
 *     native_row is already a mirror of screen_x;
 *   - the y half is exactly "reverse the 128-bit row, MSB-first across its
 *     16 bytes", which is reversing byte order AND bit-reversing each byte
 *     (disp_flip_row() below).
 *
 * Chosen as a software (write-time byte reorder), not a hardware
 * (SSD1680 data-entry-mode/driver-output register) rotation: the panel's
 * 0x01 driver output control only mirrors the gate (native-row) scan
 * direction, not the source (byte/bit) direction, so a hardware-only
 * rotation still can't do the y half, and disp.c would need two more
 * permanently-different register configurations (plus re-deriving the
 * partial-window math in panel-address space either way) for no savings —
 * the byte reorder below is the same number of SPI bytes either way, just
 * reordered before they're sent.
 */
#ifndef DISP_FLIP_H
#define DISP_FLIP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bit-reverses one byte (bit 7 <-> bit 0, bit 6 <-> bit 1, ...) — the
 * per-byte half of the row reversal below. */
uint8_t disp_flip_bitreverse(uint8_t b);

/* Writes the 180-degree-rotated (y-axis half only) version of one native
 * row's `len` bytes into `out`: byte order reversed (out[i] = in[len-1-i])
 * AND each byte bit-reversed. `in` and `out` must NOT alias. Applying this
 * twice (disp_flip_row(out, back, len) after disp_flip_row(in, out, len))
 * reproduces `in` exactly — this function is its own inverse. */
void disp_flip_row(const uint8_t *in, uint8_t *out, int len);

/* x-axis half of the 180-degree rotation: maps a panel RAM row address
 * (0..rows-1) to the source framebuffer row (gfx_fb_native_row() index)
 * whose (disp_flip_row()-rotated) bytes belong there. Its own inverse:
 * disp_flip_src_row(disp_flip_src_row(r, rows), rows) == r. */
int disp_flip_src_row(int panel_row, int rows);

/* Maps a partial-refresh window in source (unflipped, as gfx_fb_native_row()
 * indexes it and as the diff against the shadow plane is computed) row
 * space [first, last] (inclusive, 0 <= first <= last < rows) to the panel
 * RAM address window [*out_first, *out_last] that must actually be
 * programmed (SSD1680 0x44/0x45/0x4E/0x4F) so the same visual rows land,
 * post-rotation, in the same place — see disp_flip_src_row() above (this is
 * exactly that map applied to both ends, with first/last swapping roles
 * since the map reverses order). Its own inverse: feeding the output window
 * back in reproduces the input. If the input window is aligned to an
 * 8-row-native boundary and `rows` is itself a multiple of 8 (true for this
 * panel: GFX_FB_ROWS=296=8*37, PAGER_UI_PARTIAL_ROW_ALIGN=8 in disp.c), the
 * output window is aligned the same way — so the partial-refresh row
 * alignment invariant holds after mirroring, which is why disp.c calls this
 * AFTER PAGER_UI_PARTIAL_ROW_ALIGN widening, not before. */
void disp_flip_mirror_window(int first, int last, int rows, int *out_first, int *out_last);

#ifdef __cplusplus
}
#endif

#endif /* DISP_FLIP_H */

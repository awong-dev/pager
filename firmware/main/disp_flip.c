/* disp_flip.c — see disp_flip.h for the two-axis argument and the choice of
 * software over hardware rotation. */
#include "disp_flip.h"

uint8_t disp_flip_bitreverse(uint8_t b)
{
    b = (uint8_t) (((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
    b = (uint8_t) (((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
    b = (uint8_t) (((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
    return b;
}

void disp_flip_row(const uint8_t *in, uint8_t *out, int len)
{
    for (int i = 0; i < len; i++) {
        out[i] = disp_flip_bitreverse(in[len - 1 - i]);
    }
}

int disp_flip_src_row(int panel_row, int rows) { return (rows - 1) - panel_row; }

void disp_flip_mirror_window(int first, int last, int rows, int *out_first, int *out_last)
{
    *out_first = disp_flip_src_row(last, rows);
    *out_last = disp_flip_src_row(first, rows);
}

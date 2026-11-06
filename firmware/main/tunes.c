/* tunes.c -- see tunes.h. Super Mario Bros. 3, World 1 map, lead line, 200 bpm
 * (docs/CARRIER_V02_FIRMWARE_DESIGN.md §5). Onsets are in 1/4-beat units of 75 ms. */
#include "tunes.h"

typedef struct {
    uint16_t hz;
    uint16_t unit; /* onset, in TUNE_MARIO_UNIT_MS units */
} mario_note_t;

static const mario_note_t k_mario[TUNE_MARIO_NOTES] = {
    {NOTE_G4, 0},   {NOTE_G4, 4},   {NOTE_A4, 8},   {NOTE_A4, 12},  {NOTE_B4, 16},
    {NOTE_B4, 20},  {NOTE_A4, 23},  {NOTE_GS4, 27}, {NOTE_A4, 28},  {NOTE_G4, 32},
    {NOTE_G4, 36},  {NOTE_A4, 40},  {NOTE_A4, 44},  {NOTE_B4, 47},
    {NOTE_C5, 64},  {NOTE_C5, 68},  {NOTE_D5, 72},  {NOTE_D5, 76},  {NOTE_E5, 80},
    {NOTE_E5, 84},  {NOTE_D5, 87},  {NOTE_CS5, 91}, {NOTE_D5, 92},  {NOTE_C5, 96},
    {NOTE_C5, 100}, {NOTE_D5, 104}, {NOTE_D5, 108}, {NOTE_E5, 111},
};

uint32_t tune_mario_onset_ms(size_t i)
{
    return i < TUNE_MARIO_NOTES ? (uint32_t) k_mario[i].unit * TUNE_MARIO_UNIT_MS : 0;
}

uint16_t tune_mario_hz(size_t i)
{
    return i < TUNE_MARIO_NOTES ? k_mario[i].hz : 0;
}

size_t tune_mario_expand(size_t n, buzzer_note_t *out, size_t cap)
{
    if (n > TUNE_MARIO_NOTES) {
        n = TUNE_MARIO_NOTES;
    }
    if (out == NULL || n == 0) {
        return 0;
    }
    size_t need = 2 * n - 1;
    if (cap < need) {
        return 0;
    }
    size_t w = 0;
    for (size_t i = 0; i < n; i++) {
        out[w++] = (buzzer_note_t) {.hz = k_mario[i].hz, .ms = TUNE_MARIO_ON_MS};
        if (i + 1 < n) {
            uint32_t gap = tune_mario_onset_ms(i + 1) - tune_mario_onset_ms(i) - TUNE_MARIO_ON_MS;
            out[w++] = (buzzer_note_t) {.hz = 0, .ms = (uint16_t) gap};
        }
    }
    return w;
}

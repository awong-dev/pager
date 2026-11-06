/* Host test for tunes.c: the Mario table expands to the right onsets in ms. */
#include <stdio.h>

#include "tunes.h"

static int fails;
#define CHECK(c)                                                                              \
    do {                                                                                      \
        if (!(c)) {                                                                           \
            printf("FAIL line %d: %s\n", __LINE__, #c);                                       \
            fails++;                                                                          \
        }                                                                                     \
    } while (0)

/* Expected onsets in ms for all 28 notes (units * 75). */
static const uint32_t k_onset[28] = {0,    300,  600,  900,  1200, 1500, 1725, 2025, 2100, 2400,
                                     2700, 3000, 3300, 3525, 4800, 5100, 5400, 5700, 6000, 6300,
                                     6525, 6825, 6900, 7200, 7500, 7800, 8100, 8325};
static const uint16_t k_hz[28] = {392, 392, 440, 440, 494, 494, 440, 415, 440, 392, 392, 440, 440, 494,
                                  523, 523, 587, 587, 659, 659, 587, 554, 587, 523, 523, 587, 587, 659};

static void check_expand(size_t n)
{
    buzzer_note_t seq[TUNE_MARIO_MAX_STEPS];
    size_t steps = tune_mario_expand(n, seq, TUNE_MARIO_MAX_STEPS);
    CHECK(steps == 2 * n - 1);
    uint32_t t = 0;
    size_t note = 0;
    for (size_t i = 0; i < steps; i++) {
        if (i % 2 == 0) {
            CHECK(seq[i].hz == k_hz[note]);
            CHECK(seq[i].ms == TUNE_MARIO_ON_MS);
            CHECK(t == k_onset[note]);
            note++;
        } else {
            CHECK(seq[i].hz == 0);
            CHECK(seq[i].ms > 0 || k_onset[note] - k_onset[note - 1] == TUNE_MARIO_ON_MS);
        }
        t += seq[i].ms;
    }
    CHECK(note == n);
    /* the last note ends 72 ms after its onset */
    CHECK(t == k_onset[n - 1] + TUNE_MARIO_ON_MS);
}

int main(void)
{
    CHECK(TUNE_MARIO_DEFAULT_NOTES == 22);
    for (size_t i = 0; i < 28; i++) {
        CHECK(tune_mario_onset_ms(i) == k_onset[i]);
        CHECK(tune_mario_hz(i) == k_hz[i]);
    }
    CHECK(tune_mario_onset_ms(28) == 0 && tune_mario_hz(28) == 0);
    check_expand(TUNE_MARIO_DEFAULT_NOTES);
    check_expand(28);
    check_expand(1);

    buzzer_note_t small[4];
    CHECK(tune_mario_expand(22, small, 4) == 0);
    CHECK(tune_mario_expand(0, small, 4) == 0);
    /* clamp above the table */
    buzzer_note_t big[TUNE_MARIO_MAX_STEPS];
    CHECK(tune_mario_expand(500, big, TUNE_MARIO_MAX_STEPS) == 55);
    /* swing figure: 225 ms between notes 6 and 7, rest 153 ms */
    CHECK(big[11].hz == 0 && big[11].ms == 153);
    /* 75 ms between notes 8 and 9: a 3 ms rest */
    CHECK(big[15].hz == 0 && big[15].ms == 3);

    if (fails == 0) {
        printf("test_tunes: all passed\n");
        return 0;
    }
    printf("test_tunes: %d failures\n", fails);
    return 1;
}

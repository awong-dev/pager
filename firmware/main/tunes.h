/* tunes.h -- note table and the built-in Mario tune (docs/CARRIER_V02_FIRMWARE_DESIGN.md §5).
 *
 * Pure (no ESP-IDF): compiled on the host by host/test_tunes.c. buzzer.c plays the expansion.
 */
#ifndef TUNES_H
#define TUNES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One step of a sequence: hz 0 = rest. Shared by tunes.c and buzzer.c. */
typedef struct {
    uint16_t hz;
    uint16_t ms;
} buzzer_note_t;

/* Equal-tempered note frequencies used by the tunes (Hz, rounded). */
#define NOTE_G4 392
#define NOTE_GS4 415
#define NOTE_A4 440
#define NOTE_B4 494
#define NOTE_C5 523
#define NOTE_CS5 554
#define NOTE_D5 587
#define NOTE_E5 659

#define TUNE_MARIO_NOTES 28          /* notes in the table (two phrases of 14) */
#define TUNE_MARIO_DEFAULT_NOTES 22  /* the owner asked for the first 22; 28 = both phrases */
#define TUNE_MARIO_ON_MS 72          /* every note sounds for 72 ms, then rests until the next onset */
#define TUNE_MARIO_UNIT_MS 75        /* one 1/4-beat unit at 200 bpm */
#define TUNE_MARIO_MAX_STEPS (2 * TUNE_MARIO_NOTES)

/* Onset of note `i` (0-based, i < TUNE_MARIO_NOTES) in ms from the start; 0 if out of range. */
uint32_t tune_mario_onset_ms(size_t i);
/* Frequency of note `i` in Hz; 0 if out of range. */
uint16_t tune_mario_hz(size_t i);

/* Expands the first `n` notes (clamped to TUNE_MARIO_NOTES) into on/rest pairs in `out` (capacity
 * `cap`). The last note has no trailing rest. Returns the number of steps written (0 if `cap` is too
 * small for the whole expansion; at most TUNE_MARIO_MAX_STEPS are ever needed). */
size_t tune_mario_expand(size_t n, buzzer_note_t *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TUNES_H */

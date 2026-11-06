/* buzzer.h -- piezo on IO6/IO7 driven differentially from LEDC (carrier v0.2 only;
 * docs/CARRIER_V02_FIRMWARE_DESIGN.md §3.1). On the breadboard every call is a no-op.
 *
 * Threading: buzzer_play and buzzer_stop may be called from any task (a mutex serialises them against
 * the sequencer). The sequencer is an esp_timer one-shot chain whose callback runs on the esp_timer
 * task and only touches LEDC/GPIO. No ISR involvement.
 */
#ifndef BUZZER_H
#define BUZZER_H

#include <stdbool.h>
#include <stddef.h>

#include "tunes.h" /* buzzer_note_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Built-in sequences, usable as `buzzer_play(BUZZ_NEW_MSG)`. */
extern const buzzer_note_t buzzer_seq_new_msg[];
#define BUZZER_SEQ_NEW_MSG_N 3
#define BUZZ_NEW_MSG buzzer_seq_new_msg, BUZZER_SEQ_NEW_MSG_N

/* Configures the two pins as outputs driven low (idle) and the LEDC timer. Idempotent; carrier only.
 * Power effect: none (no tone). */
void buzzer_init(void);

/* Plays `n` notes (copied; max BUZZER_MAX_STEPS), replacing any sequence in progress. Returns false
 * on the breadboard or for an empty/oversize sequence. Power effect: the piezo draws only while a
 * note sounds; the esp_timer wakes the CPU per note (the caller keeps the chip awake). */
#define BUZZER_MAX_STEPS 64
bool buzzer_play(const buzzer_note_t *seq, size_t n);

/* Plays the first `n` notes of the Mario tune (tunes.h; n clamped to 28). */
bool buzzer_play_mario(size_t n);

/* Silences immediately and cancels the sequence; both pins end low. Power effect: stops the piezo. */
void buzzer_stop(void);

bool buzzer_busy(void);

#ifdef __cplusplus
}
#endif

#endif /* BUZZER_H */

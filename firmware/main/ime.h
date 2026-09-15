/* ime.h — the IME hook between the input event queue and a text field
 * (docs/DEVICE_PLAN.md §5.3, decision H15).
 *
 * No IME is built in this plan: hangul composition is algorithmic and
 * small, pinyin and kana->kanji need dictionaries of tens of kB to a few
 * MB, and both are out of scope here. What §5.3 asks F6.2 to land is the
 * *hook* — one entry point, `ime_feed()` — plus the identity
 * implementation (ASCII straight through, nothing pre-edited), which is
 * the only `ime_t` this codebase wires up today. A text field with more
 * than one IME to choose from, the NVS "which IME" setting next to *Text
 * size*, and the pre-edit/candidate-bar rendering are all F6.3+ (they need
 * an actual text field to hook into, which doesn't exist until the screen
 * stack does).
 *
 * Header-only by design (no ime.c): the identity IME has no state, so
 * `static inline` plus one `static const ime_t` avoids a translation unit
 * for a single trivial function while keeping the interface real.
 *
 * No modem or sleep-state effect: this module never touches the radio or
 * FreeRTOS timers.
 */
#ifndef IME_H
#define IME_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "input.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest single commit/pre-edit chunk an IME emits per feed(): one 4-byte
 * UTF-8 code point (docs/DEVICE_PLAN.md §5.2's Unicode text fields) + NUL. */
#define IME_UTF8_MAX 5
#define IME_MAX_CANDIDATES 5

typedef struct {
    bool consumed; /* true if the IME ate the key: caller must not also
                     * treat it as raw text/navigation. */
    char commit_utf8[IME_UTF8_MAX];  /* finalized text to append; "" if none */
    char preedit_utf8[IME_UTF8_MAX]; /* in-progress composition to render
                                       * underlined; "" if none */
    char candidates[IME_MAX_CANDIDATES][IME_UTF8_MAX];
    uint8_t n_candidates;
} ime_result_t;

typedef struct ime_s {
    /* The one entry point (§5.3). Implementations must not block and must
     * not allocate (CLAUDE.md: no dynamic allocation after init). */
    ime_result_t (*feed)(struct ime_s *self, input_key_t key);
} ime_t;

/* Identity IME: printable-char keys commit straight through, everything
 * else (backspace/enter/esc/tab/arrows) is left unconsumed for the text
 * field to handle itself (navigation, submit, cancel). No pre-edit, no
 * candidates, no state. */
static inline ime_result_t ime_identity_feed(ime_t *self, input_key_t key)
{
    (void) self;
    ime_result_t r;
    memset(&r, 0, sizeof(r));

    if (key.type == INPUT_KEY_CHAR) {
        r.consumed = true;
        r.commit_utf8[0] = key.ch;
        r.commit_utf8[1] = '\0';
    }
    return r;
}

/* Factory rather than a file-scope `static const ime_t` instance: every
 * translation unit that includes this header would otherwise get its own
 * copy of an object nothing references yet (F6.3 wires the first real
 * caller), which trips -Wunused-variable/-Werror. `static inline`
 * functions are exempt from the equivalent -Wunused-function warning, so
 * this stays warning-clean whether or not anything calls it yet. */
static inline ime_t ime_identity(void)
{
    ime_t ime;
    ime.feed = ime_identity_feed;
    return ime;
}

#ifdef __cplusplus
}
#endif

#endif /* IME_H */

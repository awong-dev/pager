/* input.h — CardKB key decode, the button short/long/stuck FSM, one input
 * event queue and the UI-awake window (docs/DEVICE_TASKS.md F6.2,
 * docs/DEVICE_PLAN.md §5.3).
 *
 * Split from modes.c/ui.c: modes.c used to own the button FSM directly and
 * ui.c decoded only the ASCII subset of CardKB bytes it needed for the
 * composer. This module is the single owner of both going forward and adds
 * what neither had: the full key table (arrows/esc/tab), `BTN_STUCK`
 * (firmware/README.md R2), a queue instead of direct calls back into
 * modes.c, and the 30s UI-awake window that arms on any key or button
 * event.
 *
 * `input_decode_key()` has no ESP-IDF dependency and is compiled on the
 * host for `firmware/host/test_input.c` (same `#ifdef ESP_PLATFORM` split
 * as gfx.c/setup.c) — everything else in this header needs FreeRTOS/
 * driver/esp_timer and is device-only.
 *
 * Scope note (flagged in the F6.2 report): this module does NOT read the
 * CardKB over I2C itself yet. ui.c still owns that I2C transaction
 * exclusively for the reply composer (F6.2's Files list is input.c/h,
 * ime.h, modes.c — not ui.c, which F6.3 rewrites into the real screen
 * stack). Reading the CardKB from two places at once would race ui.c's own
 * read for the single buffered byte the keyboard holds, so
 * `input_feed_key()` below — which decodes a byte and arms the awake
 * window/queues an event exactly like a hypothetical internal poll would —
 * is exposed for F6.3's screens to call once they own their own CardKB
 * reads outside the composer. Until then, `INPUT_EVT_KEY` only flows from
 * that (currently uncalled) entry point; `input_awake()` today is armed by
 * button activity, and independently by `ui_composer_is_open()` at
 * `modes_run()`'s call site (unchanged legacy behaviour, since ui.c is not
 * touched here).
 */
#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Key decode (docs/DEVICE_PLAN.md §5.3). Pure function, no state, no I/O -
 * host-testable.
 * --------------------------------------------------------------------- */

typedef enum {
    INPUT_KEY_NONE = 0,
    INPUT_KEY_CHAR,       /* ch = printable ASCII 0x20-0x7E */
    INPUT_KEY_BACKSPACE,  /* CardKB 0x08 */
    INPUT_KEY_ENTER,      /* CardKB 0x0D */
    INPUT_KEY_ESC,        /* CardKB 0x1B */
    INPUT_KEY_TAB,        /* CardKB 0x09 */
    INPUT_KEY_LEFT,       /* CardKB 0xB4 - UNVERIFIED (M13) */
    INPUT_KEY_UP,         /* CardKB 0xB5 - UNVERIFIED (M13) */
    INPUT_KEY_DOWN,       /* CardKB 0xB6 - UNVERIFIED (M13) */
    INPUT_KEY_RIGHT,      /* CardKB 0xB7 - UNVERIFIED (M13) */
} input_key_type_t;

typedef struct {
    input_key_type_t type;
    char ch; /* valid iff type == INPUT_KEY_CHAR */
} input_key_t;

#define INPUT_KEY_NONE_VAL ((input_key_t) { .type = INPUT_KEY_NONE, .ch = 0 })

/* Decodes one CardKB byte. 0x00 ("no key") and any byte outside the table
 * both decode to INPUT_KEY_NONE — dropped, same as before this task
 * (docs/DEVICE_PLAN.md §5.3: "Anything else is still dropped"). No side
 * effects. */
input_key_t input_decode_key(uint8_t byte);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * Button FSM, one event queue, the UI-awake window. Device-only (FreeRTOS
 * queue + GPIO + esp_timer).
 * --------------------------------------------------------------------- */

typedef enum {
    INPUT_EVT_KEY,       /* key, see .key; today only reaches the queue via input_feed_key() */
    INPUT_EVT_BTN_DOWN,  /* debounced press registered (not yet resolved short/long) */
    INPUT_EVT_BTN_SHORT, /* released before the long-press threshold */
    INPUT_EVT_BTN_LONG,  /* held past the long-press threshold (fires once, not on release) */
} input_evt_type_t;

typedef struct {
    input_evt_type_t type;
    input_key_t key; /* valid iff type == INPUT_EVT_KEY */
} input_event_t;

/* One-time init: button GPIO (input, pull-up, active-low, per pins.h) and
 * the static event queue. Call once from modes_boot(), before modes_run().
 * Power effect: GPIO config + queue allocation only — no modem or
 * sleep-state change. */
void input_init(void);

/* Call once per modes_run() loop iteration. Steps the button FSM against
 * the current PAGER_PIN_BUTTON level; a resolved event (BTN_DOWN/SHORT/
 * LONG) arms the UI-awake window and is pushed to the queue
 * input_get_event() drains. Power effect: one GPIO read — no I2C, no modem
 * effect (see this header's scope note on why CardKB reads are not done
 * here yet). */
void input_poll(void);

/* Decodes `byte` and, if it is not INPUT_KEY_NONE, arms the UI-awake window
 * and pushes an INPUT_EVT_KEY event. Not called by anything in this task
 * (see the scope note above) — the entry point F6.3's screens use once
 * they read the CardKB outside the composer. Power effect: none of its
 * own; the caller's I2C read is what costs power. */
void input_feed_key(uint8_t byte);

/* Non-blocking dequeue. Returns false (leaving *out untouched) if the
 * queue is empty. */
bool input_get_event(input_event_t *out);

/* True for PAGER_UI_AWAKE_S (30s, compile-time) after the most recent
 * event armed the window. Independent of the 10-minute modem active
 * window (docs/DEVICE_PLAN.md §5.3) — modes.c's set_mode()/
 * active_until_us own that one. modes_run() uses this to decide the
 * 100ms-poll/no-light-sleep cadence vs. the ordinary wake-and-drain
 * cadence. */
bool input_awake(void);

/* True while the button FSM is mid-press (BTN_DOWN) or mid-hold (BTN_HELD,
 * i.e. before the BTN_STUCK cutoff) and so needs frequent polling to
 * measure press/hold duration accurately. False for BTN_IDLE and
 * BTN_STUCK. */
bool input_button_busy(void);

/* True once a held button has crossed PAGER_BTN_STUCK_MS in BTN_HELD
 * (firmware/README.md R2). What this deliberately does NOT do: make
 * modes_run() call net_sleep() again. net_sleep() (net.cpp) arms
 * esp_sleep_enable_ext0_wakeup() on PAGER_PIN_BUTTON at level 0
 * (active-low) — with the button still physically held (level stuck at
 * 0), esp_light_sleep_start() would return immediately on every call
 * (the original hazard this state exists to name), and net_sleep() also
 * deasserts RTS around that call, so spinning it at high frequency risks
 * doing that to the modem mid-transaction repeatedly. Fixing that needs a
 * net_sleep() variant that arms ext0 on level 1 (wake on release) —
 * README R2's own fix spec — which touches net.cpp and is out of this
 * task's Files list. What BTN_STUCK buys today, in scope: modes_run()
 * stops needing input_button_busy()'s fine ~20ms polling once a press
 * stops being measured, so callers should fall back to a much coarser
 * poll interval instead (still not net_sleep()) while this is true. */
bool input_button_stuck(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* INPUT_H */

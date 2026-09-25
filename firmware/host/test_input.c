/* test_input.c — host test for input.c's key decode table (task F6.2,
 * docs/DEVICE_PLAN.md §5.3) and, since task 1.0 (docs/V03_TASKS.md), the
 * event queue/awake-window section too.
 *
 * Builds and runs with the host compiler (see Makefile). input_decode_key()
 * has no ESP-IDF dependency at all; the queue/awake-window code below it in
 * input.c (input.c's own `#ifdef ESP_PLATFORM` split) needs FreeRTOS/
 * driver/esp_timer, which the headers under firmware/host/idf_stub/ now
 * stand in for (see that directory's module comment) so this binary is built with
 * -DESP_PLATFORM against input.c's real queue code, not a second
 * reimplementation of it.
 *
 * Coverage: every byte 0x00-0xFF is checked against the exact table in
 * docs/DEVICE_PLAN.md §5.3 - printable ASCII 0x20-0x7E, 0x08 backspace,
 * 0x09 tab, 0x0D enter, 0x1B esc, 0xB4-0xB7 arrows (left/up/down/right in
 * that order, per §5.3 - UNVERIFIED against real hardware, M13) - plus
 * everything else, including 0x00, decoding to INPUT_KEY_NONE. Plus (task
 * 1.0): N keys fed to input_feed_key() back-to-back (no drain in between,
 * modelling the console's `key <text>` command, main.c's cmd_key()) are
 * all delivered, in order, by input_get_event().
 */
#include "input.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
/* idf_stub/ test-only hooks (idf_stub_set_button_level()/
 * idf_stub_advance_us()) - see their own doc comments. */
#include "driver/gpio.h"
#include "esp_timer.h"
#endif

static int g_failures = 0;

#define CHECK(cond, ...)                                               \
    do {                                                                \
        if (!(cond)) {                                                  \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                 \
            printf(__VA_ARGS__);                                        \
            printf("\n");                                               \
            g_failures++;                                               \
        }                                                               \
    } while (0)

static const char *key_type_name(input_key_type_t t)
{
    switch (t) {
    case INPUT_KEY_NONE: return "NONE";
    case INPUT_KEY_CHAR: return "CHAR";
    case INPUT_KEY_BACKSPACE: return "BACKSPACE";
    case INPUT_KEY_ENTER: return "ENTER";
    case INPUT_KEY_ESC: return "ESC";
    case INPUT_KEY_TAB: return "TAB";
    case INPUT_KEY_LEFT: return "LEFT";
    case INPUT_KEY_UP: return "UP";
    case INPUT_KEY_DOWN: return "DOWN";
    case INPUT_KEY_RIGHT: return "RIGHT";
    default: return "?";
    }
}

static void expect(uint8_t byte, input_key_type_t want_type, char want_ch)
{
    input_key_t got = input_decode_key(byte);
    CHECK(got.type == want_type, "byte 0x%02X: got type %s, want %s", byte,
          key_type_name(got.type), key_type_name(want_type));
    if (want_type == INPUT_KEY_CHAR) {
        CHECK(got.ch == want_ch, "byte 0x%02X: got ch '%c' (0x%02X), want '%c' (0x%02X)", byte,
              got.ch, (unsigned) got.ch, want_ch, (unsigned) want_ch);
    }
}

static void test_no_key_sentinel(void)
{
    expect(0x00, INPUT_KEY_NONE, 0);
}

static void test_printable_ascii(void)
{
    for (int b = 0x20; b <= 0x7E; b++) {
        expect((uint8_t) b, INPUT_KEY_CHAR, (char) b);
    }
}

static void test_control_keys(void)
{
    expect(0x08, INPUT_KEY_BACKSPACE, 0);
    expect(0x09, INPUT_KEY_TAB, 0);
    expect(0x0D, INPUT_KEY_ENTER, 0);
    expect(0x1B, INPUT_KEY_ESC, 0);
}

static void test_arrows(void)
{
    /* docs/DEVICE_PLAN.md §5.3: "0xB4-0xB7 = left/up/down/right". */
    expect(0xB4, INPUT_KEY_LEFT, 0);
    expect(0xB5, INPUT_KEY_UP, 0);
    expect(0xB6, INPUT_KEY_DOWN, 0);
    expect(0xB7, INPUT_KEY_RIGHT, 0);
}

/* Exhaustive: every byte not covered above must decode to NONE - this is
 * what pins down "anything else is still dropped" (§5.3) against silent
 * regressions as the table grows. */
static void test_everything_else_is_dropped(void)
{
    for (int b = 0; b <= 0xFF; b++) {
        uint8_t byte = (uint8_t) b;
        bool covered = (byte == 0x00) || (byte >= 0x20 && byte <= 0x7E) || byte == 0x08 ||
                       byte == 0x09 || byte == 0x0D || byte == 0x1B ||
                       (byte >= 0xB4 && byte <= 0xB7);
        if (covered) {
            continue;
        }
        input_key_t got = input_decode_key(byte);
        CHECK(got.type == INPUT_KEY_NONE, "byte 0x%02X: got type %s, want NONE (dropped)", byte,
              key_type_name(got.type));
    }
}

static void test_no_side_effects(void)
{
    /* Pure function: same byte decoded twice must decode identically -
     * guards against the decoder accidentally growing hidden state. */
    input_key_t a = input_decode_key(0x41);
    input_key_t b = input_decode_key(0x41);
    CHECK(a.type == b.type && a.ch == b.ch, "input_decode_key(0x41) is not idempotent/pure");
}

#ifdef ESP_PLATFORM
/* Task 1.0 (docs/V03_TASKS.md): bench evidence (build/bench-logs/phaseG.log)
 * showed `key a`..`key d` (one character per console call) each reaching
 * the composer while `key efghijkl` (8 characters in one call) did not.
 * main.c's cmd_key() feeds every character of one console call to
 * input_feed_key() back-to-back, with no drain in between (draining is
 * modes_run()'s job, on a different task, later) - this reproduces exactly
 * that shape against the real input.c queue code. */
static void test_n_keys_fed_at_once_are_all_delivered(void)
{
    input_init();

    const char *text = "efghijkl"; /* the exact failing bench input */
    int n = (int) strlen(text);
    for (int i = 0; i < n; i++) {
        input_feed_key((uint8_t) text[i]);
    }

    for (int i = 0; i < n; i++) {
        input_event_t evt;
        bool got = input_get_event(&evt);
        CHECK(got, "key %d/%d ('%c') never reached the queue", i + 1, n, text[i]);
        if (!got) {
            continue;
        }
        CHECK(evt.type == INPUT_EVT_KEY, "key %d/%d: got event type %d, want INPUT_EVT_KEY", i + 1,
              n, (int) evt.type);
        CHECK(evt.key.type == INPUT_KEY_CHAR && evt.key.ch == text[i],
              "key %d/%d: got ch '%c', want '%c' (queue reordered or dropped an event)", i + 1, n,
              evt.key.ch, text[i]);
    }

    input_event_t extra;
    CHECK(!input_get_event(&extra), "queue had a leftover event after draining exactly %d", n);
}

/* Bug fix (25 Sep, "lock screen never switches to password: on a quick IO1
 * press", firmware/README.md/docs/DEVICE_PLAN.md §5.5): ext0 is a LEVEL
 * wake on PAGER_PIN_BUTTON, so the wake edge IS the press, but modes.c's
 * wake path (rail restore/probe/post-wake yield) runs before this
 * iteration's input_poll() ever samples the pin. input_note_ext0_wake()
 * seeds the FSM with that press so input_poll() resolves short/long from
 * it exactly as for a polled press (input.h's own doc comment). These
 * three cases are the fix's contract:
 *  (a) ext0 wake, then the pin already reads released at the first poll:
 *      BTN_DOWN + BTN_SHORT, same as a real quick press/release.
 *  (b) ext0 wake, then the pin is still held past the long-press
 *      threshold: BTN_DOWN + BTN_LONG, no BTN_SHORT.
 *  (c) ext0 wake while the FSM has already resolved BTN_DOWN for this same
 *      press (a held button spanning more than one ext0 wake, the
 *      BTN_STUCK hazard input.h documents): merges into it instead of
 *      seeding a second BTN_DOWN - exactly one event total.
 * idf_stub_set_button_level()/idf_stub_advance_us() (idf_stub/) are
 * host-test-only hooks; input.c itself never calls either. */
static void expect_event(input_evt_type_t want, const char *case_name)
{
    input_event_t evt;
    bool got = input_get_event(&evt);
    CHECK(got, "%s: expected an event (type %d), queue was empty", case_name, (int) want);
    if (got) {
        CHECK(evt.type == want, "%s: got event type %d, want %d", case_name, (int) evt.type,
              (int) want);
    }
}

static void expect_no_event(const char *case_name)
{
    input_event_t evt;
    CHECK(!input_get_event(&evt), "%s: unexpected extra event, type %d", case_name, (int) evt.type);
}

static void test_ext0_wake_then_released_is_short(void)
{
    input_init();
    idf_stub_set_button_level(1); /* released */

    int64_t t0 = esp_timer_get_time();
    input_note_ext0_wake(t0);
    expect_event(INPUT_EVT_BTN_DOWN, "ext0/released: seed");

    /* modes.c's next input_poll() this same iteration - pin already back
     * to released, exactly like a real quick press/release. */
    idf_stub_set_button_level(1);
    input_poll();
    expect_event(INPUT_EVT_BTN_SHORT, "ext0/released: resolve");
    expect_no_event("ext0/released: drained");
}

static void test_ext0_wake_then_held_past_long_threshold_is_long(void)
{
    input_init();
    idf_stub_set_button_level(0); /* still held at the wake */

    int64_t t0 = esp_timer_get_time();
    input_note_ext0_wake(t0);
    expect_event(INPUT_EVT_BTN_DOWN, "ext0/held: seed");

    /* Jump the fake clock past PAGER_BTN_LONG_PRESS_MS (600ms) while the
     * pin stays low (still held) - the long-press threshold must fire off
     * the seeded press start (t0), not a fresh one. */
    idf_stub_advance_us(700000);
    idf_stub_set_button_level(0);
    input_poll();
    expect_event(INPUT_EVT_BTN_LONG, "ext0/held: resolve");
    expect_no_event("ext0/held: no SHORT queued alongside LONG");

    /* Release: BTN_HELD -> BTN_IDLE pushes no event of its own (matches a
     * real long-press release) - confirms SHORT never sneaks in here
     * either. */
    idf_stub_set_button_level(1);
    input_poll();
    expect_no_event("ext0/held: no event on release after LONG");
}

static void test_ext0_wake_merges_into_press_fsm_already_saw(void)
{
    input_init();

    /* A press input_poll() itself already debounced into BTN_DOWN, before
     * any ext0 wake call this test injects - models a held button
     * spanning more than one ext0 wake (input.h's BTN_STUCK doc comment). */
    idf_stub_set_button_level(0);
    input_poll(); /* starts debounce, no event yet */
    idf_stub_advance_us(40000); /* > PAGER_BTN_DEBOUNCE_MS (30ms) */
    idf_stub_set_button_level(0);
    input_poll(); /* debounce resolved -> BTN_DOWN */
    expect_event(INPUT_EVT_BTN_DOWN, "ext0/merge: press already seen");

    /* A second ext0 wake fires (still held) - must be a no-op, not a
     * second BTN_DOWN. */
    int64_t t1 = esp_timer_get_time();
    input_note_ext0_wake(t1);
    expect_no_event("ext0/merge: no second BTN_DOWN");

    /* Release so this test (and whichever runs next) starts from BTN_IDLE. */
    idf_stub_set_button_level(1);
    input_poll();
    expect_event(INPUT_EVT_BTN_SHORT, "ext0/merge: release resolves the one press tracked");
    expect_no_event("ext0/merge: drained");
}
#endif

int main(void)
{
    test_no_key_sentinel();
    test_printable_ascii();
    test_control_keys();
    test_arrows();
    test_everything_else_is_dropped();
    test_no_side_effects();
#ifdef ESP_PLATFORM
    test_n_keys_fed_at_once_are_all_delivered();
    test_ext0_wake_then_released_is_short();
    test_ext0_wake_then_held_past_long_threshold_is_long();
    test_ext0_wake_merges_into_press_fsm_already_saw();
#endif

    if (g_failures == 0) {
#ifdef ESP_PLATFORM
        printf("PASS: input_decode_key() table + queue delivery (N keys fed at once), 0 "
               "failures\n");
#else
        printf("PASS: input_decode_key() table, 0 failures\n");
#endif
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

/* test_input.c — host test for input.c's key decode table (task F6.2,
 * docs/DEVICE_PLAN.md §5.3).
 *
 * Builds and runs with the host compiler (see Makefile), no ESP-IDF:
 * input_decode_key() has no ESP-IDF dependency (input.c's `#ifdef
 * ESP_PLATFORM` split keeps the button-FSM/queue/awake-window code, which
 * does need FreeRTOS/driver/esp_timer, out of the host build entirely).
 *
 * Coverage: every byte 0x00-0xFF is checked against the exact table in
 * docs/DEVICE_PLAN.md §5.3 - printable ASCII 0x20-0x7E, 0x08 backspace,
 * 0x09 tab, 0x0D enter, 0x1B esc, 0xB4-0xB7 arrows (left/up/down/right in
 * that order, per §5.3 - UNVERIFIED against real hardware, M13) - plus
 * everything else, including 0x00, decoding to INPUT_KEY_NONE.
 */
#include "input.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

int main(void)
{
    test_no_key_sentinel();
    test_printable_ascii();
    test_control_keys();
    test_arrows();
    test_everything_else_is_dropped();
    test_no_side_effects();

    if (g_failures == 0) {
        printf("PASS: input_decode_key() table, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

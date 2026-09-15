/* test_msg.c — host test for msg.c's UTF-8 composer (task F6.4,
 * docs/DEVICE_PLAN.md §5.2/§9.4).
 *
 * Builds and runs with the host compiler (see Makefile), no ESP-IDF: only
 * msg.c's composer section is compiled on the host (msg.c's `#ifdef
 * ESP_PLATFORM` split, same pattern as input.c/auth.c, keeps the
 * NVS/RTC/net/auth/cbor-dependent ingest/pump code out of this build
 * entirely — see msg.c's own module comment).
 *
 * Coverage:
 *  1. Plain ASCII fills to exactly 320 bytes / 160 code points and then
 *     refuses further input (both caps checked independently).
 *  2. A 3-byte UTF-8 code point (docs/DEVICE_TASKS.md F6.4's own example) is
 *     accepted or refused as a whole unit, never split — pushed one byte at
 *     a time (msg_composer_push_char()'s real calling convention, see
 *     scr_chat.c), a code point that would cross either cap must leave the
 *     buffer exactly as it was before the push started (no partial/invalid
 *     UTF-8 ever lands in the buffer).
 *  3. Backspace removes one whole code point (all its bytes), not one byte.
 *  4. msg_composer_reset() clears both counters and any in-progress partial
 *     sequence.
 */
#include "msg.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

/* '€' U+20AC, EUR SIGN — a well-formed 3-byte UTF-8 sequence. */
static const uint8_t k_euro[3] = { 0xE2, 0x82, 0xAC };

/* U+1F600 GRINNING FACE — a well-formed 4-byte UTF-8 sequence, used only as
 * byte-cap filler (4 bytes/code point lets a test reach close to the
 * 320-byte cap while staying far under the 160-codepoint cap, so the two
 * caps can be tested independently). */
static const uint8_t k_emoji[4] = { 0xF0, 0x9F, 0x98, 0x80 };

static bool push_bytes(const uint8_t *b, size_t n)
{
    bool ok = true;
    for (size_t i = 0; i < n; i++) {
        ok = msg_composer_push_char((char) b[i]) && ok;
    }
    return ok;
}

static void push_str(const char *s)
{
    for (const char *p = s; *p != '\0'; p++) {
        CHECK(msg_composer_push_char(*p), "push('%c') unexpectedly refused mid-fill", *p);
    }
}

static void test_ascii_byte_cap(void)
{
    msg_composer_reset();
    /* 320 'a's fills the byte cap exactly (1 byte/code point each, so the
     * code point cap of 160 binds first — see test_ascii_codepoint_cap()).
     * Use a short run well under 160 to isolate nothing but "does pushing
     * work and report lengths correctly". */
    for (int i = 0; i < 50; i++) {
        CHECK(msg_composer_push_char('a'), "push #%d refused", i);
    }
    CHECK(msg_composer_len() == 50, "len == %u, want 50", (unsigned) msg_composer_len());
    CHECK(msg_composer_codepoint_count() == 50, "codepoints == %u, want 50",
          (unsigned) msg_composer_codepoint_count());
    CHECK(strlen(msg_composer_text()) == 50, "text length mismatch");
}

static void test_ascii_codepoint_cap(void)
{
    msg_composer_reset();
    for (int i = 0; i < MSG_COMPOSER_MAX_CODEPOINTS; i++) {
        CHECK(msg_composer_push_char('x'), "push #%d refused before the cap", i);
    }
    CHECK(msg_composer_codepoint_count() == MSG_COMPOSER_MAX_CODEPOINTS,
          "codepoints == %u, want %u", (unsigned) msg_composer_codepoint_count(),
          MSG_COMPOSER_MAX_CODEPOINTS);
    CHECK(msg_composer_len() == MSG_COMPOSER_MAX_CODEPOINTS, "len == %u, want %u",
          (unsigned) msg_composer_len(), MSG_COMPOSER_MAX_CODEPOINTS);

    /* One more single-byte ASCII code point must now be refused (160 cap
     * binds first here since every code point so far was 1 byte). */
    CHECK(!msg_composer_push_char('x'), "161st code point was accepted, cap not enforced");
    CHECK(msg_composer_codepoint_count() == MSG_COMPOSER_MAX_CODEPOINTS,
          "codepoint count changed after a refused push");
    CHECK(msg_composer_len() == MSG_COMPOSER_MAX_CODEPOINTS, "byte len changed after a refused push");
}

/* The scenario docs/DEVICE_TASKS.md F6.4 names explicitly: a 3-byte code
 * point pushed one byte at a time must be accepted or refused as a whole
 * unit. First, well inside both caps: accepted, and both counters advance
 * by exactly one code point / three bytes. */
static void test_three_byte_codepoint_accepted(void)
{
    msg_composer_reset();
    push_str("hi ");
    uint16_t len_before = msg_composer_len();
    uint16_t cp_before = msg_composer_codepoint_count();

    CHECK(msg_composer_push_char((char) k_euro[0]), "euro byte 0 refused");
    CHECK(msg_composer_push_char((char) k_euro[1]), "euro byte 1 refused");
    CHECK(msg_composer_push_char((char) k_euro[2]), "euro byte 2 refused");

    CHECK(msg_composer_len() == len_before + 3, "len == %u, want %u",
          (unsigned) msg_composer_len(), (unsigned) (len_before + 3));
    CHECK(msg_composer_codepoint_count() == cp_before + 1, "codepoints == %u, want %u",
          (unsigned) msg_composer_codepoint_count(), (unsigned) (cp_before + 1));

    const char *text = msg_composer_text();
    size_t tlen = strlen(text);
    CHECK(tlen >= 3 && (unsigned char) text[tlen - 3] == k_euro[0] &&
              (unsigned char) text[tlen - 2] == k_euro[1] &&
              (unsigned char) text[tlen - 1] == k_euro[2],
          "euro sign bytes not found intact at the end of the buffer");
}

/* Fill with 4-byte code points (79 * 4 = 316 bytes, 79 code points — far
 * under the 160-codepoint cap) plus two plain ASCII bytes, landing at
 * exactly 318 bytes / 2 bytes of headroom. Then push a 3-byte code point
 * one byte at a time: byte 1 and 2 are buffered (not yet checked against
 * the cap — msg.h's documented contract: "a byte that only continues an
 * incomplete sequence... returns true without changing either count"),
 * byte 3 completes the sequence and must be REFUSED because 318+3 > 320 —
 * and, critically, none of the three bytes may have been written into the
 * composer buffer: byte length and text must be back to exactly what they
 * were before this push sequence started (§9.4: "refusing further input
 * rather than truncating" — never a partial/split code point). Isolates
 * the byte cap from the code point cap (both are exercised independently
 * elsewhere in this file). */
static void test_three_byte_codepoint_refused_at_byte_cap(void)
{
    msg_composer_reset();
    for (int i = 0; i < 79; i++) {
        CHECK(push_bytes(k_emoji, sizeof(k_emoji)), "emoji fill #%d refused", i);
    }
    CHECK(msg_composer_push_char('a'), "ascii fill byte 0 refused");
    CHECK(msg_composer_push_char('b'), "ascii fill byte 1 refused");

    uint16_t len_before = msg_composer_len();
    uint16_t cp_before = msg_composer_codepoint_count();
    CHECK(len_before == 318, "did not fill to exactly 2 bytes of headroom (len=%u)",
          (unsigned) len_before);
    CHECK(cp_before == 81, "code point count == %u, want 81 (79 emoji + 2 ascii)",
          (unsigned) cp_before);
    char snapshot[MSG_COMPOSER_MAX];
    strcpy(snapshot, msg_composer_text());

    bool r0 = msg_composer_push_char((char) k_euro[0]);
    bool r1 = msg_composer_push_char((char) k_euro[1]);
    bool r2 = msg_composer_push_char((char) k_euro[2]);
    CHECK(r0 && r1, "the two buffering bytes of an incomplete sequence must return true");
    CHECK(!r2, "3-byte code point was accepted 2 bytes over the 320-byte cap");

    CHECK(msg_composer_len() == len_before, "byte length changed after a refused code point: %u != %u",
          (unsigned) msg_composer_len(), (unsigned) len_before);
    CHECK(msg_composer_codepoint_count() == cp_before,
          "code point count changed after a refused code point: %u != %u",
          (unsigned) msg_composer_codepoint_count(), (unsigned) cp_before);
    CHECK(strcmp(msg_composer_text(), snapshot) == 0,
          "composer text changed (partial UTF-8 leaked in) after a refused code point");

    /* The composer must still be usable afterwards (pending state cleared,
     * not wedged) — a plain ASCII push into the two remaining bytes of
     * headroom must succeed. */
    CHECK(msg_composer_push_char('z'), "composer left wedged after a refused multi-byte push");
    CHECK(msg_composer_len() == len_before + 1, "post-refusal push did not land");
}

static void test_backspace_removes_whole_codepoint(void)
{
    msg_composer_reset();
    push_str("ab");
    CHECK(msg_composer_push_char((char) k_euro[0]), "euro byte 0 refused");
    CHECK(msg_composer_push_char((char) k_euro[1]), "euro byte 1 refused");
    CHECK(msg_composer_push_char((char) k_euro[2]), "euro byte 2 refused");
    CHECK(msg_composer_len() == 5, "len == %u, want 5 ('a','b',euro x3)",
          (unsigned) msg_composer_len());
    CHECK(msg_composer_codepoint_count() == 3, "codepoints == %u, want 3",
          (unsigned) msg_composer_codepoint_count());

    CHECK(msg_composer_backspace(), "backspace on a non-empty composer returned false");
    CHECK(msg_composer_len() == 2, "backspace removed %u bytes, want exactly the 3-byte euro sign "
                                    "(len now %u, want 2)",
          (unsigned) (5 - msg_composer_len()), (unsigned) msg_composer_len());
    CHECK(strcmp(msg_composer_text(), "ab") == 0, "text after backspace == \"%s\", want \"ab\"",
          msg_composer_text());
    CHECK(msg_composer_codepoint_count() == 2, "codepoints after backspace == %u, want 2",
          (unsigned) msg_composer_codepoint_count());
}

static void test_reset_clears_partial_sequence(void)
{
    msg_composer_reset();
    push_str("x");
    CHECK(msg_composer_push_char((char) k_euro[0]), "euro byte 0 refused"); /* leaves a pending seq */
    msg_composer_reset();
    CHECK(msg_composer_len() == 0, "reset did not clear len");
    CHECK(msg_composer_codepoint_count() == 0, "reset did not clear codepoint count");
    CHECK(strcmp(msg_composer_text(), "") == 0, "reset did not clear text");

    /* A fresh push right after reset must behave as a new code point, not
     * as a continuation of whatever was pending before the reset. */
    CHECK(msg_composer_push_char('y'), "push after reset refused");
    CHECK(msg_composer_len() == 1 && msg_composer_codepoint_count() == 1,
          "push after reset did not land as a clean single-byte code point");
}

int main(void)
{
    test_ascii_byte_cap();
    test_ascii_codepoint_cap();
    test_three_byte_codepoint_accepted();
    test_three_byte_codepoint_refused_at_byte_cap();
    test_backspace_removes_whole_codepoint();
    test_reset_clears_partial_sequence();

    if (g_failures == 0) {
        printf("PASS: msg.c composer (320-byte/160-codepoint caps, 3-byte code point atomicity), "
               "0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

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

/* ---------------------------------------------------------------------
 * msghist: the persisted-history record codec, terminal-ack predicate and
 * restore ordering (owner task 2026-09-20, "keep recent messages across
 * reboots"). Pure logic, no NVS/ESP-IDF — see msg.h's long comment above
 * MSGHIST_REC_VERSION and msg.c's own module comment for the design.
 * --------------------------------------------------------------------- */

static msg_t make_msg(const char *id, const char *from, const char *to, const char *body,
                      uint8_t dir, uint8_t ack_state, uint8_t flags, int64_t ts)
{
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.ts = ts;
    strncpy(m.id, id, sizeof(m.id) - 1);
    strncpy(m.from, from, sizeof(m.from) - 1);
    strncpy(m.to, to, sizeof(m.to) - 1);
    size_t blen = strlen(body);
    memcpy(m.body, body, blen);
    m.body[blen] = '\0';
    m.body_len = (uint16_t) blen;
    m.dir = dir;
    m.ack_state = ack_state;
    m.flags = flags;
    m.in_use = true;
    return m;
}

static void test_msghist_roundtrip_basic(void)
{
    msg_t m = make_msg("m_7f3a", "mom", "", "Pickup at 3:15 by the gym", MSG_DIR_DOWN,
                       MSG_ACK_SHOWN, MSG_F_RECOVERED, 1757700000);
    uint8_t buf[MSGHIST_REC_MAX];
    size_t len = msghist_record_encode(&m, 42, buf, sizeof(buf));
    CHECK(len > 0, "encode of a normal record returned 0");

    msg_t out;
    uint32_t seq = 0;
    CHECK(msghist_record_decode(buf, len, &out, &seq), "decode of a just-encoded record failed");
    CHECK(seq == 42, "seq == %u, want 42", (unsigned) seq);
    CHECK(strcmp(out.id, m.id) == 0, "id mismatch after round trip: '%s' != '%s'", out.id, m.id);
    CHECK(strcmp(out.from, m.from) == 0, "from mismatch after round trip");
    CHECK(strcmp(out.body, m.body) == 0, "body mismatch after round trip: '%s' != '%s'", out.body,
          m.body);
    CHECK(out.body_len == m.body_len, "body_len == %u, want %u", (unsigned) out.body_len,
          (unsigned) m.body_len);
    CHECK(out.ts == m.ts, "ts mismatch after round trip");
    CHECK(out.dir == m.dir, "dir mismatch after round trip");
    CHECK(out.ack_state == m.ack_state, "ack_state mismatch after round trip");
    CHECK(out.flags == m.flags, "flags mismatch after round trip");
    CHECK(out.hist_seq == 42, "decoded hist_seq == %u, want 42", (unsigned) out.hist_seq);
    CHECK(out.in_use, "decoded record not marked in_use");
}

/* An empty body (a `loc_req`/ack has none) and a full 320-byte body are the
 * two edges msg.c's own body_rules_ok() allows through; both must round
 * trip without truncation. */
static void test_msghist_roundtrip_edges(void)
{
    msg_t empty = make_msg("u_00000001", "student", "mom", "", MSG_DIR_UP, MSG_ACK_UP_PENDING, 0, 0);
    uint8_t buf[MSGHIST_REC_MAX];
    size_t len = msghist_record_encode(&empty, 1, buf, sizeof(buf));
    CHECK(len > 0, "encode of an empty-body record returned 0");
    msg_t out;
    uint32_t seq;
    CHECK(msghist_record_decode(buf, len, &out, &seq), "decode of an empty-body record failed");
    CHECK(out.body_len == 0, "decoded body_len == %u, want 0", (unsigned) out.body_len);
    CHECK(out.body[0] == '\0', "decoded body not empty");

    char full_body[321];
    memset(full_body, 'x', 320);
    full_body[320] = '\0';
    msg_t full = make_msg("m_deadbeef", "mom", "", full_body, MSG_DIR_DOWN, MSG_ACK_UNSHOWN, 0,
                          1700000000);
    len = msghist_record_encode(&full, 7, buf, sizeof(buf));
    CHECK(len > 0 && len <= MSGHIST_REC_MAX, "encode of a full 320-byte body out of range: %u",
          (unsigned) len);
    CHECK(msghist_record_decode(buf, len, &out, &seq), "decode of a full 320-byte body failed");
    CHECK(out.body_len == 320, "decoded body_len == %u, want 320", (unsigned) out.body_len);
    CHECK(memcmp(out.body, full_body, 320) == 0, "decoded 320-byte body content mismatch");
}

/* A single flipped byte anywhere in a valid record (a torn nvs_commit(), or
 * simply reading a foreign/garbage slot) MUST be rejected, never silently
 * accepted with wrong content — msg.c's history_restore() relies on this to
 * treat a corrupt slot as "skip it", never fatal. */
static void test_msghist_decode_rejects_corruption(void)
{
    msg_t m = make_msg("m_7f3a", "mom", "", "ok", MSG_DIR_DOWN, MSG_ACK_READ, 0, 1700000000);
    uint8_t buf[MSGHIST_REC_MAX];
    size_t len = msghist_record_encode(&m, 5, buf, sizeof(buf));
    CHECK(len > 0, "encode failed");

    for (size_t i = 0; i < len; i++) {
        uint8_t saved = buf[i];
        buf[i] ^= 0xFF;
        msg_t out;
        uint32_t seq;
        bool ok = msghist_record_decode(buf, len, &out, &seq);
        buf[i] = saved;
        CHECK(!ok, "corrupting byte %u of a valid record was NOT detected", (unsigned) i);
    }

    /* A truncated buffer (a short nvs_get_blob() read) must also be rejected. */
    for (size_t trunc = 0; trunc < len; trunc++) {
        msg_t out;
        uint32_t seq;
        CHECK(!msghist_record_decode(buf, trunc, &out, &seq),
              "a %u-byte truncation of a %u-byte record was NOT detected", (unsigned) trunc,
              (unsigned) len);
    }

    /* A version byte from an unknown future format must also be rejected,
     * not partially interpreted. */
    uint8_t bad_version[MSGHIST_REC_MAX];
    memcpy(bad_version, buf, len);
    bad_version[0] = (uint8_t) (MSGHIST_REC_VERSION + 1);
    msg_t out;
    uint32_t seq;
    CHECK(!msghist_record_decode(bad_version, len, &out, &seq),
          "a record with an unknown version was accepted");
}

static void test_msghist_terminal_ack(void)
{
    CHECK(!msghist_is_terminal_ack(MSG_DIR_DOWN, MSG_ACK_UNSHOWN), "UNSHOWN reported terminal");
    CHECK(!msghist_is_terminal_ack(MSG_DIR_DOWN, MSG_ACK_SHOWN), "SHOWN reported terminal");
    CHECK(msghist_is_terminal_ack(MSG_DIR_DOWN, MSG_ACK_READ), "READ not reported terminal");
    CHECK(!msghist_is_terminal_ack(MSG_DIR_UP, MSG_ACK_UP_PENDING), "UP_PENDING reported terminal");
    CHECK(msghist_is_terminal_ack(MSG_DIR_UP, MSG_ACK_UP_SENT), "UP_SENT not reported terminal");
    CHECK(msghist_is_terminal_ack(MSG_DIR_UP, MSG_ACK_UP_FAILED), "UP_FAILED not reported terminal");
}

/* history_restore()'s own "which of the decoded slots is newest" step —
 * ring indexing/ordering only, no msg_t/NVS knowledge (msg.h's own doc
 * comment on msghist_restore_order()). */
static void test_msghist_restore_order(void)
{
    /* Seqs out of order, as they would be reading slots "m0".."m4" back in
     * slot order rather than insertion order. */
    uint32_t seqs[5] = { 103, 101, 105, 102, 104 };
    int order[5];
    int n = msghist_restore_order(seqs, 5, order, 5);
    CHECK(n == 5, "n == %d, want 5", n);
    uint32_t expect_desc[5] = { 105, 104, 103, 102, 101 };
    for (int i = 0; i < 5; i++) {
        CHECK(order[i] >= 0 && order[i] < 5, "order[%d] == %d out of range", i, order[i]);
        CHECK(seqs[order[i]] == expect_desc[i], "order[%d] -> seq %u, want %u (newest-first)", i,
              (unsigned) seqs[order[i]], (unsigned) expect_desc[i]);
    }

    /* n clamped to max_out (a defensive floor this file's own caller never
     * actually needs, msg.h's doc comment) — must not overflow order_out. */
    int order2[2];
    int n2 = msghist_restore_order(seqs, 5, order2, 2);
    CHECK(n2 == 2, "clamped n == %d, want 2", n2);
}

int main(void)
{
    test_ascii_byte_cap();
    test_ascii_codepoint_cap();
    test_three_byte_codepoint_accepted();
    test_three_byte_codepoint_refused_at_byte_cap();
    test_backspace_removes_whole_codepoint();
    test_reset_clears_partial_sequence();

    test_msghist_roundtrip_basic();
    test_msghist_roundtrip_edges();
    test_msghist_decode_rejects_corruption();
    test_msghist_terminal_ack();
    test_msghist_restore_order();

    if (g_failures == 0) {
        printf("PASS: msg.c composer (320-byte/160-codepoint caps, 3-byte code point atomicity) "
               "and msghist record codec/terminal-ack/restore-order, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

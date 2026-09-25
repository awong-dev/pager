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

/* S1 (docs/DEVICE_NEXT_TASKS.md): msg_insert_sms_in() now stores the
 * caller's already-fetched net_get_clock() result instead of hardcoding 0 —
 * itself not host-testable (sms.c/msg.c's ESP_PLATFORM-guarded section), but
 * the codec it now actually exercises with a nonzero `ts` is: an "x_"-id,
 * MSG_ACK_READ, MSG_DIR_DOWN entry (msg_insert_sms_in()'s exact shape) with
 * a real ts round-trips; with ts 0 (no clock yet, §3.5's existing sentinel,
 * "today's behaviour" pre-S1) it round-trips identically. */
static void test_msghist_roundtrip_sms_ts(void)
{
    msg_t with_ts = make_msg("x_deadbeef", "mom", "", "call me back", MSG_DIR_DOWN, MSG_ACK_READ, 0,
                             1757700000);
    uint8_t buf[MSGHIST_REC_MAX];
    size_t len = msghist_record_encode(&with_ts, 21, buf, sizeof(buf));
    CHECK(len > 0, "encode of an SMS-shaped record with a real ts returned 0");
    msg_t out;
    uint32_t seq = 0;
    CHECK(msghist_record_decode(buf, len, &out, &seq), "decode of an SMS-shaped record failed");
    CHECK(out.ts == 1757700000, "ts mismatch after round trip: %lld != 1757700000",
          (long long) out.ts);
    CHECK(out.dir == MSG_DIR_DOWN, "dir mismatch");
    CHECK(out.ack_state == MSG_ACK_READ, "ack_state mismatch");

    msg_t no_ts = make_msg("x_cafef00d", "mom", "", "call me back", MSG_DIR_DOWN, MSG_ACK_READ, 0, 0);
    len = msghist_record_encode(&no_ts, 22, buf, sizeof(buf));
    CHECK(len > 0, "encode of an SMS-shaped record with ts 0 returned 0");
    CHECK(msghist_record_decode(buf, len, &out, &seq), "decode of the ts-0 record failed");
    CHECK(out.ts == 0, "ts-0 record decoded with ts == %lld, want 0", (long long) out.ts);
}

/* G7 (docs/GROUP_CHAT_DESIGN.md §4): `sndr` round-trips through the v2
 * codec, and a 16-char `sndr` alongside a full 320-byte body still fits
 * MSGHIST_REC_MAX (420). */
static void test_msghist_roundtrip_sndr(void)
{
    msg_t m = make_msg("m_7f3a", "family-grp", "", "Pickup at 3:15 by the gym", MSG_DIR_DOWN,
                       MSG_ACK_SHOWN, 0, 1757700000);
    strncpy(m.sndr, "alice", sizeof(m.sndr) - 1);

    uint8_t buf[MSGHIST_REC_MAX];
    size_t len = msghist_record_encode(&m, 9, buf, sizeof(buf));
    CHECK(len > 0, "encode of a record with sndr returned 0");

    msg_t out;
    uint32_t seq = 0;
    CHECK(msghist_record_decode(buf, len, &out, &seq), "decode of a record with sndr failed");
    CHECK(strcmp(out.sndr, "alice") == 0, "sndr mismatch after round trip: '%s' != 'alice'",
          out.sndr);
    CHECK(strcmp(out.from, m.from) == 0, "from mismatch after round trip (should stay the group alias)");

    /* Worst case: a 16-char sndr (MSG_FROM_MAX - 1) plus a full 320-byte
     * body plus full-length id/from/to must still fit MSGHIST_REC_MAX. */
    msg_t big = make_msg("m_deadbeefdeadbe", "0123456789abcdef", "0123456789abcdef", "", MSG_DIR_DOWN,
                         MSG_ACK_UNSHOWN, 0, 1700000000);
    char full_body[321];
    memset(full_body, 'x', 320);
    full_body[320] = '\0';
    memcpy(big.body, full_body, 320);
    big.body[320] = '\0';
    big.body_len = 320;
    strncpy(big.sndr, "0123456789abcdef", sizeof(big.sndr) - 1);

    len = msghist_record_encode(&big, 11, buf, sizeof(buf));
    CHECK(len > 0 && len <= MSGHIST_REC_MAX,
          "worst-case sndr+body record out of range: %u (cap %u)", (unsigned) len,
          (unsigned) MSGHIST_REC_MAX);
    CHECK(msghist_record_decode(buf, len, &out, &seq), "decode of the worst-case record failed");
    CHECK(strcmp(out.sndr, "0123456789abcdef") == 0, "worst-case sndr mismatch: '%s'", out.sndr);
}

/* --- little-endian raw encoder, mirroring msg.c's private w_*() helpers,
 * used only to hand-build a version-1 (pre-G7, no `sndr` field) record byte
 * for byte the way msg.c's OLD encoder would have, so this test does not
 * depend on msghist_record_encode() (which, post-G7, only ever writes
 * version 2). msghist_crc32() is msg.c-private too, so it is reimplemented
 * here bit for bit from its own doc comment ("the same one zlib/PNG/
 * esp_rom_crc32_le() use"). --- */
static uint32_t crc32_like_msg_c(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = (uint32_t) (-(int32_t) (crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void put_u8(uint8_t *buf, size_t *off, uint8_t v) { buf[(*off)++] = v; }

static void put_u16(uint8_t *buf, size_t *off, uint16_t v)
{
    buf[(*off)++] = (uint8_t) (v & 0xFF);
    buf[(*off)++] = (uint8_t) (v >> 8);
}

static void put_u32(uint8_t *buf, size_t *off, uint32_t v)
{
    buf[(*off)++] = (uint8_t) (v & 0xFF);
    buf[(*off)++] = (uint8_t) ((v >> 8) & 0xFF);
    buf[(*off)++] = (uint8_t) ((v >> 16) & 0xFF);
    buf[(*off)++] = (uint8_t) ((v >> 24) & 0xFF);
}

static void put_i64(uint8_t *buf, size_t *off, int64_t v)
{
    uint64_t u = (uint64_t) v;
    put_u32(buf, off, (uint32_t) (u & 0xFFFFFFFFu));
    put_u32(buf, off, (uint32_t) (u >> 32));
}

static void put_str_field(uint8_t *buf, size_t *off, const char *s)
{
    size_t len = strlen(s);
    put_u8(buf, off, (uint8_t) len);
    memcpy(buf + *off, s, len);
    *off += len;
}

/* Builds a version-1-format record (header, id/from/to, body_len, body,
 * crc32 over everything before the crc) — no `sndr` field at all, matching
 * exactly what a pre-G7 firmware would have written to `msghist`. */
static size_t encode_v1_record(uint8_t *out, uint32_t seq, int64_t ts, uint8_t dir,
                               uint8_t ack_state, uint8_t flags, const char *id, const char *from,
                               const char *to, const char *body, uint16_t body_len)
{
    size_t off = 0;
    put_u8(out, &off, 1); /* version */
    put_u32(out, &off, seq);
    put_i64(out, &off, ts);
    put_u8(out, &off, dir);
    put_u8(out, &off, ack_state);
    put_u8(out, &off, flags);
    put_str_field(out, &off, id);
    put_str_field(out, &off, from);
    put_str_field(out, &off, to);
    put_u16(out, &off, body_len);
    memcpy(out + off, body, body_len);
    off += body_len;
    uint32_t crc = crc32_like_msg_c(out, off);
    put_u32(out, &off, crc);
    return off;
}

/* G7's dual-read requirement: "a v1 record round-trips through the v2
 * decoder with sndr == ''". Without this, every message a pre-G7 firmware
 * ever persisted would be dropped (decode failure) on the first boot after
 * the upgrade — msg.h's own comment on MSGHIST_REC_VERSION. */
static void test_msghist_v1_record_decodes_with_empty_sndr(void)
{
    uint8_t buf[MSGHIST_REC_MAX];
    size_t len = encode_v1_record(buf, 3, 1700000000, MSG_DIR_DOWN, MSG_ACK_READ, 0, "m_old",
                                  "mom", "", "hi there", 8);
    CHECK(len > 0 && len <= MSGHIST_REC_MAX, "v1 record encode out of range");

    msg_t out;
    uint32_t seq = 0;
    CHECK(msghist_record_decode(buf, len, &out, &seq), "v2 decoder rejected a valid v1 record");
    CHECK(seq == 3, "v1 decode: seq == %u, want 3", (unsigned) seq);
    CHECK(strcmp(out.id, "m_old") == 0, "v1 decode: id mismatch");
    CHECK(strcmp(out.from, "mom") == 0, "v1 decode: from mismatch");
    CHECK(strcmp(out.body, "hi there") == 0, "v1 decode: body mismatch");
    CHECK(out.sndr[0] == '\0', "v1 decode: sndr should be empty, got '%s'", out.sndr);
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

/* ---------------------------------------------------------------------
 * T4 (docs/CHAT_UI_DESIGN.md §3 "Chat"): msg_peer_of() — the ONE
 * peer-attribution rule shared by scr_home.c's home_peers_build() and this
 * file's own (ESP-only) msg_iter_peer(), so the two screens can never
 * disagree. Pure logic, no NVS/ESP-IDF, same split as the msghist section
 * above.
 * --------------------------------------------------------------------- */

/* Reproduces msg_iter_peer()'s own newest-first filter over a plain array
 * (msg_iter_peer() itself walks msg.c's ESP-only s_thread ring under a
 * lock, not host-testable directly) using the real, shared msg_peer_of() —
 * so the FILTERING behaviour a per-peer Chat view relies on, not just one
 * message's peer in isolation, is exercised here too. `msgs` must already
 * be newest-first (msg_thread_at()'s own convention, index 0 == newest). */
static size_t filter_peer(const msg_t *msgs, size_t n, const char *alias, bool have_default,
                          const char *default_alias, const msg_t **out, size_t out_cap)
{
    size_t count = 0;
    for (size_t i = 0; i < n; i++) {
        char peer[MSG_FROM_MAX];
        msg_peer_of(&msgs[i], have_default, default_alias, peer, sizeof(peer));
        if (strcmp(peer, alias) == 0 && count < out_cap) {
            out[count++] = &msgs[i];
        }
    }
    return count;
}

/* The five cases docs/CHAT_UI_DESIGN.md §3's own Verify list names
 * explicitly. */
static void test_msg_peer_of(void)
{
    char peer[MSG_FROM_MAX];

    msg_t down = make_msg("m_1", "mom", "", "hi", MSG_DIR_DOWN, MSG_ACK_SHOWN, 0, 100);
    msg_peer_of(&down, true, "mom", peer, sizeof(peer));
    CHECK(strcmp(peer, "mom") == 0, "down message peer == '%s', want 'mom'", peer);

    msg_t group_down = make_msg("m_2", "fam", "", "party", MSG_DIR_DOWN, MSG_ACK_SHOWN, 0, 101);
    strncpy(group_down.sndr, "ben", sizeof(group_down.sndr) - 1);
    msg_peer_of(&group_down, true, "mom", peer, sizeof(peer));
    CHECK(strcmp(peer, "fam") == 0, "group down message peer == '%s', want 'fam' (never sndr)", peer);

    msg_t up_explicit = make_msg("u_1", "student", "ben", "ok", MSG_DIR_UP, MSG_ACK_UP_PENDING, 0, 102);
    msg_peer_of(&up_explicit, true, "mom", peer, sizeof(peer));
    CHECK(strcmp(peer, "ben") == 0, "up message with to=='ben' peer == '%s', want 'ben'", peer);

    msg_t up_default = make_msg("u_2", "student", "", "ok", MSG_DIR_UP, MSG_ACK_UP_PENDING, 0, 103);
    msg_peer_of(&up_default, true, "mom", peer, sizeof(peer));
    CHECK(strcmp(peer, "mom") == 0,
          "up message with empty to, default 'mom', peer == '%s', want 'mom'", peer);

    msg_peer_of(&up_default, false, "", peer, sizeof(peer));
    CHECK(strcmp(peer, "(default)") == 0,
          "up message with empty to, no book, peer == '%s', want '(default)'", peer);
}

/* A thread with messages to/from two peers ("mom", "ben") yields the right
 * per-peer subsets, each still in the thread's own newest-first order —
 * docs/CHAT_UI_DESIGN.md §3's own Verify list, second bullet. */
static void test_msg_peer_subsets_newest_first(void)
{
    msg_t msgs[5] = {
        make_msg("m_5", "ben", "", "b5", MSG_DIR_DOWN, MSG_ACK_SHOWN, 0, 500),
        make_msg("u_4", "student", "mom", "m4", MSG_DIR_UP, MSG_ACK_UP_SENT, 0, 400),
        make_msg("m_3", "mom", "", "m3", MSG_DIR_DOWN, MSG_ACK_SHOWN, 0, 300),
        make_msg("u_2", "student", "ben", "b2", MSG_DIR_UP, MSG_ACK_UP_SENT, 0, 200),
        make_msg("m_1", "mom", "", "m1", MSG_DIR_DOWN, MSG_ACK_READ, 0, 100),
    };

    const msg_t *mom_out[8];
    size_t mom_n = filter_peer(msgs, 5, "mom", true, "mom", mom_out, 8);
    CHECK(mom_n == 3, "mom subset size == %zu, want 3", mom_n);
    if (mom_n == 3) {
        CHECK(strcmp(mom_out[0]->id, "u_4") == 0, "mom_out[0] == '%s', want 'u_4'", mom_out[0]->id);
        CHECK(strcmp(mom_out[1]->id, "m_3") == 0, "mom_out[1] == '%s', want 'm_3'", mom_out[1]->id);
        CHECK(strcmp(mom_out[2]->id, "m_1") == 0, "mom_out[2] == '%s', want 'm_1'", mom_out[2]->id);
    }

    const msg_t *ben_out[8];
    size_t ben_n = filter_peer(msgs, 5, "ben", true, "mom", ben_out, 8);
    CHECK(ben_n == 2, "ben subset size == %zu, want 2", ben_n);
    if (ben_n == 2) {
        CHECK(strcmp(ben_out[0]->id, "m_5") == 0, "ben_out[0] == '%s', want 'm_5'", ben_out[0]->id);
        CHECK(strcmp(ben_out[1]->id, "u_2") == 0, "ben_out[1] == '%s', want 'u_2'", ben_out[1]->id);
    }
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
    test_msghist_roundtrip_sms_ts();
    test_msghist_roundtrip_sndr();
    test_msghist_v1_record_decodes_with_empty_sndr();
    test_msghist_decode_rejects_corruption();
    test_msghist_terminal_ack();
    test_msghist_restore_order();

    test_msg_peer_of();
    test_msg_peer_subsets_newest_first();

    if (g_failures == 0) {
        printf("PASS: msg.c composer (320-byte/160-codepoint caps, 3-byte code point atomicity), "
               "msghist record codec/terminal-ack/restore-order (v1+v2, G7 sndr), and msg_peer_of() "
               "(T4 shared peer rule), 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

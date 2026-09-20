/* test_sms.c — host test harness for main/sms.c's pure functions
 * (docs/V02_DESIGN.md §6/§7 device-direct SMS).
 *
 * Builds with the plain host compiler, no ESP-IDF (sms.c's pure section has
 * no ESP-IDF dependency — same `#ifdef ESP_PLATFORM` split as loc.c/cfg.c/
 * catrust.c; see firmware/host/Makefile).
 *
 * Coverage required by the task brief:
 *  - GSM-7 (IRA-preferred, GSM-narrow-fallback) vs UCS-2 decision, including
 *    `@`/`$`/`_`, every escape-table character (counted as 2 septets under
 *    IRA, excluded entirely under the GSM-narrow fallback), the backtick
 *    (no GSM mapping at all) and non-ASCII (`é`), and the exact 160/70
 *    boundaries
 *  - emoji (surrogate pairs, code points outside the BMP counted as 2
 *    UTF-16 units)
 *  - allow-list matching, including a national-format sender and near-miss
 *    numbers that must NOT match
 *  - cfg.sms decode: valid, empty, 9 entries (over cap), bad phone,
 *    over-long name, duplicate phone
 *  - the DCS-driven receive decode (coordinator fix #2): sms_classify_dcs(),
 *    sms_parse_cmgr_header() (with/without AT+CSDH=1's extra fields, a
 *    quoted alpha field containing a comma, an empty alpha field), and
 *    sms_decode_received() (7-bit/UCS-2/8-bit-undisplayable/fallback)
 *  - audit queue ordering/overflow/sms_lost/slot indices (the coordinator's
 *    per-entry-NVS fix changed sms_audit_enqueue()/sms_audit_pop_oldest()
 *    to report which ring slot changed)
 *  - the `sms_log` CBOR encoder, byte-compared against bytes produced by
 *    relay/.venv/bin/python + relay/app/wirecbor.py (see
 *    test_sms_log_cbor_vs_relay() below for the exact command used to
 *    generate the expected hex strings).
 */
#include "sms.h"
#include "cbor.h"

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

/* ---------------------------------------------------------------------
 * sms_name_valid() / sms_phone_valid()
 * --------------------------------------------------------------------- */
static void test_name_phone_valid(void)
{
    CHECK(sms_name_valid("Mom", 3), "plain ASCII name must be valid");
    CHECK(!sms_name_valid("", 0), "empty name must be invalid");
    CHECK(sms_name_valid("123456789012345Z", 16), "exactly 16 code points must be valid");
    CHECK(!sms_name_valid("1234567890123456Z", 17), "17 code points must be invalid");
    // 12 two-byte UTF-8 characters ("é" x12) = 12 code points, 24 UTF-8
    // bytes — the exact worst case V02_DESIGN.md §6/§7 sizes the cap
    // against (relay/tests/test_sms.py's own
    // test_push_sms_contacts_eight_worst_case_entries_fits_both_encodings).
    const char *e_x12 = "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";
    CHECK(strlen(e_x12) == 24, "test setup: 12x 'e-acute' must be 24 UTF-8 bytes, got %zu",
          strlen(e_x12));
    CHECK(sms_name_valid(e_x12, strlen(e_x12)), "12 code points / 24 UTF-8 bytes must be valid");

    CHECK(sms_phone_valid("+12065550100", 12), "valid E.164 number");
    CHECK(!sms_phone_valid("0000", 4), "no leading '+' / too short must be invalid");
    CHECK(!sms_phone_valid("+0123456", 8), "leading digit after '+' must be 1-9, not 0");
    CHECK(sms_phone_valid("+1234567", 8),
          "'+1234567': 1 leading digit + 6 more digits = the minimum legal length (len 8)");
    CHECK(!sms_phone_valid("+123456", 7), "one digit short of the floor must be invalid");
    CHECK(!sms_phone_valid("+123456789012345678", 19), "over 15 total digits must be invalid");
}

/* ---------------------------------------------------------------------
 * sms_numbers_match(): E.164-exact, national-format fallback, near-miss.
 * --------------------------------------------------------------------- */
static void test_numbers_match(void)
{
    CHECK(sms_numbers_match("+12065550100", "+12065550100"), "exact E.164 match");
    CHECK(sms_numbers_match("+12065550100", "2065550100"), "national format (no country code) must match");
    CHECK(sms_numbers_match("+12065550100", "(206) 555-0100"), "formatted national number must match");
    CHECK(!sms_numbers_match("+12065550100", "+12065550199"), "near-miss (last two digits differ) must NOT match");
    CHECK(!sms_numbers_match("+12065550100", "+13065550100"), "different area code must NOT match");
    CHECK(!sms_numbers_match("", "+12065550100"), "empty allow-list entry must never match");
    CHECK(!sms_numbers_match("+12065550100", ""), "empty sender must never match");

    sms_contact_list_t list;
    memset(&list, 0, sizeof(list));
    list.count = 2;
    strncpy(list.contacts[0].name, "Mom", sizeof(list.contacts[0].name) - 1);
    strncpy(list.contacts[0].phone, "+12065550100", sizeof(list.contacts[0].phone) - 1);
    strncpy(list.contacts[1].name, "Dad", sizeof(list.contacts[1].name) - 1);
    strncpy(list.contacts[1].phone, "+12065550101", sizeof(list.contacts[1].phone) - 1);

    sms_contact_t out;
    int idx = sms_find_contact_by_number(&list, "2065550101", &out);
    CHECK(idx == 1 && strcmp(out.name, "Dad") == 0, "national-format Dad must resolve to index 1");
    CHECK(sms_find_contact_by_number(&list, "+19995551234", NULL) == -1,
          "an unlisted number must not match anything");

    idx = sms_find_contact_by_name(&list, "Mom", 3, &out);
    CHECK(idx == 0 && strcmp(out.phone, "+12065550100") == 0, "name lookup must find Mom");
    CHECK(sms_find_contact_by_name(&list, "mom", 3, NULL) == -1, "name lookup is case-sensitive");
}

/* ---------------------------------------------------------------------
 * cfg.sms decode: valid, empty, over-cap, bad phone, over-long name,
 * duplicate phone (V02_DESIGN.md §6: reject the WHOLE list on any bad entry).
 * --------------------------------------------------------------------- */
// cbor_w_array() always writes a key; a bare top-level array (no enclosing
// map key) is what cfg.c's own span-extraction hands sms_parse_cfg_submap()
// — reproduced here by writing the array header directly rather than
// through cbor_w_array()'s keyed form.
static size_t build_sms_array_bare(uint8_t *buf, size_t cap, const char **names,
                                   const char **phones, size_t n)
{
    /* cbor_w.c has no unkeyed array-header writer exposed; a definite-length
     * array header for n<24 items is one byte (0x80 | n) per RFC 8949 §3.1
     * — written directly here, matching the exact byte cfg.c's own
     * cbor_r_array() call expects (cfg_dispatch_t.sms_off/sms_len is the
     * raw span starting at this very header). */
    if (n >= 24 || cap == 0) {
        return 0;
    }
    buf[0] = (uint8_t) (0x80 | n);
    cbor_w_t w;
    cbor_w_init(&w, buf + 1, cap - 1);
    for (size_t i = 0; i < n; i++) {
        cbor_w_map(&w, 2);
        cbor_w_tstr(&w, 0, names[i], strlen(names[i]));
        cbor_w_tstr(&w, 1, phones[i], strlen(phones[i]));
    }
    if (w.err) {
        return 0;
    }
    return 1 + w.len;
}

static void test_cfg_sms_decode(void)
{
    uint8_t buf[1024];

    {
        const char *names[] = { "Mom", "Dad" };
        const char *phones[] = { "+12065550100", "+12065550101" };
        size_t len = build_sms_array_bare(buf, sizeof(buf), names, phones, 2);
        CHECK(len > 0, "test setup: valid 2-entry array must encode");
        sms_contact_list_t out;
        CHECK(sms_parse_cfg_submap(buf, (uint16_t) len, &out), "a valid 2-entry list must decode");
        CHECK(out.count == 2 && strcmp(out.contacts[0].name, "Mom") == 0 &&
                  strcmp(out.contacts[1].phone, "+12065550101") == 0,
              "decoded contacts must match input");
    }
    {
        size_t len = build_sms_array_bare(buf, sizeof(buf), NULL, NULL, 0);
        CHECK(len > 0, "test setup: empty array must encode");
        sms_contact_list_t out;
        memset(&out, 0xAA, sizeof(out));
        CHECK(sms_parse_cfg_submap(buf, (uint16_t) len, &out), "an empty array must be a VALID push");
        CHECK(out.count == 0, "an empty push must clear the allow-list (count 0)");
    }
    {
        const char *names[9] = { "a", "b", "c", "d", "e", "f", "g", "h", "i" };
        const char *phones[9] = { "+12065550100", "+12065550101", "+12065550102", "+12065550103",
                                  "+12065550104", "+12065550105", "+12065550106", "+12065550107",
                                  "+12065550108" };
        // 9 entries needs a 1-byte-count-inline-24..? n=9 < 24 so still fits
        // the bare single-byte header helper above.
        size_t len = build_sms_array_bare(buf, sizeof(buf), names, phones, 9);
        CHECK(len > 0, "test setup: 9-entry array must encode");
        sms_contact_list_t out;
        CHECK(!sms_parse_cfg_submap(buf, (uint16_t) len, &out), "9 entries (over SMS_MAX_CONTACTS=8) must be rejected");
    }
    {
        const char *names[] = { "Mom" };
        const char *phones[] = { "0000" }; // not E.164
        size_t len = build_sms_array_bare(buf, sizeof(buf), names, phones, 1);
        sms_contact_list_t out;
        CHECK(!sms_parse_cfg_submap(buf, (uint16_t) len, &out), "a bad phone must reject the whole list");
    }
    {
        const char *names[] = { "ThisNameIsWayTooLong" }; // > 16 code points
        const char *phones[] = { "+12065550100" };
        size_t len = build_sms_array_bare(buf, sizeof(buf), names, phones, 1);
        sms_contact_list_t out;
        CHECK(!sms_parse_cfg_submap(buf, (uint16_t) len, &out), "an over-long name must reject the whole list");
    }
    {
        const char *names[] = { "Mom", "Mom Cell" };
        const char *phones[] = { "+12065550100", "+12065550100" }; // duplicate
        size_t len = build_sms_array_bare(buf, sizeof(buf), names, phones, 2);
        sms_contact_list_t out;
        CHECK(!sms_parse_cfg_submap(buf, (uint16_t) len, &out), "a duplicate phone must reject the whole list");
    }
    {
        // Do-not-partially-apply check: entry 0 valid, entry 1 bad -> `out`
        // must come back untouched (still whatever the caller had before).
        const char *names[] = { "Mom", "Bad" };
        const char *phones[] = { "+12065550100", "0000" };
        size_t len = build_sms_array_bare(buf, sizeof(buf), names, phones, 2);
        sms_contact_list_t out;
        memset(&out, 0, sizeof(out));
        out.count = 5; // sentinel: must NOT be overwritten to 1 or left at a partial state
        CHECK(!sms_parse_cfg_submap(buf, (uint16_t) len, &out), "entry 1 is bad -> whole list rejected");
        CHECK(out.count == 5, "a rejected list must not partially apply (out left untouched)");
    }
}

/* ---------------------------------------------------------------------
 * GSM-7 vs UCS-2 decision: basic alphabet, escape/extension characters
 * (2 septets), 160/70 boundaries, emoji (surrogate pairs).
 * --------------------------------------------------------------------- */
static void test_gsm7_vs_ucs2_basic(void)
{
    sms_measure_t m;

    sms_measure("Hello, world!", 13, SMS_CHARSET_IRA, &m);
    CHECK(m.valid_utf8 && m.all_gsm7, "plain ASCII must be all_gsm7 (IRA mode)");
    CHECK(m.gsm7_septets == 13, "plain ASCII: 1 septet per character, got %zu", m.gsm7_septets);
    CHECK(sms_decide_encoding(&m) == SMS_ENC_GSM7, "plain ASCII must decide GSM7");

    // Coordinator fix (2026-09-20): under AT+CSCS="IRA", '@' ($0x40), '$'
    // (0x24) and '_' (0x5F) are all ordinary IRA/ASCII characters the modem
    // itself converts to their (differently-valued) GSM septets -- eligible
    // at 1 septet each, same as any other basic ASCII character. `@name`
    // addressing alone makes '@' common.
    sms_measure("@", 1, SMS_CHARSET_IRA, &m);
    CHECK(m.all_gsm7 && m.gsm7_septets == 1, "'@' must be a 1-septet IRA-eligible character");
    sms_measure("$", 1, SMS_CHARSET_IRA, &m);
    CHECK(m.all_gsm7 && m.gsm7_septets == 1, "'$' must be a 1-septet IRA-eligible character");
    sms_measure("_", 1, SMS_CHARSET_IRA, &m);
    CHECK(m.all_gsm7 && m.gsm7_septets == 1, "'_' must be a 1-septet IRA-eligible character");

    // Every escape-table character costs 2 septets under IRA (ESC + one
    // more) -- V02_DESIGN.md §6 / coordinator fix: "[ ] { } \ ^ ~ |".
    static const char *escape_chars = "[]{}\\^~|";
    for (size_t i = 0; escape_chars[i] != '\0'; i++) {
        char one[2] = { escape_chars[i], '\0' };
        sms_measure(one, 1, SMS_CHARSET_IRA, &m);
        CHECK(m.all_gsm7 && m.gsm7_septets == 2,
              "escape-table char '%c' must cost exactly 2 septets under IRA, got septets=%zu "
              "all_gsm7=%d",
              escape_chars[i], m.gsm7_septets, (int) m.all_gsm7);
    }
    sms_measure("A[B", 3, SMS_CHARSET_IRA, &m);
    CHECK(m.all_gsm7, "'[' is in the GSM extension table, still all_gsm7 under IRA");
    CHECK(m.gsm7_septets == 4, "A(1)+[(2)+B(1) = 4 septets, got %zu", m.gsm7_septets);
    CHECK(m.codepoints == 3, "3 code points regardless of septet cost, got %zu", m.codepoints);

    // Backtick has NO GSM 03.38 mapping at all (coordinator fix) -- must
    // force UCS-2 even under IRA.
    sms_measure("`", 1, SMS_CHARSET_IRA, &m);
    CHECK(!m.all_gsm7, "backtick must NOT be 7-bit-eligible under IRA (no GSM mapping)");
    CHECK(sms_decide_encoding(&m) == SMS_ENC_UCS2, "a lone backtick must decide UCS2");

    // Non-ASCII (e.g. 'é') has no IRA code point at all -- coordinator fix:
    // "Everything else (é, £, any non-ASCII, backtick) takes the UCS-2
    // path", even though 'é' IS in the underlying GSM 03.38 basic alphabet.
    sms_measure("\xc3\xa9", 2, SMS_CHARSET_IRA, &m); // 'é' U+00E9
    CHECK(m.valid_utf8, "test setup: valid 2-byte UTF-8 for 'é'");
    CHECK(!m.all_gsm7, "'é' must NOT be 7-bit-eligible under IRA (IRA has no code point for it)");
    CHECK(sms_decide_encoding(&m) == SMS_ENC_UCS2, "'é' must decide UCS2 under IRA");

    // A character genuinely outside the GSM 7-bit alphabet (Cyrillic Zhe).
    sms_measure("\xd0\x96", 2, SMS_CHARSET_IRA, &m); // U+0416
    CHECK(m.valid_utf8, "test setup: valid 2-byte UTF-8");
    CHECK(!m.all_gsm7, "Cyrillic character must not be all_gsm7");
    CHECK(m.ucs2_units == 1, "a BMP character costs 1 UTF-16 unit, got %zu", m.ucs2_units);
    CHECK(sms_decide_encoding(&m) == SMS_ENC_UCS2, "non-GSM BMP character must decide UCS2");
}

// Coordinator fix: the AT+CSCS="GSM" fallback (only used when "IRA" itself
// was rejected at init) narrows the eligible range to ASCII characters
// whose GSM septet index is numerically identical to their ASCII code --
// '@', '$', '_' and the whole escape table are all EXCLUDED here (unlike
// under IRA above), since none of their GSM septets match their ASCII
// value and this layer cannot hand-construct arbitrary GSM septet bytes.
static void test_gsm7_narrow_fallback_mode(void)
{
    sms_measure_t m;

    sms_measure("Hello, World! 123", 17, SMS_CHARSET_GSM_NARROW, &m);
    CHECK(m.all_gsm7 && m.gsm7_septets == 17,
          "letters/digits/space/!/, are all GSM-narrow-eligible at 1 septet each, got septets=%zu "
          "all_gsm7=%d",
          m.gsm7_septets, (int) m.all_gsm7);

    static const char *not_eligible_in_narrow_mode = "@$_[]{}\\^~|";
    for (size_t i = 0; not_eligible_in_narrow_mode[i] != '\0'; i++) {
        char one[2] = { not_eligible_in_narrow_mode[i], '\0' };
        sms_measure(one, 1, SMS_CHARSET_GSM_NARROW, &m);
        CHECK(!m.all_gsm7,
              "'%c' must NOT be 7-bit-eligible in the GSM-narrow fallback (septet != ASCII code)",
              not_eligible_in_narrow_mode[i]);
        CHECK(sms_decide_encoding(&m) == SMS_ENC_UCS2,
              "'%c' must decide UCS2 in GSM-narrow fallback mode", not_eligible_in_narrow_mode[i]);
    }

    // Same backtick/non-ASCII exclusions apply in narrow mode too.
    sms_measure("`", 1, SMS_CHARSET_GSM_NARROW, &m);
    CHECK(!m.all_gsm7, "backtick must NOT be 7-bit-eligible in GSM-narrow fallback mode either");
}

static void repeat_ascii(char *out, size_t cap, char c, size_t n)
{
    size_t i = 0;
    for (; i < n && i + 1 < cap; i++) {
        out[i] = c;
    }
    out[i] = '\0';
}

static void test_gsm7_160_septet_boundary(void)
{
    static char buf[400];
    sms_measure_t m;

    repeat_ascii(buf, sizeof(buf), 'x', 160);
    sms_measure(buf, 160, SMS_CHARSET_IRA, &m);
    CHECK(m.all_gsm7 && m.gsm7_septets == 160, "160 plain ASCII chars = 160 septets exactly");
    CHECK(sms_decide_encoding(&m) == SMS_ENC_GSM7, "exactly 160 septets must still be GSM7");

    repeat_ascii(buf, sizeof(buf), 'x', 161);
    sms_measure(buf, 161, SMS_CHARSET_IRA, &m);
    CHECK(m.gsm7_septets == 161, "161 plain ASCII chars = 161 septets");
    // 161 septets is over the GSM7 cap, and 161 ASCII chars is also over the
    // UCS-2 70-unit cap -> too long for a single SMS (no concatenation).
    CHECK(sms_decide_encoding(&m) == SMS_ENC_TOO_LONG,
          "161 plain-ASCII characters must be too long for GSM7 (161>160) AND UCS2 (161>70)");

    // 80 extension-table characters = 160 septets exactly (2 each) -- still
    // fits GSM7, proving the escape-character accounting is what is being
    // exercised here, not a coincidence of the 160 ASCII case above.
    repeat_ascii(buf, sizeof(buf), '[', 80);
    sms_measure(buf, 80, SMS_CHARSET_IRA, &m);
    CHECK(m.all_gsm7 && m.gsm7_septets == 160, "80 extension-table chars = 160 septets exactly");
    CHECK(sms_decide_encoding(&m) == SMS_ENC_GSM7, "80x '[' (160 septets) must still be GSM7");
}

static void test_ucs2_70_unit_boundary_and_emoji(void)
{
    static char buf[600];
    sms_measure_t m;

    // 70 non-GSM BMP characters (Cyrillic Zhe, 2 UTF-8 bytes each) = 70
    // UTF-16 units exactly.
    size_t pos = 0;
    for (int i = 0; i < 70; i++) {
        buf[pos++] = (char) 0xD0;
        buf[pos++] = (char) 0x96;
    }
    sms_measure(buf, pos, SMS_CHARSET_IRA, &m);
    CHECK(!m.all_gsm7 && m.ucs2_units == 70, "70 Cyrillic chars = 70 UTF-16 units exactly, got %zu",
          m.ucs2_units);
    CHECK(sms_decide_encoding(&m) == SMS_ENC_UCS2, "exactly 70 UCS-2 units must fit");

    pos = 0;
    for (int i = 0; i < 71; i++) {
        buf[pos++] = (char) 0xD0;
        buf[pos++] = (char) 0x96;
    }
    sms_measure(buf, pos, SMS_CHARSET_IRA, &m);
    CHECK(m.ucs2_units == 71, "71 Cyrillic chars = 71 UTF-16 units");
    CHECK(sms_decide_encoding(&m) == SMS_ENC_TOO_LONG, "71 UCS-2 units must be too long");

    // Emoji (outside the BMP, U+1F600 "😀", UTF-8 F0 9F 98 80) -- a
    // surrogate pair, 2 UTF-16 units each (V02_DESIGN.md §6: "characters
    // outside the BMP count as two"). 35 emoji = 70 units exactly.
    pos = 0;
    for (int i = 0; i < 35; i++) {
        buf[pos++] = (char) 0xF0;
        buf[pos++] = (char) 0x9F;
        buf[pos++] = (char) 0x98;
        buf[pos++] = (char) 0x80;
    }
    sms_measure(buf, pos, SMS_CHARSET_IRA, &m);
    CHECK(m.codepoints == 35, "35 emoji = 35 code points, got %zu", m.codepoints);
    CHECK(m.ucs2_units == 70, "35 emoji = 70 UTF-16 units (surrogate pairs), got %zu", m.ucs2_units);
    CHECK(sms_decide_encoding(&m) == SMS_ENC_UCS2, "35 emoji (70 units) must fit UCS2 exactly");

    pos = 0;
    for (int i = 0; i < 36; i++) {
        buf[pos++] = (char) 0xF0;
        buf[pos++] = (char) 0x9F;
        buf[pos++] = (char) 0x98;
        buf[pos++] = (char) 0x80;
    }
    sms_measure(buf, pos, SMS_CHARSET_IRA, &m);
    CHECK(m.ucs2_units == 72, "36 emoji = 72 UTF-16 units, got %zu", m.ucs2_units);
    CHECK(sms_decide_encoding(&m) == SMS_ENC_TOO_LONG, "36 emoji (72 units) must be too long");
}

/* ---------------------------------------------------------------------
 * Coordinator fix #2: sms_classify_dcs() (3GPP TS 23.038 §4).
 * --------------------------------------------------------------------- */
static void test_classify_dcs(void)
{
    CHECK(sms_classify_dcs(-1) == SMS_DCS_UNKNOWN, "dcs<0 (absent/no CSDH) must be UNKNOWN");
    CHECK(sms_classify_dcs(0x00) == SMS_DCS_7BIT, "dcs=0x00 must be 7BIT");
    CHECK(sms_classify_dcs(0x04) == SMS_DCS_8BIT, "dcs=0x04 must be 8BIT (undisplayable)");
    CHECK(sms_classify_dcs(0x08) == SMS_DCS_UCS2, "dcs=0x08 must be UCS2");
    CHECK(sms_classify_dcs(0x0C) == SMS_DCS_UNKNOWN, "dcs=0x0C (reserved alphabet) must be UNKNOWN");
    // A common real-world DCS byte for a 7-bit message with a message class
    // (e.g. 0x11 = class 1, general group, 7-bit) -- still 7BIT since only
    // bits 3-2 select the alphabet within the general data coding group.
    CHECK(sms_classify_dcs(0x11) == SMS_DCS_7BIT, "dcs=0x11 (7-bit + message class) must be 7BIT");
    // Outside the "general data coding" group entirely (top two bits != 00,
    // e.g. a message waiting indication group) -> UNKNOWN, not guessed.
    CHECK(sms_classify_dcs(0xC0) == SMS_DCS_UNKNOWN, "a non-general coding group must be UNKNOWN");
}

/* ---------------------------------------------------------------------
 * Coordinator fix #2: sms_parse_cmgr_header() -- with and without the
 * AT+CSDH=1 fields, a quoted alpha field containing a comma, and an empty
 * alpha field.
 * --------------------------------------------------------------------- */
static void test_parse_cmgr_header(void)
{
    sms_cmgr_header_t h;

    // Without CSDH (only the original 4 fields) -- dcs must be -1.
    {
        const char *raw = "\"REC UNREAD\",\"+12065550100\",,\"24/01/15,10:30:15+32\"";
        CHECK(sms_parse_cmgr_header(raw, strlen(raw), &h), "a well-formed no-CSDH header must parse");
        CHECK(strcmp(h.sender, "+12065550100") == 0, "sender mismatch: %s", h.sender);
        CHECK(strcmp(h.timestamp, "24/01/15,10:30:15+32") == 0, "timestamp mismatch: %s",
              h.timestamp);
        CHECK(h.dcs == -1, "dcs must be -1 when the CSDH fields are absent, got %d", h.dcs);
    }

    // With CSDH (11 fields, dcs is field 7, 0-based: stat,oa,alpha,scts,
    // tooa,fo,pid,dcs,sca,tosca,length).
    {
        const char *raw =
            "\"REC UNREAD\",\"+12065550100\",,\"24/01/15,10:30:15+32\",145,4,0,8,,145,20";
        CHECK(sms_parse_cmgr_header(raw, strlen(raw), &h), "a well-formed CSDH header must parse");
        CHECK(strcmp(h.sender, "+12065550100") == 0, "sender mismatch (CSDH): %s", h.sender);
        CHECK(h.dcs == 8, "dcs must be parsed as 8 (UCS-2) from the CSDH fields, got %d", h.dcs);
    }

    // Quoted alpha field containing a comma (a phonebook name like "Smith,
    // John") must not desynchronise the field count or swallow <scts>.
    {
        const char *raw =
            "\"REC READ\",\"+12065550101\",\"Smith, John\",\"24/01/15,11:00:00+32\",145,0,0,0,,145,5";
        CHECK(sms_parse_cmgr_header(raw, strlen(raw), &h),
              "a header with a quoted alpha containing a comma must parse");
        CHECK(strcmp(h.sender, "+12065550101") == 0, "sender mismatch (quoted alpha): %s", h.sender);
        CHECK(strcmp(h.timestamp, "24/01/15,11:00:00+32") == 0,
              "timestamp must still be found correctly after a comma-containing quoted alpha: %s",
              h.timestamp);
        CHECK(h.dcs == 0, "dcs must be parsed as 0 (7-bit) after a quoted alpha with a comma, got %d",
              h.dcs);
    }

    // Empty alpha field (",," between oa and scts) -- the common case, no
    // phonebook match.
    {
        const char *raw = "\"REC UNREAD\",\"+12065550102\",,\"24/01/15,12:00:00+32\",145,0,0,8,,145,10";
        CHECK(sms_parse_cmgr_header(raw, strlen(raw), &h), "an empty-alpha header must parse");
        CHECK(strcmp(h.sender, "+12065550102") == 0, "sender mismatch (empty alpha): %s", h.sender);
        CHECK(h.dcs == 8, "dcs must still be parsed correctly past an empty alpha field, got %d",
              h.dcs);
    }

    // Malformed: cannot even find <stat>/<oa>.
    {
        CHECK(!sms_parse_cmgr_header("", 0, &h), "an empty header must fail to parse");
    }
}

/* ---------------------------------------------------------------------
 * Coordinator fix #2: sms_decode_received() -- DCS-driven decode, the
 * fallback heuristic when dcs is unavailable, and 8-bit "never shown".
 * --------------------------------------------------------------------- */
static void test_decode_received(void)
{
    char out[SMS_BODY_MAX];
    size_t out_len;
    bool shown, used_fallback;

    // 7-bit DCS: body is already plain text (AT+CSCS="IRA" makes this
    // unambiguous) -- copied through as-is, no fallback used.
    CHECK(sms_decode_received("Hello", 5, 0x00, out, sizeof(out), &out_len, &shown, &used_fallback),
          "7-bit decode must succeed");
    CHECK(shown && !used_fallback, "7-bit DCS must be shown, no fallback needed");
    CHECK(out_len == 5 && memcmp(out, "Hello", 5) == 0, "7-bit body must pass through unchanged");

    // UCS-2 DCS: body is hex, decoded via sms_decode_ucs2_hex().
    CHECK(sms_decode_received("00480069", 8, 0x08, out, sizeof(out), &out_len, &shown,
                              &used_fallback),
          "UCS-2 decode must succeed");
    CHECK(shown && !used_fallback, "UCS-2 DCS must be shown, no fallback needed");
    CHECK(out_len == 2 && memcmp(out, "Hi", 2) == 0, "UCS-2 'Hi' (0048,0069) must decode correctly");

    // 8-bit DCS: undisplayable -- shown=false, empty body, regardless of the
    // actual bytes.
    CHECK(sms_decode_received("\x01\x02\x03", 3, 0x04, out, sizeof(out), &out_len, &shown,
                              &used_fallback),
          "8-bit decode call itself must report success (it IS handled, just not shown)");
    CHECK(!shown, "8-bit DCS must never be shown");
    CHECK(out_len == 0, "8-bit DCS must produce an empty body, got %zu bytes", out_len);

    // dcs unavailable (-1): falls back to the shape-based heuristic.
    CHECK(sms_decode_received("00480069", 8, -1, out, sizeof(out), &out_len, &shown, &used_fallback),
          "fallback-path decode must succeed");
    CHECK(shown && used_fallback, "an unavailable dcs must use the fallback and still show the result");
    CHECK(out_len == 2 && memcmp(out, "Hi", 2) == 0,
          "the fallback must still correctly identify hex-looking UCS-2 and decode it");

    CHECK(sms_decode_received("Hello", 5, -1, out, sizeof(out), &out_len, &shown, &used_fallback),
          "fallback-path plain-text decode must succeed");
    CHECK(shown && used_fallback, "an unavailable dcs must use the fallback for plain text too");
    CHECK(out_len == 5 && memcmp(out, "Hello", 5) == 0,
          "the fallback must treat non-hex-looking text as plain 7-bit text");
}

/* ---------------------------------------------------------------------
 * Encode/decode round trip + the UCS-2-hex receive heuristic.
 * --------------------------------------------------------------------- */
static void test_ucs2_roundtrip(void)
{
    const char *text = "Caf\xc3\xa9 \xf0\x9f\x98\x80"; // "Café 😀"
    size_t len = strlen(text);

    char hex[SMS_UCS2_HEX_MAX + 1];
    size_t hex_len = 0;
    CHECK(sms_encode_ucs2_hex(text, len, hex, sizeof(hex), &hex_len), "UCS-2 hex encode must succeed");
    CHECK(sms_looks_like_ucs2_hex(hex, hex_len), "the encoder's own output must pass the RX heuristic");

    char decoded[128];
    size_t decoded_len = 0;
    CHECK(sms_decode_ucs2_hex(hex, hex_len, decoded, sizeof(decoded), &decoded_len),
          "UCS-2 hex decode must succeed");
    CHECK(decoded_len == len && memcmp(decoded, text, len) == 0,
          "round trip must reproduce the original UTF-8 bytes exactly");

    // Documented heuristic risk: an all-numeric GSM-7 message that happens
    // to be even-length hex-looking digits is indistinguishable from UCS-2
    // hex by this rule alone.
    CHECK(sms_looks_like_ucs2_hex("112233", 6), "risk case: a bare numeric body reads as hex-looking");

    char gsm7[8];
    size_t gsm7_len = 0;
    CHECK(sms_encode_gsm7("hi", 2, gsm7, sizeof(gsm7), &gsm7_len), "GSM7 passthrough must succeed");
    CHECK(gsm7_len == 2 && memcmp(gsm7, "hi", 2) == 0, "GSM7 passthrough must be byte-identical");
}

/* ---------------------------------------------------------------------
 * Audit ring: ordering, overflow (oldest dropped), sms_lost, and (since the
 * coordinator's per-entry-NVS fix) the slot indices sms_audit_enqueue()/
 * sms_audit_pop_oldest() now report.
 * --------------------------------------------------------------------- */
static void test_audit_ring(void)
{
    sms_audit_ring_t r;
    sms_audit_ring_init(&r);

    sms_audit_entry_t out;
    CHECK(!sms_audit_peek_oldest(&r, &out), "an empty ring must have nothing to peek");

    int evicted = -99;
    for (int i = 0; i < SMS_AUDIT_MAX; i++) {
        char peer[SMS_PEER_MAX];
        snprintf(peer, sizeof(peer), "+1206555%04d", i);
        int slot = sms_audit_enqueue(&r, peer, "out", "sent", "x", 1, 1000 + i, &evicted);
        CHECK(slot == i, "with an empty ring, entry %d must land in slot %d, got %d", i, i, slot);
        CHECK(evicted == -1, "no eviction expected while filling the ring, got %d", evicted);
    }
    CHECK(r.count == SMS_AUDIT_MAX, "16 enqueues must fill the ring exactly, got %u", r.count);
    CHECK(r.lost == 0, "no overflow yet -> sms_lost must still be 0");

    CHECK(sms_audit_peek_oldest(&r, &out) && strcmp(out.peer, "+12065550000") == 0,
          "oldest must be entry 0's peer, got %s", out.peer);

    // Overflow: the 17th entry must evict the oldest (slot 0) and bump `lost`.
    int slot17 = sms_audit_enqueue(&r, "+19995551111", "in", "blocked", "", 0, 2000, &evicted);
    CHECK(evicted == 0, "overflow must report the evicted slot (0), got %d", evicted);
    CHECK(slot17 == 0, "the freed slot 0 must be reused for the new entry, got %d", slot17);
    CHECK(r.count == SMS_AUDIT_MAX, "the ring must stay at its cap after overflow");
    CHECK(r.lost == 1, "one overflow must increment sms_lost exactly once, got %u", r.lost);
    CHECK(sms_audit_peek_oldest(&r, &out) && strcmp(out.peer, "+12065550001") == 0,
          "after overflow, the new oldest must be what was entry 1 (entry 0 was dropped)");

    // Pop-oldest ordering: draining must reproduce FIFO order (1..15, then
    // the 17th entry appended last), and pop must report the slot removed.
    for (int i = 1; i < SMS_AUDIT_MAX; i++) {
        CHECK(sms_audit_peek_oldest(&r, &out), "must still have entries to pop, i=%d", i);
        char want[SMS_PEER_MAX];
        snprintf(want, sizeof(want), "+1206555%04d", i);
        CHECK(strcmp(out.peer, want) == 0, "FIFO order violated at i=%d: got %s want %s", i, out.peer,
              want);
        int popped = sms_audit_pop_oldest(&r);
        CHECK(popped == i, "pop_oldest must report the slot it removed (%d), got %d", i, popped);
    }
    CHECK(sms_audit_peek_oldest(&r, &out) && strcmp(out.peer, "+19995551111") == 0,
          "the overflow-time entry must be the last one left");
    int last_popped = sms_audit_pop_oldest(&r);
    CHECK(last_popped == 0, "the last entry (reused slot 0) must report slot 0, got %d", last_popped);
    CHECK(!sms_audit_peek_oldest(&r, &out), "the ring must be empty after draining every entry");
    CHECK(sms_audit_pop_oldest(&r) == -1, "popping an empty ring must return -1");
    CHECK(r.lost == 1, "sms_lost must persist (not reset) across draining");
}

/* ---------------------------------------------------------------------
 * sms_log CBOR encoding, byte-compared against relay/app/wirecbor.py.
 *
 * Expected bytes generated with:
 *   cd /Users/albert/src/pager && relay/.venv/bin/python - <<'EOF'
 *   import sys; sys.path.insert(0, "relay")
 *   from app import wirecbor
 *   sent = {"v":1,"id":"s_1a2b3c4d","ts":1757700000,"kind":"sms_log",
 *           "peer":"+12065550100","dir":"out","st":"sent","body":"On my way",
 *           "sms_ts":1757700000}
 *   print(wirecbor.encode(sent).hex())
 *   sent["n"] = 12
 *   fields = wirecbor.translate_to_int(sent)
 *   import cbor2
 *   body = cbor2.dumps(fields)
 *   count, header_len = wirecbor.parse_map_header(body)
 *   # devauth.py's own sign_cbor(): header bumped to count+1 (room for the
 *   # not-yet-appended `sig` pair), body bytes otherwise untouched.
 *   print((wirecbor.map_header(count + 1) + body[header_len:]).hex())
 *   blocked = {"v":1,"id":"s_deadbeef","ts":1757700100,"kind":"sms_log",
 *              "peer":"+19995551234","dir":"in","st":"blocked","body":"",
 *              "sms_ts":1757700050}
 *   print(wirecbor.encode(blocked).hex())
 *   EOF
 * (run 2026-09-20; both wirecbor.py and cbor.c encode CBOR maps/strings the
 * same way — definite-length headers, minimal-length integers — so this
 * only needs to be regenerated if either encoder's wire format changes, not
 * on every edit. The Python dict field order above matches
 * sms_build_log_cbor()'s own fixed field order exactly, per its own doc
 * comment in sms.h, since cbor2.dumps() preserves Python dict insertion
 * order.)
 * --------------------------------------------------------------------- */
static void hex_decode(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t n = strlen(hex) / 2;
    if (n > out_cap) {
        n = out_cap;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t) v;
    }
    *out_len = n;
}

static void test_sms_log_cbor_vs_relay(void)
{
    static const char *SENT_UNSIGNED_HEX =
        "a90001016a735f3161326233633464021a68c45fa00667736d735f6c6f67182c6c2b3132303635353530313030"
        "182d636f7574182e6473656e7404694f6e206d7920776179182f1a68c45fa0";
    static const char *SENT_SIGNED_NOSIG_HEX =
        "ab0001016a735f3161326233633464021a68c45fa00667736d735f6c6f67182c6c2b3132303635353530313030"
        "182d636f7574182e6473656e7404694f6e206d7920776179182f1a68c45fa00c0c";
    static const char *BLOCKED_UNSIGNED_HEX =
        "a90001016a735f6465616462656566021a68c460040667736d735f6c6f67182c6c2b31393939353535313233"
        "34182d62696e182e67626c6f636b65640460182f1a68c45fd2";

    uint8_t want[160];
    size_t want_len;
    uint8_t got[192];
    size_t got_len;

    bool ok = sms_build_log_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "s_1a2b3c4d",
                                 1757700000, "+12065550100", "out", "sent", "On my way", 9,
                                 1757700000);
    CHECK(ok, "sms_build_log_cbor() must succeed for the unsigned 'sent' vector");
    hex_decode(SENT_UNSIGNED_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "unsigned 'sent' vector must byte-match relay/app/wirecbor.py exactly (got %zu want %zu bytes)",
          got_len, want_len);

    ok = sms_build_log_cbor(got, sizeof(got), &got_len, /*signed_env=*/true, 12, "s_1a2b3c4d",
                            1757700000, "+12065550100", "out", "sent", "On my way", 9, 1757700000);
    CHECK(ok, "sms_build_log_cbor() must succeed for the signed (minus sig) 'sent' vector");
    hex_decode(SENT_SIGNED_NOSIG_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "signed-minus-sig 'sent' vector must byte-match relay/app/wirecbor.py exactly");

    // The empty-body case (§3.6: "empty body allowed", an audit record of a
    // blocked receive).
    ok = sms_build_log_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "s_deadbeef",
                            1757700100, "+19995551234", "in", "blocked", "", 0, 1757700050);
    CHECK(ok, "sms_build_log_cbor() must succeed for the empty-body 'blocked' vector");
    hex_decode(BLOCKED_UNSIGNED_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "empty-body 'blocked' vector must byte-match relay/app/wirecbor.py exactly");
}

int main(void)
{
    test_name_phone_valid();
    test_numbers_match();
    test_cfg_sms_decode();
    test_gsm7_vs_ucs2_basic();
    test_gsm7_narrow_fallback_mode();
    test_gsm7_160_septet_boundary();
    test_ucs2_70_unit_boundary_and_emoji();
    test_classify_dcs();
    test_parse_cmgr_header();
    test_decode_received();
    test_ucs2_roundtrip();
    test_audit_ring();
    test_sms_log_cbor_vs_relay();

    if (g_failures == 0) {
        printf("PASS: sms, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

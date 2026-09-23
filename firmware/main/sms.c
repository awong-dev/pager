// sms.c — see sms.h for the module split, NVS namespaces, and every
// function's doc comment.
//
// All power-effect comments are PENDING_HW/UNVERIFIED (no device attached
// while writing this — see this task's own report for exactly which log
// lines to look for on real hardware, smstest/smslist included).

#include "sms.h"

#include <stdlib.h>
#include <string.h>

#include "cbor.h"

// ---------------------------------------------------------------------------
// Pure functions (no ESP-IDF dependency) — host-tested by
// firmware/host/test_sms.c.
// ---------------------------------------------------------------------------

bool sms_name_valid(const char *name, size_t len)
{
    if (!name || len == 0 || len >= SMS_CONTACT_NAME_MAX_UTF8) {
        return false;
    }
    size_t codepoints = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) name[i];
        if (c < 0x20 || c == 0x7F) {
            return false; // control character
        }
        if ((c & 0xC0) != 0x80) {
            codepoints++; // count lead/single bytes only, not UTF-8 continuations
        }
    }
    return codepoints <= SMS_CONTACT_NAME_MAX_CODEPOINTS;
}

bool sms_phone_valid(const char *phone, size_t len)
{
    // `^\+[1-9]\d{6,14}$`: '+' + one leading [1-9] digit + 6..14 more digits
    // -> total length 8..16.
    if (!phone || len < 8 || len > 16 || phone[0] != '+') {
        return false;
    }
    if (phone[1] < '1' || phone[1] > '9') {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        if (phone[i] < '0' || phone[i] > '9') {
            return false;
        }
    }
    size_t digits_after_first = len - 2;
    return digits_after_first >= 6 && digits_after_first <= 14;
}

// PROTOCOL.md §10 `cfg.sms[]` item sub-map keys: n=0 (name), p=1 (phone).
#define SMSCK_N 0
#define SMSCK_P 1

bool sms_parse_cfg_submap(const uint8_t *buf, uint16_t len, sms_contact_list_t *out)
{
    // Deliberately does NOT touch `out` until the very end (a successful
    // `*out = tmp;`) — V02_DESIGN.md §6: "reject the whole list if any
    // entry is bad (do not partially apply)" extends to never even
    // clobbering the caller's existing list on a rejected push.
    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_array(&r, &count)) {
        return false;
    }
    if (count > SMS_MAX_CONTACTS) {
        return false; // over the cap: reject the whole list, never truncate
    }

    // Built into a scratch copy so a bad entry midway through never
    // partially applies (V02_DESIGN.md §6: "reject the whole list if any
    // entry is bad").
    sms_contact_list_t tmp;
    memset(&tmp, 0, sizeof(tmp));

    for (uint32_t i = 0; i < count; i++) {
        uint32_t mcount;
        if (!cbor_r_map(&r, &mcount)) {
            return false;
        }
        char name[SMS_CONTACT_NAME_MAX_UTF8] = "";
        char phone[SMS_PHONE_MAX] = "";
        bool have_name = false, have_phone = false;

        for (uint32_t k = 0; k < mcount; k++) {
            uint32_t key;
            if (!cbor_r_key(&r, &key)) {
                return false;
            }
            switch (key) {
            case SMSCK_N: {
                const char *s;
                size_t slen;
                if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(name)) {
                    return false;
                }
                memcpy(name, s, slen);
                name[slen] = '\0';
                have_name = true;
                break;
            }
            case SMSCK_P: {
                const char *s;
                size_t slen;
                if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(phone)) {
                    return false;
                }
                memcpy(phone, s, slen);
                phone[slen] = '\0';
                have_phone = true;
                break;
            }
            default:
                if (!cbor_r_skip(&r)) {
                    return false;
                }
                break;
            }
        }

        if (!have_name || !have_phone || !sms_name_valid(name, strlen(name)) ||
            !sms_phone_valid(phone, strlen(phone))) {
            return false;
        }
        for (uint8_t j = 0; j < tmp.count; j++) {
            if (strcmp(tmp.contacts[j].phone, phone) == 0) {
                return false; // duplicate phone within this same push
            }
        }
        strncpy(tmp.contacts[tmp.count].name, name, sizeof(tmp.contacts[0].name) - 1);
        strncpy(tmp.contacts[tmp.count].phone, phone, sizeof(tmp.contacts[0].phone) - 1);
        tmp.count++;
    }

    *out = tmp;
    return true;
}

static void extract_digits(const char *s, char *out, size_t out_cap)
{
    size_t n = 0;
    if (s) {
        for (; *s != '\0' && n + 1 < out_cap; s++) {
            if (*s >= '0' && *s <= '9') {
                out[n++] = *s;
            }
        }
    }
    out[n] = '\0';
}

bool sms_numbers_match(const char *allow_e164, const char *sender)
{
    if (!allow_e164 || !sender || allow_e164[0] == '\0' || sender[0] == '\0') {
        return false;
    }
    char a[24], b[24];
    extract_digits(allow_e164, a, sizeof(a));
    extract_digits(sender, b, sizeof(b));
    if (a[0] == '\0' || b[0] == '\0') {
        return false;
    }
    if (strcmp(a, b) == 0) {
        return true;
    }
    size_t la = strlen(a), lb = strlen(b);
    if (la >= 10 && lb >= 10 && strcmp(a + la - 10, b + lb - 10) == 0) {
        return true; // last-10-digit fallback — see this function's own doc comment for the risk
    }
    return false;
}

int sms_find_contact_by_number(const sms_contact_list_t *list, const char *sender, sms_contact_t *out)
{
    if (!list) {
        return -1;
    }
    for (uint8_t i = 0; i < list->count; i++) {
        if (sms_numbers_match(list->contacts[i].phone, sender)) {
            if (out) {
                *out = list->contacts[i];
            }
            return (int) i;
        }
    }
    return -1;
}

int sms_find_contact_by_name(const sms_contact_list_t *list, const char *word, size_t word_len,
                             sms_contact_t *out)
{
    if (!list || !word || word_len == 0) {
        return -1;
    }
    for (uint8_t i = 0; i < list->count; i++) {
        size_t nlen = strlen(list->contacts[i].name);
        if (nlen == word_len && strncmp(list->contacts[i].name, word, word_len) == 0) {
            if (out) {
                *out = list->contacts[i];
            }
            return (int) i;
        }
    }
    return -1;
}

// ---------------------------------------------------------------------------
// UTF-8 decode (shared by sms_measure()/sms_encode_ucs2_hex()) and GSM 03.38
// basic-alphabet classification.
// ---------------------------------------------------------------------------

// Decodes one code point starting at `s` (up to `remaining` bytes available).
// Validates continuation-byte shape (stricter than msg.c's own composer
// decoder, which never needed to reject malformed input, only buffer it).
static bool utf8_decode_one(const char *s, size_t remaining, uint32_t *cp, size_t *consumed)
{
    if (remaining == 0) {
        return false;
    }
    unsigned char c0 = (unsigned char) s[0];
    if (c0 < 0x80) {
        *cp = c0;
        *consumed = 1;
        return true;
    }
    size_t need;
    uint32_t val;
    if ((c0 & 0xE0) == 0xC0) {
        need = 1;
        val = c0 & 0x1F;
    } else if ((c0 & 0xF0) == 0xE0) {
        need = 2;
        val = c0 & 0x0F;
    } else if ((c0 & 0xF8) == 0xF0) {
        need = 3;
        val = c0 & 0x07;
    } else {
        return false; // invalid lead byte (or a stray continuation byte)
    }
    if (remaining < need + 1) {
        return false;
    }
    for (size_t i = 1; i <= need; i++) {
        unsigned char c = (unsigned char) s[i];
        if ((c & 0xC0) != 0x80) {
            return false;
        }
        val = (val << 6) | (uint32_t) (c & 0x3F);
    }
    *cp = val;
    *consumed = need + 1;
    return true;
}

typedef enum { GSMCP_NOT = 0, GSMCP_BASIC = 1, GSMCP_EXT = 2 } gsmcp_class_t;

// Coordinator fix (2026-09-20): under AT+CSCS="IRA" the eligible 7-bit range
// is "printable ASCII (0x20-0x7E) plus LF/CR, excluding the backtick" — the
// modem itself does IRA<->GSM conversion, including the escape-table
// characters, so nothing here needs to know actual GSM septet VALUES, only
// which ASCII characters have SOME representation in the GSM 03.38 alphabet
// (basic or extension table) — see sms.h's own module comment for the full
// reasoning and why non-ASCII (é, £, Ω, ...) is deliberately excluded even
// though the raw GSM alphabet itself contains those characters: IRA has no
// code point for them at all, so they cannot be typed/sent via the IRA path
// regardless of what the GSM alphabet supports.
static gsmcp_class_t classify_7bit_ira(uint32_t cp)
{
    if (cp == 0x0A || cp == 0x0D) {
        return GSMCP_BASIC; // LF, CR
    }
    if (cp < 0x20 || cp > 0x7E) {
        return GSMCP_NOT; // outside printable ASCII entirely
    }
    if (cp == 0x60) {
        return GSMCP_NOT; // backtick: no GSM 03.38 mapping (basic or extension), UNVERIFIED
                           // exactly how this modem reports/rejects it if sent anyway
    }
    switch (cp) {
    case 0x005B: /* [ */
    case 0x005D: /* ] */
    case 0x007B: /* { */
    case 0x007D: /* } */
    case 0x005C: /* \ */
    case 0x005E: /* ^ */
    case 0x007E: /* ~ */
    case 0x007C: /* | */
        return GSMCP_EXT; // escape table: ESC + one more septet
    default:
        return GSMCP_BASIC;
    }
}

// Coordinator fix (2026-09-20): the AT+CSCS="GSM" fallback (only used when
// "IRA" was rejected at init) narrows the eligible range further, to ASCII
// characters whose GSM 03.38 septet index is NUMERICALLY IDENTICAL to their
// ASCII code — checked against the actual 03.38 table (sms.h's own module
// comment lists exactly which ranges these are), not guessed: under a raw
// "GSM" TE charset this layer must hand the modem the correct septet value
// directly as a byte, and it has no IRA->GSM conversion table of its own to
// fall back on (implementing one would defeat the point of using
// AT+CSCS="IRA" as the primary path at all). Every escape-table character
// AND '_' (GSM septet 0x11, not ASCII 0x5F) are therefore NOT eligible here
// — sms_decide_encoding() sends them via UCS-2 instead when this mode is
// active.
static gsmcp_class_t classify_7bit_gsm_narrow(uint32_t cp)
{
    if (cp == 0x0A || cp == 0x0D) {
        return GSMCP_BASIC;
    }
    if ((cp >= 0x20 && cp <= 0x23) || (cp >= 0x25 && cp <= 0x3F) || (cp >= 0x41 && cp <= 0x5A) ||
        (cp >= 0x61 && cp <= 0x7A)) {
        return GSMCP_BASIC;
    }
    return GSMCP_NOT; // includes '$'(0x24, GSM 0x02), '_'(0x5F, GSM 0x11), the whole
                       // escape table, '@', and every non-ASCII character
}

void sms_measure(const char *utf8, size_t len, sms_charset_mode_t mode, sms_measure_t *out)
{
    memset(out, 0, sizeof(*out));
    out->valid_utf8 = true;
    out->all_gsm7 = true;

    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t consumed;
        if (!utf8_decode_one(utf8 + i, len - i, &cp, &consumed)) {
            out->valid_utf8 = false;
            return;
        }
        i += consumed;
        out->codepoints++;

        gsmcp_class_t cls = (mode == SMS_CHARSET_IRA) ? classify_7bit_ira(cp)
                                                       : classify_7bit_gsm_narrow(cp);
        if (cls == GSMCP_NOT) {
            out->all_gsm7 = false;
        } else {
            out->gsm7_septets += (cls == GSMCP_EXT) ? 2 : 1;
        }
        out->ucs2_units += (cp > 0xFFFF) ? 2 : 1; // outside the BMP -> a surrogate pair
    }
}

sms_encoding_t sms_decide_encoding(const sms_measure_t *m)
{
    if (!m->valid_utf8) {
        return SMS_ENC_TOO_LONG; // fail closed: never guess an encoding for unmeasurable input
    }
    if (m->all_gsm7 && m->gsm7_septets <= SMS_GSM7_MAX_SEPTETS) {
        return SMS_ENC_GSM7;
    }
    if (m->ucs2_units <= SMS_UCS2_MAX_UNITS) {
        return SMS_ENC_UCS2;
    }
    return SMS_ENC_TOO_LONG;
}

bool sms_encode_gsm7(const char *utf8, size_t len, char *out, size_t cap, size_t *out_len)
{
    if (len + 1 > cap) {
        return false;
    }
    memcpy(out, utf8, len);
    out[len] = '\0';
    *out_len = len;
    return true;
}

static const char SMS_HEXD[] = "0123456789abcdef";

bool sms_encode_ucs2_hex(const char *utf8, size_t len, char *out, size_t cap, size_t *out_len)
{
    size_t pos = 0;
    size_t i = 0;
    while (i < len) {
        uint32_t cp;
        size_t consumed;
        if (!utf8_decode_one(utf8 + i, len - i, &cp, &consumed)) {
            return false;
        }
        i += consumed;

        uint16_t units[2];
        int n_units;
        if (cp > 0xFFFF) {
            uint32_t v = cp - 0x10000;
            units[0] = (uint16_t) (0xD800 + (v >> 10));
            units[1] = (uint16_t) (0xDC00 + (v & 0x3FF));
            n_units = 2;
        } else {
            units[0] = (uint16_t) cp;
            n_units = 1;
        }
        for (int u = 0; u < n_units; u++) {
            if (pos + 4 >= cap) {
                return false;
            }
            uint16_t v = units[u];
            out[pos++] = SMS_HEXD[(v >> 12) & 0xF];
            out[pos++] = SMS_HEXD[(v >> 8) & 0xF];
            out[pos++] = SMS_HEXD[(v >> 4) & 0xF];
            out[pos++] = SMS_HEXD[v & 0xF];
        }
    }
    out[pos] = '\0';
    *out_len = pos;
    return true;
}

bool sms_looks_like_ucs2_hex(const char *body, size_t len)
{
    if (!body || len < 4 || (len % 2) != 0) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = body[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        if (!hex) {
            return false;
        }
    }
    return true;
}

static int sms_hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool sms_read_hex_unit(const char *hex, uint16_t *out)
{
    uint16_t v = 0;
    for (int k = 0; k < 4; k++) {
        int hv = sms_hex_val(hex[k]);
        if (hv < 0) {
            return false;
        }
        v = (uint16_t) ((v << 4) | (uint16_t) hv);
    }
    *out = v;
    return true;
}

bool sms_decode_ucs2_hex(const char *hex, size_t len, char *out, size_t cap, size_t *out_len)
{
    if (!hex || len == 0 || (len % 4) != 0) {
        return false;
    }
    size_t n_units = len / 4;
    size_t pos = 0;

    for (size_t u = 0; u < n_units; u++) {
        uint16_t v;
        if (!sms_read_hex_unit(hex + u * 4, &v)) {
            return false;
        }
        uint32_t cp;
        if (v >= 0xD800 && v <= 0xDBFF) {
            if (u + 1 >= n_units) {
                return false; // unpaired high surrogate
            }
            uint16_t v2;
            if (!sms_read_hex_unit(hex + (u + 1) * 4, &v2) || v2 < 0xDC00 || v2 > 0xDFFF) {
                return false;
            }
            cp = 0x10000 + ((uint32_t) (v - 0xD800) << 10) + (uint32_t) (v2 - 0xDC00);
            u++; // consumed the low surrogate too
        } else if (v >= 0xDC00 && v <= 0xDFFF) {
            return false; // unpaired low surrogate
        } else {
            cp = v;
        }

        size_t need = (cp < 0x80) ? 1 : (cp < 0x800) ? 2 : (cp < 0x10000) ? 3 : 4;
        if (pos + need >= cap) {
            return false;
        }
        if (need == 1) {
            out[pos++] = (char) cp;
        } else if (need == 2) {
            out[pos++] = (char) (0xC0 | (cp >> 6));
            out[pos++] = (char) (0x80 | (cp & 0x3F));
        } else if (need == 3) {
            out[pos++] = (char) (0xE0 | (cp >> 12));
            out[pos++] = (char) (0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char) (0x80 | (cp & 0x3F));
        } else {
            out[pos++] = (char) (0xF0 | (cp >> 18));
            out[pos++] = (char) (0x80 | ((cp >> 12) & 0x3F));
            out[pos++] = (char) (0x80 | ((cp >> 6) & 0x3F));
            out[pos++] = (char) (0x80 | (cp & 0x3F));
        }
    }
    out[pos] = '\0';
    *out_len = pos;
    return true;
}

// ---------------------------------------------------------------------------
// Coordinator fix #2 (2026-09-20): DCS-based receive decode, replacing the
// old "always guess from the body's shape" approach — sms_looks_like_ucs2_hex()
// (above) is now only the fallback for when <dcs> is unavailable.
// ---------------------------------------------------------------------------

sms_dcs_class_t sms_classify_dcs(int dcs)
{
    if (dcs < 0) {
        return SMS_DCS_UNKNOWN;
    }
    // TS 23.038 §4: only the "general data coding" group (top two bits 00)
    // is interpreted here; every other group (message waiting indication,
    // data coding/message class variants, etc.) is rare for a received text
    // SMS and falls back to the heuristic exactly like an absent <dcs>.
    if ((dcs & 0xC0) != 0x00) {
        return SMS_DCS_UNKNOWN;
    }
    switch (dcs & 0x0C) {
    case 0x00:
        return SMS_DCS_7BIT;
    case 0x04:
        return SMS_DCS_8BIT;
    case 0x08:
        return SMS_DCS_UCS2;
    default:
        return SMS_DCS_UNKNOWN; // 0x0C: reserved alphabet value
    }
}

// Mirrors WalterModem.cpp's own `_smsSplitCmgrFields()` (PATCHES.md "Patch
// 1.4") byte-for-byte — see sms.h's own doc comment on sms_parse_cmgr_header()
// for why this duplication exists (the vendor C++ file cannot be compiled
// on the host) and the "kept in sync by hand" caveat that comes with it.
static int cmgr_split_fields(const char *s, char out[][32], int out_cap, int max_fields)
{
    int n = 0;
    while (*s != '\0' && n < max_fields) {
        while (*s == ' ') {
            s++;
        }
        int len = 0;
        if (*s == '"') {
            s++;
            while (*s != '"' && *s != '\0' && len < out_cap - 1) {
                out[n][len++] = *s++;
            }
            if (*s == '"') {
                s++;
            }
        } else {
            while (*s != ',' && *s != '\0' && len < out_cap - 1) {
                out[n][len++] = *s++;
            }
        }
        out[n][len] = '\0';
        n++;
        while (*s != ',' && *s != '\0') {
            s++;
        }
        if (*s == ',') {
            s++;
        }
    }
    return n;
}

bool sms_parse_cmgr_header(const char *raw, size_t len, sms_cmgr_header_t *out)
{
    memset(out, 0, sizeof(*out));
    out->dcs = -1;
    if (!raw) {
        return false;
    }
    (void) len; // cmgr_split_fields() is NUL-terminated-string-shaped, matching the vendor
                // patch's own _buffStr()-then-parse convention (the raw AT response line
                // is always NUL-terminated in that layer too).

    char fields[11][32];
    memset(fields, 0, sizeof(fields));
    int n = cmgr_split_fields(raw, fields, 32, 11);
    if (n < 2) {
        return false; // cannot even find <stat>,<oa>
    }
    strncpy(out->sender, fields[1], sizeof(out->sender) - 1);
    if (n >= 4) {
        strncpy(out->timestamp, fields[3], sizeof(out->timestamp) - 1);
    }
    if (n >= 8 && fields[7][0] != '\0') {
        out->dcs = atoi(fields[7]);
    }
    return true;
}

bool sms_decode_received(const char *body, size_t body_len, int dcs, char *out, size_t out_cap,
                         size_t *out_len, bool *out_shown, bool *out_used_fallback)
{
    *out_shown = true;
    *out_used_fallback = false;
    if (out_cap > 0) {
        out[0] = '\0';
    }
    *out_len = 0;

    sms_dcs_class_t cls = sms_classify_dcs(dcs);
    if (cls == SMS_DCS_UNKNOWN) {
        *out_used_fallback = true;
        cls = sms_looks_like_ucs2_hex(body, body_len) ? SMS_DCS_UCS2 : SMS_DCS_7BIT;
    }

    if (cls == SMS_DCS_8BIT) {
        *out_shown = false; // coordinator fix: undisplayable, audited only, never shown
        return true;
    }
    if (cls == SMS_DCS_UCS2) {
        return sms_decode_ucs2_hex(body, body_len, out, out_cap, out_len);
    }
    // SMS_DCS_7BIT: already plain text (AT+CSCS="IRA" makes this
    // unambiguous — see sms.h's own module comment).
    size_t n = body_len;
    if (n > out_cap - 1) {
        n = out_cap - 1;
    }
    if (body && n > 0) {
        memcpy(out, body, n);
    }
    out[n] = '\0';
    *out_len = n;
    return true;
}

// ---------------------------------------------------------------------------
// Audit ring (pure).
// ---------------------------------------------------------------------------

void sms_audit_ring_init(sms_audit_ring_t *r) { memset(r, 0, sizeof(*r)); }

int sms_audit_enqueue(sms_audit_ring_t *r, const char *peer, const char *dir, const char *st,
                      const char *body, uint16_t body_len, int64_t sms_ts, int *evicted_slot)
{
    if (evicted_slot) {
        *evicted_slot = -1;
    }
    if (r->count == SMS_AUDIT_MAX) {
        if (evicted_slot) {
            *evicted_slot = r->head;
        }
        r->head = (uint8_t) ((r->head + 1) % SMS_AUDIT_MAX);
        r->count--;
        r->lost++;
    }
    uint8_t idx = (uint8_t) ((r->head + r->count) % SMS_AUDIT_MAX);
    sms_audit_entry_t *e = &r->entries[idx];
    memset(e, 0, sizeof(*e));
    strncpy(e->peer, peer ? peer : "", sizeof(e->peer) - 1);
    strncpy(e->dir, dir ? dir : "", sizeof(e->dir) - 1);
    strncpy(e->st, st ? st : "", sizeof(e->st) - 1);
    size_t n = body_len;
    if (n > sizeof(e->body) - 1) {
        n = sizeof(e->body) - 1;
    }
    if (body && n > 0) {
        memcpy(e->body, body, n);
    }
    e->body[n] = '\0';
    e->body_len = (uint16_t) n;
    e->sms_ts = sms_ts;
    r->count++;
    return (int) idx;
}

bool sms_audit_peek_oldest(const sms_audit_ring_t *r, sms_audit_entry_t *out)
{
    if (r->count == 0) {
        return false;
    }
    *out = r->entries[r->head];
    return true;
}

int sms_audit_pop_oldest(sms_audit_ring_t *r)
{
    if (r->count == 0) {
        return -1;
    }
    int slot = r->head;
    r->head = (uint8_t) ((r->head + 1) % SMS_AUDIT_MAX);
    r->count--;
    return slot;
}

// ---------------------------------------------------------------------------
// `sms_log` CBOR encoder (pure). PROTOCOL.md §10 keymap subset this file
// writes: v=0,id=1,ts=2,body=4,kind=6,n=12,peer=44,dir=45,st=46,sms_ts=47.
// ---------------------------------------------------------------------------
#define SMSLOGK_V 0
#define SMSLOGK_ID 1
#define SMSLOGK_TS 2
#define SMSLOGK_BODY 4
#define SMSLOGK_KIND 6
#define SMSLOGK_N 12
#define SMSLOGK_PEER 44
#define SMSLOGK_DIR 45
#define SMSLOGK_ST 46
#define SMSLOGK_SMS_TS 47

bool sms_build_log_cbor(uint8_t *out, size_t cap, size_t *out_len, bool signed_env, uint64_t n,
                        const char *id, int64_t ts, const char *peer, const char *dir,
                        const char *st, const char *body, size_t body_len, int64_t sms_ts)
{
    if (!out || !out_len || !id || !peer || !dir || !st || !body) {
        return false;
    }

    uint32_t nfields = 9; // v,id,ts,kind,peer,dir,st,body,sms_ts
    if (signed_env) {
        nfields += 2; // n (written below) + sig (appended by the caller's auth_sign())
    }

    cbor_w_t w;
    cbor_w_init(&w, out, cap);
    cbor_w_map(&w, nfields);
    // Field order matches docs/V02_DESIGN.md §6's own wire-shape listing and
    // relay/tests/test_sms.py's sms_log_payload() JSON field order exactly
    // (see this function's own doc comment in sms.h) — NOT ascending key
    // order, deliberately: `n` is always last among the fields this
    // function writes, matching every other signed envelope's
    // "no re-serialisation" convention.
    cbor_w_uint(&w, SMSLOGK_V, 1);
    cbor_w_tstr(&w, SMSLOGK_ID, id, strlen(id));
    cbor_w_uint(&w, SMSLOGK_TS, (uint64_t) ts);
    cbor_w_tstr(&w, SMSLOGK_KIND, "sms_log", 7);
    cbor_w_tstr(&w, SMSLOGK_PEER, peer, strlen(peer));
    cbor_w_tstr(&w, SMSLOGK_DIR, dir, strlen(dir));
    cbor_w_tstr(&w, SMSLOGK_ST, st, strlen(st));
    cbor_w_tstr(&w, SMSLOGK_BODY, body, body_len);
    cbor_w_uint(&w, SMSLOGK_SMS_TS, (uint64_t) sms_ts);
    if (signed_env) {
        cbor_w_uint(&w, SMSLOGK_N, n);
    }

    if (w.err) {
        return false;
    }
    *out_len = w.len;
    return true;
}

#ifdef ESP_PLATFORM

#include "ident.h"
#include "modes.h"
#include "msg.h"
#include "net.h"

#include <stdio.h>

#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"

static const char *TAG = "sms";

// ---------------------------------------------------------------------------
// Cross-task wiring (sms.h: sms_bind()) — same pattern as catrust_bind()
// plus the EXISTING g_rtc.auth binding book_bind()/loc_bind() also use.
// Defaults are no-ops so an accessor called before sms_bind() (should not
// happen — modes_boot() binds before anything else can run) never
// dereferences NULL, same defensive pattern lock.c/loc.c document for the
// identical real-hardware finding.
// ---------------------------------------------------------------------------

static void sms_rtc_noop(void) {}

static auth_rtc_t *s_auth_rtc = NULL;
static sms_rtc_lock_fn s_lock = sms_rtc_noop;
static sms_rtc_unlock_fn s_unlock = sms_rtc_noop;
static sms_rtc_save_fn s_save = sms_rtc_noop;
static sms_epoch_wrap_fn s_on_epoch_wrap = sms_rtc_noop;

void sms_bind(auth_rtc_t *auth_rtc, sms_rtc_lock_fn lock, sms_rtc_unlock_fn unlock,
             sms_rtc_save_fn save, sms_epoch_wrap_fn on_wrap)
{
    s_auth_rtc = auth_rtc;
    s_lock = lock ? lock : sms_rtc_noop;
    s_unlock = unlock ? unlock : sms_rtc_noop;
    s_save = save ? save : sms_rtc_noop;
    s_on_epoch_wrap = on_wrap ? on_wrap : sms_rtc_noop;
}

// ---------------------------------------------------------------------------
// NVS: namespace "smscts" (allow-list, one blob — small, 337 bytes measured,
// and written only on a `cfg.sms` push, which is rare) and "smsaud" (the
// audit ring). RAM copies guarded by s_lock/s_unlock, flash I/O with the
// lock released (same no-I/O-under-lock discipline msg.c's msgq_* functions
// document).
//
// Coordinator fix (2026-09-20): "smsaud" used to be ONE blob for the whole
// 16-entry ring (5768 bytes measured) rewritten on every single SMS event —
// "too much flash wear and too long a write". Replaced with one small NVS
// entry per ring slot (`e0`..`e15`, each one `sms_audit_entry_t`, ~360
// bytes) plus a tiny `head`/`count`/`lost` metadata record — an enqueue now
// writes at most one ~360-byte entry (two, on the rare eviction-generation
// wake) plus the small metadata record, instead of rewriting all 16 every
// time.
// ---------------------------------------------------------------------------

#define SMSCTS_NS "smscts"
#define SMSAUD_NS "smsaud"

static bool s_available = false; // §0: false whenever net_sms_config() failed this boot
static sms_charset_mode_t s_charset_mode = SMS_CHARSET_IRA; // meaningless until s_available
static sms_contact_list_t s_contacts;
static sms_audit_ring_t s_audit;

static void load_contacts_from_nvs(void)
{
    memset(&s_contacts, 0, sizeof(s_contacts));
    nvs_handle_t h;
    if (nvs_open(SMSCTS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; // never configured yet — empty allow-list stands
    }
    size_t sz = sizeof(s_contacts);
    nvs_get_blob(h, "list", &s_contacts, &sz);
    nvs_close(h);
}

static bool save_contacts_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(SMSCTS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, "list", &s_contacts, sizeof(s_contacts)) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static void audit_slot_key(uint8_t slot, char *out, size_t cap) { snprintf(out, cap, "e%u", (unsigned) slot); }

static bool save_audit_entry_to_nvs(uint8_t slot, const sms_audit_entry_t *e)
{
    nvs_handle_t h;
    if (nvs_open(SMSAUD_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    char key[4];
    audit_slot_key(slot, key, sizeof(key));
    bool ok = nvs_set_blob(h, key, e, sizeof(*e)) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static void erase_audit_entry_from_nvs(uint8_t slot)
{
    nvs_handle_t h;
    if (nvs_open(SMSAUD_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char key[4];
    audit_slot_key(slot, key, sizeof(key));
    nvs_erase_key(h, key); // fine if it was never written
    nvs_commit(h);
    nvs_close(h);
}

static bool save_audit_meta(uint8_t head, uint8_t count, uint32_t lost)
{
    nvs_handle_t h;
    if (nvs_open(SMSAUD_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_u8(h, "head", head) == ESP_OK;
    ok = ok && nvs_set_u8(h, "count", count) == ESP_OK;
    ok = ok && nvs_set_u32(h, "lost", lost) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

static void load_audit_from_nvs(void)
{
    sms_audit_ring_init(&s_audit);
    nvs_handle_t h;
    if (nvs_open(SMSAUD_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t head = 0, count = 0;
    uint32_t lost = 0;
    nvs_get_u8(h, "head", &head);
    nvs_get_u8(h, "count", &count);
    nvs_get_u32(h, "lost", &lost);
    if (count > SMS_AUDIT_MAX) {
        count = SMS_AUDIT_MAX; // defensive: corrupt/foreign NVS content must never overrun the ring
    }
    s_audit.head = head;
    s_audit.count = count;
    s_audit.lost = lost;
    for (uint8_t i = 0; i < count; i++) {
        uint8_t slot = (uint8_t) ((head + i) % SMS_AUDIT_MAX);
        char key[4];
        audit_slot_key(slot, key, sizeof(key));
        size_t sz = sizeof(s_audit.entries[slot]);
        nvs_get_blob(h, key, &s_audit.entries[slot], &sz);
    }
    nvs_close(h);
}

// Pure-then-persist wrapper used by every audit-enqueue call site below:
// takes the cross-task lock only for the (fast, RAM-only) pure
// sms_audit_enqueue() call, then does the (flash I/O) NVS writes with the
// lock released — same discipline msg.c's msgq_* functions document. Persists
// only the ONE new entry's slot (plus the evicted slot's erase, on overflow)
// and the small head/count/lost record, never the whole ring.
static void audit_enqueue_and_persist(const char *peer, const char *dir, const char *st,
                                      const char *body, uint16_t body_len, int64_t sms_ts)
{
    int evicted = -1;
    sms_audit_entry_t entry_copy;
    uint8_t head, count;
    uint32_t lost;

    s_lock();
    int slot = sms_audit_enqueue(&s_audit, peer, dir, st, body, body_len, sms_ts, &evicted);
    entry_copy = s_audit.entries[slot];
    head = s_audit.head;
    count = s_audit.count;
    lost = s_audit.lost;
    s_unlock();

    if (evicted >= 0) {
        erase_audit_entry_from_nvs((uint8_t) evicted);
    }
    save_audit_entry_to_nvs((uint8_t) slot, &entry_copy);
    save_audit_meta(head, count, lost);
}

// ---------------------------------------------------------------------------
// Boot-drain / `+CMTI` / pending-send state machine.
// ---------------------------------------------------------------------------

typedef enum {
    SMS_PH_IDLE = 0,
    SMS_PH_BOOT_DRAIN,
} sms_phase_t;

static sms_phase_t s_phase = SMS_PH_IDLE;

// Bounded scan (V02_DESIGN.md §6: "at boot, drain any messages that arrived
// while the pager was off"). Coordinator fix (smaller item a): the scan's
// upper bound now comes from `AT+CPMS="ME","ME","ME"`'s own SET-command
// response (net_sms_config_result_t.storage_total/storage_used — 3GPP TS
// 27.005 §3.2.2's `+CPMS: <usedr>,<totalr>,...` — no extra AT round trip:
// this is the response to the command smsConfig() already issues at init)
// rather than a hardcoded, arbitrary guess. `SMS_BOOT_DRAIN_SAFETY_CAP` is a
// hard ceiling applied regardless (in case a modem reports an implausible
// total), and the fallback (`storage_total` unavailable — UNVERIFIED whether
// this modem's +CPMS response actually includes it) is that same value.
#define SMS_BOOT_DRAIN_SAFETY_CAP 64
static int s_boot_drain_index = 1;
static int s_boot_drain_max_index = SMS_BOOT_DRAIN_SAFETY_CAP; // narrowed by AT+CPMS's own reply
static int s_boot_drain_expected_used = -1;                    // -1 = unknown; stop by index only
static int s_boot_drain_found = 0;

#define SMS_SEND_QUEUE_MAX 2 // mirrors MSG_PENDING_UP_MAX (msg.h)
typedef struct {
    bool in_use;
    char thread_id[MSG_ID_MAX];
    char phone[SMS_PHONE_MAX];
    char name[SMS_CONTACT_NAME_MAX_UTF8];
    bool use_ucs2;
    char encoded[SMS_UCS2_HEX_MAX + 1];
    size_t encoded_len;
    char raw_body[SMS_BODY_MAX]; // for the audit entry — the original UTF-8 text, not the encoded form
    uint16_t raw_body_len;
} sms_pending_send_t;

static sms_pending_send_t s_send_queue[SMS_SEND_QUEUE_MAX];

void sms_init(void)
{
    load_contacts_from_nvs();
    load_audit_from_nvs();

    // Power effect: net_sms_config() -> WalterModem::smsConfig(), up to six
    // AT round trips (AT+CMGF, AT+CSCS x1-2, AT+CSDH, AT+CSMP, AT+CNMI,
    // AT+CPMS), no RRC of its own beyond that.
    net_sms_config_result_t cfg;
    if (!net_sms_config(&cfg)) {
        // §0: fail open. Logged once here; s_available then reads false for
        // the rest of this boot and every other public entry point in this
        // file degrades to a safe no-op (see each one's own doc comment) —
        // never touches paging.
        ESP_LOGI(TAG, "SMS unavailable this boot (net_sms_config() failed - the production SIM "
                      "may not carry SMS at all) - feature disabled, paging unaffected");
        s_available = false;
        return;
    }
    s_available = true;
    s_charset_mode = cfg.used_ira ? SMS_CHARSET_IRA : SMS_CHARSET_GSM_NARROW;

    s_boot_drain_max_index = SMS_BOOT_DRAIN_SAFETY_CAP;
    if (cfg.storage_total > 0 && cfg.storage_total < SMS_BOOT_DRAIN_SAFETY_CAP) {
        s_boot_drain_max_index = cfg.storage_total;
    }
    s_boot_drain_expected_used = cfg.storage_used >= 0 ? cfg.storage_used : -1;
    s_boot_drain_found = 0;
    s_boot_drain_index = 1;
    s_phase = SMS_PH_BOOT_DRAIN;

    ESP_LOGI(TAG,
             "SMS ready: charset=%s (AT+CSCS %s), storage used/total=%d/%d, %u allow-listed "
             "contact(s), %u queued audit entr%s, sms_lost=%u so far",
             s_charset_mode == SMS_CHARSET_IRA ? "IRA" : "GSM-narrow-fallback",
             cfg.used_ira ? "IRA accepted" : "IRA rejected, fell back to GSM", cfg.storage_used,
             cfg.storage_total, (unsigned) s_contacts.count, (unsigned) s_audit.count,
             s_audit.count == 1 ? "y" : "ies", (unsigned) s_audit.lost);
}

sms_charset_mode_t sms_get_charset_mode(void) { return s_charset_mode; }

uint32_t sms_get_lost_count(void)
{
    s_lock();
    uint32_t v = s_audit.lost;
    s_unlock();
    return v;
}

// Shared by the boot-drain scan and the `+CMTI` path: decode, allow-list
// check, insert-and-alert or audit-blocked, then delete from storage
// (V02_DESIGN.md §6: "read, delete from storage, then..."). Concatenated/
// multipart SMS: each part arrives as its own `+CMTI`/storage index and is
// handled here as its own, independent message — no UDH-based reassembly is
// attempted (V02_DESIGN.md §6: "each part is handled as its own message").
static void process_inbound(int idx, const net_sms_read_t *r)
{
    char decoded[SMS_BODY_MAX] = "";
    size_t decoded_len = 0;
    bool shown = true;
    bool used_fallback = false;
    if (!sms_decode_received(r->body, r->body_len, r->dcs, decoded, sizeof(decoded), &decoded_len,
                             &shown, &used_fallback)) {
        ESP_LOGI(TAG, "sms index %d: body decode failed, treating as empty", idx);
    }
    if (used_fallback) {
        // Coordinator fix #2: <dcs> should be available whenever AT+CSDH=1
        // (smsConfig()'s own init sequence) took effect — this fires only
        // when it did not (older/odd firmware ignoring AT+CSDH), or the
        // header was otherwise too short to carry it.
        ESP_LOGI(TAG, "sms index %d: no usable <dcs> (AT+CSDH=1 ignored by this modem?), used the "
                      "UCS-2-hex heuristic fallback",
                 idx);
    }

    s_lock();
    sms_contact_t match;
    int found = sms_find_contact_by_number(&s_contacts, r->sender, &match);
    s_unlock();

    int64_t sms_ts = 0;
    net_get_clock(&sms_ts); // best-effort; §3.5: ts:0 on failure

    if (!shown) {
        // Coordinator fix #2: 8-bit DCS (binary/undisplayable) — audited
        // only, NEVER shown to the student, regardless of allow-list status.
        ESP_LOGI(TAG, "sms index %d from %s: 8-bit DCS (undisplayable), audited only, never shown",
                 idx, r->sender);
        audit_enqueue_and_persist(r->sender, "in", found >= 0 ? "recv" : "blocked", "", 0, sms_ts);
        net_sms_delete(idx);
        return;
    }

    if (found >= 0) {
        char thread_id[MSG_ID_MAX];
        if (msg_insert_sms_in(match.name, decoded, (uint16_t) decoded_len, thread_id,
                              sizeof(thread_id))) {
            modes_alert_incoming(thread_id, match.name);
        }
        ESP_LOGI(TAG, "sms received from %s (%s): shown to the student", r->sender, match.name);
        audit_enqueue_and_persist(r->sender, "in", "recv", decoded, (uint16_t) decoded_len, sms_ts);
    } else {
        ESP_LOGI(TAG, "sms received from %s: not on the allow-list, blocked (never shown)",
                 r->sender);
        audit_enqueue_and_persist(r->sender, "in", "blocked", decoded, (uint16_t) decoded_len,
                                  sms_ts);
    }

    net_sms_delete(idx); // best-effort; a delete failure just leaves storage one slot fuller
}

void sms_service(void)
{
    if (!s_available) {
        return;
    }

    // Phase 1: boot-drain scan, one index per call (never the whole scan in
    // one call — same discipline loc_service()/catrust_service() document).
    // Stops at whichever comes first: the AT+CPMS-reported total (or the
    // safety cap), or having found as many messages as AT+CPMS reported
    // `used` (coordinator fix, smaller item a).
    if (s_phase == SMS_PH_BOOT_DRAIN) {
        bool done_by_count =
            (s_boot_drain_expected_used >= 0 && s_boot_drain_found >= s_boot_drain_expected_used);
        if (s_boot_drain_index > s_boot_drain_max_index || done_by_count) {
            ESP_LOGI(TAG, "sms boot-drain scan complete (checked indices 1..%d, found %d message(s))",
                     s_boot_drain_index - 1, s_boot_drain_found);
            s_phase = SMS_PH_IDLE;
        } else {
            int idx = s_boot_drain_index++;
            net_sms_read_t r;
            if (net_sms_read(idx, &r) && r.valid) {
                s_boot_drain_found++;
                process_inbound(idx, &r);
            }
        }
        return;
    }

    // Phase 2: a new `+CMTI` URC (net.cpp's own single-flag event handoff).
    net_sms_event_t ev;
    if (net_sms_poll_event(&ev)) {
        net_sms_read_t r;
        if (net_sms_read((int) ev.index, &r) && r.valid) {
            process_inbound((int) ev.index, &r);
        } else {
            // Could not read it cleanly; still try to free the storage slot
            // rather than leaving it to wedge "ME" storage full forever.
            net_sms_delete((int) ev.index);
        }
        return;
    }

    // Phase 3: one pending outbound send, run to completion (a few seconds
    // at most, PENDING_HW/UNVERIFIED — same class of blocking AT work
    // loc.c's own ASSIST_UPDATE phase already tolerates in one call).
    s_lock();
    int slot = -1;
    for (int i = 0; i < SMS_SEND_QUEUE_MAX; i++) {
        if (s_send_queue[i].in_use) {
            slot = i;
            break;
        }
    }
    s_unlock();
    if (slot < 0) {
        return;
    }
    sms_pending_send_t p = s_send_queue[slot]; // small, fixed-size snapshot

    bool ok = net_sms_send(p.phone, p.encoded, p.use_ucs2);
    ESP_LOGI(TAG, "sms send to %s: %s (%s, %u bytes encoded)", p.phone, ok ? "OK" : "FAILED",
             p.use_ucs2 ? "ucs2" : "gsm7", (unsigned) p.encoded_len);

    msg_finish_sms_out(p.thread_id, ok);

    int64_t sms_ts = 0;
    net_get_clock(&sms_ts);
    audit_enqueue_and_persist(p.phone, "out", ok ? "sent" : "failed", p.raw_body, p.raw_body_len,
                              sms_ts);
    s_lock();
    s_send_queue[slot].in_use = false;
    s_unlock();
}

void sms_try_publish_one(void)
{
    s_lock();
    sms_audit_entry_t e;
    bool have = sms_audit_peek_oldest(&s_audit, &e);
    s_unlock();
    if (!have) {
        return;
    }

    char id[SMS_LOG_ID_MAX];
    snprintf(id, sizeof(id), "s_%08x", (unsigned) esp_random());
    int64_t ts = 0;
    net_get_clock(&ts); // best-effort; §3.5: ts:0 on failure, never retry just for the clock

    bool signed_env = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    uint64_t n = 0;
    bool wrapped = false;
    if (signed_env) {
        s_lock();
        if (s_auth_rtc) {
            n = auth_next_up_n(s_auth_rtc, ident_get_n_epoch(), &wrapped);
        }
        s_save();
        s_unlock();
    }

    uint8_t buf[384]; // envelope + 320-byte body worst case, generous
    size_t len;
    if (!sms_build_log_cbor(buf, sizeof(buf), &len, signed_env, n, id, ts, e.peer, e.dir, e.st,
                            e.body, e.body_len, e.sms_ts)) {
        ESP_LOGI(TAG, "sms_log CBOR build failed for %s", id);
        return;
    }

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/up", net_get_device_id());

    if (signed_env) {
        if (!auth_sign(topic, buf, &len, sizeof(buf))) {
            ESP_LOGI(TAG, "sms_log auth_sign() failed for %s", id);
            return;
        }
        if (wrapped) {
            s_on_epoch_wrap();
        }
    }

    // Same "accepted by the modem, not confirmed delivered" simplification
    // msg.c's msg_pump() already documents for net_publish_raw().
    if (net_publish_raw(topic, buf, (uint16_t) len, 1)) {
        ESP_LOGI(TAG, "sms_log %s published (peer=%s dir=%s st=%s)", id, e.peer, e.dir, e.st);
        uint8_t head, count;
        uint32_t lost;
        s_lock();
        int popped = sms_audit_pop_oldest(&s_audit);
        head = s_audit.head;
        count = s_audit.count;
        lost = s_audit.lost;
        s_unlock();
        if (popped >= 0) {
            erase_audit_entry_from_nvs((uint8_t) popped); // this entry's own NVS record only
        }
        save_audit_meta(head, count, lost);
    } else {
        ESP_LOGI(TAG, "sms_log publish failed for %s, will retry next cycle", id);
    }
}

void sms_apply_cfg_submap(const uint8_t *buf, uint16_t len, const char *id)
{
    sms_contact_list_t parsed;
    if (!sms_parse_cfg_submap(buf, len, &parsed)) {
        ESP_LOGI(TAG, "malformed cfg.sms sub-map dropped (id=%s)", id ? id : "");
        return;
    }
    s_lock();
    s_contacts = parsed;
    s_unlock();
    save_contacts_to_nvs(); // flash I/O, lock released
    ESP_LOGI(TAG, "cfg.sms applied: %u contact(s) (id=%s)", (unsigned) parsed.count, id ? id : "");
    if (id && id[0] != '\0') {
        // §6: "acked shown on apply", like cfg.lock — immediate, unconditional.
        msg_mark_shown(id);
    }
}

size_t sms_contact_count(void)
{
    s_lock();
    size_t n = s_available ? s_contacts.count : 0;
    s_unlock();
    return n;
}

bool sms_contact_at(size_t index, sms_contact_t *out)
{
    s_lock();
    bool ok = s_available && index < s_contacts.count;
    if (ok && out) {
        *out = s_contacts.contacts[index];
    }
    s_unlock();
    return ok;
}

int sms_find_by_name(const char *word, size_t word_len, sms_contact_t *out)
{
    if (!s_available) {
        return -1;
    }
    s_lock();
    int idx = sms_find_contact_by_name(&s_contacts, word, word_len, out);
    s_unlock();
    return idx;
}

bool sms_queue_send(const sms_contact_t *to, const char *body, uint16_t body_len)
{
    if (!s_available || !to || !body) {
        return false;
    }
    sms_measure_t m;
    sms_measure(body, body_len, s_charset_mode, &m);
    sms_encoding_t enc = sms_decide_encoding(&m);
    if (enc == SMS_ENC_TOO_LONG) {
        return false; // scr_chat.c's own composer limit should have already caught this
    }

    s_lock();
    int slot = -1;
    for (int i = 0; i < SMS_SEND_QUEUE_MAX; i++) {
        if (!s_send_queue[i].in_use) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        s_unlock();
        return false;
    }
    s_send_queue[slot].in_use = true; // reserve immediately, before any further work
    s_unlock();

    sms_pending_send_t *p = &s_send_queue[slot];
    memset(p->encoded, 0, sizeof(p->encoded));
    bool use_ucs2 = (enc == SMS_ENC_UCS2);
    bool enc_ok = use_ucs2 ? sms_encode_ucs2_hex(body, body_len, p->encoded, sizeof(p->encoded),
                                                 &p->encoded_len)
                           : sms_encode_gsm7(body, body_len, p->encoded, sizeof(p->encoded),
                                             &p->encoded_len);
    if (!enc_ok) {
        s_lock();
        p->in_use = false;
        s_unlock();
        return false;
    }
    p->use_ucs2 = use_ucs2;
    strncpy(p->phone, to->phone, sizeof(p->phone) - 1);
    strncpy(p->name, to->name, sizeof(p->name) - 1);
    size_t n = body_len;
    if (n > sizeof(p->raw_body) - 1) {
        n = sizeof(p->raw_body) - 1;
    }
    memcpy(p->raw_body, body, n);
    p->raw_body[n] = '\0';
    p->raw_body_len = (uint16_t) n;

    char id[MSG_ID_MAX];
    if (!msg_insert_sms_out_pending(to->name, body, body_len, id, sizeof(id))) {
        s_lock();
        p->in_use = false;
        s_unlock();
        return false;
    }
    strncpy(p->thread_id, id, sizeof(p->thread_id) - 1);
    p->thread_id[sizeof(p->thread_id) - 1] = '\0';
    return true;
}

bool sms_debug_send(const char *number, const char *text)
{
    if (!s_available) {
        ESP_LOGI(TAG, "smstest: SMS unavailable this boot");
        return false;
    }
    size_t len = strlen(text);
    sms_measure_t m;
    sms_measure(text, len, s_charset_mode, &m);
    sms_encoding_t enc = sms_decide_encoding(&m);
    if (enc == SMS_ENC_TOO_LONG) {
        ESP_LOGI(TAG, "smstest: message too long for one SMS (gsm7_septets=%u ucs2_units=%u)",
                 (unsigned) m.gsm7_septets, (unsigned) m.ucs2_units);
        return false;
    }

    char encoded[SMS_UCS2_HEX_MAX + 1];
    size_t encoded_len = 0;
    bool use_ucs2 = (enc == SMS_ENC_UCS2);
    bool enc_ok = use_ucs2 ? sms_encode_ucs2_hex(text, len, encoded, sizeof(encoded), &encoded_len)
                           : sms_encode_gsm7(text, len, encoded, sizeof(encoded), &encoded_len);
    if (!enc_ok) {
        ESP_LOGI(TAG, "smstest: encoding failed unexpectedly");
        return false;
    }

    bool ok = net_sms_send(number, encoded, use_ucs2);
    ESP_LOGI(TAG, "smstest: send to %s: %s (%s, %u bytes encoded)", number, ok ? "OK" : "FAILED",
             use_ucs2 ? "ucs2" : "gsm7", (unsigned) encoded_len);

    // §6/§7.3: "non-negotiable" in both directions — smstest bypasses the
    // allow-list, not the audit trail.
    int64_t sms_ts = 0;
    net_get_clock(&sms_ts);
    audit_enqueue_and_persist(number, "out", ok ? "sent" : "failed", text, (uint16_t) len, sms_ts);

    return ok;
}

void sms_debug_list(void)
{
    s_lock();
    sms_contact_list_t contacts = s_contacts;
    uint8_t depth = s_audit.count;
    uint32_t lost = s_audit.lost;
    s_unlock();

    ESP_LOGI(TAG, "smslist: available=%d, %u allow-listed contact(s), audit depth=%u, sms_lost=%u",
             (int) s_available, (unsigned) contacts.count, (unsigned) depth, (unsigned) lost);
    for (uint8_t i = 0; i < contacts.count; i++) {
        ESP_LOGI(TAG, "smslist:   [%u] %s <%s>", (unsigned) i, contacts.contacts[i].name,
                 contacts.contacts[i].phone);
    }
}

#endif /* ESP_PLATFORM */

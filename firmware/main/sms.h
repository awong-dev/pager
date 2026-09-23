/* sms.h — device-direct SMS: parent-managed allow-list, GSM-7/UCS-2 encoding
 * decision, the audit queue, and the `sms_log` wire encoder (docs/V02_DESIGN.md
 * §6, docs/PROTOCOL.md §3.6/§10, docs/DEVICE_PLAN.md §4.4).
 *
 * Split the usual way (lock.c/loc.c/catrust.c/cfg.c): everything above the
 * `#ifdef ESP_PLATFORM` banner is pure C, no ESP-IDF dependency, host-tested
 * by firmware/host/test_sms.c: allow-list phone/name validation and the
 * `cfg.sms` sub-map decode, E.164 number matching (E.164-exact, then
 * last-10-digit fallback — see sms_numbers_match()'s own doc comment for the
 * risk this accepts), the GSM 03.38 basic-alphabet/UCS-2 encoding decision
 * and the two encoders, the UCS-2-hex receive heuristic and decoder, the
 * audit ring (insert/overflow/oldest), and the `sms_log` CBOR encoder
 * (byte-order matching the exact field order the relay's own encoder and
 * `relay/tests/test_sms.py` expect — see sms_build_log_cbor()'s own doc
 * comment).
 *
 * Below the banner: NVS-backed allow-list + audit-ring storage, the
 * `+CMTI`/boot-drain/pending-send state machine driven one bounded step per
 * call from modes_run()'s own loop (sms_service(), same "never the whole
 * thing in one call" discipline loc.c/catrust.c already establish), the
 * `cfg.sms` intercept (cfg.c's cfg_ingest_cbor()), and the `smstest`/
 * `smslist` debug hooks. All modem access goes through net.h's small facade
 * (net_sms_*()) — this file never touches WalterModem directly, same rule
 * every other main module follows.
 *
 * §0 is the law here above all: an SMS initialisation failure (a data-only
 * production SIM may not carry SMS at all) is logged once at INFO and
 * disables the feature for this boot — never touches paging, never retries
 * in a loop. Every AT-level assumption this file's device section relies on
 * (via net.h/the vendor patch) is UNVERIFIED on real hardware; see
 * PATCHES.md's own "Patch 1.4" entry and this file's inline comments for
 * exactly what to look for in the log.
 */
#ifndef SMS_H
#define SMS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing constants (docs/V02_DESIGN.md §6/§7, docs/PROTOCOL.md §10).
 * --------------------------------------------------------------------- */

#define SMS_MAX_CONTACTS 8
#define SMS_CONTACT_NAME_MAX_CODEPOINTS 16
#define SMS_CONTACT_NAME_MAX_UTF8 25 /* 24 UTF-8 bytes + NUL */
#define SMS_PHONE_MAX 17             /* "+" + up to 15 digits (E.164) + NUL */

#define SMS_GSM7_MAX_SEPTETS 160 /* V02_DESIGN.md §6: GSM 7-bit cap */
#define SMS_UCS2_MAX_UNITS 70    /* V02_DESIGN.md §6: UCS-2 cap, UTF-16 code units */
#define SMS_UCS2_HEX_MAX (SMS_UCS2_MAX_UNITS * 4) /* 4 hex chars per 16-bit unit */

#define SMS_AUDIT_MAX 16   /* PROTOCOL.md §3.6: "at least 16 entries" */
#define SMS_BODY_MAX 321   /* 320 UTF-8 bytes + NUL, same convention as MSG_RAM_BODY_MAX */
#define SMS_LOG_ID_MAX 11  /* "s_" + 8 hex + NUL, PROTOCOL.md §1/§3.6 */
#define SMS_PEER_MAX SMS_PHONE_MAX

/* S2 (docs/DEVICE_NEXT_TASKS.md, multipart/UDH reassembly): concurrent
 * in-flight concatenated messages this device tracks at once. 2, not more —
 * a normal owner/parent conversation essentially never has two different
 * multipart texts arriving interleaved from two different senders at the
 * same time, and every slot costs real RAM (SMS_REASM_MAX_PARTS *
 * SMS_BODY_MAX bytes each, see sms_reasm_slot_t below). */
#define SMS_REASM_SLOTS 2
/* Generous cap on parts-per-message: real GSM-7/UCS-2 concatenated SMS this
 * device could ever reassemble already tops out around 2-3 parts before the
 * combined body would exceed SMS_BODY_MAX (320 bytes) anyway (§6's own
 * single-SMS caps: 153 GSM-7 septets or 67 UCS-2 units per part after the
 * UDH). A `total` above this is rejected outright (SMS_REASM_DROPPED) as
 * something this device cannot represent, not a crash risk. */
#define SMS_REASM_MAX_PARTS 4
/* 120 s from the first part (S2's own instruction): long enough that a
 * normal two-part text — sent back-to-back by every carrier this project has
 * seen — always lands before its slot would expire, short enough that a
 * stalled/lost middle part never blocks the next unrelated concatenated
 * message from using this ring for more than two minutes. */
#define SMS_REASM_WINDOW_US (120LL * 1000000LL)

/* ---------------------------------------------------------------------
 * Allow-list (pure section) — no ESP-IDF dependency.
 * --------------------------------------------------------------------- */

typedef struct {
    char name[SMS_CONTACT_NAME_MAX_UTF8];
    char phone[SMS_PHONE_MAX]; /* E.164, as pushed */
} sms_contact_t;

typedef struct {
    uint8_t count;
    sms_contact_t contacts[SMS_MAX_CONTACTS];
} sms_contact_list_t;

/* <=16 code points AND <=24 UTF-8 bytes, no control characters — tighter
 * than book.h's book_name_valid() (48 UTF-8 bytes/16 code points):
 * V02_DESIGN.md §6's own byte-budget note (8 maximal entries must always fit
 * a 640-byte cfg.sms envelope in both encodings). Empty is invalid (unlike
 * an SMS body, a contact must have a name). */
bool sms_name_valid(const char *name, size_t len);

/* E.164: `^\+[1-9]\d{6,14}$` (PROTOCOL.md §3.6's own regex for `peer`, reused
 * here for the allow-list's own phone field — same shape). */
bool sms_phone_valid(const char *phone, size_t len);

/* Decodes `buf`/`len` (the raw CBOR bytes of the `cfg.sms` sub-map value —
 * cfg.h's cfg_dispatch_t.sms_off/sms_len span, an array of `{n=0 tstr, p=1
 * tstr}` maps) into `out`. Rejects (returns false, `out` untouched) the
 * WHOLE list if ANY entry is malformed (bad phone, over-long/empty name),
 * if there are more than SMS_MAX_CONTACTS entries, or if any two entries
 * share the same phone number — V02_DESIGN.md §6: "validate on the device
 * too and reject the whole list if any entry is bad (do not partially
 * apply)". An empty array is valid (`out->count == 0`, clears the
 * allow-list). Name comparison for the duplicate-phone check is irrelevant
 * (only phone uniqueness is required, matching the relay's own PUT
 * validation, `devices_store`/`app/routers/devices.py`). */
bool sms_parse_cfg_submap(const uint8_t *buf, uint16_t len, sms_contact_list_t *out);

/* E.164-exact match first (after stripping everything but digits from both
 * sides, so "+12065550100" vs "12065550100" — no leading "+" — still
 * matches); failing that, a last-10-digit match (V02_DESIGN.md §6: "a
 * sender may arrive as +12065550100 or national format"). RISK, documented
 * per this task's own instruction: the last-10-digit fallback is US/NANP-
 * centric (assumes a 10-digit national significant number) and could
 * false-match two genuinely different international numbers that happen to
 * share their last 10 digits, or fail to match a legitimate national-format
 * sender from a country whose national number is not 10 digits long. Chosen
 * anyway because the alternative — requiring the sender's caller ID to
 * exactly match the E.164 form the parent typed into the web app — was
 * observed (V02_DESIGN.md §6's own text) to fail in practice for real
 * carriers that deliver caller ID in national format. Neither side may be
 * empty. */
bool sms_numbers_match(const char *allow_e164, const char *sender);

/* Looks up `sender` in `list` by sms_numbers_match(); returns the matching
 * contact's index (0-based) or -1 if none match. `out` (if non-NULL) is
 * filled with a copy of the matched contact. */
int sms_find_contact_by_number(const sms_contact_list_t *list, const char *sender, sms_contact_t *out);

/* Case-sensitive exact match of `word`/`word_len` against each contact's
 * `name` (docs/DEVICE_PLAN.md §5.5/V02_DESIGN.md §6: "@name" resolution in
 * the composer) — same exact-match convention book.c's own alias/nickname
 * resolution uses. Returns the matching index or -1. */
int sms_find_contact_by_name(const sms_contact_list_t *list, const char *word, size_t word_len,
                             sms_contact_t *out);

/* ---------------------------------------------------------------------
 * GSM 7-bit / UCS-2 encoding decision (pure). UTF-8 in, no ESP-IDF/mbedTLS
 * dependency (a hand-rolled UTF-8 decoder, same style msg.c's composer
 * uses).
 *
 * Coordinator fix (2026-09-20): `AT+CSCS="GSM"` makes the TE character set
 * literally BE the GSM 03.38 alphabet — the bytes handed to `AT+CMGS` must
 * already be GSM septet values, not ASCII/UTF-8, so sending raw UTF-8 bytes
 * mangled every character whose GSM code differs from its ASCII code (`@`
 * GSM 0x00, `$` GSM 0x02, `_` GSM 0x11, the whole `[ ] { } \ ^ ~ |` escape
 * table, plus every accented Latin/Greek letter). `@name` addressing alone
 * makes `@` a common character in a real message. The fix: prefer
 * `AT+CSCS="IRA"` (net_sms_config()/smsConfig()), under which the TE
 * character set is plain ASCII (IRA) and the MODEM does the IRA<->GSM
 * conversion itself — the eligible 7-bit range then becomes "printable
 * ASCII (0x20-0x7E) plus LF/CR, excluding the backtick" (backtick has no
 * GSM 03.38 mapping at all, neither basic nor extension table). The
 * extension-table subset (`[ ] { } \ ^ ~ |`) still costs 2 septets (ESC +
 * one more) even though they are ordinary ASCII bytes under IRA. If
 * `AT+CSCS="IRA"` is rejected at init, `smsConfig()` falls back to
 * `AT+CSCS="GSM"` and this module's `sms_charset_mode_t` narrows the
 * eligible 7-bit range further, to ONLY the ASCII characters whose GSM
 * septet index is numerically identical to their ASCII code (checked
 * against the 03.38 table, not guessed — see classify_7bit_ascii()'s own
 * table in sms.c): LF, CR, space through `#`, `%` through `?`, `A`-`Z`,
 * `a`-`z`. Notably `_` (GSM septet 0x11 vs ASCII 0x5F) and the whole
 * escape table are OUTSIDE that narrowed set — under a raw "GSM" TE
 * charset, sending the ASCII byte for `_` or `[` would not correctly
 * reach the modem's own GSM alphabet slot for that character, and this
 * layer has no way to hand-construct GSM septet byte sequences (that would
 * need re-implementing the modem's own IRA->GSM table, which `AT+CSCS=
 * "IRA"` exists specifically to avoid) — so in fallback mode those
 * characters, and anything non-ASCII, go via UCS-2 instead. Either way,
 * `sms_encode_gsm7()` itself stays a plain byte-for-byte copy: the ONLY
 * thing that changes between the two modes is which characters are
 * ELIGIBLE for the 7-bit path (`sms_measure()`'s own `mode` argument),
 * never how the eligible bytes are encoded.
 * --------------------------------------------------------------------- */

/* Which TE character set `AT+CSCS` is actually resting on this boot
 * (net_sms_config()/smsConfig() picks one at init and never changes it
 * outside a UCS-2 send's own transient toggle) — determines which ASCII
 * characters `sms_measure()` treats as 7-bit-eligible. See this section's
 * own module comment above for the exact two rule sets. */
typedef enum {
    SMS_CHARSET_IRA = 0,        /* AT+CSCS="IRA" succeeded (preferred) */
    SMS_CHARSET_GSM_NARROW = 1, /* AT+CSCS="IRA" was rejected; fell back to "GSM" */
} sms_charset_mode_t;

typedef struct {
    bool valid_utf8;     /* false: malformed UTF-8 in the input; every other field is 0 */
    bool all_gsm7;       /* every code point is eligible for the 7-bit path under the ACTIVE
                           * sms_charset_mode_t this sms_measure() call was given (escape-table
                           * characters counted as 2 septets below, IRA mode only) */
    size_t codepoints;   /* Unicode code point count */
    size_t gsm7_septets; /* total septet count if sent as GSM-7 (only meaningful if all_gsm7) */
    size_t ucs2_units;   /* total UTF-16 code unit count if sent as UCS-2 (a code point outside
                           * the BMP, e.g. most emoji, counts as 2 — a surrogate pair) */
} sms_measure_t;

/* Walks `utf8`/`len` once, filling every field of `*out`, judging 7-bit
 * eligibility against `mode` (see this section's own module comment).
 * Never fails outward (a malformed byte sequence sets `valid_utf8` false
 * and stops there, leaving the other fields at their last-computed value —
 * the caller must check `valid_utf8` before trusting the rest). */
void sms_measure(const char *utf8, size_t len, sms_charset_mode_t mode, sms_measure_t *out);

typedef enum {
    SMS_ENC_GSM7 = 0,     /* all_gsm7 && gsm7_septets <= SMS_GSM7_MAX_SEPTETS */
    SMS_ENC_UCS2 = 1,     /* otherwise, if ucs2_units <= SMS_UCS2_MAX_UNITS */
    SMS_ENC_TOO_LONG = 2, /* fits neither (V02_DESIGN.md §6: "no concatenated SMS") */
} sms_encoding_t;

/* Pure decision from an already-computed sms_measure_t (V02_DESIGN.md §6:
 * "GSM 7-bit if every character is in the basic set ... <=160 septets;
 * otherwise UCS-2 ... <=70 characters"). Malformed UTF-8 (`!m->valid_utf8`)
 * decides SMS_ENC_TOO_LONG (fail closed: never guess an encoding for input
 * that could not even be measured). */
sms_encoding_t sms_decide_encoding(const sms_measure_t *m);

/* Copies `utf8`/`len` verbatim into `out` (the literal bytes handed to
 * net_sms_send() with use_ucs2=false) — correct for BOTH charset modes:
 * under `AT+CSCS="IRA"` the modem itself converts IRA (ASCII) to the GSM
 * alphabet, including the escape-table characters; under the narrowed
 * `AT+CSCS="GSM"` fallback, sms_measure()'s own eligibility rule already
 * restricted the input to characters whose GSM septet equals their ASCII
 * byte, so the raw ASCII byte IS the correct GSM septet value. Returns
 * false (out untouched) if `len` does not fit `cap-1`. */
bool sms_encode_gsm7(const char *utf8, size_t len, char *out, size_t cap, size_t *out_len);

/* Encodes `utf8`/`len` into `out` as a hex string (uppercase-independent —
 * lowercase hex emitted, net.cpp/the modem is expected to accept either)
 * of big-endian UTF-16 code units, surrogate pairs for code points outside
 * the BMP — the exact text net_sms_send() expects when use_ucs2=true.
 * Returns false (out untouched) on malformed UTF-8 or if the encoded form
 * would not fit `cap` (SMS_UCS2_HEX_MAX+1 is always enough for a message
 * sms_decide_encoding() accepted as SMS_ENC_UCS2). */
bool sms_encode_ucs2_hex(const char *utf8, size_t len, char *out, size_t cap, size_t *out_len);

/* Heuristic (V02_DESIGN.md §6, `net.cpp`'s own WalterModemSmsReadData doc
 * comment): true iff `body` looks like a UCS-2 hex payload — length >= 4,
 * even, every character an ASCII hex digit. RISK, documented per this
 * task's own instruction: a legitimate GSM-7 message that happens to be
 * all-numeric-or-hex-letters and of even length (e.g. a bare "112233" or
 * "deadbeef" sent as plain text) is indistinguishable from UCS-2 hex by this
 * rule alone and WILL be mis-decoded as UCS-2 (producing garbage or, worse,
 * plausible-looking-but-wrong text) — accepted because the alternative
 * (always trusting the modem's own reported charset) is not available: this
 * modem's text-mode AT+CMGR response carries no explicit indicator of which
 * representation it used (net.cpp's own module comment). */
bool sms_looks_like_ucs2_hex(const char *body, size_t len);

/* Decodes `hex`/`len` (big-endian UTF-16 code units, surrogate pairs) into
 * UTF-8 `out`. Returns false (out untouched) on an odd length, a non-hex
 * character, an unpaired/malformed surrogate, or overflow of `cap`. */
bool sms_decode_ucs2_hex(const char *hex, size_t len, char *out, size_t cap, size_t *out_len);

/* ---------------------------------------------------------------------
 * Coordinator fix #2 (2026-09-20): decide the received message's encoding
 * from the modem-reported <dcs> (AT+CSDH=1's extra +CMGR fields, 3GPP TS
 * 27.005 §4.4 / TS 23.038 §4) instead of guessing from the body's own
 * shape. sms_looks_like_ucs2_hex() above is now only the FALLBACK for when
 * <dcs> is unavailable (older/odd modem firmware that ignores AT+CSDH=1).
 * --------------------------------------------------------------------- */

typedef enum {
    SMS_DCS_7BIT = 0,   /* general data coding group, alphabet bits 00: GSM 7-bit default */
    SMS_DCS_8BIT = 1,   /* general data coding group, alphabet bits 01: 8-bit data, undisplayable */
    SMS_DCS_UCS2 = 2,   /* general data coding group, alphabet bits 10: UCS-2 */
    SMS_DCS_UNKNOWN = 3, /* dcs < 0 (absent/no CSDH), or a coding group this module does not
                          * interpret (message waiting indication, etc. — rare for a received
                          * text SMS) — the caller falls back to sms_looks_like_ucs2_hex(). */
} sms_dcs_class_t;

/* TS 23.038 §4: only the "general data coding" group (top two bits 00) is
 * interpreted; anything else (or `dcs < 0`) is SMS_DCS_UNKNOWN. Within that
 * group, bits 3-2 (`dcs & 0x0C`) select the alphabet: 0x00=7-bit default,
 * 0x04=8-bit data, 0x08=UCS-2, 0x0C=reserved (folded into UNKNOWN). */
sms_dcs_class_t sms_classify_dcs(int dcs);

/* Mirrors the vendor patch's own `_smsSplitCmgrFields()`/dcs extraction
 * (WalterModem.cpp, PATCHES.md "Patch 1.4") byte-for-byte, so this exact
 * parsing logic is host-testable (the vendor C++ file cannot be compiled on
 * the host — see PATCHES.md's own note on why the two copies exist and how
 * they are kept in sync by hand). Parses a text-mode `+CMGR` header's
 * comma-separated, optionally-quoted fields, 3GPP TS 27.005 §4.4:
 * `<stat>,<oa>,[<alpha>],<scts>[,<tooa>,<fo>,<pid>,<dcs>,<sca>,<tosca>,
 * <length>]` — the bracketed tail only present when `AT+CSDH=1` was set.
 * `raw` is everything AFTER the `"+CMGR: "` prefix. `<alpha>` (field 2) is
 * parsed but not returned (never used) — correctly handled whether quoted
 * with an embedded comma or empty (`,,`). `dcs` (field 7, 0-based) is -1 if
 * that field is absent (CSDH not set, or a shorter-than-expected header).
 * Returns false only if `<stat>`/`<oa>` themselves cannot be found (a
 * genuinely malformed header) — a merely-absent optional field is not an
 * error. */
typedef struct {
    char sender[32];    /* <oa> */
    char timestamp[32]; /* <scts> */
    int dcs;            /* -1 if absent */
} sms_cmgr_header_t;

bool sms_parse_cmgr_header(const char *raw, size_t len, sms_cmgr_header_t *out);

/* Decides how to interpret a received SMS body given its `dcs` (-1 if
 * unavailable) and decodes it into UTF-8 `out`:
 *   SMS_DCS_7BIT -> `body` is already plain text (the modem's own IRA/ASCII
 *                   representation under AT+CSCS="IRA" — see this file's
 *                   module comment on why the resting read charset makes
 *                   this unambiguous), copied through as-is.
 *   SMS_DCS_UCS2 -> `body` is hex-encoded UTF-16 (sms_decode_ucs2_hex()).
 *   SMS_DCS_8BIT -> undisplayable binary data (coordinator fix #2): `*out_shown`
 *                   is set false and `out`/`*out_len` are set to an empty
 *                   string — the caller must still audit the receive (an
 *                   empty-body `recv`/`blocked` entry) but must NEVER show
 *                   it to the student.
 *   dcs unavailable (SMS_DCS_UNKNOWN from `dcs < 0`) -> falls back to
 *                   sms_looks_like_ucs2_hex()'s heuristic, `*out_used_fallback`
 *                   set true so the caller can log it.
 * `*out_shown` is always set (true unless SMS_DCS_8BIT). Returns false only
 * on an actual decode failure (malformed UCS-2 hex) — `out` is still a valid
 * empty string in that case, `*out_shown` unaffected. */
bool sms_decode_received(const char *body, size_t body_len, int dcs, char *out, size_t out_cap,
                         size_t *out_len, bool *out_shown, bool *out_used_fallback);

/* ---------------------------------------------------------------------
 * S2 (docs/DEVICE_NEXT_TASKS.md, pure logic only — device wiring is S3's
 * job): multipart (concatenated SMS) UDH decode + reassembly.
 *
 * 3GPP TS 23.040 §9.2.3.24: a concatenated-SMS UDH is `UDHL` (one byte, the
 * length of everything that follows in the UDH) followed by one or more
 * Information Elements, each `IEI` (1 byte) `IEDL` (1 byte, length of this
 * IE's own data) `<IEDL bytes of data>`. This device only recognises the two
 * concatenation IEIs (everything else in a UDH — port addressing, etc. — is
 * out of scope and causes rejection, not a crash): IEI 0x00 (8-bit reference,
 * 3-byte data: ref, total, part -> a 5-byte IE incl. its own IEI/IEDL) and
 * IEI 0x08 (16-bit reference, 4-byte data: ref-hi, ref-lo, total, part -> a
 * 6-byte IE). Only the FIRST recognised concatenation IE in the UDH is used;
 * any other IEs present are skipped over (their IEDL is still bounds-checked)
 * rather than rejected, since a UDH carrying an unrelated IE alongside
 * concatenation info (e.g. a port-addressed concatenated message) is legal.
 * --------------------------------------------------------------------- */

/* `tpdu_udh`/`len` is the UDH INCLUDING its own leading UDHL byte (exactly
 * what a caller would slice out of a decoded TPDU-UD, before the actual
 * message text). Returns false, `*ref`/`*total`/`*part` untouched, for:
 * anything that is not IEI 0x00 or 0x08 (no recognised concatenation IE
 * found anywhere in the UDH), a length mismatch (UDHL, or an individual IE's
 * IEDL, running past `len`), `total == 0`, `part == 0`, or `part > total`.
 *
 * NOTE on the 16-bit reference (IEI 0x08): `*ref` is `uint8_t` (matching
 * IEI 0x00's native 8-bit reference), so a 16-bit reference is folded to its
 * LOW byte. This device only needs `ref` to disambiguate the handful of
 * concurrently in-flight concatenated messages this device ever tracks at
 * once (SMS_REASM_SLOTS, 2) — a same-low-byte collision between two
 * genuinely different 16-bit references arriving in the same ~2-minute
 * window is astronomically unlikely for a personal-use pager, and even if it
 * happened the failure mode is a garbled reassembly of one already-rare
 * multipart text, not data loss elsewhere or a crash. Documented risk,
 * same convention as sms_numbers_match()'s own accepted-risk comment. */
bool sms_parse_udh(const uint8_t *tpdu_udh, size_t len, uint8_t *ref, uint8_t *total,
                   uint8_t *part);

typedef enum {
    SMS_REASM_COMPLETE = 0, /* every part 1..total now present; `out`/`*out_len` filled */
    SMS_REASM_PENDING = 1,  /* stored; still waiting on at least one more part */
    SMS_REASM_DROPPED = 2,  /* invalid input, or the completed message would not fit `out_cap` */
} sms_reasm_result_t;

typedef struct {
    bool in_use;
    uint8_t ref;
    uint8_t total;
    uint8_t parts_seen_mask; /* bit (part-1) set once that part has arrived; total <= SMS_REASM_MAX_PARTS <= 8 */
    char part_body[SMS_REASM_MAX_PARTS][SMS_BODY_MAX];
    size_t part_len[SMS_REASM_MAX_PARTS];
    int64_t first_part_us; /* esp_timer_get_time() at this slot's first part — monotonic, NEVER wall
                            * clock (the same deviation msg.h documents for pending_up.created_us) */
} sms_reasm_slot_t;

typedef struct {
    sms_reasm_slot_t slots[SMS_REASM_SLOTS];
} sms_reasm_t;

void sms_reasm_init(sms_reasm_t *r);

/* Adds one already-decoded part (`text`/`len` — the part's OWN body, UDH
 * already stripped by the caller, decoded to UTF-8 the same way a
 * single-part message already is by sms_decode_received()) to the
 * reassembly ring. `ref`/`total`/`part` as decoded by sms_parse_udh() (this
 * function re-validates them independently, so a caller that skips
 * sms_parse_udh() cannot desync the ring). `now_us` is monotonic
 * (esp_timer_get_time()), used only for this slot's 120 s window
 * (SMS_REASM_WINDOW_US) and for oldest-slot eviction.
 *
 * A slot is identified by (ref, total) — a duplicate `part` for a slot
 * already holding that part number simply overwrites it (idempotent, no
 * double-count). If neither existing slot matches (ref, total) and both are
 * already in_use, the OLDEST slot (by first_part_us) is evicted to make
 * room — "a third concurrent reference evicts the oldest slot" (S2's own
 * instruction) — losing whatever partial progress it had; nothing calls
 * sms_reasm_expire() for it first, so that eviction is not separately
 * reported as "expired".
 *
 * Returns SMS_REASM_DROPPED (nothing stored) for: `total == 0`, `part == 0`,
 * `part > total`, `total > SMS_REASM_MAX_PARTS` (cannot represent), or
 * `len >= SMS_BODY_MAX` (one part alone cannot possibly fit). Returns
 * SMS_REASM_PENDING once this part is stored but at least one other part
 * for this (ref, total) has not arrived yet. Returns SMS_REASM_COMPLETE the
 * moment every part 1..total is present, with the parts concatenated IN
 * PART-NUMBER ORDER (not arrival order — this is what makes out-of-order
 * arrival transparent to the caller) into `out`/`*out_len`; if the
 * concatenated length would not fit `out_cap`, the slot is freed and
 * SMS_REASM_DROPPED is returned instead (out/`*out_len` untouched) rather
 * than silently truncating (msg.c's own "never truncate" convention). A
 * completed or dropped-for-overflow slot is freed immediately; there is
 * nothing left to expire. */
int sms_reasm_add(sms_reasm_t *r, uint8_t ref, uint8_t total, uint8_t part, const char *text,
                  size_t len, int64_t now_us, char *out, size_t out_cap, size_t *out_len);

typedef struct {
    uint8_t ref;
    uint8_t total;
    char body[SMS_BODY_MAX]; /* whichever parts had arrived, concatenated in part order; a still-
                              * missing part is simply skipped (never invented/blanked-in), per
                              * §6's "never silently lose a text" rule: deliver what exists rather
                              * than nothing at all. */
    size_t body_len;
} sms_reasm_expired_t;

/* Frees and reports every slot whose window (SMS_REASM_WINDOW_US from its
 * first part) has elapsed as of `now_us` — the caller (S3) is expected to
 * log each one and still deliver its (partial) `body` via
 * msg_insert_sms_in() rather than dropping it silently, per the same
 * "never silently lose a text" rule sms_reasm_add()'s overflow case follows.
 * A backwards/stalled `now_us` (`now_us < first_part_us`) never expires a
 * slot early — elapsed time is computed as a plain subtraction and only
 * ever compared as ">=", so a clock that has not advanced simply leaves
 * every slot pending, it can never wedge into an incorrect early expiry.
 * Returns the number of slots expired (0..SMS_REASM_SLOTS), writing that
 * many entries to `out` (capped at `out_cap`; a slot beyond `out_cap` is
 * still freed and counted in the return value, just not reported in detail
 * — `out_cap >= SMS_REASM_SLOTS` from every caller in this codebase means
 * this never actually happens in practice). */
int sms_reasm_expire(sms_reasm_t *r, int64_t now_us, sms_reasm_expired_t *out, int out_cap);

/* ---------------------------------------------------------------------
 * Audit ring (pure) — the "at least 16 entries, oldest dropped, sms_lost
 * counted" queue (PROTOCOL.md §3.6). A plain circular buffer over caller
 * memory; the device section below is what persists it to NVS.
 * --------------------------------------------------------------------- */

typedef struct {
    char peer[SMS_PEER_MAX];
    char dir[4];              /* "out" | "in" */
    char st[8];               /* "sent" | "failed" | "recv" | "blocked" */
    char body[SMS_BODY_MAX];
    uint16_t body_len;
    int64_t sms_ts;
} sms_audit_entry_t;

typedef struct {
    sms_audit_entry_t entries[SMS_AUDIT_MAX];
    uint8_t head;   /* index of the oldest entry (valid iff count > 0) */
    uint8_t count;
    uint32_t lost;  /* PROTOCOL.md §5.1 `sms_lost`: cumulative, never reset */
} sms_audit_ring_t;

void sms_audit_ring_init(sms_audit_ring_t *r);

/* Inserts one entry at the tail, returning the ring-array slot index it was
 * written to (0..SMS_AUDIT_MAX-1) — the device section persists exactly
 * that one slot's NVS entry rather than the whole ring (coordinator fix:
 * "store one NVS entry per audit record ... each event rewrites only its
 * own small entry"). If the ring is already full (SMS_AUDIT_MAX), the
 * OLDEST entry is evicted first, `r->lost` is incremented (PROTOCOL.md
 * §3.6: "the oldest entry is dropped and the drop is counted"), and its own
 * slot index is reported via `*evicted_slot` (so the device section can
 * erase exactly that one NVS entry); `*evicted_slot` is -1 when nothing was
 * evicted. `body`/`body_len` are copied, truncated to SMS_BODY_MAX-1 bytes
 * if somehow longer (never expected in practice: a body this long would
 * already have failed sms_decide_encoding()). Always succeeds. */
int sms_audit_enqueue(sms_audit_ring_t *r, const char *peer, const char *dir, const char *st,
                      const char *body, uint16_t body_len, int64_t sms_ts, int *evicted_slot);

/* Copies the oldest entry into `out` without removing it (for the caller to
 * attempt a publish). Returns false (out untouched) if the ring is empty. */
bool sms_audit_peek_oldest(const sms_audit_ring_t *r, sms_audit_entry_t *out);

/* Removes the oldest entry — call only once its publish has actually been
 * accepted (PROTOCOL.md §3.6: "removed only after the publish is
 * acknowledged" — see this file's own device-section note on the same
 * "accepted, not confirmed" simplification msg.c's msg_pump() already
 * documents). Returns the ring-array slot index that was removed (for the
 * device section to erase that one NVS entry), or -1 if the ring was
 * already empty. */
int sms_audit_pop_oldest(sms_audit_ring_t *r);

/* ---------------------------------------------------------------------
 * `sms_log` CBOR encoder (pure) — PROTOCOL.md §3.6/§10.
 * --------------------------------------------------------------------- */

/* Builds the `kind:"sms_log"` envelope body: `v,id,ts,kind,peer,dir,st,body,
 * sms_ts[,n]` in EXACTLY that field order (matches the order
 * `docs/V02_DESIGN.md` §6's own wire-shape listing gives, and
 * `relay/tests/test_sms.py`'s `sms_log_payload()` JSON field order — see
 * firmware/host/test_sms.c for the byte-compare against
 * `relay/app/wirecbor.py`'s own encoding of the equivalent object built in
 * that same field order, cbor2.dumps() preserves Python dict insertion
 * order). `n` is sized into the map header's pair count but appended as the
 * LAST body field (docs/DEVICE_PLAN.md §2.4's "no re-serialisation" rule,
 * same convention every other signed envelope in this codebase uses) so the
 * caller can immediately follow with auth_sign(). `dir`/`st` are passed as
 * plain C strings (not an enum) — this module does not itself enforce the
 * out/in or sent/failed/recv/blocked value sets; sms.c's device section
 * only ever calls this with a literal from that set. Returns false (out_len
 * untouched) on a buffer overflow. */
bool sms_build_log_cbor(uint8_t *out, size_t cap, size_t *out_len, bool signed_env, uint64_t n,
                        const char *id, int64_t ts, const char *peer, const char *dir,
                        const char *st, const char *body, size_t body_len, int64_t sms_ts);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * Device wiring — needs ESP-IDF (NVS, esp_timer/FreeRTOS via net.h, modes.h,
 * msg.h). Same `#ifdef ESP_PLATFORM` split every other main module uses.
 * --------------------------------------------------------------------- */
#include "auth.h" /* auth_rtc_t — sms_bind() below, same reason loc.h/book.h/msg.h include it:
                    * a signed `sms_log` shares the ONE strictly-increasing /up,/status,/loc
                    * counter every other signed publish uses (PROTOCOL.md §14.2). */

typedef void (*sms_rtc_lock_fn)(void);
typedef void (*sms_rtc_unlock_fn)(void);
typedef void (*sms_rtc_save_fn)(void); /* must be called with the lock already held */
typedef void (*sms_epoch_wrap_fn)(void); /* same contract as msg_epoch_wrap_fn/loc_epoch_wrap_fn */

/* Binds the cross-task mutex modes.c already owns (same reuse pattern
 * catrust_bind() documents) — sms.c's own RAM state (the allow-list cache,
 * the audit ring, the pending-send queue) is written from the MQTT event
 * task (sms_apply_cfg_submap()) and read/advanced from modes_run()'s task
 * (sms_service(), scr_chat.c/scr_pick.c's UI accessors) — plus the EXISTING
 * g_rtc.auth binding (same pattern book_bind()/loc_bind() use) so
 * sms_try_publish_one() can sign a `sms_log` envelope. No RTC sub-struct of
 * this module's OWN: allow-list + audit ring both live in NVS (this task's
 * own "prefer NVS/RAM, report any RTC growth" instruction — RTC growth from
 * this task is 0 bytes). Call once, before sms_init(). */
void sms_bind(auth_rtc_t *auth_rtc, sms_rtc_lock_fn lock, sms_rtc_unlock_fn unlock,
             sms_rtc_save_fn save, sms_epoch_wrap_fn on_wrap);

/* Loads NVS namespaces "smscts" (allow-list) and "smsaud" (audit ring) into
 * RAM, then calls net_sms_config() (AT+CMGF/CSCS/CSMP/CNMI/CPMS). A false
 * return from net_sms_config() is logged once at INFO and disables the
 * feature for this boot (every other public entry point in this section
 * degrades to a safe no-op/false return: the picker shows no SMS contacts
 * as sendable, sms_service() becomes a no-op, `cfg.sms` pushes are still
 * stored to NVS/acked but never make an SMS contact sendable) — V02_DESIGN.md
 * §0's fail-open rule; nothing here can block or fail boot. Also arms the
 * boot-drain scan (V02_DESIGN.md §6: "drain any messages that arrived while
 * the pager was off"), stepped one index per sms_service() call, never all
 * at once. Call once from modes_boot(), after net_init() (net_sms_config()
 * needs the modem) and after sms_bind(). */
void sms_init(void);

/* Which TE charset AT+CSCS is resting on this boot (SMS_CHARSET_IRA unless
 * smsConfig() had to fall back) — scr_chat.c's own render-time composer
 * limit display and send-time sms_measure() calls both need this. Reads a
 * plain RAM flag; meaningless (defaults to SMS_CHARSET_IRA) when SMS is
 * unavailable this boot (net_sms_config() failed). */
sms_charset_mode_t sms_get_charset_mode(void);

/* One non-blocking-ish step of the boot-drain / `+CMTI`-drain / pending-send
 * state machine, called every modes_run() loop iteration regardless of
 * whether anything is pending (a few RAM reads when idle) — same "never the
 * whole thing in one call" discipline loc_service()/catrust_service()
 * document, EXCEPT for a pending send's own AT+CMGS round trip, which does
 * run to completion inside one call (a few seconds at most, PENDING_HW/
 * UNVERIFIED — the same class of blocking AT work loc.c's own
 * ASSIST_UPDATE phase already tolerates in one call). No return value:
 * every outcome is either logged, or lands in msg.c's thread (via
 * msg_insert_sms_in()/msg_finish_sms_out()) and/or the audit ring. */
void sms_service(void);

/* Called once per msg_pump() call (msg.c's own wake-cycle publish rotation,
 * PROTOCOL.md §9.5), ONLY when neither a pending ack nor a pending reply had
 * anything to do this cycle — V02_DESIGN.md §6's own instruction: "share
 * that one-publish-per-cycle discipline rather than adding a second
 * publisher". Attempts to publish the audit ring's oldest entry (if any) as
 * a signed `sms_log` /up envelope, QoS 1; on success, pops it
 * (sms_audit_pop_oldest()); on failure, leaves it for the next cycle. Same
 * "accepted by the modem, not confirmed delivered" simplification
 * msg.c's own msg_pump() documents for net_publish_raw(). No-op if the
 * ring is empty or the MQTT session is not connected (msg_pump()'s own
 * caller in modes.c already gates on `st.mqtt_connected` before calling
 * msg_pump() at all, so this function does not re-check). */
void sms_try_publish_one(void);

/* `/status` `sms_lost` (PROTOCOL.md §5.1/§7 key 48): the audit ring's
 * cumulative persisted drop counter. Plain read of already-resident state. */
uint32_t sms_get_lost_count(void);

/* `cfg.sms` intercept (cfg.c's cfg_ingest_cbor(), MQTT event task). `buf`/
 * `len` are the raw `sms` sub-map bytes (cfg_dispatch_t.sms_off/sms_len);
 * `id` is the envelope's own id, for the immediate `shown` ack
 * (V02_DESIGN.md §6: "acked shown on apply", same immediate-apply-and-ack
 * timing `cfg.lock` already uses — unlike `cfg.ca`'s deferred two-phase
 * apply, there is nothing to validate against the network here). A
 * malformed list (sms_parse_cfg_submap() returns false) is logged and
 * dropped, exactly like any other malformed `/down` content: no ack, no
 * partial apply, no crash — the relay's own "only the newest unacked cfg is
 * re-published" rule then keeps re-offering it. Full replacement of the
 * allow-list on success (newest wins, PROTOCOL.md §3.6), persisted to NVS.
 * Power effect: one NVS write. */
void sms_apply_cfg_submap(const uint8_t *buf, uint16_t len, const char *id);

/* ---------------------------------------------------------------------
 * Read-only accessors for scr_pick.c/scr_chat.c (the recipient picker and
 * the composer's `@name` resolution) — plain reads of the RAM-cached
 * allow-list, no NVS I/O of their own. Return a false/empty result
 * (never a crash) when SMS is unavailable this boot, so the picker simply
 * shows no SMS contacts. */
size_t sms_contact_count(void);
bool sms_contact_at(size_t index, sms_contact_t *out);

/* Device-side wrapper around the pure sms_find_contact_by_name() against
 * the RAM-cached allow-list (cross-task locked) — scr_chat.c's own `@name`
 * composer resolution and its render-time "is the current @word an SMS
 * contact" peek. Returns the matching index or -1 (also -1, never a crash,
 * when SMS is unavailable this boot). */
int sms_find_by_name(const char *word, size_t word_len, sms_contact_t *out);

/* Composer send entry point (scr_chat.c, modes_run()'s own task): `to` is
 * the already-resolved SMS contact (scr_chat.c's own `@name`/picker
 * resolution — this function does not itself search the allow-list by
 * name); `body`/`body_len` is the UTF-8 text already validated by the
 * caller against sms_decide_encoding()'s own limit for `to`. Inserts a
 * PENDING ("...") thread entry immediately (msg_insert_sms_out_pending(),
 * msg.h) and queues the actual send for sms_service() to perform on a later
 * iteration — "send first, log second" (PROTOCOL.md §3.6) means the
 * *audit* entry is only written once the send has actually been attempted,
 * not that the send itself is synchronous with this call (mirrors
 * msg_queue_reply()'s own queued-then-pumped shape for an ordinary reply).
 * Returns false (nothing queued, thread unchanged) only if the tiny
 * pending-send queue is already full (both slots busy — a rapid-fire
 * double-send; the composer shows "reply full" the same way
 * msg_queue_reply() does for its own queue-full case) or if SMS is
 * unavailable this boot. */
bool sms_queue_send(const sms_contact_t *to, const char *body, uint16_t body_len);

/* `smstest <number> <text>` (main.c, PAGER_DEBUG_NO_LIGHT_SLEEP builds
 * only): sends one SMS bypassing the allow-list entirely (an arbitrary
 * number, not looked up against the contact list) but NOT bypassing the
 * audit trail (PROTOCOL.md §3.6/§7.3: "non-negotiable" in both directions,
 * with no debug carve-out stated) — enqueues an `st:"sent"`/`"failed"`
 * audit entry the same way a real composer send does, just with no thread
 * entry (this is a console command, not the chat UI). Blocks the CALLING
 * task (the debug console's own, never modes_run()'s — same discipline
 * loc_debug_run()/catrust_debug_cafetch() document) for the duration of the
 * AT+CMGS round trip. Logs the encoding chosen (GSM-7/UCS-2/too-long) and
 * the raw net_sms_send() result. Returns true iff the modem reported "OK". */
bool sms_debug_send(const char *number, const char *text);

/* `smslist` (main.c, PAGER_DEBUG_NO_LIGHT_SLEEP builds only): logs the
 * allow-list (name/phone per entry) and the audit ring's current depth
 * (sms.c does the ESP_LOGI calls itself, main.c only invokes this). */
void sms_debug_list(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* SMS_H */

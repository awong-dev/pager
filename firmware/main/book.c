// book.c — see book.h for the module split, the "no RTC of its own"
// rationale, and every function's doc comment.
//
// All power-effect comments are PENDING_HW.

#include "book.h"

#include <string.h>

#include "cbor.h"

// ---------------------------------------------------------------------------
// Pure functions (no ESP-IDF dependency) — a future firmware/host/test_book.c
// can link this section directly, same `#ifdef ESP_PLATFORM` split as
// lock.c/auth.c.
// ---------------------------------------------------------------------------

static bool is_cont_byte(uint8_t b) { return (b & 0xC0) == 0x80; }

// Shared UTF-8 shape check for both `name` (book.h's book_name_valid(),
// 16 code points/48 bytes, §3.1/§4.2) and the device-local `nickname`
// (12 code points/36 bytes, §5.5) — same control-character/code-point
// counting logic msg.c's body_rules_ok() uses, duplicated rather than
// shared across modules (this project's existing convention: auth.c/lock.c/
// msg.c each keep their own copy of this kind of small helper rather than
// take a cross-module dependency for it).
static bool text_field_valid(const char *s, size_t len, size_t max_bytes, size_t max_codepoints)
{
    if (!s || len == 0 || len > max_bytes) {
        return false;
    }
    size_t codepoints = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) s[i];
        if (c < 0x20 || c == 0x7F) {
            return false; // control character
        }
        if (!is_cont_byte(c)) {
            codepoints++;
        }
    }
    return codepoints <= max_codepoints;
}

bool book_name_valid(const char *name, size_t len)
{
    return text_field_valid(name, len, BOOK_NAME_MAX - 1, 16);
}

// PROTOCOL.md §10 envelope keymap subset this file reads/writes, plus
// CONTACT_KEY_*/REQUEST_KEY_* — the `c[]`/`p[]` sub-map keys from §10's
// "Sub-map keys" list (also relay/app/wirecbor.py's CONTACT_KEYMAP/
// REQUEST_KEYMAP).
#define BK_V 0
#define BK_ID 1
#define BK_TS 2
#define BK_ACK 5
#define BK_KIND 6
#define BK_N 12
#define BK_BV 14
#define BK_NAME 15
#define BK_PH 16
#define BK_D 17
#define BK_C 18
#define BK_P 19

#define CONTACT_KEY_A 0
#define CONTACT_KEY_N 1
#define CONTACT_KEY_T 2

#define REQUEST_KEY_N 0
#define REQUEST_KEY_S 1

// `slen >= cap` is treated as malformed (reject the whole envelope) rather
// than silently truncated — the relay already enforces every one of these
// caps server-side (devcfg.py's _BOOK_NAME_MAX_CODEPOINTS etc.), so a
// too-long field here means either a relay bug or a hostile MITM, and
// PROTOCOL.md §3.4's "reject, don't guess" discipline applies the same way
// msg.c's MK_ID/MK_FROM handling already does.
static bool copy_tstr(const char *s, size_t slen, char *out, size_t cap)
{
    if (slen >= cap) {
        return false;
    }
    memcpy(out, s, slen);
    out[slen] = '\0';
    return true;
}

static bool parse_contact(cbor_r_t *r, book_contact_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t count;
    if (!cbor_r_map(r, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(r, &key)) {
            return false;
        }
        switch (key) {
        case CONTACT_KEY_A: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(r, &s, &slen) || !copy_tstr(s, slen, out->alias, sizeof(out->alias))) {
                return false;
            }
            break;
        }
        case CONTACT_KEY_N: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(r, &s, &slen) || !copy_tstr(s, slen, out->name, sizeof(out->name))) {
                return false;
            }
            break;
        }
        case CONTACT_KEY_T: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(r, &s, &slen) || !copy_tstr(s, slen, out->type, sizeof(out->type))) {
                return false;
            }
            break;
        }
        default:
            if (!cbor_r_skip(r)) {
                return false;
            }
            break;
        }
    }
    return out->alias[0] != '\0'; // `a` is required, §3.1
}

static bool parse_request(cbor_r_t *r, book_request_t *out)
{
    memset(out, 0, sizeof(*out));
    uint32_t count;
    if (!cbor_r_map(r, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(r, &key)) {
            return false;
        }
        switch (key) {
        case REQUEST_KEY_N: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(r, &s, &slen) || !copy_tstr(s, slen, out->name, sizeof(out->name))) {
                return false;
            }
            break;
        }
        case REQUEST_KEY_S: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(r, &s, &slen) || !copy_tstr(s, slen, out->status, sizeof(out->status))) {
                return false;
            }
            break;
        }
        default:
            if (!cbor_r_skip(r)) {
                return false;
            }
            break;
        }
    }
    return out->name[0] != '\0'; // `n` is required, §3.1
}

bool book_parse(const uint8_t *buf, uint16_t len, bool sig_pair_present, book_parsed_t *out)
{
    memset(out, 0, sizeof(*out));

    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count) || count == 0) {
        return false;
    }
    if (sig_pair_present) {
        // docs/DEVICE_PLAN.md §2.4's "no re-serialisation" rule — same
        // adjustment msg.c's MK_* switch and lock.c's parse_cfg_map() make.
        count -= 1;
    }

    bool is_book = false;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            return false;
        }
        switch (key) {
        case BK_ID: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) {
                return false;
            }
            if (slen < sizeof(out->id)) {
                memcpy(out->id, s, slen);
                out->id[slen] = '\0';
            } // else: id too long to fit — left empty, ack is simply skipped later
            break;
        }
        case BK_KIND: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen)) {
                return false;
            }
            is_book = (slen == 4 && memcmp(s, "book", 4) == 0);
            break;
        }
        case BK_BV: {
            uint64_t v;
            if (!cbor_r_uint(&r, &v) || v > 0xFFFFFFFFu) {
                return false;
            }
            out->bv = (uint32_t) v;
            break;
        }
        case BK_D: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen == 0 || slen >= sizeof(out->default_alias)) {
                return false;
            }
            memcpy(out->default_alias, s, slen);
            out->default_alias[slen] = '\0';
            out->have_default = true;
            break;
        }
        case BK_C: {
            uint32_t n;
            if (!cbor_r_array(&r, &n)) {
                return false;
            }
            for (uint32_t j = 0; j < n; j++) {
                book_contact_t c;
                if (!parse_contact(&r, &c)) {
                    return false;
                }
                if (out->n_contacts < BOOK_MAX_CONTACTS) {
                    out->contacts[out->n_contacts++] = c;
                } // else: over cap — parsed (buffer position correct) but dropped
            }
            break;
        }
        case BK_P: {
            uint32_t n;
            if (!cbor_r_array(&r, &n)) {
                return false;
            }
            for (uint32_t j = 0; j < n; j++) {
                book_request_t rq;
                if (!parse_request(&r, &rq)) {
                    return false;
                }
                if (out->n_requests < BOOK_MAX_REQUESTS) {
                    out->requests[out->n_requests++] = rq;
                }
            }
            break;
        }
        default:
            if (!cbor_r_skip(&r)) {
                return false;
            }
            break;
        }
    }

    return is_book;
}

#ifdef ESP_PLATFORM

#include "auth.h"
#include "esp_log.h"
#include "esp_random.h"
#include "ident.h"
#include "msg.h"
#include "net.h"
#include "nvs.h"

static const char *TAG = "book";

// Device-local nickname bounds, docs/DEVICE_PLAN.md §5.5 ("Nicknames"): <=12
// code points / 36 UTF-8 bytes.
static bool nickname_valid(const char *s, size_t len) { return text_field_valid(s, len, BOOK_NICK_MAX - 1, 12); }

// ---------------------------------------------------------------------------
// auth_rtc_t / cross-task lock wiring (book.h: book_bind()) — see book.h's
// module comment for why this reuses modes.c's EXISTING g_rtc.auth and
// shared mutex instead of a book_rtc_t of its own.
// ---------------------------------------------------------------------------

// Real hardware finding (see msg.c's identical fix): scr_home.c/scr_book.c
// can call book.c accessors before book_bind() ever runs (setup_run(),
// F3.5, calls ui_init() standalone ahead of modes_boot()) -- a NULL
// s_lock()/s_unlock() there is a jump to address 0, not just a bad read.
// Defaulting to a no-op keeps every accessor safe pre-bind.
static void book_lock_noop(void) {}

static auth_rtc_t *s_auth_rtc = NULL;
static book_lock_fn s_lock = book_lock_noop;
static book_unlock_fn s_unlock = book_lock_noop;
static book_save_fn s_rtc_save = book_lock_noop; // ONLY for auth_rtc_t mutation
                                                  // (next_up_n_locked) — never for this
                                                  // module's own NVS blob, which has no
                                                  // RTC mirror
static book_epoch_wrap_fn s_on_wrap = book_lock_noop;

void book_bind(auth_rtc_t *auth_rtc, book_lock_fn lock, book_unlock_fn unlock, book_save_fn save,
               book_epoch_wrap_fn on_wrap)
{
    s_auth_rtc = auth_rtc;
    s_lock = lock;
    s_unlock = unlock;
    s_rtc_save = save;
    s_on_wrap = on_wrap;
}

// Same pattern as msg.c's next_up_n_locked() (F3.6) — every signed
// /up,/status,/loc envelope shares one strictly-increasing counter.
static uint64_t next_up_n_locked(bool *wrapped)
{
    if (wrapped) {
        *wrapped = false;
    }
    if (!s_auth_rtc) {
        return 0;
    }
    s_lock();
    uint64_t n = auth_next_up_n(s_auth_rtc, ident_get_n_epoch(), wrapped);
    s_rtc_save();
    s_unlock();
    return n;
}

// ---------------------------------------------------------------------------
// NVS namespace "book" (docs/DEVICE_PLAN.md §4.3): one blob, no per-field
// keys (contrast lock.c/msg.c's own namespaces, which use several small
// named keys) — the whole address book is always replaced as a unit
// (§4.3's "applied atomically ... as a full replacement"), so one blob
// under one key is the natural shape. `layout` is this blob's own version
// tag (NVS already checksums/wear-levels the underlying flash write, so —
// unlike RTC_DATA_ATTR state, CLAUDE.md's convention — no separate CRC is
// added here; same precedent lock.c's/msg.c's own NVS-resident fields set).
// ---------------------------------------------------------------------------

#define BOOK_NS "book"
#define BOOK_BLOB_VERSION 1u

typedef struct {
    uint32_t layout;
    uint32_t bv;
    bool have_default;
    char default_alias[BOOK_ALIAS_MAX];
    uint8_t n_contacts;
    uint8_t n_requests;
    book_contact_t contacts[BOOK_MAX_CONTACTS];
    book_request_t requests[BOOK_MAX_REQUESTS];
} book_blob_t;

// RAM cache, guarded by s_lock/s_unlock (the same shared cross-task mutex
// book_bind() hands over) — see book.h's module comment for why reusing it
// here (rather than a dedicated mutex) is the same "one shared mutex"
// discipline msg.h documents.
static book_blob_t s_book;

static void book_defaults(book_blob_t *b)
{
    memset(b, 0, sizeof(*b));
    b->layout = BOOK_BLOB_VERSION;
}

static void load_from_nvs(void)
{
    book_defaults(&s_book);

    nvs_handle_t h;
    if (nvs_open(BOOK_NS, NVS_READONLY, &h) != ESP_OK) {
        return; // never applied a book yet — defaults stand (bv=0)
    }
    book_blob_t tmp;
    size_t len = sizeof(tmp);
    if (nvs_get_blob(h, "blob", &tmp, &len) == ESP_OK && len == sizeof(tmp) &&
        tmp.layout == BOOK_BLOB_VERSION) {
        s_book = tmp;
    }
    nvs_close(h);
}

// Takes a locked snapshot copy (fast, no I/O under the lock — same
// discipline msg.c/lock.c already use for flash writes), then writes it out
// with the lock released. Power effect: one NVS blob write (~1.25 kB,
// docs/DEVICE_PLAN.md §4.3's own sizing estimate).
static bool persist_snapshot(void)
{
    book_blob_t snap;
    s_lock();
    snap = s_book;
    s_unlock();

    nvs_handle_t h;
    if (nvs_open(BOOK_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, "blob", &snap, sizeof(snap)) == ESP_OK;
    if (ok) {
        ok = nvs_commit(h) == ESP_OK;
    }
    nvs_close(h);
    return ok;
}

void book_init(void) { load_from_nvs(); }

uint32_t book_get_bv(void)
{
    s_lock();
    uint32_t v = s_book.bv;
    s_unlock();
    return v;
}

bool book_get_default_alias(char *out, size_t cap)
{
    s_lock();
    bool have = s_book.have_default;
    char tmp[BOOK_ALIAS_MAX];
    memcpy(tmp, s_book.default_alias, sizeof(tmp));
    s_unlock();
    if (!have) {
        return false;
    }
    strncpy(out, tmp, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

size_t book_contact_count(void)
{
    s_lock();
    size_t n = s_book.n_contacts;
    s_unlock();
    return n;
}

bool book_contact_at(size_t index, book_contact_t *out)
{
    s_lock();
    bool ok = index < (size_t) s_book.n_contacts;
    if (ok) {
        *out = s_book.contacts[index];
    }
    s_unlock();
    return ok;
}

size_t book_request_count(void)
{
    s_lock();
    size_t n = s_book.n_requests;
    s_unlock();
    return n;
}

bool book_request_at(size_t index, book_request_t *out)
{
    s_lock();
    bool ok = index < (size_t) s_book.n_requests;
    if (ok) {
        *out = s_book.requests[index];
    }
    s_unlock();
    return ok;
}

bool book_set_nickname(const char *alias, const char *nickname, size_t nickname_len)
{
    if (!alias) {
        return false;
    }
    bool clearing = (nickname == NULL || nickname_len == 0);
    if (!clearing && !nickname_valid(nickname, nickname_len)) {
        return false;
    }

    s_lock();
    int idx = -1;
    for (int i = 0; i < s_book.n_contacts; i++) {
        if (strncmp(s_book.contacts[i].alias, alias, BOOK_ALIAS_MAX) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        s_unlock();
        return false; // not an approved contact in the current book
    }
    if (clearing) {
        s_book.contacts[idx].nickname[0] = '\0';
    } else {
        memcpy(s_book.contacts[idx].nickname, nickname, nickname_len);
        s_book.contacts[idx].nickname[nickname_len] = '\0';
    }
    s_unlock();

    return persist_snapshot();
}

bool book_ingest_cbor(const uint8_t *buf, uint16_t len)
{
    bool sig_present = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    book_parsed_t parsed;
    if (!book_parse(buf, len, sig_present, &parsed)) {
        return false; // not book, or malformed book — caller falls through to msg.c
    }

    s_lock();
    book_blob_t newb;
    book_defaults(&newb);
    newb.bv = parsed.bv;
    newb.have_default = parsed.have_default;
    strncpy(newb.default_alias, parsed.default_alias, sizeof(newb.default_alias) - 1);
    newb.n_contacts = parsed.n_contacts;
    for (uint8_t i = 0; i < parsed.n_contacts; i++) {
        book_contact_t c = parsed.contacts[i]; // wire copy — .nickname is "" here
        // §5.5: carry the nickname forward by alias match against the
        // book being replaced; a contact whose alias is no longer present
        // simply does not match anything and its nickname is dropped with
        // it, exactly as documented.
        for (uint8_t j = 0; j < s_book.n_contacts; j++) {
            if (strncmp(s_book.contacts[j].alias, c.alias, BOOK_ALIAS_MAX) == 0) {
                strncpy(c.nickname, s_book.contacts[j].nickname, sizeof(c.nickname) - 1);
                break;
            }
        }
        newb.contacts[i] = c;
    }
    newb.n_requests = parsed.n_requests;
    for (uint8_t i = 0; i < parsed.n_requests; i++) {
        newb.requests[i] = parsed.requests[i];
    }
    s_book = newb;
    s_unlock();

    persist_snapshot(); // unlocked flash write, §4.3 "applied atomically into NVS"

    ESP_LOGI(TAG, "book applied: bv=%u contacts=%u requests=%u", (unsigned) newb.bv,
             (unsigned) newb.n_contacts, (unsigned) newb.n_requests);

    if (parsed.id[0] != '\0') {
        // §4.3: acked `shown` once applied, not a thread entry, regardless
        // of lock.c's lock state (same rule lock_ingest_cfg_cbor() documents
        // for `cfg` — "the lock is about the screen").
        msg_mark_shown(parsed.id);
    }
    return true;
}

bool book_request(const char *name, const char *ph_or_alias)
{
    if (!name) {
        return false;
    }
    size_t name_len = strlen(name);
    if (!book_name_valid(name, name_len)) {
        return false;
    }
    bool has_ph = (ph_or_alias != NULL && ph_or_alias[0] != '\0');
    size_t ph_len = has_ph ? strlen(ph_or_alias) : 0;
    if (has_ph && ph_len >= BOOK_REQ_PH_MAX) {
        return false;
    }

    char id[BOOK_ID_MAX];
    snprintf(id, sizeof(id), "u_%08x", (unsigned) esp_random());

    int64_t ts = 0;
    net_get_clock(&ts); // best-effort; leaves ts=0 on failure per §3.5

    bool signed_env = (ident_get_flags() & IDENT_FLAG_REQ_SIG) != 0;
    uint32_t nfields = 6; // v, id, ts, kind, name, ack
    if (has_ph) {
        nfields += 1;
    }
    if (signed_env) {
        nfields += 2; // n (written below) + sig (appended by auth_sign())
    }

    uint8_t buf[192]; // envelope + <=48-byte name + <=16-byte ph, generous
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, nfields);
    cbor_w_uint(&w, BK_V, 1);
    cbor_w_tstr(&w, BK_ID, id, strlen(id));
    cbor_w_uint(&w, BK_TS, (uint64_t) ts);
    cbor_w_tstr(&w, BK_KIND, "contact_req", 11);
    cbor_w_tstr(&w, BK_NAME, name, name_len);
    if (has_ph) {
        cbor_w_tstr(&w, BK_PH, ph_or_alias, ph_len);
    }
    cbor_w_null(&w, BK_ACK);

    char topic[48];
    snprintf(topic, sizeof(topic), "pager/%s/up", net_get_device_id());

    if (!signed_env) {
        if (w.err) {
            return false;
        }
        bool ok = net_publish_raw(topic, buf, (uint16_t) w.len, 1);
        if (ok) {
            ESP_LOGI(TAG, "contact_req %s queued: name=%s ph=%s", id, name, has_ph ? ph_or_alias : "(none)");
        }
        return ok;
    }

    bool wrapped = false;
    uint64_t n = next_up_n_locked(&wrapped);
    cbor_w_uint(&w, BK_N, n);
    if (w.err) {
        return false;
    }
    size_t len = w.len;
    if (!auth_sign(topic, buf, &len, sizeof(buf))) {
        return false;
    }
    if (wrapped && s_on_wrap) {
        s_on_wrap();
    }
    bool ok = net_publish_raw(topic, buf, (uint16_t) len, 1);
    if (ok) {
        ESP_LOGI(TAG, "contact_req %s queued: name=%s ph=%s", id, name, has_ph ? ph_or_alias : "(none)");
    }
    return ok;
}

#endif /* ESP_PLATFORM */

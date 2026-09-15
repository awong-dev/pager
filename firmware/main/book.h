/* book.h — device-side address book: NVS-backed projection of the server's
 * allow-list (`kind:"book"` down ingest), the student's own pending
 * `contact_req` publish path, and device-local nickname slots.
 *
 * Authority: docs/DEVICE_PLAN.md §4.2 (requesting), §4.3 (approving/pushing
 * the book down, the device storage shape), §5.5 ("Nicknames" paragraph);
 * docs/PROTOCOL.md §3.1/§3.2 (`contact_req`/`book` field tables and rules),
 * §10 (CBOR keymap: `kind`=6, `bv`=14, `name`=15, `ph`=16, `d`=17, `c`=18,
 * `p`=19; sub-map `c[]` a=0/n=1/t=2, `p[]` n=0/s=1).
 *
 * **No RTC storage.** Unlike msg.c/lock.c/auth.c, this module has no
 * RTC-resident sub-struct: docs/PROTOCOL.md §9.6's survival table lists
 * `book` only under the NVS column (namespace `book`), never under RTC. A
 * `book` is idempotent server state (the relay re-publishes it on every
 * online edge, §5.3, and again whenever `/status`'s `bv` looks stale, §4.3)
 * — nothing here needs to survive a crash any faster than the next
 * `/status` round trip already provides, so there is no RTC bump/no new
 * `pager_rtc_t` field for modes.c to embed.
 *
 * **The one thing this module DOES need from modes.c's RTC:** a
 * `contact_req` is a signed `/up` envelope like any other (docs/DEVICE_PLAN.md
 * §4.2: "The device includes `n` and `sig` as with any `/up` from an `hmac`
 * device"), and every signed `/up`/`/status`/`/loc` envelope shares one
 * strictly-increasing counter, `auth_rtc_t` (docs/PROTOCOL.md §14.2),
 * embedded in modes.c's `pager_rtc_t` as `g_rtc.auth`. `book_bind()` below is
 * the same handoff pattern `msg_bind_auth()` (msg.h) and modes.c's own
 * `/status` path (`build_status_cbor()`, which calls `auth_next_up_n(&g_rtc.auth,
 * ...)` directly) already use — modes.c hands over the raw `auth_rtc_t*`
 * plus its cross-task lock/unlock/save trio (the same ones msg.c/lock.c
 * reuse) and the epoch-wrap callback. This module reuses that SAME
 * lock/unlock pair to guard its own RAM cache of the applied book too (the
 * same cross-task rationale msg.h's header comment gives: `book_ingest_cbor()`
 * runs on WalterModem's `_eventProcessingTask` via modes.c's
 * `on_incoming_message()`, while a future UI reader runs on `modes_run()`'s
 * task) — there being exactly one shared mutex in this whole design, per
 * msg.h's own comment.
 *
 * Split the same way lock.c/auth.c already are: `book_name_valid()` and
 * `book_parse()` below this banner have no ESP-IDF dependency (pure
 * functions over caller memory plus cbor.h, itself host-buildable) and are
 * suitable for a future `firmware/host/test_book.c` (not added by this task
 * — not in its Files list). Everything else — NVS I/O, the RAM cache, the
 * signed-publish path — is `#ifdef ESP_PLATFORM`-only.
 */
#ifndef BOOK_H
#define BOOK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "auth.h" /* auth_rtc_t — book_bind() below, same reason msg.h includes it */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing constants — docs/DEVICE_PLAN.md §4.3's storage paragraph and
 * §3.1's field table.
 * --------------------------------------------------------------------- */

#define BOOK_ID_MAX 17       /* 16 chars + NUL, PROTOCOL.md §1 */
#define BOOK_ALIAS_MAX 17    /* 16 chars + NUL, same regex as `from`/`to` */
#define BOOK_NAME_MAX 49     /* 48 UTF-8 bytes + NUL, <=16 code points, §3.1 */
#define BOOK_NICK_MAX 37     /* 36 UTF-8 bytes + NUL, <=12 code points, §5.5 */
#define BOOK_TYPE_MAX 5      /* "web" | "sms" | "chat" + NUL */
#define BOOK_STATUS_MAX 5    /* "pend" | "no" + NUL */
#define BOOK_REQ_PH_MAX 17   /* E.164 phone or alias reference, §4.2 */

#define BOOK_MAX_CONTACTS 10 /* §4.3 cap */
#define BOOK_MAX_REQUESTS 4  /* §4.3 cap */

/* ---------------------------------------------------------------------
 * Applied-book shapes (RAM cache / NVS blob). `book_contact_t.nickname` is
 * the ONE field in this file that never comes over the wire — device-local,
 * keyed by alias, preserved across a full `book_apply()` replacement
 * (docs/DEVICE_PLAN.md §5.5: "never sent up, never overwritten by a `book`
 * push, dropped when the alias leaves the book").
 * --------------------------------------------------------------------- */
typedef struct {
    char alias[BOOK_ALIAS_MAX];
    char name[BOOK_NAME_MAX];
    char nickname[BOOK_NICK_MAX]; /* "" = unset */
    char type[BOOK_TYPE_MAX];     /* "web" | "sms" | "chat", icon hint */
} book_contact_t;

typedef struct {
    char name[BOOK_NAME_MAX];
    char status[BOOK_STATUS_MAX]; /* "pend" | "no" */
} book_request_t;

/* ---------------------------------------------------------------------
 * Pure functions — no ESP-IDF dependency.
 * --------------------------------------------------------------------- */

/* 1-16 code points, <=48 UTF-8 bytes, no control characters — the same
 * shape rule §3.1/§4.2 give `contact_req.name` and `book.c[].n`/`p[].n`. */
bool book_name_valid(const char *name, size_t len);

/* Parsed shape of one `kind:"book"` `/down` envelope — mirrors
 * lock.h's `lock_parse_cfg()` contract exactly: `buf` MUST already have
 * passed auth_verify() when signed (trailing `sig` bytes trimmed, map
 * header's declared pair count left untouched), `sig_pair_present` stands
 * in for `ident_get_flags() & IDENT_FLAG_REQ_SIG` so this stays
 * host-testable. Returns false if `buf` does not decode as a well-formed
 * `kind:"book"` envelope — covers both "not book" and "book but malformed",
 * deliberately conflated the same way lock_parse_cfg() documents (the
 * caller, book_ingest_cbor(), falls through to msg.c's own ingest either
 * way). `out->id` is truncated-away silently if it doesn't fit (only used
 * for acking, never rendered); contacts/requests beyond the cap are parsed
 * (so the buffer position stays correct) but not copied into `out`. */
typedef struct {
    char id[BOOK_ID_MAX];
    uint32_t bv;
    bool have_default;
    char default_alias[BOOK_ALIAS_MAX];
    uint8_t n_contacts;
    book_contact_t contacts[BOOK_MAX_CONTACTS]; /* .nickname always "" here — wire has no nickname */
    uint8_t n_requests;
    book_request_t requests[BOOK_MAX_REQUESTS];
} book_parsed_t;

bool book_parse(const uint8_t *buf, uint16_t len, bool sig_pair_present, book_parsed_t *out);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * RTC/auth wiring (modes.c calls this once, before book_init() need not
 * strictly precede it, but doing so first matches msg_bind_rtc()/
 * lock_bind_rtc()'s own ordering convention) — see this header's own module
 * comment for why there is no book_bind_rtc()/book_rtc_t: this binds
 * modes.c's EXISTING `g_rtc.auth` (auth_rtc_t) plus the shared cross-task
 * lock/unlock/save trio, not a new RTC sub-struct of this module's own.
 * --------------------------------------------------------------------- */
typedef void (*book_lock_fn)(void);
typedef void (*book_unlock_fn)(void);
typedef void (*book_save_fn)(void); /* must be called with the lock already held */
typedef void (*book_epoch_wrap_fn)(void); /* same contract as msg_epoch_wrap_fn, msg.h */

void book_bind(auth_rtc_t *auth_rtc, book_lock_fn lock, book_unlock_fn unlock, book_save_fn save,
               book_epoch_wrap_fn on_wrap);

/* Loads NVS namespace "book" (one blob, docs/DEVICE_PLAN.md §4.3) into this
 * module's own RAM cache. Missing/corrupt/wrong-layout-version NVS content
 * is treated as "never applied a book yet": `book_get_bv()` reads 0 (which
 * is exactly what makes the relay's own "`bv` lower than `bookVersion` ->
 * push again" rule (§4.3) self-heal a factory-reset or a never-provisioned
 * device once `/status` is first published). Power effect: a handful of NVS
 * reads, no modem/sleep-state effect. */
void book_init(void);

/* `/status`'s `bv` field (docs/PROTOCOL.md §5.1/§4.3). 0 if no book has ever
 * been applied. */
uint32_t book_get_bv(void);

/* Default recipient alias (`d`, §4.3) — what an `/up` without `to` routes
 * to. Returns false (out untouched) if the current book carries none. */
bool book_get_default_alias(char *out, size_t cap);

size_t book_contact_count(void);
/* Copies a snapshot of contact `index` (0-based, book order) into `out`.
 * False if index is out of range. Same "copy, not a live pointer" discipline
 * msg.h's README R6 fix documents for msg_thread_at() — this module's RAM
 * cache can be rewritten by book_ingest_cbor() on a different task at any
 * time. */
bool book_contact_at(size_t index, book_contact_t *out);

size_t book_request_count(void);
bool book_request_at(size_t index, book_request_t *out);

/* Device-local nickname (docs/DEVICE_PLAN.md §5.5): sets/clears (empty
 * string clears) the nickname for the contact whose alias matches `alias`.
 * Returns false if `alias` is not an approved contact in the current book,
 * or if `nickname` fails book_name_valid()-style bounds (<=12 code points /
 * 36 UTF-8 bytes; empty is always accepted as "clear"). Persists the whole
 * blob to NVS on success. Not wired to any UI by this task — F7.2's
 * Nickname screen is the first real caller. Power effect: one NVS write. */
bool book_set_nickname(const char *alias, const char *nickname, size_t nickname_len);

/* `kind:"book"` `/down` ingest (docs/PROTOCOL.md §3.2/§4.3): called from
 * modes.c's on_incoming_message() the same way lock_ingest_cfg_cbor() is —
 * BEFORE msg_ingest_down_cbor(), since a book is not a thread entry and
 * carries neither `from` nor `body`. Returns true iff `buf` decoded as a
 * `kind:"book"` envelope and was handled here (caller MUST NOT also pass it
 * to msg_ingest_down_cbor()); false means "not book (or malformed) — fall
 * through" (see book_parse()'s own doc comment for why those two cases
 * share one return value).
 *
 * On success: full replacement (docs/DEVICE_PLAN.md §4.3 "applied
 * atomically into NVS as a full replacement") of contacts/requests/`bv`/
 * default alias, EXCEPT each surviving contact's `nickname` is carried
 * forward from the previous book by matching alias (§5.5: nicknames are
 * "never overwritten by a `book` push"; a contact whose alias is no longer
 * present drops its nickname along with it). Then acks `shown`
 * (msg_mark_shown()) once applied, regardless of lock.c's lock state
 * (docs/DEVICE_PLAN.md §5.8: the lock is about the screen, not `book`/`cfg`
 * apply-and-ack; same rule lock_ingest_cfg_cbor() already documents for
 * `cfg`). Power effect: one NVS write (the whole blob) plus the ack queued
 * for msg_pump()'s next publish; no modem/sleep-state effect beyond that. */
bool book_ingest_cbor(const uint8_t *buf, uint16_t len);

/* Builds, signs (when ident's IDENT_FLAG_REQ_SIG is set) and publishes a
 * `kind:"contact_req"` `/up` envelope (docs/DEVICE_PLAN.md §4.2,
 * docs/PROTOCOL.md §3.2): `name` (1-16 code points/<=48 UTF-8 bytes,
 * book_name_valid()) is required; `ph_or_alias` (E.164 phone or an alias
 * reference — the relay's own `app/ingest.py` resolves the single wire
 * field `ph` this way: a leading `+` means phone, anything else means
 * alias, doc-commented there since docs/PROTOCOL.md §3.2's prose alone does
 * not say how an alias reference is carried) may be NULL/empty for neither.
 * Single publish attempt (no device-side retry queue — see this task's own
 * report for why: no RTC storage backs this module, matching net_publish_raw()'s
 * existing "accepted by the modem, not confirmed delivered" semantics
 * msg.c's msg_pump() already documents for every other /up publish this
 * firmware makes). Returns false without publishing on a bad `name` or a
 * `w.err`/auth_sign() failure; the relay's own rate-limit/dedup (docs/PROTOCOL.md
 * §3.2: 5 pending per device, dedup on `id`) is enforced server-side, not
 * here. Power effect: one MQTT publish (QoS 1), no modem/sleep-state effect
 * beyond that. */
bool book_request(const char *name, const char *ph_or_alias);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* BOOK_H */

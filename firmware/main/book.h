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

#define BOOK_MAX_CONTACTS 32 /* v0.4 §3.7: 10 -> 32, the §14.7 fetch response's own cap ("the
                              * device's stored capacity") — the legacy `/down book` envelope
                              * (§3.2) still caps ITS OWN `c[]` at 10 server-side (devcfg.py's
                              * MAX_APPROVED_CONTACTS), so a full-book device never actually sees
                              * more than 10 here either; this constant is sized for the pull path. */
#define BOOK_MAX_REQUESTS 4  /* §4.3 cap */
#define BOOK_URL_MAX 201     /* 200 UTF-8 bytes + NUL, §3.7/§10 key 57 (`url`, v0.4) */

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

/* Parsed shape of one `kind:"book"` envelope — either the `/down` down
 * message (§3.2/§3.7, `id`/`ack` present on the wire, `is_nudge`/`nudge_url`
 * meaningful) or the §14.7 fetch response body (no `id`/`ack`, always
 * `is_nudge == false` since a response always carries `c`/`p`, even if
 * empty arrays — see book_parse()'s own doc comment for the `have_c`/
 * `have_p` distinction that makes this exact). */
typedef struct {
    char id[BOOK_ID_MAX];
    uint32_t bv;
    bool have_default;
    char default_alias[BOOK_ALIAS_MAX];
    uint8_t n_contacts;
    book_contact_t contacts[BOOK_MAX_CONTACTS]; /* .nickname always "" here — wire has no nickname */
    uint8_t n_requests;
    book_request_t requests[BOOK_MAX_REQUESTS];
    uint64_t n;   /* key 12 `n`, when present — only meaningful for the §14.7 fetch response,
                   * which book_apply_fetched() compares against the request's own `X-N`. Absent
                   * (0) on the legacy `/down book`/nudge path, which never reads this field. */
    bool more;    /* key 20 `more`, §14.7 fetch response only — "c[] was truncated at 32
                   * server-side"; log-only, never a rejection reason (book.c/book_apply_fetched()). */
    bool is_nudge; /* v0.4 §3.7: kind:"book" with NEITHER key 18 (`c`) NOR key 19 (`p`) present
                    * (an empty `c:[]`/`p:[]` with the KEY present is still a full book, not a
                    * nudge — see `have_c`/`have_p` below). Always false for a well-formed §14.7
                    * fetch response body, which always carries both keys. */
    char nudge_url[BOOK_URL_MAX]; /* key 57 `url`, only when is_nudge — already validated against
                                   * cafetch_parse_url() by book_parse() itself (§3.7: "must pass
                                   * cafetch_parse_url"); a nudge whose url fails that check makes
                                   * book_parse() return false (malformed), not is_nudge=true with
                                   * an empty/bad url. */
} book_parsed_t;

/* Pure, no ESP-IDF dependency (book.c's own `#ifdef ESP_PLATFORM` split) —
 * declared here (rather than kept file-private, the one place this module
 * used to differ from lock.c's lock_parse_cfg()/loc.c's loc_parse_req_cbor(),
 * both of which are public for exactly this reason) so
 * firmware/host/test_bookpull.c can exercise the nudge-vs-full-book
 * detection and the §14.7 response decode directly, without ESP-IDF/NVS.
 *
 * `buf` MUST already have passed auth_verify()/auth_verify_label() when
 * signed (trailing `sig` bytes trimmed, map header's declared pair count
 * left untouched per §14.3's "no re-serialisation" rule — `sig_pair_present`
 * stands in for that, same convention lock_parse_cfg() uses). Returns false
 * for anything that does not decode as a well-formed `kind:"book"` map —
 * covers "not book", "book but malformed", AND "a nudge whose `url` is
 * missing or fails cafetch_parse_url()" (§3.7: malformed, counted, no ack) —
 * deliberately conflated the same way lock_parse_cfg() documents; the
 * caller (book_ingest_cbor()/book_apply_fetched()) falls through to the
 * ordinary malformed-`/down` handling either way. `out->id` is
 * truncated-away silently if it doesn't fit (only used for acking, never
 * rendered); contacts/requests beyond BOOK_MAX_CONTACTS/BOOK_MAX_REQUESTS
 * are parsed (so the buffer position stays correct) but not copied into
 * `out` — "keep the first N", §3.7's own truncation rule for `more`. */
bool book_parse(const uint8_t *buf, uint16_t len, bool sig_pair_present, book_parsed_t *out);

/* §5.5 nickname carry-over, factored out of book_ingest_cbor()'s old
 * inline loop so book_apply_fetched() (§14.7) can reuse the EXACT same
 * logic ("same nickname carry-over as book_ingest_cbor", this header's own
 * book_apply_fetched() doc comment below) — and so it is host-testable
 * (test_bookpull.c) without NVS: pure array operation over caller-owned
 * memory, book_contact_t has no ESP-IDF dependency of its own. For each of
 * `new_contacts[0..new_n)`, if its `alias` matches one of
 * `old_contacts[0..old_n)`, copies that old entry's `nickname` into the new
 * one in place; a new contact whose alias was not in the old book keeps
 * whatever `nickname` it already had (always "" for a freshly wire-decoded
 * contact, since the wire never carries one). `new_contacts`/`old_contacts`
 * may be the same array only if `new_n <= old_n` and indices are not
 * reordered between calls — callers here never do that (always a fresh
 * `book_parsed_t.contacts` against the previously-applied `s_book.contacts`
 * snapshot). */
void book_carry_nicknames(book_contact_t *new_contacts, uint8_t new_n, const book_contact_t *old_contacts,
                          uint8_t old_n);

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

/* `kind:"book"` `/down` ingest (docs/PROTOCOL.md §3.2/§3.7/§4.3): called
 * from modes.c's on_incoming_message() the same way lock_ingest_cfg_cbor()
 * is — BEFORE msg_ingest_down_cbor(), since neither shape below is a thread
 * entry, and neither carries `from` or `body`. Returns true iff `buf`
 * decoded as a `kind:"book"` envelope and was handled here (caller MUST NOT
 * also pass it to msg_ingest_down_cbor()); false means "not book (or
 * malformed) — fall through" (see book_parse()'s own doc comment for why
 * those two cases share one return value, and for the third case that also
 * returns false: a v0.4 nudge whose `url` is missing/invalid).
 *
 * Two wire shapes, distinguished by book_parse()'s own `is_nudge` (§3.7):
 *
 * - **A v0.4 nudge** (`is_nudge == true`): this function does NOT touch
 *   s_book/NVS at all — it hands `{id, bv, nudge_url}` straight to
 *   bookpull_on_nudge() (bookpull.h), which implements §3.7's device rule
 *   1-2 (ack now if `bv` <= the stored book's, else record a pending fetch
 *   for bookpull_service() to act on), and returns true. No NVS write, no
 *   ack from THIS function either way — bookpull.c acks (or not) once the
 *   rule has been applied. Power effect: none beyond bookpull_on_nudge()'s
 *   own (a RAM write, or an ack enqueue — no modem/sleep-state effect).
 *
 * - **A full book** (`is_nudge == false`, `c`/`p` present — §3.2, still
 *   applied by a pull-capable device if one arrives): full replacement
 *   (docs/DEVICE_PLAN.md §4.3 "applied atomically into NVS as a full
 *   replacement") of contacts/requests/`bv`/default alias, EXCEPT each
 *   surviving contact's `nickname` is carried forward from the previous
 *   book by matching alias (book_carry_nicknames(), §5.5: nicknames are
 *   "never overwritten by a `book` push"; a contact whose alias is no
 *   longer present drops its nickname along with it). Then acks `shown`
 *   (msg_mark_shown()) once applied, regardless of lock.c's lock state
 *   (docs/DEVICE_PLAN.md §5.8: the lock is about the screen, not `book`/
 *   `cfg` apply-and-ack; same rule lock_ingest_cfg_cbor() already documents
 *   for `cfg`). Power effect: one NVS write (the whole blob) plus the ack
 *   queued for msg_pump()'s next publish; no modem/sleep-state effect
 *   beyond that. */
bool book_ingest_cbor(const uint8_t *buf, uint16_t len);

/* §3.7/§14.7 fetch response: applies `body[0..len)` — a CBOR map whose
 * trailing `sig` MUST already have been trimmed by the caller
 * (auth_verify_label("/api/device/book", ...), bookpull.c's job, mirroring
 * on_incoming_message()'s existing auth_verify()-then-book_ingest_cbor()
 * split for the `/down` path — this function never touches signature
 * bytes). Decodes via book_parse() (sig_pair_present is always true here:
 * the §14.7 response is always signed, unconditionally, unlike
 * book_ingest_cbor()'s ident-flag-conditional caller — book pull only ever
 * runs for an `authMode: "hmac"` device, §3.7's capability gate), then
 * checks, in order: well-formed `kind:"book"` (book_parse()'s own return
 * value), `n` (key 12) == `expect_n` (the request's own `X-N` — §14.7 "the
 * device verifies the tag and the `n` echo"), `bv` (key 14) >= `min_bv`
 * (the nudge's own `bv` — §3.7 rule 2). Any failure: returns false, touches
 * NEITHER s_book NOR NVS — the caller (bookpull.c) keeps the nudge pending
 * and does not ack (§3.7 rule 2 / §14.7's failure table).
 *
 * On success: same full-replacement + nickname carry-over as
 * book_ingest_cbor()'s full-book path (book_carry_nicknames()), `c[]`
 * already capped at BOOK_MAX_CONTACTS (32) by book_parse() itself ("keep
 * the first 32", §3.7's own truncation rule — `more` (key 20) is logged
 * only, never a rejection reason), NVS write included. Does NOT call
 * msg_mark_shown() itself — the response carries no `id` at all ("There is
 * no `id` and no `ack`: the response is not a down message and is never
 * acked itself; the nudge is", §3.7) — the caller acks the ORIGINAL
 * nudge's `id` (which this function never sees) once this returns true.
 * Power effect: one NVS write on success, none on failure. */
bool book_apply_fetched(const uint8_t *body, size_t len, uint64_t expect_n, uint32_t min_bv);

/* Draws the NEXT value of the one shared /up,/status,/loc counter (§14.2) —
 * "the SAME counter... it is spent" (§3.7 Do step 3) — for bookpull.c's
 * §14.7 request `X-N`, the same RTC-resident auth_rtc_t (g_rtc.auth) and
 * cross-task lock/save trio book_bind() already wired up for
 * `contact_req`'s own signed publish. `wrapped` has the same "caller MUST
 * bump/persist a new n_epoch" contract as auth_next_up_n() itself. Returns
 * 0 if book_bind() has never run (no auth_rtc_t bound yet) — bookpull.c
 * treats that as "cannot fetch right now", same as a bad URL. Power effect:
 * none beyond the RTC write auth_next_up_n()/the save callback already
 * cost. */
uint64_t book_next_up_n(bool *wrapped);

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

/* bookpull.h — v0.4 book pull, docs/PROTOCOL.md §3.7 (nudge, device rule,
 * fetch, response) and §14.7 (authenticating the HTTPS request/response).
 * `docs/CHAT_UI_DESIGN.md` decision 4, task T1f.
 *
 * Split the same way catrust.c/loc.c already are: everything above the
 * `#ifdef ESP_PLATFORM` banner is pure C, no ESP-IDF dependency, so
 * firmware/host/test_bookpull.c can run it without a device. It owns:
 *
 *   - Device rule 1 (§3.7): "nudge bv <= stored bv -> ack now, no fetch" —
 *     a plain comparison, but named so the rule lives in exactly one place.
 *   - §14.7's request construction: `M = "<device_id>|<n>|<bv>"`, the
 *     base64url(no padding) encoding of the 8-byte tag auth_request_tag()
 *     returns, and the three `X-Device-Id`/`X-N`/`X-Sig` header lines handed
 *     to cafetch_begin_ex()'s `extra_hdrs`.
 *   - §14.7's "Device on failure" table, as a pure HTTP-status classifier.
 *
 * Nudge *parsing* (bv+url, "neither c nor p is a nudge") lives in book.c's
 * own book_parse() (book.h), not here — see book.h's own doc comment for
 * why book_parse() is public rather than file-private (the one thing that
 * used to differ from lock.c's lock_parse_cfg()/loc.c's loc_parse_req_cbor()):
 * a nudge is still one CBOR shape of `kind:"book"`, and book.c is where
 * every other book shape is already decoded, so this module receives
 * `{id, bv, url}` already extracted (bookpull_on_nudge(), below the banner)
 * rather than re-parsing the envelope itself.
 *
 * Below the banner: the device-only nudge state (one pending nudge, RAM
 * only — "losing it on reset is fine, the relay re-publishes on session
 * change", §3.7 Do step 2) and the one-fetch-at-a-time state machine driven
 * by bookpull_service() from modes_run()'s own loop, mirroring catrust.c's
 * own "never the whole attempt in one call" discipline and literally
 * reusing its cafetch.c plumbing (cafetch_begin_ex()/cafetch_poll()/
 * cafetch_body()/cafetch_end() — cafetch_in_progress() is the single-flight
 * guard shared between the two: only a CA fetch or a book fetch runs at a
 * time, never both).
 */
#ifndef BOOKPULL_H
#define BOOKPULL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Device rule 1 (§3.7).
 * --------------------------------------------------------------------- */

/* True means "ack `id` now, no fetch needed" (nudge_bv <= stored_bv). Pure
 * comparison, no state. */
bool bookpull_rule1_ack_now(uint32_t nudge_bv, uint32_t stored_bv);

/* ---------------------------------------------------------------------
 * §14.7 request construction.
 * --------------------------------------------------------------------- */

#define BOOKPULL_M_MAX 64        /* "<=24-char device id>|<=20-digit n>|<=10-digit bv>" + NUL */
#define BOOKPULL_SIG_B64_LEN 11  /* 8 raw tag bytes, base64url, no padding — §14.3's own arithmetic */
#define BOOKPULL_HDRS_MAX 128    /* X-Device-Id/X-N/X-Sig lines, generous */

/* `M = ASCII "<device_id>|<n>|<bv>"` (§14.7), written into `out`
 * (NUL-terminated). Returns false (out untouched) if it would not fit
 * `cap`. */
bool bookpull_build_m(char *out, size_t cap, const char *device_id, uint64_t n, uint32_t bv);

/* base64url, no padding, of an 8-byte tag — always exactly
 * BOOKPULL_SIG_B64_LEN characters + NUL (§14.3's arithmetic: ceil(8*4/3)
 * with the trailing `=` dropped). `out` must have room for
 * BOOKPULL_SIG_B64_LEN+1 bytes. */
void bookpull_b64url_tag(const uint8_t tag[8], char out[BOOKPULL_SIG_B64_LEN + 1]);

/* Builds the three request headers cafetch_begin_ex()'s `extra_hdrs` wants
 * (§14.7): "X-Device-Id: <id>\r\nX-N: <n>\r\nX-Sig: <sig_b64>\r\n" — `n` is
 * formatted decimal, no leading zeros (matches §14.7's own "X-N ... decimal,
 * no leading zeros" for any n, since %llu never produces one). Returns
 * false (out untouched) if it would not fit `cap`. */
bool bookpull_build_headers(char *out, size_t cap, const char *device_id, uint64_t n, const char *sig_b64);

/* ---------------------------------------------------------------------
 * §14.7 "Device on failure" table.
 * --------------------------------------------------------------------- */
typedef enum {
    BOOKPULL_HTTP_APPLY = 0, /* 200 -- verify + apply is the caller's job, not this classifier's */
    BOOKPULL_HTTP_RETRY_NOW, /* 409 -- "retry once, immediately, with a fresh n" */
    BOOKPULL_HTTP_DROP,      /* 400/401/404 -- "log and count, no retry until the next nudge" */
    BOOKPULL_HTTP_RETRY_LATER, /* 5xx (or anything else this table does not name) -- "no ack; retry
                                 * on the next nudge or online edge, at most once per 60s" */
} bookpull_http_outcome_t;

/* Pure classification of an HTTP status code per §14.7's failure table.
 * `cafetch`-layer failures that never produced a real status code
 * (timeout, oversize body, a malformed response) are reported by the
 * caller as status 0, which this function also maps to
 * BOOKPULL_HTTP_RETRY_LATER — "5xx, a timeout, an oversize body or a
 * verification failure" are grouped identically by §14.7's own prose. */
bookpull_http_outcome_t bookpull_classify_http(int http_status);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * Device wiring — one fetch in flight at a time (module-static state),
 * driven from modes_run()'s own loop via bookpull_service(), alongside
 * catrust_service()/loc_service().
 * --------------------------------------------------------------------- */

typedef void (*bookpull_lock_fn)(void);
typedef void (*bookpull_unlock_fn)(void);

/* Binds the cross-task mutex modes.c already owns (same reuse pattern
 * catrust_bind() documents) — this module's own RAM state (the one pending
 * nudge) is written from the MQTT event task (bookpull_on_nudge(), called
 * from book.c's book_ingest_cbor()) and read/advanced from modes_run()'s
 * task (bookpull_service()). Call once, before the first nudge could
 * possibly arrive (modes_boot(), alongside catrust_bind()). No RTC pointer
 * here (contrast book_bind()): this module keeps no RTC-resident state of
 * its own — see this header's own module comment. */
void bookpull_bind(bookpull_lock_fn lock, bookpull_unlock_fn unlock);

/* Called from book.c's book_ingest_cbor() when book_parse() decoded a
 * v0.4 nudge (`is_nudge == true`) — see book.h's own book_ingest_cbor() doc
 * comment. Implements §3.7's device rule 1-2: `bv <= book_get_bv()` acks
 * `id` immediately (msg_mark_shown()) and returns without touching any
 * pending-fetch state; otherwise records `{id, bv, url}` as the one pending
 * nudge (RAM only, newest always wins — a fetch already in flight for an
 * OLDER nudge is not interrupted, per rule 4: it is simply re-evaluated
 * against whatever is pending once it ends). Runs on the MQTT event task.
 * Power effect: a msg_mark_shown() ack enqueue, or a RAM write — no modem/
 * sleep-state effect either way. */
void bookpull_on_nudge(const char *id, uint32_t bv, const char *url);

/* One non-blocking step of the pending-nudge / fetch state machine. Call
 * every modes_run() iteration, alongside catrust_service() (cafetch.c's
 * single-flight guard, cafetch_in_progress(), is shared between the two —
 * this call is a no-op whenever catrust owns it, and vice versa, retried
 * the next iteration). Power effect: none when idle; while fetching, the
 * same per-step cost cafetch_poll() itself already documents (none when
 * nothing is pending on the socket, one bounded AT round trip when a RING
 * was). */
void bookpull_service(void);

/* A thin passthrough to cafetch_in_progress() (cafetch.h) — declared here so
 * modes.c's skip_sleep computation (§3.7 Do step 6: "a pending fetch holds
 * the loop out of light sleep only while cafetch_in_progress() (<=30s)")
 * does not need its own #include "cafetch.h" just for this one check. Also
 * true while catrust.c owns a CA fetch through the same shared guard — that
 * is a pre-existing gap this task does not change (catrust.c's own fetches
 * were never gated into skip_sleep before this task either), simply a side
 * effect of the two modules sharing one guard; harmless (it can only ever
 * make the device wait a *bit* longer for an already-rare CA push to
 * finish, never the reverse). */
bool bookpull_fetch_in_progress(void);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* BOOKPULL_H */

/* cafetch.h — CA-over-HTTPS fetch (docs/V02_DESIGN.md §4.4, docs/CA_TRUST_PLAN.md §3.4).
 *
 * Split the same way lock.c/loc.c/setup.c already are: everything above the
 * `#ifdef ESP_PLATFORM` banner is pure C, no ESP-IDF dependency — the HTTP/1.1
 * response parser (status line, headers, `Content-Length` and
 * `Transfer-Encoding: chunked` bodies, a `Connection: close` end-of-body with
 * neither), the URL parser (`https://host[:port]/path` only), the "exactly
 * one PEM certificate" structural check and the SHA-256 comparison — all
 * host-tested by firmware/host/test_cafetch.c, including the case that split
 * a header or a chunk boundary across two `cafetch_feed()` calls, since that
 * is exactly how bytes arrive off the modem's socket (WalterSocket.cpp: a
 * `+SQNSRING` URC announces N bytes available, `socketReceive()` returns at
 * most 1500 of them per call, so a caller must be able to feed this parser
 * in arbitrary-sized pieces).
 *
 * Below the banner: the device-only socket driver, built on net.h's small
 * `net_ca_fetch_*()` facade (TLS profile 3, validation off, cert slot 12
 * still named — BRINGUP_NOTES.md's rule applies to this profile too, even
 * though nothing validates against it here: trust comes from the SHA-256
 * check above, not from the transport, docs/CA_TRUST_PLAN.md §3.4). The
 * non-blocking `cafetch_begin()`/`cafetch_poll()`/`cafetch_result()`/
 * `cafetch_end()` quartet is meant to be driven one small step per call from
 * modes_run()'s own loop (catrust.c's `catrust_service()`, mirroring
 * loc_service()'s "never the whole attempt in one call" discipline) so a
 * slow or stalled fetch cannot block paging; `cafetch_run_blocking()` is a
 * convenience wrapper (vTaskDelay-polls the same non-blocking quartet — not
 * a busy-wait) for callers that already own a dedicated task and can afford
 * to block it: setup.c's one-shot bootstrap task and the `cafetch` debug
 * console command (main.c, PAGER_DEBUG_NO_LIGHT_SLEEP builds only).
 *
 * `UNVERIFIED` (docs/CA_TRUST_PLAN.md §3.4): a second socket (this one) while
 * the MQTT session is up. The `cafetch` debug command logs whether the MQTT
 * session survived — see cafetch_run_blocking()'s own doc comment.
 */
#ifndef CAFETCH_H
#define CAFETCH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing (docs/V02_DESIGN.md §4.4).
 * --------------------------------------------------------------------- */
#define CAFETCH_HOST_MAX 128   /* matches IDENT_HOST_MAX */
#define CAFETCH_PATH_MAX 256   /* "/ca/<64 hex>.pem" is 68 bytes; generous headroom */
#define CAFETCH_BODY_MAX 4096  /* "cap the body at 4096 bytes, refuse over" */
#define CAFETCH_LINE_MAX 512   /* one HTTP status/header line; refuse anything longer as malformed */
#define CAFETCH_SHA_LEN 32
#define CAFETCH_PEM_MAX (CAFETCH_BODY_MAX + 1)

/* ---------------------------------------------------------------------
 * URL parsing — "https://host[:port]/path only" (V02_DESIGN.md §4.4).
 * --------------------------------------------------------------------- */

/* Returns false for anything that is not exactly `https://host[:port]/path`
 * (no scheme other than https, no missing path, a port outside 1..65535, a
 * host or path that does not fit the *_out buffers). Port defaults to 443
 * when absent. `path_out` includes the leading '/'. */
bool cafetch_parse_url(const char *url, char *host_out, size_t host_cap, uint16_t *port_out,
                       char *path_out, size_t path_cap);

/* Builds "GET <path> HTTP/1.1\r\nHost: <host>\r\nConnection: close\r\n\r\n"
 * into `out` (NUL-terminated). Returns false (out untouched) if it would not
 * fit `cap`. */
bool cafetch_build_request(const char *host, const char *path, char *out, size_t cap,
                           size_t *out_len);

/* ---------------------------------------------------------------------
 * Incremental HTTP/1.1 response parser.
 * --------------------------------------------------------------------- */

typedef enum {
    CAFETCH_PHASE_STATUS_LINE = 0,
    CAFETCH_PHASE_HEADER_LINE,
    CAFETCH_PHASE_BODY_LENGTH,        /* Content-Length known */
    CAFETCH_PHASE_BODY_UNTIL_CLOSE,   /* neither Content-Length nor chunked */
    CAFETCH_PHASE_CHUNK_SIZE_LINE,
    CAFETCH_PHASE_CHUNK_DATA,
    CAFETCH_PHASE_CHUNK_CRLF,         /* the CRLF that follows each chunk's data */
    CAFETCH_PHASE_CHUNK_TRAILER,      /* trailer header lines after the 0-length chunk */
    CAFETCH_PHASE_DONE,
    CAFETCH_PHASE_ERROR,
} cafetch_phase_t;

typedef struct {
    cafetch_phase_t phase;

    char line[CAFETCH_LINE_MAX];
    size_t line_len;

    int status_code;
    bool have_content_length;
    size_t content_length;
    bool chunked;

    uint8_t body[CAFETCH_BODY_MAX];
    size_t body_len;

    size_t chunk_remaining; /* CHUNK_DATA: bytes left in the current chunk */

    /* Sticky failure reasons, for the caller's log line. Only one is ever
     * set (the first one hit); `phase` becomes CAFETCH_PHASE_ERROR either
     * way. */
    bool err_oversize;   /* body (or a declared Content-Length) > CAFETCH_BODY_MAX */
    bool err_malformed;  /* could not parse the status line/headers/chunk framing */
    bool err_non200;     /* status line parsed fine but code != 200 (redirects included) */
} cafetch_parser_t;

void cafetch_parser_init(cafetch_parser_t *p);

/* Feeds `len` more bytes (may be zero — a no-op). Advances `p->phase` as far
 * as the data allows; safe to call repeatedly with arbitrarily small or
 * large chunks, including a chunk that splits a header or chunk-size line
 * mid-way. Returns false once `p->phase == CAFETCH_PHASE_ERROR` (including
 * on the very call that caused it) — the caller should stop feeding and
 * report failure via the `err_*` flags; true otherwise (including once
 * already CAFETCH_PHASE_DONE, a no-op). */
bool cafetch_parser_feed(cafetch_parser_t *p, const uint8_t *data, size_t len);

/* Signals that the underlying socket has closed (a `Connection: close`
 * server, or any other disconnect). Only meaningful in
 * CAFETCH_PHASE_BODY_UNTIL_CLOSE (ends the body cleanly -> DONE); in every
 * other phase a close before CAFETCH_PHASE_DONE is a truncated response
 * (-> ERROR/err_malformed). No-op once already DONE or ERROR. */
void cafetch_parser_closed(cafetch_parser_t *p);

/* ---------------------------------------------------------------------
 * Body validation: exactly one PEM certificate, hashed exactly as received.
 * --------------------------------------------------------------------- */

/* `body`/`body_len` MUST be hashed and scanned exactly as delivered — "hash
 * the body bytes exactly as received, the relay serves exactly the bytes it
 * hashed" (V02_DESIGN.md §4.4/§8): no trimming of trailing whitespace/
 * newlines before the SHA-256 comparison, tolerant of them being present
 * (the relay's own file may or may not end in one). Verifies, in order: (1)
 * SHA-256(body) == expected_sha32 (mbedtls_sha256, real mbedtls both host
 * and device, same convention lock.c/setup.c already use), (2) the body
 * contains exactly one `-----BEGIN CERTIFICATE-----` marker, matched by
 * exactly one `-----END CERTIFICATE-----` after it. On success, copies the
 * substring from the BEGIN marker through the END marker (inclusive, plus
 * one trailing newline if present) into `pem_out` (NUL-terminated) — this is
 * what net_write_ca_slot()/ident_t.ca expect, not necessarily byte-identical
 * to `body` if the relay ever wrapped the PEM in extra whitespace (tolerated
 * by the marker scan, not by the hash, which always covers the whole body).
 * Returns false (pem_out untouched) on a hash mismatch, zero or more than
 * one PEM block, or `pem_out` too small. */
bool cafetch_validate_body(const uint8_t *body, size_t body_len, const uint8_t expected_sha[CAFETCH_SHA_LEN],
                          char *pem_out, size_t pem_cap, size_t *pem_len);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * Device wiring — one fetch in flight at a time (module-static state, same
 * "only one attempt in flight" discipline loc.c's GNSS attempt uses), driven
 * from net.h's net_ca_fetch_*() facade (TLS profile 3, PAGER_TLS_CA_SLOT
 * named, socket id distinct from nettest's).
 * --------------------------------------------------------------------- */

/* Parses `url`, opens the cafetch socket (net_ca_fetch_open()) and sends the
 * GET request (net_ca_fetch_send()). False means it could not even start
 * (bad URL, or the socket/TLS layer refused) — already logged. Fails
 * immediately (false) if a fetch is already in progress (cafetch_in_progress()). */
bool cafetch_begin(const char *url);

typedef enum {
    CAFETCH_PENDING = 0, /* still waiting on more data or on the 30s overall timeout */
    CAFETCH_OK,          /* full body received (status 200, within every cap) — call cafetch_result() */
    CAFETCH_FAILED,      /* malformed/oversize/non-200/timeout/socket error — already logged */
} cafetch_status_t;

/* One non-blocking step: polls net_ca_fetch_poll() once (at most one
 * <=1500-byte socketReceive() AT round trip) and feeds whatever arrived to
 * the parser. `now_us` (esp_timer_get_time()-shaped) is compared against the
 * 30s overall budget started by cafetch_begin(). Call this every
 * modes_run() iteration (via catrust.c's catrust_service()) while a fetch is
 * in progress — never loops or blocks itself. */
cafetch_status_t cafetch_poll(int64_t now_us);

/* Valid only right after cafetch_poll() returns CAFETCH_OK. Runs
 * cafetch_validate_body() against the accumulated body. `out_http_status`/
 * `out_bytes` are filled regardless of the hash outcome (for logging).
 * Returns false on a hash/PEM-shape failure (already logged by the caller,
 * not here — this function has no log dependency so it stays reusable). */
bool cafetch_result(const uint8_t expected_sha[CAFETCH_SHA_LEN], char *pem_out, size_t pem_cap,
                    size_t *pem_len, int *out_http_status, size_t *out_bytes);

/* Tears down the cafetch socket and resets state. MUST be called after
 * CAFETCH_OK or CAFETCH_FAILED (or to abort a CAFETCH_PENDING fetch early)
 * before the next cafetch_begin(). */
void cafetch_end(void);

bool cafetch_in_progress(void);

/* Blocking convenience for a caller that owns its own task (setup.c's
 * bootstrap fetch, the `cafetch` debug console command): begin() + a
 * vTaskDelay-polled loop (FreeRTOS primitive, not a busy-wait) around
 * poll()/result()/end(), bounded by the same 30s budget cafetch_poll()
 * itself enforces. `out_mqtt_survived`, when non-NULL, is filled from
 * net_get_mqtt_status() taken immediately before and after the fetch —
 * *out_mqtt_survived is true iff the session was connected both times
 * (false if it was never connected to begin with) — the
 * `UNVERIFIED`/`cafetch` debug command's own "did opening a second socket
 * disturb the MQTT session" check (docs/CA_TRUST_PLAN.md §3.4). */
bool cafetch_run_blocking(const char *url, const uint8_t expected_sha[CAFETCH_SHA_LEN], char *pem_out,
                         size_t pem_cap, size_t *pem_len, int *out_http_status, size_t *out_bytes,
                         uint32_t *out_elapsed_ms, bool *out_mqtt_survived);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* CAFETCH_H */

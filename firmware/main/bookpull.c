/* bookpull.c — see bookpull.h for the module split and every function's
 * doc comment.
 *
 * All power-effect comments are PENDING_HW.
 */
#include "bookpull.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------
 * Pure functions (no ESP-IDF dependency) — host-tested by
 * firmware/host/test_bookpull.c.
 * --------------------------------------------------------------------- */

bool bookpull_rule1_ack_now(uint32_t nudge_bv, uint32_t stored_bv)
{
    return nudge_bv <= stored_bv;
}

bool bookpull_build_m(char *out, size_t cap, const char *device_id, uint64_t n, uint32_t bv)
{
    int written = snprintf(out, cap, "%s|%llu|%u", device_id ? device_id : "",
                           (unsigned long long) n, (unsigned) bv);
    return written >= 0 && (size_t) written < cap;
}

static const char k_b64url_alphabet[65] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

void bookpull_b64url_tag(const uint8_t tag[8], char out[BOOKPULL_SIG_B64_LEN + 1])
{
    // §14.3: 8 raw bytes -> two full 3-byte groups (6 bytes, 8 chars) + one
    // 2-byte remainder group (3 chars, no padding char emitted) = 11 chars.
    int oi = 0;
    for (int i = 0; i < 6; i += 3) {
        uint32_t v = ((uint32_t) tag[i] << 16) | ((uint32_t) tag[i + 1] << 8) | tag[i + 2];
        out[oi++] = k_b64url_alphabet[(v >> 18) & 0x3F];
        out[oi++] = k_b64url_alphabet[(v >> 12) & 0x3F];
        out[oi++] = k_b64url_alphabet[(v >> 6) & 0x3F];
        out[oi++] = k_b64url_alphabet[v & 0x3F];
    }
    uint32_t v = ((uint32_t) tag[6] << 16) | ((uint32_t) tag[7] << 8);
    out[oi++] = k_b64url_alphabet[(v >> 18) & 0x3F];
    out[oi++] = k_b64url_alphabet[(v >> 12) & 0x3F];
    out[oi++] = k_b64url_alphabet[(v >> 6) & 0x3F];
    out[oi] = '\0';
}

bool bookpull_build_headers(char *out, size_t cap, const char *device_id, uint64_t n, const char *sig_b64)
{
    int written = snprintf(out, cap, "X-Device-Id: %s\r\nX-N: %llu\r\nX-Sig: %s\r\n",
                           device_id ? device_id : "", (unsigned long long) n, sig_b64 ? sig_b64 : "");
    return written >= 0 && (size_t) written < cap;
}

bookpull_http_outcome_t bookpull_classify_http(int http_status)
{
    switch (http_status) {
    case 409:
        return BOOKPULL_HTTP_RETRY_NOW;
    case 400:
    case 401:
    case 404:
        return BOOKPULL_HTTP_DROP;
    case 200:
        return BOOKPULL_HTTP_APPLY;
    default:
        // 5xx, 0 (cafetch-layer timeout/oversize/malformed, never got a real
        // status), and anything else this table does not name — §14.7:
        // "5xx, a timeout, an oversize body or a verification failure ->
        // no ack; retry ... at most once per 60s".
        return BOOKPULL_HTTP_RETRY_LATER;
    }
}

/* ======================================================================
 * Device wiring — needs book.h, auth.h, cafetch.h, msg.h, net.h, esp_timer,
 * FreeRTOS's absence-of-busy-wait discipline only (no direct FreeRTOS API
 * used here — bookpull_service() is driven by modes_run()'s own loop, same
 * as catrust_service()).
 * ====================================================================== */
#ifdef ESP_PLATFORM

#include "auth.h"
#include "book.h"
#include "cafetch.h"
#include "msg.h"
#include "net.h"

#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "bookpull";

#define BOOKPULL_RETRY_MIN_US ((int64_t) 60 * 1000000) /* §3.7/§14.7: "at most once per 60s" */
#define BOOKPULL_REQUEST_TOPIC "GET /api/device/book"  /* §14.7 */
#define BOOKPULL_RESPONSE_LABEL "/api/device/book"     /* §14.7 response signature */

// ---------------------------------------------------------------------------
// Cross-task binding — see bookpull.h's own module comment.
// ---------------------------------------------------------------------------
static void bookpull_lock_noop(void) {}
static bookpull_lock_fn s_lock = bookpull_lock_noop;
static bookpull_unlock_fn s_unlock = bookpull_lock_noop;

void bookpull_bind(bookpull_lock_fn lock, bookpull_unlock_fn unlock)
{
    s_lock = lock;
    s_unlock = unlock;
}

// ---------------------------------------------------------------------------
// Pending nudge (newest always wins), RAM only — §3.7 Do step 2: "losing it
// on reset is fine — relay re-publishes on session change". Guarded by
// s_lock/s_unlock: written from bookpull_on_nudge() (MQTT event task),
// read/cleared from bookpull_service() (modes_run()'s task).
// ---------------------------------------------------------------------------
static bool s_pending_present = false;
static uint32_t s_pending_bv = 0;
static char s_pending_id[BOOK_ID_MAX];
static char s_pending_url[BOOK_URL_MAX];

// In-flight fetch record — modes_run()'s task only, no lock needed (only
// bookpull_service() and its own static helpers below ever touch it).
typedef enum {
    BOOKPULL_IDLE = 0,
    BOOKPULL_FETCHING,
} bookpull_phase_t;

static bookpull_phase_t s_phase = BOOKPULL_IDLE;
static char s_fetch_id[BOOK_ID_MAX];
static uint32_t s_fetch_bv = 0;
static char s_fetch_url[BOOK_URL_MAX];
static uint64_t s_fetch_n = 0;
static int64_t s_fetch_started_us = 0;
static bool s_fetch_mqtt_before = false; // net_get_mqtt_status() taken at start_fetch() time —
                                         // the "before" half of the UNVERIFIED second-socket
                                         // log line, cafetch.h's own module comment.
static bool s_retry_409_once = false; // §14.7: "409 -> retry once, immediately, with a fresh n"
static int64_t s_last_attempt_us = 0; // 0 = never attempted this boot

// Response body copy, own buffer (not cafetch.c's internal one, which
// cafetch_end() resets) — sized to the same 4096-byte cap §3.7 gives the
// response body.
static uint8_t s_resp_buf[CAFETCH_BODY_MAX];

void bookpull_on_nudge(const char *id, uint32_t bv, const char *url)
{
    uint32_t stored_bv = book_get_bv();
    if (bookpull_rule1_ack_now(bv, stored_bv)) {
        // §3.7 rule 1: ack `id` now, no fetch.
        if (id && id[0] != '\0') {
            msg_mark_shown(id);
        }
        ESP_LOGI(TAG, "nudge bv=%u <= stored bv=%u - acked, no fetch", (unsigned) bv, (unsigned) stored_bv);
        return;
    }

    // §3.7 rule 2: record pending (newest always wins — this simply
    // overwrites whatever was there, including one already being serviced
    // by an in-flight fetch's own s_fetch_* snapshot, which is untouched by
    // this).
    s_lock();
    s_pending_present = true;
    s_pending_bv = bv;
    strncpy(s_pending_id, id ? id : "", sizeof(s_pending_id) - 1);
    s_pending_id[sizeof(s_pending_id) - 1] = '\0';
    strncpy(s_pending_url, url ? url : "", sizeof(s_pending_url) - 1);
    s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    s_unlock();
    ESP_LOGI(TAG, "nudge bv=%u > stored bv=%u - pending fetch", (unsigned) bv, (unsigned) stored_bv);
}

bool bookpull_fetch_in_progress(void)
{
    return cafetch_in_progress();
}

// Puts {id, bv, url} back as the pending nudge, UNLESS a newer one already
// landed while the just-finished fetch was in flight (§3.7 rule 4: that
// newer one wins outright — re-adding the one that just failed would go
// backwards).
static void requeue_pending(const char *id, uint32_t bv, const char *url)
{
    s_lock();
    if (!s_pending_present) {
        s_pending_present = true;
        s_pending_bv = bv;
        strncpy(s_pending_id, id, sizeof(s_pending_id) - 1);
        s_pending_id[sizeof(s_pending_id) - 1] = '\0';
        strncpy(s_pending_url, url, sizeof(s_pending_url) - 1);
        s_pending_url[sizeof(s_pending_url) - 1] = '\0';
    }
    s_unlock();
}

static void start_fetch(const char *id, uint32_t bv, const char *url)
{
    bool wrapped = false;
    uint64_t n = book_next_up_n(&wrapped); // §3.7 Do step 3: "the SAME counter /up,/status,/loc
                                            // use; it is spent" — book_next_up_n() itself handles
                                            // the epoch-wrap bump, same as book_request()'s own
                                            // signed-publish path.

    char m[BOOKPULL_M_MAX];
    char sig_b64[BOOKPULL_SIG_B64_LEN + 1];
    char hdrs[BOOKPULL_HDRS_MAX];
    uint8_t tag[8];
    const char *device_id = net_get_device_id();

    s_last_attempt_us = esp_timer_get_time(); // counts as an attempt even if request-building
                                               // fails below — a device_id/n/bv combination this
                                               // pathological will not fix itself by retrying
                                               // sooner than 60s.

    bool ok = bookpull_build_m(m, sizeof(m), device_id, n, bv) &&
             auth_request_tag(BOOKPULL_REQUEST_TOPIC, (const uint8_t *) m, strlen(m), tag);
    if (ok) {
        bookpull_b64url_tag(tag, sig_b64);
        ok = bookpull_build_headers(hdrs, sizeof(hdrs), device_id, n, sig_b64);
    }
    if (!ok) {
        ESP_LOGI(TAG, "book fetch: could not build the signed request (M/tag/headers) - dropping");
        return; // stays IDLE; nudge already consumed from s_pending_* by the caller
    }

    char full_url[BOOK_URL_MAX + 24];
    int n_written = snprintf(full_url, sizeof(full_url), "%s?bv=%u", url, (unsigned) bv);
    if (n_written < 0 || (size_t) n_written >= sizeof(full_url)) {
        ESP_LOGI(TAG, "book fetch: url+query too long for '%s' - dropping", url);
        return;
    }

    // Power effect: one TLS handshake (VALIDATION_NONE, profile 3) plus the
    // RRC time it takes — same cost catrust.c's own CA fetch already
    // documents (cafetch_begin_ex()/net_ca_fetch_open()); cafetch_in_progress()
    // is the shared single-flight guard that keeps the two from overlapping
    // (bookpull_service() never reaches here while it is already true —
    // see its own caller).
    net_mqtt_status_t mqtt_before = { 0 };
    net_get_mqtt_status(&mqtt_before);

    if (!cafetch_begin_ex(full_url, hdrs)) {
        ESP_LOGI(TAG, "book fetch: cafetch_begin_ex() failed - will retry >=60s from now");
        return;
    }

    s_fetch_mqtt_before = mqtt_before.mqtt_connected;
    strncpy(s_fetch_id, id, sizeof(s_fetch_id) - 1);
    s_fetch_id[sizeof(s_fetch_id) - 1] = '\0';
    strncpy(s_fetch_url, url, sizeof(s_fetch_url) - 1);
    s_fetch_url[sizeof(s_fetch_url) - 1] = '\0';
    s_fetch_bv = bv;
    s_fetch_n = n;
    s_fetch_started_us = esp_timer_get_time();
    s_retry_409_once = false;
    s_phase = BOOKPULL_FETCHING;
    ESP_LOGI(TAG, "book fetch: GET %s (bv wanted=%u, n=%llu)", full_url, (unsigned) bv,
             (unsigned long long) n);
}

static void service_idle(void)
{
    bool present;
    uint32_t bv;
    char id[BOOK_ID_MAX];
    char url[BOOK_URL_MAX];
    s_lock();
    present = s_pending_present;
    bv = s_pending_bv;
    strncpy(id, s_pending_id, sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    strncpy(url, s_pending_url, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    s_unlock();
    if (!present) {
        return;
    }

    // §3.7 rule 4: "re-evaluated by rules 1-2 when the fetch ends" — also
    // applied here, every idle iteration, not just at arrival time
    // (bookpull_on_nudge()): the stored bv can have moved since this nudge
    // was queued (this module's own prior fetch, or a full book applied via
    // the legacy §3.2 path) without going through bookpull_on_nudge() again.
    uint32_t stored_bv = book_get_bv();
    if (bookpull_rule1_ack_now(bv, stored_bv)) {
        s_lock();
        s_pending_present = false;
        s_unlock();
        if (id[0] != '\0') {
            msg_mark_shown(id);
        }
        ESP_LOGI(TAG, "pending nudge bv=%u now <= stored bv=%u - acked without fetching", (unsigned) bv,
                 (unsigned) stored_bv);
        return;
    }

    if (cafetch_in_progress()) {
        return; // catrust owns the shared guard right now; try again next iteration
    }

    int64_t now = esp_timer_get_time();
    bool due =
        s_retry_409_once || s_last_attempt_us == 0 || (now - s_last_attempt_us) >= BOOKPULL_RETRY_MIN_US;
    if (!due) {
        return; // still cooling down since the last attempt; s_pending_present stays set
    }

    s_lock();
    s_pending_present = false; // consumed -- a fresh bookpull_on_nudge() sets it again if a newer
                               // one lands meanwhile (rule 4)
    s_unlock();

    start_fetch(id, bv, url);
}

// Called once per fetch attempt's end (success or failure), `http_status`/
// `body_len` already logged by the caller. Verifies + applies on a 200,
// otherwise reacts per §14.7's failure table.
static void finish_attempt(int http_status, size_t body_len)
{
    uint32_t elapsed_ms = (uint32_t) ((esp_timer_get_time() - s_fetch_started_us) / 1000);
    bookpull_http_outcome_t outcome = bookpull_classify_http(http_status);
    bool applied = false;

    if (outcome == BOOKPULL_HTTP_APPLY) {
        // §14.7: "The device verifies the tag and the `n` echo before
        // parsing and applies nothing on failure." s_resp_buf is this
        // module's OWN copy (taken before cafetch_end() reset cafetch.c's
        // internal buffer), safe to trim in place.
        size_t vlen = body_len;
        if (!auth_verify_label(BOOKPULL_RESPONSE_LABEL, s_resp_buf, &vlen)) {
            ESP_LOGI(TAG, "book fetch: response signature verify failed - rejected, no ack");
        } else if (book_apply_fetched(s_resp_buf, vlen, s_fetch_n, s_fetch_bv)) {
            applied = true;
        }
        // else: book_apply_fetched() already logged why (n mismatch / bv too
        // low / malformed CBOR).
    }

    net_mqtt_status_t mqtt_after = { 0 };
    net_get_mqtt_status(&mqtt_after);
    // bv served: only meaningful once applied (book_get_bv() now reflects
    // whatever book_apply_fetched() just wrote — may be higher than
    // s_fetch_bv, §3.7: "it may be higher than the requested bv"); 0
    // otherwise, matching book_get_bv()'s own "never applied" sentinel.
    uint32_t bv_served = applied ? book_get_bv() : 0;
    ESP_LOGI(TAG,
             "book fetch done: bv wanted=%u served=%u applied=%d bytes=%u elapsed_ms=%u http=%d "
             "mqtt_connected_before=%d after=%d (UNVERIFIED second-socket check, cafetch.h)",
             (unsigned) s_fetch_bv, (unsigned) bv_served, (int) applied, (unsigned) body_len,
             (unsigned) elapsed_ms, http_status, (int) s_fetch_mqtt_before,
             (int) mqtt_after.mqtt_connected);

    if (applied) {
        msg_mark_shown(s_fetch_id);
    } else {
        switch (outcome) {
        case BOOKPULL_HTTP_APPLY: // verify/apply failed above -- §14.7 "keep pending, retry later"
        case BOOKPULL_HTTP_RETRY_LATER:
            requeue_pending(s_fetch_id, s_fetch_bv, s_fetch_url);
            break;
        case BOOKPULL_HTTP_RETRY_NOW:
            s_retry_409_once = true;
            requeue_pending(s_fetch_id, s_fetch_bv, s_fetch_url);
            break;
        case BOOKPULL_HTTP_DROP:
            // §14.7: "log and count, no retry until the next nudge" — counted
            // by the ESP_LOGI line above already; nothing else to do, the
            // nudge stays dropped (not requeued) unless a fresh one arrives.
            break;
        }
    }

    s_phase = BOOKPULL_IDLE;
}

static void service_fetching(void)
{
    cafetch_status_t st = cafetch_poll(esp_timer_get_time());
    if (st == CAFETCH_PENDING) {
        return;
    }

    const uint8_t *body = NULL;
    size_t body_len = 0;
    int http_status = 0;
    bool ok = cafetch_body(&body, &body_len, &http_status); // fills status/len even on FAILED
                                                             // (non-200) — see cafetch.h's own
                                                             // doc comment.
    if (ok && body && body_len <= sizeof(s_resp_buf)) {
        // Own copy, taken before cafetch_end() resets cafetch.c's internal
        // buffer — finish_attempt()'s auth_verify_label() needs to trim
        // `sig` in place, which must not happen to memory this module does
        // not own.
        memcpy(s_resp_buf, body, body_len);
    } else if (ok) {
        // Should not happen (CAFETCH_BODY_MAX == sizeof(s_resp_buf)), kept
        // defensive so a future constant drift fails safe, not with a
        // buffer overrun.
        ESP_LOGI(TAG, "book fetch: body (%u bytes) exceeds this module's own %u-byte buffer - rejected",
                 (unsigned) body_len, (unsigned) sizeof(s_resp_buf));
        http_status = 0; // force BOOKPULL_HTTP_RETRY_LATER below; body_len still logged as-is
    }
    cafetch_end();

    (void) st; // st == CAFETCH_OK implied by `ok`; st == CAFETCH_FAILED implied by !ok — both
               // already folded into http_status above.
    finish_attempt(http_status, body_len);
}

void bookpull_service(void)
{
    switch (s_phase) {
    case BOOKPULL_IDLE:
        service_idle();
        break;
    case BOOKPULL_FETCHING:
        service_fetching();
        break;
    }
}

#endif /* ESP_PLATFORM */

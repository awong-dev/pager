/* cafetch.c — see cafetch.h for the module split and every function's doc
 * comment.
 */
#include "cafetch.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/sha256.h"

/* ---------------------------------------------------------------------
 * URL parsing / request building.
 * --------------------------------------------------------------------- */

bool cafetch_parse_url(const char *url, char *host_out, size_t host_cap, uint16_t *port_out,
                       char *path_out, size_t path_cap)
{
    if (!url || !host_out || host_cap == 0 || !port_out || !path_out || path_cap == 0) {
        return false;
    }
    static const char prefix[] = "https://";
    size_t plen = sizeof(prefix) - 1;
    if (strncmp(url, prefix, plen) != 0) {
        return false; /* only https://, per V02_DESIGN.md §4.4 */
    }

    const char *host_start = url + plen;
    const char *q = host_start;
    while (*q != '\0' && *q != ':' && *q != '/') {
        q++;
    }
    size_t host_len = (size_t) (q - host_start);
    if (host_len == 0 || host_len >= host_cap) {
        return false;
    }

    uint16_t port = 443;
    if (*q == ':') {
        q++;
        const char *port_start = q;
        uint32_t v = 0;
        while (*q >= '0' && *q <= '9') {
            v = v * 10 + (uint32_t) (*q - '0');
            if (v > 65535) {
                return false;
            }
            q++;
        }
        if (q == port_start) {
            return false; /* "host:" with no digits */
        }
        port = (uint16_t) v;
    }

    if (*q != '/') {
        return false; /* "host[:port]" with no path at all is not accepted */
    }
    size_t path_len = strlen(q);
    if (path_len == 0 || path_len >= path_cap) {
        return false;
    }

    memcpy(host_out, host_start, host_len);
    host_out[host_len] = '\0';
    memcpy(path_out, q, path_len + 1);
    *port_out = port;
    return true;
}

bool cafetch_build_request_hdrs(const char *host, const char *path, const char *extra_hdrs, char *out,
                                size_t cap, size_t *out_len)
{
    int n = snprintf(out, cap, "GET %s HTTP/1.1\r\nHost: %s\r\n%sConnection: close\r\n\r\n", path,
                     host, extra_hdrs ? extra_hdrs : "");
    if (n < 0 || (size_t) n >= cap) {
        return false;
    }
    if (out_len) {
        *out_len = (size_t) n;
    }
    return true;
}

bool cafetch_build_request(const char *host, const char *path, char *out, size_t cap,
                           size_t *out_len)
{
    return cafetch_build_request_hdrs(host, path, NULL, out, cap, out_len);
}

/* ---------------------------------------------------------------------
 * Incremental HTTP/1.1 response parser.
 * --------------------------------------------------------------------- */

void cafetch_parser_init(cafetch_parser_t *p)
{
    memset(p, 0, sizeof(*p));
    p->phase = CAFETCH_PHASE_STATUS_LINE;
}

static void set_error(cafetch_parser_t *p, bool oversize, bool malformed, bool non200)
{
    p->phase = CAFETCH_PHASE_ERROR;
    p->err_oversize = oversize;
    p->err_malformed = malformed;
    p->err_non200 = non200;
}

static bool line_push(cafetch_parser_t *p, uint8_t c)
{
    if (p->line_len >= sizeof(p->line) - 1) {
        if (p->phase == CAFETCH_PHASE_HEADER_LINE) {
            /* An over-long HEADER line is dropped from here to its LF, not
             * treated as malformed. Found on hardware: real servers send
             * Content-Security-Policy headers of well over 1 kB, and the only
             * headers this parser acts on (Content-Length, Transfer-Encoding,
             * Location) are short. The kept prefix is non-empty, so a
             * truncated line can never be mistaken for the blank line that
             * ends the headers. Status and chunk-size lines stay strict. */
            return true;
        }
        set_error(p, false, true, false); /* a status/chunk line too long to be real */
        return false;
    }
    p->line[p->line_len++] = (char) c;
    return true;
}

static void line_reset(cafetch_parser_t *p)
{
    p->line_len = 0;
}

/* Consumes one byte into `p->line`. On '\n' (LF), trims one preceding '\r'
 * if present, NUL-terminates `p->line` in place and sets `*have_line`. */
static bool feed_byte_line(cafetch_parser_t *p, uint8_t c, bool *have_line)
{
    *have_line = false;
    if (c == '\n') {
        size_t l = p->line_len;
        if (l > 0 && p->line[l - 1] == '\r') {
            l--;
        }
        p->line[l] = '\0';
        p->line_len = l;
        *have_line = true;
        return true;
    }
    return line_push(p, c);
}

static bool parse_status_line(cafetch_parser_t *p)
{
    const char *s = p->line;
    if (strncmp(s, "HTTP/", 5) != 0) {
        return false;
    }
    const char *sp = strchr(s, ' ');
    if (!sp) {
        return false;
    }
    sp++;
    int code = 0;
    int n = 0;
    while (n < 3 && sp[n] >= '0' && sp[n] <= '9') {
        code = code * 10 + (sp[n] - '0');
        n++;
    }
    if (n != 3) {
        return false;
    }
    p->status_code = code;
    return true;
}

static bool ascii_ieq_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        char a = *s++;
        char b = *prefix++;
        if (a >= 'A' && a <= 'Z') {
            a = (char) (a + 32);
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char) (b + 32);
        }
        if (a != b) {
            return false;
        }
    }
    return true;
}

static void process_header_line(cafetch_parser_t *p)
{
    const char *s = p->line;
    if (ascii_ieq_prefix(s, "content-length:")) {
        const char *v = s + strlen("content-length:");
        while (*v == ' ') {
            v++;
        }
        size_t n = 0;
        while (*v >= '0' && *v <= '9') {
            n = n * 10 + (size_t) (*v - '0');
            v++;
        }
        p->have_content_length = true;
        p->content_length = n;
    } else if (ascii_ieq_prefix(s, "transfer-encoding:")) {
        const char *v = s + strlen("transfer-encoding:");
        while (*v == ' ') {
            v++;
        }
        if (ascii_ieq_prefix(v, "chunked")) {
            p->chunked = true;
        }
    }
    /* `Connection: close` needs no bookkeeping here: CAFETCH_PHASE_BODY_UNTIL_CLOSE
     * is entered structurally (no Content-Length, not chunked) regardless of
     * whether this header was actually present on the wire. */
}

static bool end_of_headers(cafetch_parser_t *p)
{
    if (p->status_code != 200) {
        /* Also covers redirects (3xx): "follow no redirects" (V02_DESIGN.md
         * §4.4) is satisfied by requiring exactly 200 and nothing else. */
        set_error(p, false, false, true);
        return false;
    }
    if (p->have_content_length) {
        if (p->content_length > CAFETCH_BODY_MAX) {
            set_error(p, true, false, false);
            return false;
        }
        p->phase = (p->content_length == 0) ? CAFETCH_PHASE_DONE : CAFETCH_PHASE_BODY_LENGTH;
    } else if (p->chunked) {
        p->phase = CAFETCH_PHASE_CHUNK_SIZE_LINE;
        line_reset(p);
    } else {
        p->phase = CAFETCH_PHASE_BODY_UNTIL_CLOSE;
    }
    return true;
}

bool cafetch_parser_feed(cafetch_parser_t *p, const uint8_t *data, size_t len)
{
    if (p->phase == CAFETCH_PHASE_ERROR) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = data[i];
        switch (p->phase) {
        case CAFETCH_PHASE_STATUS_LINE: {
            bool have_line;
            if (!feed_byte_line(p, c, &have_line)) {
                return false;
            }
            if (have_line) {
                if (!parse_status_line(p)) {
                    set_error(p, false, true, false);
                    return false;
                }
                line_reset(p);
                p->phase = CAFETCH_PHASE_HEADER_LINE;
            }
            break;
        }
        case CAFETCH_PHASE_HEADER_LINE: {
            bool have_line;
            if (!feed_byte_line(p, c, &have_line)) {
                return false;
            }
            if (have_line) {
                if (p->line_len == 0) {
                    if (!end_of_headers(p)) {
                        return false;
                    }
                } else {
                    process_header_line(p);
                    line_reset(p);
                }
            }
            break;
        }
        case CAFETCH_PHASE_BODY_LENGTH: {
            if (p->body_len >= CAFETCH_BODY_MAX) {
                set_error(p, true, false, false);
                return false;
            }
            p->body[p->body_len++] = c;
            if (p->body_len == p->content_length) {
                p->phase = CAFETCH_PHASE_DONE;
            }
            break;
        }
        case CAFETCH_PHASE_BODY_UNTIL_CLOSE: {
            if (p->body_len >= CAFETCH_BODY_MAX) {
                set_error(p, true, false, false);
                return false;
            }
            p->body[p->body_len++] = c;
            break; /* stays pending until cafetch_parser_closed() */
        }
        case CAFETCH_PHASE_CHUNK_SIZE_LINE: {
            bool have_line;
            if (!feed_byte_line(p, c, &have_line)) {
                return false;
            }
            if (have_line) {
                size_t n = 0;
                bool any = false;
                for (size_t k = 0; k < p->line_len; k++) {
                    char ch = p->line[k];
                    if (ch == ';') {
                        break; /* chunk-extension, ignored */
                    }
                    int v;
                    if (ch >= '0' && ch <= '9') {
                        v = ch - '0';
                    } else if (ch >= 'a' && ch <= 'f') {
                        v = ch - 'a' + 10;
                    } else if (ch >= 'A' && ch <= 'F') {
                        v = ch - 'A' + 10;
                    } else {
                        set_error(p, false, true, false);
                        return false;
                    }
                    n = n * 16 + (size_t) v;
                    any = true;
                }
                if (!any) {
                    set_error(p, false, true, false);
                    return false;
                }
                line_reset(p);
                if (n == 0) {
                    p->phase = CAFETCH_PHASE_CHUNK_TRAILER;
                } else {
                    if (p->body_len + n > CAFETCH_BODY_MAX) {
                        set_error(p, true, false, false);
                        return false;
                    }
                    p->chunk_remaining = n;
                    p->phase = CAFETCH_PHASE_CHUNK_DATA;
                }
            }
            break;
        }
        case CAFETCH_PHASE_CHUNK_DATA: {
            p->body[p->body_len++] = c;
            p->chunk_remaining--;
            if (p->chunk_remaining == 0) {
                p->phase = CAFETCH_PHASE_CHUNK_CRLF;
                line_reset(p);
            }
            break;
        }
        case CAFETCH_PHASE_CHUNK_CRLF: {
            bool have_line;
            if (!feed_byte_line(p, c, &have_line)) {
                return false;
            }
            if (have_line) {
                if (p->line_len != 0) {
                    set_error(p, false, true, false); /* expected a bare CRLF */
                    return false;
                }
                p->phase = CAFETCH_PHASE_CHUNK_SIZE_LINE;
            }
            break;
        }
        case CAFETCH_PHASE_CHUNK_TRAILER: {
            bool have_line;
            if (!feed_byte_line(p, c, &have_line)) {
                return false;
            }
            if (have_line) {
                if (p->line_len == 0) {
                    p->phase = CAFETCH_PHASE_DONE;
                } else {
                    line_reset(p); /* trailer headers are ignored */
                }
            }
            break;
        }
        case CAFETCH_PHASE_DONE:
            break; /* extra bytes after completion: ignored */
        case CAFETCH_PHASE_ERROR:
            return false;
        }
    }
    return true;
}

void cafetch_parser_closed(cafetch_parser_t *p)
{
    if (p->phase == CAFETCH_PHASE_BODY_UNTIL_CLOSE) {
        p->phase = CAFETCH_PHASE_DONE;
    } else if (p->phase != CAFETCH_PHASE_DONE && p->phase != CAFETCH_PHASE_ERROR) {
        set_error(p, false, true, false); /* truncated mid-response */
    }
}

/* ---------------------------------------------------------------------
 * Body validation.
 * --------------------------------------------------------------------- */

bool cafetch_validate_body(const uint8_t *body, size_t body_len, const uint8_t expected_sha[CAFETCH_SHA_LEN],
                          char *pem_out, size_t pem_cap, size_t *pem_len)
{
    if (!body || !expected_sha || !pem_out) {
        return false;
    }

    /* "hash the body bytes exactly as received" — no trimming. */
    uint8_t got_sha[CAFETCH_SHA_LEN];
    mbedtls_sha256(body, body_len, got_sha, 0);
    if (memcmp(got_sha, expected_sha, CAFETCH_SHA_LEN) != 0) {
        return false;
    }

    static const char begin_marker[] = "-----BEGIN CERTIFICATE-----";
    static const char end_marker[] = "-----END CERTIFICATE-----";
    size_t begin_len = sizeof(begin_marker) - 1;
    size_t end_len = sizeof(end_marker) - 1;

    const uint8_t *begin_pos = NULL;
    for (size_t i = 0; i + begin_len <= body_len; i++) {
        if (memcmp(body + i, begin_marker, begin_len) == 0) {
            begin_pos = body + i;
            break;
        }
    }
    if (!begin_pos) {
        return false; /* zero PEM blocks */
    }
    size_t after_begin_off = (size_t) (begin_pos - body) + begin_len;

    const uint8_t *end_pos = NULL;
    for (size_t i = after_begin_off; i + end_len <= body_len; i++) {
        if (memcmp(body + i, end_marker, end_len) == 0) {
            end_pos = body + i;
            break;
        }
    }
    if (!end_pos) {
        return false; /* BEGIN with no matching END */
    }
    size_t end_off = (size_t) (end_pos - body);

    /* A second BEGIN anywhere else in the body (nested inside this block, or
     * after it) means "more than one PEM certificate", which V02_DESIGN.md
     * §4.4 requires rejecting. */
    for (size_t i = after_begin_off; i + begin_len <= body_len; i++) {
        if (i >= end_off && i < end_off + end_len) {
            continue; /* skip past the END marker itself */
        }
        if (memcmp(body + i, begin_marker, begin_len) == 0) {
            return false;
        }
    }

    size_t copy_end = end_off + end_len;
    if (copy_end < body_len && body[copy_end] == '\r') {
        copy_end++;
    }
    if (copy_end < body_len && body[copy_end] == '\n') {
        copy_end++;
    }

    size_t out_len = copy_end - (size_t) (begin_pos - body);
    if (out_len + 1 > pem_cap) {
        return false;
    }
    memcpy(pem_out, begin_pos, out_len);
    pem_out[out_len] = '\0';
    if (pem_len) {
        *pem_len = out_len;
    }
    return true;
}

/* ======================================================================
 * Device wiring — needs net.h's modem facade, esp_timer and FreeRTOS.
 * ====================================================================== */
#ifdef ESP_PLATFORM

#include "net.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "cafetch";

#define CAFETCH_TIMEOUT_US ((int64_t) 30 * 1000000) /* V02_DESIGN.md §4.4: "timeouts (30 s overall)" */
#define CAFETCH_RECV_BUF 1500                       /* WalterSocket.cpp: <=1500 bytes per socketReceive() */

static bool s_in_progress = false;
static cafetch_parser_t s_parser;
static int64_t s_deadline_us = 0;
static char s_host[CAFETCH_HOST_MAX];
static char s_path[CAFETCH_PATH_MAX];

bool cafetch_in_progress(void)
{
    return s_in_progress;
}

bool cafetch_begin_ex(const char *url, const char *extra_hdrs)
{
    if (s_in_progress) {
        ESP_LOGI(TAG, "cafetch_begin() called while a fetch is already in progress");
        return false;
    }
    uint16_t port;
    if (!cafetch_parse_url(url, s_host, sizeof(s_host), &port, s_path, sizeof(s_path))) {
        ESP_LOGI(TAG, "cafetch: bad URL '%s' (must be https://host[:port]/path)", url);
        return false;
    }
    // Power effect: one TLS handshake (VALIDATION_NONE, profile 3) plus the
    // RRC time it takes — see net_ca_fetch_open()'s own doc comment.
    if (!net_ca_fetch_open(s_host, port)) {
        ESP_LOGI(TAG, "cafetch: net_ca_fetch_open(%s:%u) failed", s_host, (unsigned) port);
        return false;
    }
    char req[CAFETCH_PATH_MAX + CAFETCH_HOST_MAX + 256];
    size_t req_len;
    if (!cafetch_build_request_hdrs(s_host, s_path, extra_hdrs, req, sizeof(req), &req_len)) {
        ESP_LOGI(TAG, "cafetch: request line/headers too long for host/path/extra_hdrs");
        net_ca_fetch_close();
        return false;
    }
    if (!net_ca_fetch_send((const uint8_t *) req, (uint16_t) req_len)) {
        ESP_LOGI(TAG, "cafetch: net_ca_fetch_send() failed");
        net_ca_fetch_close();
        return false;
    }
    cafetch_parser_init(&s_parser);
    s_deadline_us = esp_timer_get_time() + CAFETCH_TIMEOUT_US;
    s_in_progress = true;
    ESP_LOGI(TAG, "cafetch: GET %s HTTP/1.1 to %s:%u", s_path, s_host, (unsigned) port);
    return true;
}

bool cafetch_begin(const char *url)
{
    return cafetch_begin_ex(url, NULL);
}

cafetch_status_t cafetch_poll(int64_t now_us)
{
    if (!s_in_progress) {
        return CAFETCH_FAILED;
    }
    if (s_parser.phase == CAFETCH_PHASE_DONE) {
        return CAFETCH_OK;
    }
    if (s_parser.phase == CAFETCH_PHASE_ERROR) {
        return CAFETCH_FAILED;
    }
    if (now_us >= s_deadline_us) {
        ESP_LOGI(TAG, "cafetch: 30s overall timeout (%u body bytes received so far)",
                 (unsigned) s_parser.body_len);
        s_parser.phase = CAFETCH_PHASE_ERROR;
        s_parser.err_malformed = true;
        return CAFETCH_FAILED;
    }

    static uint8_t buf[CAFETCH_RECV_BUF];
    uint16_t got_len = 0;
    bool closed = false;
    // Power effect: none when nothing is pending; one AT round trip
    // (socketReceive(), <=1500 bytes) when a RING was pending — see
    // net_ca_fetch_poll()'s own doc comment.
    if (!net_ca_fetch_poll(buf, sizeof(buf), &got_len, &closed)) {
        return CAFETCH_PENDING;
    }
    if (closed) {
        cafetch_parser_closed(&s_parser);
    } else if (got_len > 0) {
        cafetch_parser_feed(&s_parser, buf, got_len); /* return value: phase already reflects it */
    }

    if (s_parser.phase == CAFETCH_PHASE_DONE) {
        return CAFETCH_OK;
    }
    if (s_parser.phase == CAFETCH_PHASE_ERROR) {
        const char *why =
            s_parser.err_oversize ? "oversize" : (s_parser.err_non200 ? "non-200" : "malformed");
        ESP_LOGI(TAG, "cafetch: parse failed (%s), http_status=%d, %u body bytes so far", why,
                 s_parser.status_code, (unsigned) s_parser.body_len);
        return CAFETCH_FAILED;
    }
    return CAFETCH_PENDING;
}

bool cafetch_result(const uint8_t expected_sha[CAFETCH_SHA_LEN], char *pem_out, size_t pem_cap,
                    size_t *pem_len, int *out_http_status, size_t *out_bytes)
{
    if (out_http_status) {
        *out_http_status = s_parser.status_code;
    }
    if (out_bytes) {
        *out_bytes = s_parser.body_len;
    }
    if (s_parser.phase != CAFETCH_PHASE_DONE) {
        return false;
    }
    return cafetch_validate_body(s_parser.body, s_parser.body_len, expected_sha, pem_out, pem_cap,
                                 pem_len);
}

bool cafetch_body(const uint8_t **body, size_t *len, int *status)
{
    if (status) {
        *status = s_parser.status_code;
    }
    if (len) {
        *len = s_parser.body_len;
    }
    if (body) {
        *body = s_parser.body;
    }
    return s_parser.phase == CAFETCH_PHASE_DONE;
}

void cafetch_end(void)
{
    if (!s_in_progress) {
        return;
    }
    net_ca_fetch_close(); // power effect: one AT command, no RRC of its own
    s_in_progress = false;
}

bool cafetch_run_blocking(const char *url, const uint8_t expected_sha[CAFETCH_SHA_LEN], char *pem_out,
                         size_t pem_cap, size_t *pem_len, int *out_http_status, size_t *out_bytes,
                         uint32_t *out_elapsed_ms, bool *out_mqtt_survived)
{
    net_mqtt_status_t mqtt_before = { 0 };
    net_mqtt_status_t mqtt_after = { 0 };
    net_get_mqtt_status(&mqtt_before);

    int64_t t0 = esp_timer_get_time();
    if (out_http_status) {
        *out_http_status = 0;
    }
    if (out_bytes) {
        *out_bytes = 0;
    }

    bool ok = cafetch_begin(url);
    if (ok) {
        cafetch_status_t st;
        for (;;) {
            st = cafetch_poll(esp_timer_get_time());
            if (st != CAFETCH_PENDING) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(50)); // FreeRTOS primitive, not a busy-wait
        }
        ok = (st == CAFETCH_OK) &&
            cafetch_result(expected_sha, pem_out, pem_cap, pem_len, out_http_status, out_bytes);
        if (st == CAFETCH_FAILED) {
            if (out_http_status) {
                *out_http_status = s_parser.status_code;
            }
            if (out_bytes) {
                *out_bytes = s_parser.body_len;
            }
        }
        cafetch_end();
    }

    if (out_elapsed_ms) {
        *out_elapsed_ms = (uint32_t) ((esp_timer_get_time() - t0) / 1000);
    }

    net_get_mqtt_status(&mqtt_after);
    if (out_mqtt_survived) {
        *out_mqtt_survived = mqtt_before.mqtt_connected && mqtt_after.mqtt_connected;
    }
    ESP_LOGI(TAG, "cafetch_run_blocking(%s): %s, elapsed=%llu ms, mqtt_survived=%d (before=%d after=%d)",
             url, ok ? "OK" : "FAILED",
             out_elapsed_ms ? (unsigned long long) *out_elapsed_ms : 0ULL,
             out_mqtt_survived ? (int) *out_mqtt_survived : -1, (int) mqtt_before.mqtt_connected,
             (int) mqtt_after.mqtt_connected);
    return ok;
}

#endif /* ESP_PLATFORM */

/* test_cafetch.c — host test harness for main/cafetch.c's pure section
 * (docs/V02_DESIGN.md §4.4): URL parsing, the incremental HTTP/1.1 response
 * parser (split headers, chunked encoding, oversize, non-200/redirect), and
 * the SHA-256 + "exactly one PEM" body validator.
 *
 * Links the *real* mbedtls (cafetch_validate_body() calls mbedtls_sha256()),
 * same convention lock.c/setup.c already establish for this codebase.
 */
#include "cafetch.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/sha256.h"

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
 * URL parsing / request building.
 * --------------------------------------------------------------------- */

static void test_url_parse(void)
{
    char host[128];
    char path[256];
    uint16_t port;

    CHECK(cafetch_parse_url("https://example.com/ca/abc.pem", host, sizeof(host), &port, path,
                            sizeof(path)),
          "a plain https URL with a path must parse");
    CHECK(strcmp(host, "example.com") == 0, "host mismatch: %s", host);
    CHECK(port == 443, "default port must be 443, got %u", (unsigned) port);
    CHECK(strcmp(path, "/ca/abc.pem") == 0, "path mismatch: %s", path);

    CHECK(cafetch_parse_url("https://example.com:8443/ca/abc.pem", host, sizeof(host), &port, path,
                            sizeof(path)),
          "an explicit port must parse");
    CHECK(port == 8443, "explicit port mismatch: %u", (unsigned) port);

    CHECK(!cafetch_parse_url("http://example.com/ca/abc.pem", host, sizeof(host), &port, path,
                             sizeof(path)),
          "plain http:// must be rejected");
    CHECK(!cafetch_parse_url("https://example.com", host, sizeof(host), &port, path, sizeof(path)),
          "a URL with no path must be rejected");
    CHECK(!cafetch_parse_url("https://example.com:notanumber/x", host, sizeof(host), &port, path,
                             sizeof(path)),
          "a non-numeric port must be rejected");
    CHECK(!cafetch_parse_url(NULL, host, sizeof(host), &port, path, sizeof(path)),
          "a NULL URL must be rejected");

    char req[256];
    size_t req_len = 0;
    CHECK(cafetch_build_request("example.com", "/ca/abc.pem", req, sizeof(req), &req_len),
          "request build must succeed");
    CHECK(strcmp(req, "GET /ca/abc.pem HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n") ==
              0,
          "unexpected request text: %s", req);
}

/* ---------------------------------------------------------------------
 * Helpers to feed the parser in arbitrary-sized pieces.
 * --------------------------------------------------------------------- */

static void feed_whole(cafetch_parser_t *p, const char *s)
{
    cafetch_parser_feed(p, (const uint8_t *) s, strlen(s));
}

/* Feeds one byte at a time -- the worst case for anything that accumulates
 * a line across calls (headers split arbitrarily across modem socket
 * reads). */
static void feed_byte_by_byte(cafetch_parser_t *p, const char *s)
{
    for (size_t i = 0; s[i]; i++) {
        cafetch_parser_feed(p, (const uint8_t *) &s[i], 1);
    }
}

/* ---------------------------------------------------------------------
 * Response parsing.
 * --------------------------------------------------------------------- */

static void test_content_length_split_headers(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);

    /* Split the response across many small feed() calls, including mid-header
     * and mid-body-byte boundaries -- simulates <=1500-byte modem reads
     * landing anywhere. */
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello";
    size_t len = strlen(resp);
    for (size_t i = 0; i < len; i += 3) {
        size_t n = (len - i < 3) ? (len - i) : 3;
        CHECK(cafetch_parser_feed(&p, (const uint8_t *) resp + i, n), "feed() must not fail mid-response");
    }
    CHECK(p.phase == CAFETCH_PHASE_DONE, "expected DONE, got phase=%d (status=%d)", (int) p.phase,
          p.status_code);
    CHECK(p.status_code == 200, "status_code mismatch: %d", p.status_code);
    CHECK(p.body_len == 5 && memcmp(p.body, "hello", 5) == 0, "body mismatch (%u bytes)",
          (unsigned) p.body_len);
}

static void test_header_line_split_byte_by_byte(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    feed_byte_by_byte(&p, "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nhi");
    CHECK(p.phase == CAFETCH_PHASE_DONE, "byte-by-byte feed must still reach DONE (phase=%d)",
          (int) p.phase);
    CHECK(p.body_len == 2 && memcmp(p.body, "hi", 2) == 0, "unexpected body");
}

static void test_connection_close_body(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    feed_whole(&p, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n-----BEGIN CERTIFICATE-----\nAB\n"
                   "-----END CERTIFICATE-----\n");
    CHECK(p.phase == CAFETCH_PHASE_BODY_UNTIL_CLOSE, "no Content-Length/chunked -> BODY_UNTIL_CLOSE "
                                                     "before the socket closes (phase=%d)",
          (int) p.phase);
    cafetch_parser_closed(&p);
    CHECK(p.phase == CAFETCH_PHASE_DONE, "a close in BODY_UNTIL_CLOSE must end the body cleanly");
    CHECK(p.body_len > 0, "body must be non-empty");
}

static void test_truncated_length_body_is_malformed(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    feed_whole(&p, "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc");
    CHECK(p.phase == CAFETCH_PHASE_BODY_LENGTH, "still waiting for more body bytes");
    cafetch_parser_closed(&p); /* socket closed before Content-Length bytes arrived */
    CHECK(p.phase == CAFETCH_PHASE_ERROR && p.err_malformed,
          "a close before Content-Length is satisfied must be malformed, not silently accepted");
}

static void test_chunked_encoding(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    /* "hello" (5) + " world" (6), then the terminating 0-length chunk. */
    feed_whole(&p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                   "5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n");
    CHECK(p.phase == CAFETCH_PHASE_DONE, "chunked body must reach DONE (phase=%d)", (int) p.phase);
    CHECK(p.body_len == 11 && memcmp(p.body, "hello world", 11) == 0,
          "de-chunked body mismatch (%u bytes): %.*s", (unsigned) p.body_len, (int) p.body_len,
          (const char *) p.body);
}

static void test_chunked_split_across_feeds(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    const char *resp = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                       "4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n";
    size_t len = strlen(resp);
    for (size_t i = 0; i < len; i++) {
        CHECK(cafetch_parser_feed(&p, (const uint8_t *) resp + i, 1), "byte-by-byte chunked feed failed");
    }
    CHECK(p.phase == CAFETCH_PHASE_DONE, "byte-by-byte chunked feed must reach DONE");
    CHECK(p.body_len == 9 && memcmp(p.body, "Wikipedia", 9) == 0, "unexpected de-chunked body");
}

static void test_oversize_content_length(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    char resp[128];
    snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n", CAFETCH_BODY_MAX + 1);
    feed_whole(&p, resp);
    CHECK(p.phase == CAFETCH_PHASE_ERROR && p.err_oversize,
          "a Content-Length over the cap must be refused before reading any body");
}

static void test_oversize_chunked_total(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    feed_whole(&p, "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n");
    CHECK(p.phase == CAFETCH_PHASE_CHUNK_SIZE_LINE, "must be waiting for the first chunk size");
    /* One chunk bigger than the whole cap. */
    char chunk_hdr[32];
    snprintf(chunk_hdr, sizeof(chunk_hdr), "%x\r\n", CAFETCH_BODY_MAX + 16);
    feed_whole(&p, chunk_hdr);
    CHECK(p.phase == CAFETCH_PHASE_ERROR && p.err_oversize,
          "a chunk whose declared size overflows the cap must be refused");
}

static void test_non_200_and_redirect(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    feed_whole(&p, "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n");
    CHECK(p.phase == CAFETCH_PHASE_ERROR && p.err_non200, "404 must be refused");

    cafetch_parser_init(&p);
    feed_whole(&p, "HTTP/1.1 301 Moved Permanently\r\nLocation: https://example.com/x\r\n"
                   "Content-Length: 0\r\n\r\n");
    CHECK(p.phase == CAFETCH_PHASE_ERROR && p.err_non200,
          "a redirect (3xx) must be refused, never followed");
}

static void test_malformed_status_line(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    CHECK(!cafetch_parser_feed(&p, (const uint8_t *) "not an http response\r\n", 22),
          "a garbage status line must fail feed()");
    CHECK(p.phase == CAFETCH_PHASE_ERROR && p.err_malformed, "must be flagged malformed");
}

/* ---------------------------------------------------------------------
 * Body validation: SHA-256 + exactly-one-PEM.
 * --------------------------------------------------------------------- */

/* Expected hashes are computed directly via mbedtls_sha256() -- the same
 * primitive cafetch_validate_body() itself calls -- rather than hand-computed
 * with another tool, so each test is self-verifying against whatever mbedtls
 * this host links (see the Makefile's mbedcrypto pkg-config/brew fallback). */

static void test_validate_body_good(void)
{
    const char *body = "-----BEGIN CERTIFICATE-----\nAB\n-----END CERTIFICATE-----\n";
    uint8_t sha[32];
    mbedtls_sha256((const unsigned char *) body, strlen(body), sha, 0);

    char pem[256];
    size_t pem_len = 0;
    bool ok = cafetch_validate_body((const uint8_t *) body, strlen(body), sha, pem, sizeof(pem), &pem_len);
    CHECK(ok, "a well-formed single PEM block with a matching hash must validate");
    CHECK(pem_len == strlen(body) && memcmp(pem, body, pem_len) == 0, "unexpected extracted PEM: %.*s",
          (int) pem_len, pem);

    uint8_t wrong_sha[32];
    memcpy(wrong_sha, sha, 32);
    wrong_sha[0] ^= 0xFF;
    CHECK(!cafetch_validate_body((const uint8_t *) body, strlen(body), wrong_sha, pem, sizeof(pem),
                                 &pem_len),
          "a hash mismatch must be rejected");

    /* Trailing whitespace/newline tolerance: the relay serves exactly the
     * bytes it hashed, so a body with a trailing blank line must still
     * validate as long as the hash was computed over those exact bytes
     * (never trimmed here). */
    char body_trailing[256];
    snprintf(body_trailing, sizeof(body_trailing), "%s\n\n", body);
    uint8_t sha_trailing[32];
    mbedtls_sha256((const unsigned char *) body_trailing, strlen(body_trailing), sha_trailing, 0);
    CHECK(cafetch_validate_body((const uint8_t *) body_trailing, strlen(body_trailing), sha_trailing,
                                pem, sizeof(pem), &pem_len),
          "trailing whitespace/newlines after END must not block validation when the hash covers them");
    CHECK(pem_len == strlen(body), "extracted PEM must stop at the END marker's own trailing newline, "
                                   "not include the extra trailing blank line (%u)",
          (unsigned) pem_len);

    /* The ORIGINAL (untrimmed) hash must NOT match the trailing-whitespace
     * body -- proves hashing is over the literal bytes, not some normalised
     * form. */
    CHECK(!cafetch_validate_body((const uint8_t *) body_trailing, strlen(body_trailing), sha, pem,
                                 sizeof(pem), &pem_len),
          "a hash computed over the untrimmed body must not match a body with extra trailing bytes");
}

static void test_validate_body_two_pem_blocks(void)
{
    const char *body = "-----BEGIN CERTIFICATE-----\nAB\n-----END CERTIFICATE-----\n"
                       "-----BEGIN CERTIFICATE-----\nCD\n-----END CERTIFICATE-----\n";
    uint8_t sha[32];
    mbedtls_sha256((const unsigned char *) body, strlen(body), sha, 0);
    char pem[256];
    size_t pem_len = 0;
    CHECK(!cafetch_validate_body((const uint8_t *) body, strlen(body), sha, pem, sizeof(pem), &pem_len),
          "two PEM blocks in one body must be rejected even though the hash matches");
}

static void test_validate_body_zero_pem_blocks(void)
{
    const char *body = "not a certificate at all";
    uint8_t sha[32];
    mbedtls_sha256((const unsigned char *) body, strlen(body), sha, 0);
    char pem[256];
    size_t pem_len = 0;
    CHECK(!cafetch_validate_body((const uint8_t *) body, strlen(body), sha, pem, sizeof(pem), &pem_len),
          "a body with no PEM markers at all must be rejected");
}

int main(void)
{
    test_url_parse();
    test_content_length_split_headers();
    test_header_line_split_byte_by_byte();
    test_connection_close_body();
    test_truncated_length_body_is_malformed();
    test_chunked_encoding();
    test_chunked_split_across_feeds();
    test_oversize_content_length();
    test_oversize_chunked_total();
    test_non_200_and_redirect();
    test_malformed_status_line();
    test_validate_body_good();
    test_validate_body_two_pem_blocks();
    test_validate_body_zero_pem_blocks();

    if (g_failures == 0) {
        printf("PASS: cafetch, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

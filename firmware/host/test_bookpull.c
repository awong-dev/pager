/* test_bookpull.c — host test harness for v0.4 book pull (docs/PROTOCOL.md
 * §3.7, §14.7; `docs/CHAT_UI_DESIGN.md` decision 4, task T1f).
 *
 * Covers:
 *  - nudge parse (book.c's book_parse(), now public — see book.h's own doc
 *    comment for why): bv+url with neither `c` nor `p` -> nudge; `c`
 *    present -> full book, not a nudge; url missing/bad -> malformed.
 *  - bookpull.c's pure section: device rule 1 (bv<=stored -> ack now, no
 *    fetch), request build (exact bytes of `M`, `X-Sig` against a
 *    `relay/.venv/bin/python`-computed vector), the §14.7 HTTP-status
 *    failure-table classifier.
 *  - response handling: a signed §14.7 fetch-response body (fixtures below
 *    are literal `sign_cbor()` output from a fixed key, computed the same
 *    way this file's own comments show) — valid -> decodes/applies-worthy
 *    (32 of 40 contacts kept, nickname carried via book_carry_nicknames());
 *    wrong `n` -> rejected; bad tag -> rejected; `bv` < nudge `bv` -> not
 *    acked.
 *  - cafetch.c's own oversize cap (4096+1 body), the shared component
 *    bookpull.c relies on for its own 4096-byte body guarantee.
 *
 * Fixtures are hand-built with cbor.c's own writer, same convention
 * test_cfg.c/test_lock.c already use, literal PROTOCOL.md §10 keys inline
 * (0=v, 1=id, 2=ts, 6=kind, 12=n, 14=bv, 17=d, 18=c, 19=p, 20=more, 57=url;
 * `c[]` sub-map 0=a/1=n/2=t).
 */
#include "auth.h"
#include "book.h"
#include "bookpull.h"
#include "cafetch.h"

#include <stdio.h>
#include <string.h>

#include "cbor.h"

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
 * Nudge parse (book_parse()) — §3.7.
 * --------------------------------------------------------------------- */

static void test_nudge_parse_bv_url_no_c_p(void)
{
    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 5);
    cbor_w_tstr(&w, 1, "m_11111111", 10);
    cbor_w_tstr(&w, 6, "book", 4);
    cbor_w_uint(&w, 14, 7);
    cbor_w_tstr(&w, 57, "https://relay.example/api/device/book",
               strlen("https://relay.example/api/device/book"));
    cbor_w_null(&w, 5); /* ack */
    CHECK(!w.err, "test setup: encoding the nudge fixture must not overflow");

    book_parsed_t p;
    CHECK(book_parse(buf, (uint16_t) w.len, false, &p), "a book with bv+url and no c/p must parse");
    CHECK(p.is_nudge, "no c/p present -> is_nudge must be true");
    CHECK(p.bv == 7, "bv mismatch: %u", (unsigned) p.bv);
    CHECK(strcmp(p.nudge_url, "https://relay.example/api/device/book") == 0, "url mismatch: %s",
          p.nudge_url);
    CHECK(strcmp(p.id, "m_11111111") == 0, "id mismatch: %s", p.id);
}

static void test_book_with_c_is_not_a_nudge(void)
{
    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 4);
    cbor_w_tstr(&w, 1, "m_22222222", 10);
    cbor_w_tstr(&w, 6, "book", 4);
    cbor_w_uint(&w, 14, 7);
    cbor_w_array(&w, 18, 1); /* c: [ {a,n,t} ] */
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 0, "mom", 3);
    cbor_w_tstr(&w, 1, "Mom", 3);
    cbor_w_tstr(&w, 2, "web", 3);
    CHECK(!w.err, "test setup: encoding the full-book fixture must not overflow");

    book_parsed_t p;
    CHECK(book_parse(buf, (uint16_t) w.len, false, &p), "a book with `c` present must still parse");
    CHECK(!p.is_nudge, "`c` present -> is_nudge must be false (full book path)");
    CHECK(p.n_contacts == 1 && strcmp(p.contacts[0].alias, "mom") == 0, "contact not decoded");

    /* Even an EMPTY c:[]/p:[] (key present, zero entries) must still count
     * as "have_c"/"have_p" -> not a nudge, per book.c's own have_c/have_p
     * distinction (key presence, not array length). */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 5);
    cbor_w_tstr(&w, 1, "m_33333333", 10);
    cbor_w_tstr(&w, 6, "book", 4);
    cbor_w_uint(&w, 14, 7);
    cbor_w_array(&w, 18, 0); /* c: [] */
    cbor_w_array(&w, 19, 0); /* p: [] */
    CHECK(!w.err, "test setup: encoding the empty-arrays fixture must not overflow");
    CHECK(book_parse(buf, (uint16_t) w.len, false, &p), "a book with empty c:[]/p:[] must still parse");
    CHECK(!p.is_nudge, "an empty c:[]/p:[] WITH the keys present must not be treated as a nudge");
}

static void test_nudge_missing_or_bad_url_is_malformed(void)
{
    uint8_t buf[256];
    cbor_w_t w;

    /* No `url` key at all. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 1, "m_44444444", 10);
    cbor_w_tstr(&w, 6, "book", 4);
    cbor_w_uint(&w, 14, 7);
    CHECK(!w.err, "test setup must not overflow");
    book_parsed_t p;
    CHECK(!book_parse(buf, (uint16_t) w.len, false, &p), "a nudge with no `url` at all must be malformed");

    /* `url` present but not https://host/path (fails cafetch_parse_url()). */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 4);
    cbor_w_tstr(&w, 1, "m_55555555", 10);
    cbor_w_tstr(&w, 6, "book", 4);
    cbor_w_uint(&w, 14, 7);
    cbor_w_tstr(&w, 57, "not-a-url", 9);
    CHECK(!w.err, "test setup must not overflow");
    CHECK(!book_parse(buf, (uint16_t) w.len, false, &p),
          "a nudge whose url fails cafetch_parse_url() must be malformed");
}

/* ---------------------------------------------------------------------
 * Device rule 1 (§3.7) — bookpull_rule1_ack_now().
 * --------------------------------------------------------------------- */

static void test_rule1(void)
{
    CHECK(bookpull_rule1_ack_now(5, 5), "nudge bv == stored bv -> ack now, no fetch");
    CHECK(bookpull_rule1_ack_now(3, 5), "nudge bv < stored bv -> ack now, no fetch");
    CHECK(!bookpull_rule1_ack_now(6, 5), "nudge bv > stored bv -> fetch, not an immediate ack");
}

/* ---------------------------------------------------------------------
 * §14.7 request build — exact bytes of `M`, and X-Sig against a
 * `relay/.venv/bin/python`-computed vector:
 *   base64url(hmac_sha256(k, b"GET /api/device/book\0pgr-0001|123456789|7")[:8])
 * with k = bytes(range(32)) -- computed via:
 *   relay/.venv/bin/python -c "
 *     import sys; sys.path.insert(0, 'relay')
 *     from app import devauth
 *     print(devauth.request_tag(bytes(range(32)), 'pgr-0001', 123456789, 7).hex())"
 * -> 8769fe3a1b373762 -> base64url (no padding) -> h2n-Ohs3N2I (11 chars).
 * --------------------------------------------------------------------- */

static void test_request_build(void)
{
    char m[BOOKPULL_M_MAX];
    CHECK(bookpull_build_m(m, sizeof(m), "pgr-0001", 123456789ULL, 7), "bookpull_build_m() must succeed");
    CHECK(strcmp(m, "pgr-0001|123456789|7") == 0, "M mismatch: %s", m);

    uint8_t kdev[32];
    for (int i = 0; i < 32; i++) {
        kdev[i] = (uint8_t) i;
    }
    auth_init(kdev);

    uint8_t tag[8];
    CHECK(auth_request_tag("GET /api/device/book", (const uint8_t *) m, strlen(m), tag),
          "auth_request_tag() must succeed once auth_init() has run");
    static const uint8_t expected_tag[8] = { 0x87, 0x69, 0xfe, 0x3a, 0x1b, 0x37, 0x37, 0x62 };
    CHECK(memcmp(tag, expected_tag, 8) == 0, "tag mismatch vs the Python-computed vector");

    char sig_b64[BOOKPULL_SIG_B64_LEN + 1];
    bookpull_b64url_tag(tag, sig_b64);
    CHECK(strlen(sig_b64) == BOOKPULL_SIG_B64_LEN, "sig_b64 must be exactly %d chars, got %zu",
          BOOKPULL_SIG_B64_LEN, strlen(sig_b64));
    CHECK(strcmp(sig_b64, "h2n-Ohs3N2I") == 0, "X-Sig mismatch: %s", sig_b64);

    char hdrs[BOOKPULL_HDRS_MAX];
    CHECK(bookpull_build_headers(hdrs, sizeof(hdrs), "pgr-0001", 123456789ULL, sig_b64),
          "bookpull_build_headers() must succeed");
    CHECK(strcmp(hdrs, "X-Device-Id: pgr-0001\r\nX-N: 123456789\r\nX-Sig: h2n-Ohs3N2I\r\n") == 0,
          "headers mismatch: %s", hdrs);
}

/* ---------------------------------------------------------------------
 * §14.7 "Device on failure" table — bookpull_classify_http().
 * --------------------------------------------------------------------- */

static void test_classify_http(void)
{
    CHECK(bookpull_classify_http(200) == BOOKPULL_HTTP_APPLY, "200 -> APPLY");
    CHECK(bookpull_classify_http(409) == BOOKPULL_HTTP_RETRY_NOW, "409 -> RETRY_NOW");
    CHECK(bookpull_classify_http(400) == BOOKPULL_HTTP_DROP, "400 -> DROP");
    CHECK(bookpull_classify_http(401) == BOOKPULL_HTTP_DROP, "401 -> DROP");
    CHECK(bookpull_classify_http(404) == BOOKPULL_HTTP_DROP, "404 -> DROP");
    CHECK(bookpull_classify_http(500) == BOOKPULL_HTTP_RETRY_LATER, "500 -> RETRY_LATER");
    CHECK(bookpull_classify_http(503) == BOOKPULL_HTTP_RETRY_LATER, "503 -> RETRY_LATER");
    CHECK(bookpull_classify_http(0) == BOOKPULL_HTTP_RETRY_LATER,
          "0 (cafetch-layer timeout/oversize/malformed, no real status) -> RETRY_LATER");
}

/* ---------------------------------------------------------------------
 * §14.7 fetch response — signed CBOR fixtures, literal `devauth.sign_cbor()`
 * output for key = bytes(range(32)), topic "/api/device/book". Regenerate
 * with:
 *   relay/.venv/bin/python -c "
 *     import sys; sys.path.insert(0, 'relay')
 *     from app import devauth
 *     key = bytes(range(32))
 *     obj = {'v':1,'ts':1757700000,'kind':'book','bv':7,'d':'mom',
 *            'c':[{'a':'mom','n':'Mom','t':'web'},{'a':'dad','n':'Dad','t':'web'}],'p':[]}
 *     obj['n'] = 123456789
 *     print(devauth.sign_cbor(key, '/api/device/book', obj).hex())"
 * --------------------------------------------------------------------- */

/* v=1, ts=1757700000, kind="book", bv=7, d="mom", c=[mom,dad], p=[], n=123456789, signed. */
static const uint8_t k_resp_valid[] = {
    0xa9, 0x00, 0x01, 0x02, 0x1a, 0x68, 0xc4, 0x5f, 0xa0, 0x06, 0x64, 0x62, 0x6f, 0x6f, 0x6b,
    0x0e, 0x07, 0x11, 0x63, 0x6d, 0x6f, 0x6d, 0x12, 0x82, 0xa3, 0x00, 0x63, 0x6d, 0x6f, 0x6d,
    0x01, 0x63, 0x4d, 0x6f, 0x6d, 0x02, 0x63, 0x77, 0x65, 0x62, 0xa3, 0x00, 0x63, 0x64, 0x61,
    0x64, 0x01, 0x63, 0x44, 0x61, 0x64, 0x02, 0x63, 0x77, 0x65, 0x62, 0x13, 0x80, 0x0c, 0x1a,
    0x07, 0x5b, 0xcd, 0x15, 0x0d, 0x48, 0x96, 0x0b, 0x0b, 0x27, 0x12, 0xb4, 0x4e, 0x43,
};

/* Same shape, n=999999999 instead of 123456789 (signed correctly with THAT
 * n) -- exercises "the signature verifies fine, but n does not match the
 * request's own X-N". */
static const uint8_t k_resp_wrong_n[] = {
    0xa9, 0x00, 0x01, 0x02, 0x1a, 0x68, 0xc4, 0x5f, 0xa0, 0x06, 0x64, 0x62, 0x6f, 0x6f, 0x6b,
    0x0e, 0x07, 0x11, 0x63, 0x6d, 0x6f, 0x6d, 0x12, 0x82, 0xa3, 0x00, 0x63, 0x6d, 0x6f, 0x6d,
    0x01, 0x63, 0x4d, 0x6f, 0x6d, 0x02, 0x63, 0x77, 0x65, 0x62, 0xa3, 0x00, 0x63, 0x64, 0x61,
    0x64, 0x01, 0x63, 0x44, 0x61, 0x64, 0x02, 0x63, 0x77, 0x65, 0x62, 0x13, 0x80, 0x0c, 0x1a,
    0x3b, 0x9a, 0xc9, 0xff, 0x0d, 0x48, 0x3d, 0x60, 0xb6, 0xe5, 0x9c, 0x6f, 0x4c, 0x4a,
};

/* Same shape, bv=3 instead of 7 (signed correctly). */
static const uint8_t k_resp_low_bv[] = {
    0xa9, 0x00, 0x01, 0x02, 0x1a, 0x68, 0xc4, 0x5f, 0xa0, 0x06, 0x64, 0x62, 0x6f, 0x6f, 0x6b,
    0x0e, 0x03, 0x11, 0x63, 0x6d, 0x6f, 0x6d, 0x12, 0x82, 0xa3, 0x00, 0x63, 0x6d, 0x6f, 0x6d,
    0x01, 0x63, 0x4d, 0x6f, 0x6d, 0x02, 0x63, 0x77, 0x65, 0x62, 0xa3, 0x00, 0x63, 0x64, 0x61,
    0x64, 0x01, 0x63, 0x44, 0x61, 0x64, 0x02, 0x63, 0x77, 0x65, 0x62, 0x13, 0x80, 0x0c, 0x1a,
    0x07, 0x5b, 0xcd, 0x15, 0x0d, 0x48, 0xc8, 0x37, 0x67, 0xc5, 0xc0, 0xe7, 0xf6, 0xf7,
};

static void auth_init_test_key(void)
{
    uint8_t kdev[32];
    for (int i = 0; i < 32; i++) {
        kdev[i] = (uint8_t) i;
    }
    auth_init(kdev);
}

static void test_response_valid_applies(void)
{
    auth_init_test_key();

    uint8_t buf[sizeof(k_resp_valid)];
    memcpy(buf, k_resp_valid, sizeof(buf));
    size_t len = sizeof(buf);
    CHECK(auth_verify_label("/api/device/book", buf, &len), "a validly-signed response must verify");

    book_parsed_t p;
    CHECK(book_parse(buf, (uint16_t) len, true, &p), "the verified body must decode as kind:\"book\"");
    CHECK(p.n == 123456789ULL, "n mismatch: %llu", (unsigned long long) p.n);
    CHECK(p.bv == 7, "bv mismatch: %u", (unsigned) p.bv);
    CHECK(p.n_contacts == 2, "expected 2 contacts, got %u", (unsigned) p.n_contacts);

    /* book_apply_fetched()'s own checks, exercised directly since the real
     * function is ESP_PLATFORM/NVS-only: n==expect_n, bv>=min_bv. */
    CHECK(p.n == 123456789ULL, "response would be ACCEPTED: n matches expect_n");
    CHECK(p.bv >= 7, "response would be ACCEPTED: bv >= nudge bv");
}

static void test_response_wrong_n_rejected(void)
{
    auth_init_test_key();

    uint8_t buf[sizeof(k_resp_wrong_n)];
    memcpy(buf, k_resp_wrong_n, sizeof(buf));
    size_t len = sizeof(buf);
    /* This vector is validly signed FOR ITS OWN embedded n=999999999 (the
     * HMAC covers the whole payload, n included) — the point of this test
     * is that a genuine, correctly-signed response is still rejected when
     * its n does not match what THIS device's request actually sent
     * (§14.7: "the device verifies the tag AND the n echo"), e.g. a replay
     * of an old response. */
    CHECK(auth_verify_label("/api/device/book", buf, &len),
          "signature must verify (this vector is correctly signed for n=999999999)");

    book_parsed_t p;
    CHECK(book_parse(buf, (uint16_t) len, true, &p), "must still decode");
    uint64_t expect_n = 123456789ULL;
    CHECK(p.n != expect_n, "n must NOT match expect_n -> book_apply_fetched() rejects this response");
}

static void test_response_bad_tag_rejected(void)
{
    auth_init_test_key();

    uint8_t buf[sizeof(k_resp_valid)];
    memcpy(buf, k_resp_valid, sizeof(buf));
    buf[sizeof(buf) - 1] ^= 0xFF; /* flip the last tag byte */
    size_t len = sizeof(buf);
    CHECK(!auth_verify_label("/api/device/book", buf, &len), "a tampered signature must NOT verify");
}

static void test_response_low_bv_not_acked(void)
{
    auth_init_test_key();

    uint8_t buf[sizeof(k_resp_low_bv)];
    memcpy(buf, k_resp_low_bv, sizeof(buf));
    size_t len = sizeof(buf);
    CHECK(auth_verify_label("/api/device/book", buf, &len), "signature must verify");

    book_parsed_t p;
    CHECK(book_parse(buf, (uint16_t) len, true, &p), "must still decode");
    uint32_t nudge_bv = 7;
    CHECK(p.bv < nudge_bv, "bv (%u) must be < the nudge's own bv (%u) -> book_apply_fetched() rejects",
          (unsigned) p.bv, (unsigned) nudge_bv);
}

/* 40 contacts -> book_parse() must keep exactly the first BOOK_MAX_CONTACTS
 * (32), dropping the rest (parsed for buffer-position correctness, per this
 * function's own doc comment, but not copied into `out`). */
static void test_response_32_of_40_kept(void)
{
    uint8_t buf[1024];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 4);
    cbor_w_tstr(&w, 6, "book", 4);
    cbor_w_uint(&w, 14, 7);
    cbor_w_array(&w, 18, 40);
    char alias[8];
    for (int i = 0; i < 40; i++) {
        snprintf(alias, sizeof(alias), "a%02d", i);
        cbor_w_map(&w, 3);
        cbor_w_tstr(&w, 0, alias, strlen(alias));
        cbor_w_tstr(&w, 1, alias, strlen(alias));
        cbor_w_tstr(&w, 2, "web", 3);
    }
    cbor_w_array(&w, 19, 0);
    CHECK(!w.err, "test setup: encoding 40 contacts must not overflow");

    book_parsed_t p;
    CHECK(book_parse(buf, (uint16_t) w.len, false, &p), "must decode despite 40 contacts");
    CHECK(p.n_contacts == BOOK_MAX_CONTACTS, "expected exactly %d contacts kept, got %u",
          BOOK_MAX_CONTACTS, (unsigned) p.n_contacts);
    CHECK(strcmp(p.contacts[0].alias, "a00") == 0, "first kept contact must be the first in wire order");
    CHECK(strcmp(p.contacts[31].alias, "a31") == 0, "32nd kept contact must be a31 (index 31), not a39");
}

/* ---------------------------------------------------------------------
 * §5.5 nickname carry-over — book_carry_nicknames(), pure, no NVS.
 * --------------------------------------------------------------------- */

static void test_nickname_carried(void)
{
    book_contact_t old_contacts[2];
    memset(old_contacts, 0, sizeof(old_contacts));
    strcpy(old_contacts[0].alias, "mom");
    strcpy(old_contacts[0].nickname, "Mommy");
    strcpy(old_contacts[1].alias, "dad");
    strcpy(old_contacts[1].nickname, "Daddy");

    book_contact_t new_contacts[3];
    memset(new_contacts, 0, sizeof(new_contacts));
    strcpy(new_contacts[0].alias, "dad"); /* reordered vs old, still must carry */
    strcpy(new_contacts[1].alias, "mom");
    strcpy(new_contacts[2].alias, "grandma"); /* new alias, no old nickname to carry */

    book_carry_nicknames(new_contacts, 3, old_contacts, 2);

    CHECK(strcmp(new_contacts[0].nickname, "Daddy") == 0, "dad's nickname must carry: got '%s'",
          new_contacts[0].nickname);
    CHECK(strcmp(new_contacts[1].nickname, "Mommy") == 0, "mom's nickname must carry: got '%s'",
          new_contacts[1].nickname);
    CHECK(new_contacts[2].nickname[0] == '\0',
          "a brand-new alias must have no nickname to carry: got '%s'", new_contacts[2].nickname);
}

/* ---------------------------------------------------------------------
 * cafetch.c's own oversize cap (4096+1 body) — the shared component
 * bookpull.c relies on for its own body-size guarantee.
 * --------------------------------------------------------------------- */

static void test_oversize_body_4096_plus_1(void)
{
    cafetch_parser_t p;
    cafetch_parser_init(&p);
    char resp[128];
    int n = snprintf(resp, sizeof(resp), "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n",
                     CAFETCH_BODY_MAX + 1);
    cafetch_parser_feed(&p, (const uint8_t *) resp, (size_t) n);
    CHECK(p.phase == CAFETCH_PHASE_ERROR && p.err_oversize,
          "a %d-byte body (CAFETCH_BODY_MAX+1) must be refused as oversize", CAFETCH_BODY_MAX + 1);
}

int main(void)
{
    test_nudge_parse_bv_url_no_c_p();
    test_book_with_c_is_not_a_nudge();
    test_nudge_missing_or_bad_url_is_malformed();
    test_rule1();
    test_request_build();
    test_classify_http();
    test_response_valid_applies();
    test_response_wrong_n_rejected();
    test_response_bad_tag_rejected();
    test_response_low_bv_not_acked();
    test_response_32_of_40_kept();
    test_nickname_carried();
    test_oversize_body_4096_plus_1();

    if (g_failures == 0) {
        printf("PASS: bookpull, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

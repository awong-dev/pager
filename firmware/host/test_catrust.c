/* test_catrust.c — host test harness for main/catrust.c's pure section
 * (docs/V02_DESIGN.md §4): trust-state derivation, the TLS-fail fallback
 * policy (with a caller-owned streak counter), the daily/cold-boot
 * revalidation-while-broken gate (fake clock), the `cfg.ca` sub-map decode
 * (pin and un-pin shapes), the give-up counter and the `/status` fingerprint
 * formatter.
 */
#include "catrust.h"

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
 * Trust-state derivation.
 * --------------------------------------------------------------------- */

static void test_derive_state(void)
{
    CHECK(catrust_derive_state(false, false) == CATRUST_UNPINNED, "no CA -> unpinned");
    CHECK(catrust_derive_state(false, true) == CATRUST_UNPINNED,
          "no CA -> unpinned even if the broken byte was somehow left set (stale from a prior identity)");
    CHECK(catrust_derive_state(true, false) == CATRUST_PINNED, "CA + not broken -> pinned");
    CHECK(catrust_derive_state(true, true) == CATRUST_BROKEN, "CA + broken -> broken");
}

/* ---------------------------------------------------------------------
 * TLS-fail fallback (docs/V02_DESIGN.md §4.2).
 * --------------------------------------------------------------------- */

static void test_tls_fail_pinned_retry_then_fallback(void)
{
    uint32_t streak = 0;
    catrust_tls_action_t a1 = catrust_on_tls_fail(/*currently_broken=*/false, /*have_ca=*/true, &streak);
    CHECK(a1 == CATRUST_TLS_RETRY_VALIDATED, "1st TLS_FAIL while pinned must retry validated once more");
    CHECK(streak == 1, "streak must be 1 after the first failure");

    catrust_tls_action_t a2 = catrust_on_tls_fail(false, true, &streak);
    CHECK(a2 == CATRUST_TLS_FALL_BACK, "2nd consecutive TLS_FAIL while pinned must fall back");
    CHECK(streak == 0, "streak must reset to 0 once it falls back");

    /* A fresh streak (e.g. after a successful connect cleared it) behaves
     * the same way again. */
    catrust_tls_action_t a3 = catrust_on_tls_fail(false, true, &streak);
    CHECK(a3 == CATRUST_TLS_RETRY_VALIDATED, "the streak must restart cleanly after a reset");
}

static void test_tls_fail_broken_and_unpinned_are_steady(void)
{
    uint32_t streak = 5; /* deliberately nonzero, to prove these paths reset it */
    CHECK(catrust_on_tls_fail(/*currently_broken=*/true, /*have_ca=*/true, &streak) == CATRUST_TLS_STEADY,
          "already broken -> steady (nothing left to fall back to)");
    CHECK(streak == 0, "streak must be cleared even on the steady path");

    streak = 3;
    CHECK(catrust_on_tls_fail(/*currently_broken=*/false, /*have_ca=*/false, &streak) == CATRUST_TLS_STEADY,
          "unpinned -> steady (nothing to fall back to either)");
    CHECK(streak == 0, "streak must be cleared for the unpinned case too");
}

/* ---------------------------------------------------------------------
 * Daily/cold-boot revalidation gate (fake clock).
 * --------------------------------------------------------------------- */

static void test_due_for_validated_retry(void)
{
    int64_t day_us = (int64_t) CATRUST_BROKEN_REVALIDATE_S * 1000000;

    CHECK(catrust_due_for_validated_retry(0, 1000, /*is_first_since_boot=*/true),
          "the first attempt this boot is always due, regardless of the clock");
    CHECK(catrust_due_for_validated_retry(0, 1000, false),
          "never attempted this boot yet (0) -> due, even if not flagged first-since-boot");

    int64_t last = 1000000; /* 1s */
    CHECK(!catrust_due_for_validated_retry(last, last + day_us - 1, false),
          "1 microsecond short of 24h must not be due yet");
    CHECK(catrust_due_for_validated_retry(last, last + day_us, false),
          "exactly 24h since the last attempt must be due");
    CHECK(catrust_due_for_validated_retry(last, last + day_us + 3600LL * 1000000, false),
          "well past 24h must still be due");
}

/* ---------------------------------------------------------------------
 * `cfg.ca` sub-map decode.
 * --------------------------------------------------------------------- */

static void build_ca_pin(uint8_t *buf, size_t cap, size_t *len, const char *url, const uint8_t sha[32])
{
    cbor_w_t w;
    cbor_w_init(&w, buf, cap);
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, 0, url, strlen(url));
    cbor_w_bstr(&w, 1, sha, 32);
    *len = w.len;
}

static void test_parse_cfg_pin(void)
{
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) {
        sha[i] = (uint8_t) (i * 7);
    }
    uint8_t buf[128];
    size_t len;
    build_ca_pin(buf, sizeof(buf), &len, "https://pager-relay-example.a.run.app/ca/abc.pem", sha);

    catrust_cfg_t out;
    bool ok = catrust_parse_cfg_submap(buf, (uint16_t) len, &out);
    CHECK(ok, "a well-formed {url,sha} pin sub-map must be accepted");
    CHECK(!out.unpin, "a pin sub-map must not be flagged unpin");
    CHECK(strcmp(out.url, "https://pager-relay-example.a.run.app/ca/abc.pem") == 0, "url mismatch: %s",
          out.url);
    CHECK(memcmp(out.sha, sha, 32) == 0, "sha mismatch");
}

static void test_parse_cfg_unpin(void)
{
    uint8_t buf[32];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_tstr(&w, 0, "", 0);
    CHECK(!w.err, "test setup: encoding the un-pin fixture must not overflow");

    catrust_cfg_t out;
    bool ok = catrust_parse_cfg_submap(buf, (uint16_t) w.len, &out);
    CHECK(ok, "the un-pin shape ({url:\"\"}, no sha) must be accepted");
    CHECK(out.unpin, "must be flagged unpin");
}

static void test_parse_cfg_malformed(void)
{
    catrust_cfg_t out;

    /* url present but non-empty, no sha: malformed. */
    {
        uint8_t buf[32];
        cbor_w_t w;
        cbor_w_init(&w, buf, sizeof(buf));
        cbor_w_map(&w, 1);
        cbor_w_tstr(&w, 0, "https://example.com/ca/x.pem", 29);
        CHECK(!catrust_parse_cfg_submap(buf, (uint16_t) w.len, &out),
              "a non-empty url with no sha must be rejected");
    }

    /* empty url WITH a sha: malformed (nonsensical combination). */
    {
        uint8_t sha[32] = { 0 };
        uint8_t buf[64];
        cbor_w_t w;
        cbor_w_init(&w, buf, sizeof(buf));
        cbor_w_map(&w, 2);
        cbor_w_tstr(&w, 0, "", 0);
        cbor_w_bstr(&w, 1, sha, 32);
        CHECK(!catrust_parse_cfg_submap(buf, (uint16_t) w.len, &out),
              "an empty url with a sha present must be rejected");
    }

    /* no url at all: malformed. */
    {
        uint8_t sha[32] = { 0 };
        uint8_t buf[64];
        cbor_w_t w;
        cbor_w_init(&w, buf, sizeof(buf));
        cbor_w_map(&w, 1);
        cbor_w_bstr(&w, 1, sha, 32);
        CHECK(!catrust_parse_cfg_submap(buf, (uint16_t) w.len, &out), "a sub-map with no url must be rejected");
    }

    /* wrong-length sha: malformed. */
    {
        uint8_t short_sha[16] = { 0 };
        uint8_t buf[64];
        cbor_w_t w;
        cbor_w_init(&w, buf, sizeof(buf));
        cbor_w_map(&w, 2);
        cbor_w_tstr(&w, 0, "https://example.com/ca/x.pem", 29);
        cbor_w_bstr(&w, 1, short_sha, sizeof(short_sha));
        CHECK(!catrust_parse_cfg_submap(buf, (uint16_t) w.len, &out), "a 16-byte sha must be rejected (not 32)");
    }
}

/* ---------------------------------------------------------------------
 * Give-up counter + fingerprint formatter.
 * --------------------------------------------------------------------- */

static void test_give_up(void)
{
    CHECK(!catrust_give_up(0), "0 failures -> not given up");
    CHECK(!catrust_give_up(1), "1 failure -> not given up");
    CHECK(!catrust_give_up(2), "2 failures -> not given up yet");
    CHECK(catrust_give_up(3), "3 failures -> give up");
    CHECK(catrust_give_up(255), "a very large count must stay given-up");
}

static void test_fingerprint_hex(void)
{
    uint8_t hash[32];
    for (int i = 0; i < 32; i++) {
        hash[i] = (uint8_t) i;
    }
    char out[17];
    catrust_fingerprint_hex(hash, out);
    CHECK(strcmp(out, "000102030405060708") != 0, "sanity: this comparison itself must be 16 chars, not 18");
    CHECK(strlen(out) == 16, "fingerprint must be exactly 16 hex chars, got %zu", strlen(out));
    CHECK(strcmp(out, "0001020304050607") == 0, "unexpected fingerprint: %s", out);
}

int main(void)
{
    test_derive_state();
    test_tls_fail_pinned_retry_then_fallback();
    test_tls_fail_broken_and_unpinned_are_steady();
    test_due_for_validated_retry();
    test_parse_cfg_pin();
    test_parse_cfg_unpin();
    test_parse_cfg_malformed();
    test_give_up();
    test_fingerprint_hex();

    if (g_failures == 0) {
        printf("PASS: catrust, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

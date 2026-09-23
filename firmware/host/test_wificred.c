/* test_wificred.c — host test harness for main/wificred.c's pure section
 * (docs/WIFI_TASKS.md W2/W3, docs/WIFI_DESIGN.md §4): SSID/PSK validation,
 * the replace-wholesale candidate applier, and the `cfg.wifi` sub-map decode.
 *
 * W2 coverage required by the task brief: every validation boundary
 * (0/1/32/33-byte SSID; 7/8/63/64-byte PSK; embedded NUL), 3 entries
 * refused, empty list clears. Also covers, beyond the task's own list but
 * for the same NVS str-storage reason wificred.h's own module comment gives
 * for the SSID check: an embedded NUL in the PSK is rejected too, and a
 * rejected batch never touches the caller's existing set (the
 * `sms_parse_cfg_submap()`-style "reject the whole list" contract).
 *
 * W3 coverage required by the task brief: a full push with 2 networks;
 * `en` only; `nets: []`; 3 networks rejected (whole push refused, not
 * truncated); an oversize SSID rejected; an unknown sub-key ignored; a
 * malformed array rejected without reading out of bounds. Fixtures are
 * hand-built with cbor.c's own writer, literal PROTOCOL.md §10 `cfg.wifi`
 * keys inline (en=0, nets=1; each `nets[]` item: s=0, p=1) — same convention
 * firmware/host/test_cfg.c's own fixture-building tests already use.
 */
#include "wificred.h"

#include <stdio.h>
#include <string.h>

#include "cbor.h"

static int g_failures = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                          \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

static char *repeat(char *buf, char c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        buf[i] = c;
    }
    buf[n] = '\0';
    return buf;
}

/* ---------------------------------------------------------------------
 * SSID boundaries: 0/1/32/33 bytes, embedded NUL.
 * --------------------------------------------------------------------- */
static void test_ssid_boundaries(void)
{
    char buf[40];

    CHECK(!wificred_valid_ssid("", 0), "0-byte SSID must be rejected");
    CHECK(wificred_valid_ssid(repeat(buf, 'a', 1), 1), "1-byte SSID must be accepted");
    CHECK(wificred_valid_ssid(repeat(buf, 'a', 32), 32), "32-byte SSID must be accepted");
    CHECK(!wificred_valid_ssid(repeat(buf, 'a', 33), 33), "33-byte SSID must be rejected");

    char with_nul[10] = "abc";
    with_nul[3] = '\0';
    with_nul[4] = 'd'; /* bytes after the embedded NUL, still within `len` */
    CHECK(!wificred_valid_ssid(with_nul, 5), "an SSID with an embedded NUL must be rejected");

    CHECK(!wificred_valid_ssid(NULL, 5), "a NULL SSID pointer must be rejected");
}

/* ---------------------------------------------------------------------
 * PSK boundaries: 7/8/63/64 bytes, embedded NUL (beyond the task's own list,
 * same NVS str round-trip reason as the SSID check).
 * --------------------------------------------------------------------- */
static void test_psk_boundaries(void)
{
    char buf[70];

    CHECK(!wificred_valid_psk(repeat(buf, 'a', 7), 7), "7-byte PSK must be rejected");
    CHECK(wificred_valid_psk(repeat(buf, 'a', 8), 8), "8-byte PSK must be accepted");
    CHECK(wificred_valid_psk(repeat(buf, 'a', 63), 63), "63-byte PSK must be accepted");
    CHECK(!wificred_valid_psk(repeat(buf, 'a', 64), 64), "64-byte PSK must be rejected");

    char with_nul[12] = "abcdefgh";
    with_nul[8] = '\0';
    with_nul[9] = 'i';
    CHECK(!wificred_valid_psk(with_nul, 10), "a PSK with an embedded NUL must be rejected");

    CHECK(!wificred_valid_psk(NULL, 10), "a NULL PSK pointer must be rejected");
}

/* ---------------------------------------------------------------------
 * wificred_apply_candidates(): a full push, 3 entries refused, empty
 * clears, an oversize field rejects the whole batch, and a rejected batch
 * never touches the caller's existing set.
 * --------------------------------------------------------------------- */
static void test_apply_full_push(void)
{
    wificred_candidate_t c[2] = {
        { .ssid = "home-network", .ssid_len = 12, .psk = "correcthorsebattery", .psk_len = 19 },
        { .ssid = "office", .ssid_len = 6, .psk = "anotherpassphrase", .psk_len = 17 },
    };
    wificred_set_t out;
    memset(&out, 0xAA, sizeof(out)); /* poison, so a bug can't accidentally look zero-initialised */

    bool ok = wificred_apply_candidates(c, 2, &out);
    CHECK(ok, "a 2-network push must be accepted");
    CHECK(out.count == 2, "count must be 2, got %u", (unsigned) out.count);
    CHECK(strcmp(out.nets[0].ssid, "home-network") == 0, "ssid[0] mismatch: %s", out.nets[0].ssid);
    CHECK(strcmp(out.nets[0].psk, "correcthorsebattery") == 0, "psk[0] mismatch");
    CHECK(strcmp(out.nets[1].ssid, "office") == 0, "ssid[1] mismatch: %s", out.nets[1].ssid);
    CHECK(out.nets[0].channel == 0 && out.nets[1].channel == 0,
          "a push must never set a channel (only wificred_note_channel() does, after association)");
}

static void test_apply_three_entries_refused(void)
{
    wificred_candidate_t c[3] = {
        { .ssid = "a", .ssid_len = 1, .psk = "password", .psk_len = 8 },
        { .ssid = "b", .ssid_len = 1, .psk = "password", .psk_len = 8 },
        { .ssid = "c", .ssid_len = 1, .psk = "password", .psk_len = 8 },
    };
    wificred_set_t out;
    memset(&out, 0, sizeof(out));
    out.count = 1;
    strcpy(out.nets[0].ssid, "existing");

    bool ok = wificred_apply_candidates(c, 3, &out);
    CHECK(!ok, "a 3-network push must be refused (WIFICRED_MAX_NETS is 2)");
    CHECK(out.count == 1 && strcmp(out.nets[0].ssid, "existing") == 0,
          "a refused batch must never touch the caller's existing set");
}

static void test_apply_empty_clears(void)
{
    wificred_set_t out;
    memset(&out, 0, sizeof(out));
    out.count = 2;
    strcpy(out.nets[0].ssid, "old0");
    strcpy(out.nets[1].ssid, "old1");

    bool ok = wificred_apply_candidates(NULL, 0, &out);
    CHECK(ok, "an empty (0-entry) push must be accepted");
    CHECK(out.count == 0, "an empty push must clear the set, got count=%u", (unsigned) out.count);
}

static void test_apply_oversize_rejects_whole_batch(void)
{
    char big_ssid[40];
    repeat(big_ssid, 'x', 33); /* one byte over the SSID cap */

    wificred_candidate_t c[2] = {
        { .ssid = "fine", .ssid_len = 4, .psk = "password", .psk_len = 8 },
        { .ssid = big_ssid, .ssid_len = 33, .psk = "password", .psk_len = 8 },
    };
    wificred_set_t out;
    memset(&out, 0, sizeof(out));
    out.count = 1;
    strcpy(out.nets[0].ssid, "existing");

    bool ok = wificred_apply_candidates(c, 2, &out);
    CHECK(!ok, "a batch with one oversize SSID must reject the WHOLE batch, not just that entry");
    CHECK(out.count == 1 && strcmp(out.nets[0].ssid, "existing") == 0,
          "a rejected batch must never touch the caller's existing set, even partially");
}

static void test_apply_two_max_is_ok(void)
{
    wificred_candidate_t c[WIFICRED_MAX_NETS];
    for (int i = 0; i < WIFICRED_MAX_NETS; i++) {
        c[i].ssid = "network";
        c[i].ssid_len = 7;
        c[i].psk = "password";
        c[i].psk_len = 8;
    }
    wificred_set_t out;
    bool ok = wificred_apply_candidates(c, WIFICRED_MAX_NETS, &out);
    CHECK(ok, "exactly WIFICRED_MAX_NETS entries must be accepted");
    CHECK(out.count == WIFICRED_MAX_NETS, "count must equal WIFICRED_MAX_NETS");
}

/* ---------------------------------------------------------------------
 * `cfg.wifi` sub-map decode (W3). PROTOCOL.md §10 `cfg.wifi` keys: en=0
 * (bool), nets=1 (array). Each `nets[]` item: s=0 (tstr), p=1 (tstr).
 * --------------------------------------------------------------------- */
static void test_cfg_full_push_two_networks(void)
{
    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 2);
    cbor_w_bool(&w, 0, true); /* en: true */
    cbor_w_array(&w, 1, 2);  /* nets: [ {s,p}, {s,p} ] */
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, 0, "home-network", 12);
    cbor_w_tstr(&w, 1, "correcthorsebattery", 19);
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, 0, "office", 6);
    cbor_w_tstr(&w, 1, "anotherpassphrase", 17);
    CHECK(!w.err, "test setup: encoding the full-push fixture must not overflow");

    wificred_cfg_t cfg;
    bool ok = wificred_parse_cfg_submap(buf, (uint16_t) w.len, &cfg);
    CHECK(ok, "a full cfg.wifi push with 2 networks must be accepted");
    CHECK(cfg.have_en && cfg.en, "en must be true");
    CHECK(cfg.have_nets && cfg.nets.count == 2, "nets.count must be 2, got %u",
          (unsigned) cfg.nets.count);
    CHECK(strcmp(cfg.nets.nets[0].ssid, "home-network") == 0, "ssid[0] mismatch: %s",
          cfg.nets.nets[0].ssid);
    CHECK(strcmp(cfg.nets.nets[1].ssid, "office") == 0, "ssid[1] mismatch: %s", cfg.nets.nets[1].ssid);
}

static void test_cfg_en_only(void)
{
    uint8_t buf[32];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_bool(&w, 0, false); /* en: false, no `nets` key at all */
    CHECK(!w.err, "test setup: encoding the en-only fixture must not overflow");

    wificred_cfg_t cfg;
    bool ok = wificred_parse_cfg_submap(buf, (uint16_t) w.len, &cfg);
    CHECK(ok, "an en-only push must be accepted");
    CHECK(cfg.have_en && !cfg.en, "en must be false");
    CHECK(!cfg.have_nets, "have_nets must be false when `nets` is absent -- leave stored networks alone");
}

static void test_cfg_nets_empty_clears(void)
{
    uint8_t buf[32];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_array(&w, 1, 0); /* nets: [] -- no `en` key at all */
    CHECK(!w.err, "test setup: encoding the empty-nets fixture must not overflow");

    wificred_cfg_t cfg;
    bool ok = wificred_parse_cfg_submap(buf, (uint16_t) w.len, &cfg);
    CHECK(ok, "a `nets: []` push must be accepted");
    CHECK(!cfg.have_en, "have_en must be false when `en` is absent");
    CHECK(cfg.have_nets && cfg.nets.count == 0,
          "have_nets must be true with count 0 -- `nets: []` clears the stored networks");
}

static void test_cfg_three_networks_rejected(void)
{
    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_array(&w, 1, 3); /* nets: [ {s,p} x3 ] -- one over WIFICRED_MAX_NETS */
    for (int i = 0; i < 3; i++) {
        cbor_w_map(&w, 2);
        cbor_w_tstr(&w, 0, "net", 3);
        cbor_w_tstr(&w, 1, "password", 8);
    }
    CHECK(!w.err, "test setup: encoding the 3-network fixture must not overflow");

    wificred_cfg_t cfg;
    bool ok = wificred_parse_cfg_submap(buf, (uint16_t) w.len, &cfg);
    CHECK(!ok, "a 3-network push must be refused (over WIFICRED_MAX_NETS), not truncated to 2");
}

static void test_cfg_oversize_ssid_rejected(void)
{
    char big_ssid[40];
    for (int i = 0; i < 33; i++) {
        big_ssid[i] = 'x';
    }

    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_array(&w, 1, 1);
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, 0, big_ssid, 33); /* one byte over the SSID cap */
    cbor_w_tstr(&w, 1, "password", 8);
    CHECK(!w.err, "test setup: encoding the oversize-ssid fixture must not overflow");

    wificred_cfg_t cfg;
    bool ok = wificred_parse_cfg_submap(buf, (uint16_t) w.len, &cfg);
    CHECK(!ok, "an oversize SSID must reject the whole cfg.wifi push");
}

static void test_cfg_unknown_subkey_ignored(void)
{
    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    /* Unknown key at the {en, nets} level (99), plus an unknown key inside a
     * network item (2) -- both must be skipped, never a parse failure. */
    cbor_w_map(&w, 3);
    cbor_w_bool(&w, 0, true);
    cbor_w_uint(&w, 99, 7);
    cbor_w_array(&w, 1, 1);
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 0, "home", 4);
    cbor_w_tstr(&w, 1, "password", 8);
    cbor_w_uint(&w, 2, 42); /* unknown network-item key */
    CHECK(!w.err, "test setup: encoding the unknown-subkey fixture must not overflow");

    wificred_cfg_t cfg;
    bool ok = wificred_parse_cfg_submap(buf, (uint16_t) w.len, &cfg);
    CHECK(ok, "unknown sub-keys (at either level) must be skipped, not treated as malformed");
    CHECK(cfg.have_en && cfg.en, "en must still be true");
    CHECK(cfg.have_nets && cfg.nets.count == 1, "the one well-formed network must still be captured");
}

static void test_cfg_malformed_array_rejected(void)
{
    /* A `nets` array header claiming 1 element, with no element bytes
     * following at all -- the reader must fail closed (bounds-checked by
     * cbor.c), never read past the end of the buffer. */
    uint8_t buf[64];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_array(&w, 1, 1); /* nets: [ <nothing actually follows> ] */
    CHECK(!w.err, "test setup: encoding the malformed-array fixture must not overflow");

    wificred_cfg_t cfg;
    bool ok = wificred_parse_cfg_submap(buf, (uint16_t) w.len, &cfg);
    CHECK(!ok, "a truncated `nets` array must be rejected, not read out of bounds");
}

int main(void)
{
    test_ssid_boundaries();
    test_psk_boundaries();
    test_apply_full_push();
    test_apply_three_entries_refused();
    test_apply_empty_clears();
    test_apply_oversize_rejects_whole_batch();
    test_apply_two_max_is_ok();

    test_cfg_full_push_two_networks();
    test_cfg_en_only();
    test_cfg_nets_empty_clears();
    test_cfg_three_networks_rejected();
    test_cfg_oversize_ssid_rejected();
    test_cfg_unknown_subkey_ignored();
    test_cfg_malformed_array_rejected();

    if (g_failures == 0) {
        printf("PASS: wificred, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

/* test_cfg.c — host test harness for main/cfg.c's pure section (task: CA
 * trust, docs/V02_DESIGN.md §4.4/§7): the `cfg` envelope dispatcher that
 * extracts each recognised sub-map's raw CBOR byte span (`lock`, `ca`, and
 * the not-yet-consumed `sms`) so lock.c/catrust.c can each decode their own
 * span standalone.
 *
 * Fixtures are hand-built with cbor.c's own writer, literal PROTOCOL.md §10
 * keys inline (1=id, 6=kind, 38=cfg; CFG_KEYMAP 0=lock/1=ca/2=sms;
 * LOCK_KEYMAP 0=clear/1=auto), same convention firmware/host/test_lock.c's
 * own fixture-building tests already use.
 */
#include "cfg.h"

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

static void test_lock_only(void)
{
    uint8_t buf[128];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 1, "m_11111111", 10);
    cbor_w_tstr(&w, 6, "cfg", 3);
    cbor_w_map_key(&w, 38, 1);
    cbor_w_map_key(&w, 0, 2); /* lock: {clear, auto} */
    cbor_w_bool(&w, 0, true);
    cbor_w_uint(&w, 1, 5);
    CHECK(!w.err, "test setup: encoding the lock-only fixture must not overflow");

    cfg_dispatch_t d;
    bool ok = cfg_parse(buf, (uint16_t) w.len, false, &d);
    CHECK(ok, "a cfg envelope with only `lock` must be accepted");
    CHECK(strcmp(d.id, "m_11111111") == 0, "id mismatch: %s", d.id);
    CHECK(d.have_lock, "have_lock must be true");
    CHECK(!d.have_ca, "have_ca must be false");
    CHECK(!d.have_sms, "have_sms must be false");

    /* The captured span must itself decode as the lock sub-map. */
    cbor_r_t r;
    cbor_r_init(&r, buf + d.lock_off, d.lock_len);
    uint32_t count;
    CHECK(cbor_r_map(&r, &count) && count == 2, "captured lock span must decode as a 2-pair map");
}

static void test_ca_only(void)
{
    uint8_t sha[32];
    for (int i = 0; i < 32; i++) {
        sha[i] = (uint8_t) i;
    }

    uint8_t buf[192];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 1, "m_22222222", 10);
    cbor_w_tstr(&w, 6, "cfg", 3);
    cbor_w_map_key(&w, 38, 1);
    cbor_w_map_key(&w, 1, 2); /* ca: {url, sha} */
    cbor_w_tstr(&w, 0, "https://example.com/ca/abc.pem", 31);
    cbor_w_bstr(&w, 1, sha, sizeof(sha));
    CHECK(!w.err, "test setup: encoding the ca-only fixture must not overflow");

    cfg_dispatch_t d;
    bool ok = cfg_parse(buf, (uint16_t) w.len, false, &d);
    CHECK(ok, "a cfg envelope with only `ca` must be accepted");
    CHECK(!d.have_lock, "have_lock must be false");
    CHECK(d.have_ca, "have_ca must be true");

    cbor_r_t r;
    cbor_r_init(&r, buf + d.ca_off, d.ca_len);
    uint32_t count;
    CHECK(cbor_r_map(&r, &count) && count == 2, "captured ca span must decode as a 2-pair map");
}

static void test_both_lock_and_ca(void)
{
    uint8_t sha[32] = { 0 };
    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 1, "m_33333333", 10);
    cbor_w_tstr(&w, 6, "cfg", 3);
    cbor_w_map_key(&w, 38, 2); /* cfg: {lock, ca} */
    cbor_w_map_key(&w, 0, 1);
    cbor_w_uint(&w, 1, 10); /* lock: {auto:10} */
    cbor_w_map_key(&w, 1, 2);
    cbor_w_tstr(&w, 0, "https://example.com/ca/x.pem", 29);
    cbor_w_bstr(&w, 1, sha, sizeof(sha));
    CHECK(!w.err, "test setup: encoding the both fixture must not overflow");

    cfg_dispatch_t d;
    bool ok = cfg_parse(buf, (uint16_t) w.len, false, &d);
    CHECK(ok, "a cfg envelope carrying both `lock` and `ca` must be accepted");
    CHECK(d.have_lock && d.have_ca, "both have_lock and have_ca must be true (a single push can carry both)");
}

static void test_unknown_key_skipped(void)
{
    uint8_t buf[256];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 1, "m_44444444", 10);
    cbor_w_tstr(&w, 6, "cfg", 3);
    /* cfg: {lock, sms(recognised-but-unconsumed), 99(genuinely unknown)} */
    cbor_w_map_key(&w, 38, 3);
    cbor_w_map_key(&w, 0, 1);
    cbor_w_bool(&w, 0, false);
    cbor_w_array(&w, 2, 1); /* sms: [ {n,p} ] */
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, 0, "gma", 3);
    cbor_w_tstr(&w, 1, "+15551234567", 12);
    cbor_w_uint(&w, 99, 7); /* a genuinely unknown cfg key */
    CHECK(!w.err, "test setup: encoding the unknown-key fixture must not overflow");

    cfg_dispatch_t d;
    bool ok = cfg_parse(buf, (uint16_t) w.len, false, &d);
    CHECK(ok, "an unknown cfg key must be skipped, not treated as malformed");
    CHECK(d.have_lock, "have_lock must still be true");
    CHECK(d.have_sms, "have_sms must be true (recognised, even though not yet consumed)");
    CHECK(!d.have_ca, "have_ca must be false");
}

static void test_unpin_form(void)
{
    /* Un-pin: `cfg.ca = {url: ""}` with NO `sha` key. */
    uint8_t buf[128];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 3);
    cbor_w_tstr(&w, 1, "m_55555555", 10);
    cbor_w_tstr(&w, 6, "cfg", 3);
    cbor_w_map_key(&w, 38, 1);
    cbor_w_map_key(&w, 1, 1); /* ca: {url:""} */
    cbor_w_tstr(&w, 0, "", 0);
    CHECK(!w.err, "test setup: encoding the un-pin fixture must not overflow");

    cfg_dispatch_t d;
    bool ok = cfg_parse(buf, (uint16_t) w.len, false, &d);
    CHECK(ok, "an un-pin cfg envelope must be accepted");
    CHECK(d.have_ca, "have_ca must be true for the un-pin form too");

    cbor_r_t r;
    cbor_r_init(&r, buf + d.ca_off, d.ca_len);
    uint32_t count;
    CHECK(cbor_r_map(&r, &count) && count == 1, "un-pin ca span must decode as a 1-pair map (url only)");
}

static void test_empty_cfg_map_not_malformed(void)
{
    uint8_t buf[64];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 2);
    cbor_w_tstr(&w, 6, "cfg", 3);
    cbor_w_map_key(&w, 38, 0); /* cfg: {} */
    CHECK(!w.err, "test setup: encoding the empty-cfg fixture must not overflow");

    cfg_dispatch_t d;
    bool ok = cfg_parse(buf, (uint16_t) w.len, false, &d);
    CHECK(ok, "a cfg envelope with an empty cfg map must still be accepted (not malformed)");
    CHECK(!d.have_lock && !d.have_ca && !d.have_sms, "nothing should be recognised in an empty cfg map");
}

static void test_not_cfg_rejected(void)
{
    uint8_t buf[64];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 2);
    cbor_w_uint(&w, 0, 1);
    cbor_w_tstr(&w, 6, "msg", 3);
    CHECK(!w.err, "test setup: encoding the not-cfg fixture must not overflow");

    cfg_dispatch_t d;
    CHECK(!cfg_parse(buf, (uint16_t) w.len, false, &d), "a kind:\"msg\" envelope must not be accepted as cfg");
}

int main(void)
{
    test_lock_only();
    test_ca_only();
    test_both_lock_and_ca();
    test_unknown_key_skipped();
    test_unpin_form();
    test_empty_cfg_map_not_malformed();
    test_not_cfg_rejected();

    if (g_failures == 0) {
        printf("PASS: cfg, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

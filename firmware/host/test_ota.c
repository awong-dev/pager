/* test_ota.c — host test for main/ota.c's pure section (docs/OTA_DESIGN.md
 * D5/D6/D8, section 5): cfg.ota decode, accept/reject, the start/install
 * gates, budget and expiry, and the status strings.
 */
#include "ota.h"

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

static const char *URL = "https://storage.googleapis.com/bkt/fw/0123456789abcdef/full.z";

typedef struct {
    bool img, isz, url, osz, osha, fmt, base, psz;
    int fmt_val;           /* 0 full, 1 delta, else unknown */
    size_t hash_len;       /* length of every hash bstr */
    uint64_t isz_val, osz_val, psz_val;
    const char *url_val;
    bool extra_key;        /* an unknown key 40 (a nested map) */
} spec_t;

static spec_t full_spec(void)
{
    spec_t s = { .img = true, .isz = true, .url = true, .osz = true, .osha = true, .fmt = true,
                 .fmt_val = 0, .hash_len = 32, .isz_val = 685168, .osz_val = 338784, .psz_val = 20774,
                 .url_val = URL };
    return s;
}

static size_t build(const spec_t *s, uint8_t *buf, size_t cap)
{
    uint8_t h[64];
    for (int i = 0; i < 64; i++) {
        h[i] = (uint8_t) (i + 1);
    }
    uint32_t n = (uint32_t) (s->img + s->isz + s->url + s->osz + s->osha + s->fmt + s->base + s->psz + s->extra_key);
    cbor_w_t w;
    cbor_w_init(&w, buf, cap);
    cbor_w_map(&w, n);
    if (s->img) {
        cbor_w_bstr(&w, 0, h, s->hash_len);
    }
    if (s->isz) {
        cbor_w_uint(&w, 1, s->isz_val);
    }
    if (s->url) {
        cbor_w_tstr(&w, 2, s->url_val, strlen(s->url_val));
    }
    if (s->osz) {
        cbor_w_uint(&w, 3, s->osz_val);
    }
    if (s->osha) {
        cbor_w_bstr(&w, 4, h + 8, s->hash_len);
    }
    if (s->fmt) {
        cbor_w_uint(&w, 5, (uint64_t) s->fmt_val);
    }
    if (s->base) {
        cbor_w_bstr(&w, 6, h + 16, s->hash_len);
    }
    if (s->psz) {
        cbor_w_uint(&w, 7, s->psz_val);
    }
    if (s->extra_key) {
        cbor_w_map_key(&w, 40, 1);
        cbor_w_uint(&w, 0, 7);
    }
    CHECK(!w.err, "fixture overflow");
    return w.len;
}

static bool parse(const spec_t *s, ota_cfg_t *out)
{
    uint8_t buf[700];
    size_t n = build(s, buf, sizeof(buf));
    return ota_parse_cfg_submap(buf, (uint16_t) n, out);
}

static void test_parse(void)
{
    ota_cfg_t c;
    spec_t s = full_spec();
    CHECK(parse(&s, &c), "valid full must parse");
    CHECK(c.fmt == OTA_FMT_FULL && c.isz == 685168 && c.osz == 338784 && !c.cancel, "full fields");
    CHECK(strcmp(c.url, URL) == 0, "url copied");
    CHECK(c.img[0] == 1 && c.img[31] == 32 && c.osha[0] == 9, "hashes copied");

    s = full_spec();
    s.extra_key = true;
    CHECK(parse(&s, &c), "an unknown key must be skipped");

    s = full_spec();
    s.fmt_val = 1;
    s.base = s.psz = true;
    CHECK(parse(&s, &c) && c.fmt == OTA_FMT_DELTA && c.psz == 20774 && c.base[0] == 17, "valid delta must parse");

    /* each required key missing */
    for (int k = 0; k < 6; k++) {
        s = full_spec();
        bool *f[] = { &s.img, &s.isz, &s.url, &s.osz, &s.osha, &s.fmt };
        *f[k] = false;
        CHECK(!parse(&s, &c), "missing required key %d must fail", k);
    }
    /* delta without base / without psz */
    s = full_spec();
    s.fmt_val = 1;
    s.psz = true;
    CHECK(!parse(&s, &c), "delta without base must fail");
    s = full_spec();
    s.fmt_val = 1;
    s.base = true;
    CHECK(!parse(&s, &c), "delta without psz must fail");
    s = full_spec();
    s.fmt_val = 1;
    s.base = s.psz = true;
    s.psz_val = 0;
    CHECK(!parse(&s, &c), "delta with psz 0 must fail");

    s = full_spec();
    s.hash_len = 31;
    CHECK(!parse(&s, &c), "31-byte hash must fail");
    s.hash_len = 33;
    CHECK(!parse(&s, &c), "33-byte hash must fail");

    s = full_spec();
    s.url_val = "http://storage.googleapis.com/x";
    CHECK(!parse(&s, &c), "http:// must fail");
    s.url_val = "ftp://x/y";
    CHECK(!parse(&s, &c), "non-https must fail");
    s.url_val = "https:/x";
    CHECK(!parse(&s, &c), "short url must fail");

    s = full_spec();
    s.isz_val = 0;
    CHECK(!parse(&s, &c), "isz 0 must fail");
    s.isz_val = 0x200001;
    CHECK(!parse(&s, &c), "isz > 0x200000 must fail");
    s.isz_val = 0x200000;
    CHECK(parse(&s, &c), "isz == 0x200000 is allowed");
    s = full_spec();
    s.osz_val = 0;
    CHECK(!parse(&s, &c), "osz 0 must fail");
    s.osz_val = 0x200001;
    CHECK(!parse(&s, &c), "osz > 0x200000 must fail");

    s = full_spec();
    s.fmt_val = 2;
    CHECK(!parse(&s, &c), "unknown fmt must fail");

    /* URL too long for the buffer */
    char longurl[OTA_URL_MAX + 40];
    memcpy(longurl, "https://", 8);
    memset(longurl + 8, 'a', sizeof(longurl) - 9);
    longurl[sizeof(longurl) - 1] = '\0';
    s = full_spec();
    s.url_val = longurl;
    uint8_t big[900];
    size_t n = build(&s, big, sizeof(big));
    CHECK(!ota_parse_cfg_submap(big, (uint16_t) n, &c), "an over-long url must fail");

    /* cancel */
    uint8_t buf[16];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_bool(&w, 8, true);
    CHECK(ota_parse_cfg_submap(buf, (uint16_t) w.len, &c) && c.cancel, "cancel:true alone must parse");
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_bool(&w, 8, false);
    CHECK(!ota_parse_cfg_submap(buf, (uint16_t) w.len, &c), "cancel:false alone is an empty job: must fail");

    /* not a map / truncated */
    CHECK(!ota_parse_cfg_submap(buf, 0, &c), "empty input must fail");
    uint8_t good[700];
    s = full_spec();
    n = build(&s, good, sizeof(good));
    CHECK(!ota_parse_cfg_submap(good, (uint16_t) (n - 3), &c), "truncated input must fail");
}

static void test_accept(void)
{
    uint8_t run[32], other[32], bad[32];
    memset(run, 0xA1, 32);
    memset(other, 0xB2, 32);
    memset(bad, 0xC3, 32);
    ota_cfg_t c;
    memset(&c, 0, sizeof(c));
    memcpy(c.img, other, 32);
    c.fmt = OTA_FMT_FULL;

    CHECK(ota_check_accept(&c, run, false, true, NULL) == OTA_ACCEPT_OK, "plain full accepted");
    CHECK(ota_check_accept(&c, run, false, true, bad) == OTA_ACCEPT_OK, "full accepted with a different bad id");

    memcpy(c.img, run, 32);
    CHECK(ota_check_accept(&c, run, false, true, NULL) == OTA_REJECT_RUNNING, "img == running");
    memcpy(c.img, other, 32);

    memcpy(c.img, bad, 32);
    CHECK(ota_check_accept(&c, run, false, true, bad) == OTA_REJECT_BAD, "img on the failed list");
    CHECK(ota_check_accept(&c, run, false, true, NULL) == OTA_ACCEPT_OK, "no failed list: accepted");
    memcpy(c.img, other, 32);

    CHECK(ota_check_accept(&c, run, true, true, NULL) == OTA_REJECT_PV, "pending verify");
    CHECK(ota_check_accept(&c, run, false, false, NULL) == OTA_REJECT_NOBL, "no rollback bootloader");

    c.fmt = OTA_FMT_DELTA;
    memcpy(c.base, run, 32);
    CHECK(ota_check_accept(&c, run, false, true, NULL) == OTA_ACCEPT_OK, "delta with base == running accepted");
    memcpy(c.base, other, 32);
    CHECK(ota_check_accept(&c, run, false, true, NULL) == OTA_REJECT_BASE, "delta with another base");
    c.fmt = OTA_FMT_FULL;
    CHECK(ota_check_accept(&c, run, false, true, NULL) == OTA_ACCEPT_OK, "a full ignores `base`");

    CHECK(ota_reject_err(OTA_REJECT_BASE) == OTA_ERR_BASE && ota_reject_err(OTA_REJECT_PV) == OTA_ERR_PV &&
              ota_reject_err(OTA_REJECT_NOBL) == OTA_ERR_NOBL && ota_reject_err(OTA_REJECT_RUNNING) == OTA_ERR_RUN &&
              ota_reject_err(OTA_REJECT_BAD) == OTA_ERR_BAD,
          "reject -> err mapping");
}

#define T0 ((int64_t) 1000 * 1000000)

static ota_env_t good_env(void)
{
    ota_env_t e;
    memset(&e, 0, sizeof(e));
    e.mqtt_usable = true;
    e.batt_mv = 3900;
    e.rssi = -90;
    e.input_age_ms = 120000;
    e.now_us = T0;
    e.epoch = 1790000000;
    return e;
}

static ota_job_t good_job(void)
{
    ota_job_t j;
    memset(&j, 0, sizeof(j));
    j.state = OTA_ST_WAIT;
    j.cfg.osz = 338784;
    j.created_epoch = 1790000000 - 3600;
    return j;
}

static void test_should_start(void)
{
    ota_env_t e = good_env();
    ota_job_t j = good_job();
    CHECK(ota_should_start(&e, &j), "baseline must start");

    ota_env_t x = e;
    x.airplane = true;
    CHECK(!ota_should_start(&x, &j), "airplane");
    x = e;
    x.mqtt_usable = false;
    CHECK(!ota_should_start(&x, &j), "mqtt not usable");
    x = e;
    x.other_fetch = true;
    CHECK(!ota_should_start(&x, &j), "another fetch in flight");
    x = e;
    x.pending_verify = true;
    CHECK(!ota_should_start(&x, &j), "pending verify");
    x = e;
    x.batt_mv = 3599;
    CHECK(!ota_should_start(&x, &j), "battery 3599");
    x.batt_mv = 3600;
    CHECK(ota_should_start(&x, &j), "battery 3600 is enough");
    x = e;
    x.rssi = -106;
    CHECK(!ota_should_start(&x, &j), "rssi -106");
    x.rssi = -105;
    CHECK(ota_should_start(&x, &j), "rssi -105 is enough");
    x = e;
    x.input_age_ms = 59999;
    CHECK(!ota_should_start(&x, &j), "key 59.999 s ago");
    x.input_age_ms = 60000;
    CHECK(ota_should_start(&x, &j), "key 60 s ago is enough");

    /* 10 min spacing */
    ota_job_t y = j;
    y.last_attempt_us = T0 - OTA_ATTEMPT_SPACING_US + 1;
    CHECK(!ota_should_start(&e, &y), "9:59 since the last attempt");
    y.last_attempt_us = T0 - OTA_ATTEMPT_SPACING_US;
    CHECK(ota_should_start(&e, &y), "10:00 since the last attempt");

    /* 6 per day, keyed on epoch/86400 */
    y = j;
    y.day = e.epoch / 86400u;
    y.attempts_day = 5;
    CHECK(ota_should_start(&e, &y), "5 attempts today: one more allowed");
    y.attempts_day = 6;
    CHECK(!ota_should_start(&e, &y), "6 attempts today: no more");
    y.day = e.epoch / 86400u - 1;
    CHECK(ota_should_start(&e, &y), "6 attempts but on yesterday's counter: allowed");

    /* state must be WAIT */
    for (int s = OTA_ST_NONE; s <= OTA_ST_RB; s++) {
        y = j;
        y.state = (uint8_t) s;
        CHECK(ota_should_start(&e, &y) == (s == OTA_ST_WAIT), "state %d", s);
    }
}

static void test_budget_expiry(void)
{
    ota_env_t e = good_env();
    ota_job_t j = good_job();
    CHECK(ota_budget(&j.cfg) == 3u * 338784u + 65536u, "budget = 3*osz + 64 KiB");
    j.bytes_spent = ota_budget(&j.cfg);
    CHECK(!ota_budget_exceeded(&j) && ota_should_start(&e, &j), "exactly the budget is not over it");
    j.bytes_spent = ota_budget(&j.cfg) + 1;
    CHECK(ota_budget_exceeded(&j) && !ota_should_start(&e, &j), "one byte over budget");

    j = good_job();
    j.created_epoch = e.epoch - OTA_JOB_MAX_AGE_S;
    CHECK(!ota_expired(&e, &j) && ota_should_start(&e, &j), "exactly 7 days is not expired");
    j.created_epoch = e.epoch - OTA_JOB_MAX_AGE_S - 1;
    CHECK(ota_expired(&e, &j) && !ota_should_start(&e, &j), "7 days + 1 s is expired");
    ota_env_t noclk = e;
    noclk.epoch = 0;
    CHECK(!ota_expired(&noclk, &j), "no clock: never expired");
    j.created_epoch = 0;
    CHECK(!ota_expired(&e, &j), "created without a clock: never expired");
    j.created_epoch = e.epoch + 100;
    CHECK(!ota_expired(&e, &j), "created in the future: not expired");
}

static void test_install(void)
{
    ota_env_t e = good_env();
    e.input_age_ms = 0;
    CHECK(ota_should_install(&e, false, true), "install_now wins");
    CHECK(ota_should_install(&e, true, true), "install_now wins in sleep mode");
    CHECK(!ota_should_install(&e, true, false), "sleep mode, just typed");
    e.input_age_ms = OTA_INSTALL_IDLE_MS - 1;
    CHECK(!ota_should_install(&e, true, false), "4:59 idle");
    e.input_age_ms = OTA_INSTALL_IDLE_MS;
    CHECK(ota_should_install(&e, true, false), "5:00 idle in sleep mode");
    CHECK(!ota_should_install(&e, false, false), "5 min idle but not in sleep mode");
}

static void test_strings(void)
{
    CHECK(strcmp(ota_state_str(OTA_ST_WAIT), "wait") == 0 && strcmp(ota_state_str(OTA_ST_DL), "dl") == 0 &&
              strcmp(ota_state_str(OTA_ST_READY), "ready") == 0 && strcmp(ota_state_str(OTA_ST_INST), "inst") == 0 &&
              strcmp(ota_state_str(OTA_ST_OK), "ok") == 0 && strcmp(ota_state_str(OTA_ST_FAIL), "fail") == 0 &&
              strcmp(ota_state_str(OTA_ST_RB), "rb") == 0 && strcmp(ota_state_str(OTA_ST_NONE), "") == 0,
          "state strings");
    const char *want[] = { "", "base", "hash", "osha", "http", "budget", "flash", "nobl", "pv", "size",
                           "expired", "mem", "run", "bad" };
    for (int i = 0; i <= OTA_ERR_BAD; i++) {
        CHECK(strcmp(ota_err_str((ota_err_t) i), want[i]) == 0, "err string %d", i);
        CHECK(strlen(ota_err_str((ota_err_t) i)) <= 8, "err string %d too long for ota_err", i);
    }
    uint8_t id[32];
    for (int i = 0; i < 32; i++) {
        id[i] = (uint8_t) (0xF0 + i);
    }
    char h[17];
    ota_hex16(id, h);
    CHECK(strcmp(h, "f0f1f2f3f4f5f6f7") == 0, "hex16 = %s", h);

    char t[40];
    ota_ui_text(OTA_ST_NONE, OTA_ERR_NONE, 0, t, sizeof(t));
    CHECK(strcmp(t, "-") == 0, "ui none: %s", t);
    ota_ui_text(OTA_ST_WAIT, OTA_ERR_NONE, 0, t, sizeof(t));
    CHECK(strcmp(t, "waiting") == 0, "ui wait: %s", t);
    ota_ui_text(OTA_ST_DL, OTA_ERR_NONE, 45, t, sizeof(t));
    CHECK(strcmp(t, "downloading 45%") == 0, "ui dl: %s", t);
    ota_ui_text(OTA_ST_READY, OTA_ERR_NONE, 100, t, sizeof(t));
    CHECK(strcmp(t, "ready, installs when idle") == 0, "ui ready: %s", t);
    ota_ui_text(OTA_ST_FAIL, OTA_ERR_OSHA, 0, t, sizeof(t));
    CHECK(strcmp(t, "failed (osha)") == 0, "ui fail: %s", t);
    ota_ui_text(OTA_ST_RB, OTA_ERR_NONE, 0, t, sizeof(t));
    CHECK(strcmp(t, "rolled back") == 0, "ui rb: %s", t);
}

int main(void)
{
    test_parse();
    test_accept();
    test_should_start();
    test_budget_expiry();
    test_install();
    test_strings();
    if (g_failures == 0) {
        printf("PASS: ota, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

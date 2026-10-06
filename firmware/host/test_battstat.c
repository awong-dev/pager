/* test_battstat.c — host test for main/battstat.c's pure core
 * (docs/BATTERY_STATS_DESIGN.md B1-B5). No ESP-IDF. */
#include "battstat.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define T0 1000000 /* us: any nonzero start; last_tick_us == 0 means "no baseline" */

static void fresh(battstat_rtc_t *r, bs_ram_t *ram)
{
    memset(r, 0, sizeof(*r));
    memset(ram, 0, sizeof(*ram));
    bs_core_init(r);
}

static bs_inputs_t at(int64_t now_us)
{
    bs_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.now_us = now_us;
    return in;
}

static void test_a_first_tick_baseline(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_inputs_t in = at(T0);
    in.rail_ms = 500; in.ext1 = 7; in.rf[1] = 3; in.link = 2; in.radio = 9;
    bs_core_tick(&r, &ram, &in);
    CHECK(r.dt_ms == 0 && r.sl_ms == 0 && r.rl_ms == 0 && r.x1 == 0 && r.rf[1] == 0 && r.cn == 0 && r.re == 0,
          "a: first tick must not add anything");
    CHECK(ram.last_tick_us == T0 && ram.ext1 == 7 && ram.link == 2, "a: baselines set");
    CHECK(r.crc == bs_core_crc(&r), "a: crc intact");
}

static void test_b_awake_vs_sleep(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_inputs_t in = at(T0);
    bs_core_tick(&r, &ram, &in);
    bs_core_sleep(&r, &ram, 19600000);
    in = at(T0 + 20000000);
    bs_core_tick(&r, &ram, &in);
    CHECK(r.aw_ms[BS_TIMER] == 400, "b: aw[0]=%u want 400", (unsigned) r.aw_ms[BS_TIMER]);
    CHECK(r.sl_ms == 19600, "b: sl=%u want 19600", (unsigned) r.sl_ms);
    CHECK(r.dt_ms == 20000 && r.ns == 1, "b: dt=%u ns=%u", (unsigned) r.dt_ms, (unsigned) r.ns);
    CHECK(ram.slept_us == 0 && ram.cause_max == BS_TIMER, "b: per-iteration state reset");
}

static void test_c_cause_priority(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_core_raise(&ram, BS_UI);
    bs_core_raise(&ram, BS_HOT);
    CHECK(ram.cause_max == BS_UI, "c: cause_max=%u want UI", (unsigned) ram.cause_max);
    bs_inputs_t in = at(T0);
    bs_core_tick(&r, &ram, &in);
    bs_core_raise(&ram, BS_UI);
    bs_core_raise(&ram, BS_HOT);
    in = at(T0 + 2000000);
    bs_core_tick(&r, &ram, &in);
    CHECK(r.aw_ms[BS_UI] == 2000 && r.aw_ms[BS_HOT] == 0, "c: charged to UI");
}

static void test_d_us_remainder(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_inputs_t in = at(T0);
    bs_core_tick(&r, &ram, &in);
    for (int i = 1; i <= 1000; i++) {
        in = at(T0 + (int64_t) i * 1500);
        bs_core_tick(&r, &ram, &in);
    }
    CHECK(r.dt_ms == 1500, "d: dt_ms=%u want 1500", (unsigned) r.dt_ms);
}

static void test_e_commit_keeps_remainder(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_inputs_t in = at(T0);
    bs_core_tick(&r, &ram, &in);
    bs_core_sleep(&r, &ram, 9000000);
    in = at(T0 + 10999000);
    in.search = true;
    bs_core_tick(&r, &ram, &in);
    bs_snap_t s;
    bs_core_snapshot(&r, &s);
    CHECK(s.dt == 10 && s.sl == 9 && s.aw[0] == 1 && s.md[1] == 10 && s.ns == 1 && s.sq == 0,
          "e: snapshot dt=%u sl=%u aw0=%u", (unsigned) s.dt, (unsigned) s.sl, (unsigned) s.aw[0]);
    bs_core_commit(&r, &s);
    CHECK(r.dt_ms == 999, "e: dt remainder %u want 999", (unsigned) r.dt_ms);
    CHECK(r.aw_ms[0] == 999, "e: aw remainder %u want 999", (unsigned) r.aw_ms[0]);
    CHECK(r.md_ms[1] == 999 && r.ns == 0 && r.sq == 1 && r.mvn == 0, "e: md remainder / ns / sq");
    CHECK(r.crc == bs_core_crc(&r), "e: crc after commit");
}

static void test_f_failed_publish(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_inputs_t in = at(T0);
    bs_core_tick(&r, &ram, &in);
    in = at(T0 + 5000000);
    bs_core_tick(&r, &ram, &in);
    bs_snap_t s1, s2;
    bs_core_snapshot(&r, &s1);
    /* publish failed: no commit */
    in = at(T0 + 9000000);
    bs_core_tick(&r, &ram, &in);
    bs_core_snapshot(&r, &s2);
    CHECK(s1.sq == s2.sq && s1.dt == 5 && s2.dt == 9, "f: same sq, superset window (%u -> %u)", (unsigned) s1.dt,
          (unsigned) s2.dt);
    CHECK(s2.aw[0] >= s1.aw[0], "f: superset");
}

static void test_g_counter_backwards(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_inputs_t in = at(T0);
    in.rf[0] = 10;
    in.radio = 50;
    bs_core_tick(&r, &ram, &in);
    in = at(T0 + 1000000);
    in.rf[0] = 3; /* went backwards: counts as the delta itself */
    in.radio = 52;
    bs_core_tick(&r, &ram, &in);
    CHECK(r.rf[0] == 3 && r.re == 2, "g: rf0=%u re=%u want 3,2", (unsigned) r.rf[0], (unsigned) r.re);
    in = at(T0 + 2000000);
    in.rf[0] = 4;
    in.radio = 52;
    bs_core_tick(&r, &ram, &in);
    CHECK(r.rf[0] == 4 && r.re == 2, "g: later delta from the new baseline");
}

static void test_h_saturation(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    r.dt_ms = 0xFFFFFFF0u;
    r.cn = 0xFFFFFFFFu;
    bs_inputs_t in = at(T0);
    bs_core_tick(&r, &ram, &in);
    in = at(T0 + 1000000);
    in.link = 5;
    bs_core_tick(&r, &ram, &in);
    CHECK(r.dt_ms == 0xFFFFFFFFu, "h: dt_ms saturates");
    CHECK(r.cn == 0xFFFFFFFFu, "h: cn saturates");
}

static void test_i_encode_bytes(void)
{
    bs_snap_t s;
    memset(&s, 0, sizeof(s));
    s.sq = 3; s.dt = 3600; s.sl = 3500;
    uint32_t aw[BS_NCAUSE] = {60, 10, 5, 20, 4, 1};
    memcpy(s.aw, aw, sizeof(aw));
    s.ns = 170; s.x1 = 2; s.rl = 300;
    s.rf[0] = 1; s.rf[1] = 40; s.rf[2] = 2;
    s.mvn = 3712; s.cn = 1;
    s.md[1] = 12;
    s.re = 45;
    static const char *want_hex = "1844ac" "0003" "01190e10" "02190dac" "0386" "183c0a05140401" "0418aa"
                                  "0502" "0619012c" "0783" "01182802" "08190e80" "0901" "0a83" "000c00" "0b182d";
    uint8_t buf[160];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(bs_core_encode(&w, &s), "i: encode ok");
    char hex[200];
    for (size_t i = 0; i < w.len; i++) {
        snprintf(hex + 2 * i, 3, "%02x", buf[i]);
    }
    CHECK(strcmp(hex, want_hex) == 0, "i: got %s want %s", hex, want_hex);
    CHECK(w.len <= 64, "i: size %zu > 64", w.len);
    printf("test (i): bs encode size = %zu B (mvn present)\n", w.len);

    s.mvn = 0;
    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(bs_core_encode(&w, &s) && buf[2] == 0xab && w.len == 51 - 4, "i: mvn omitted -> map(11), %zu B", w.len);
    /* worst case: every counter at 2^32-1 */
    memset(&s, 0xFF, sizeof(s));
    s.mvn = 4500;
    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(bs_core_encode(&w, &s), "i: worst case encodes");
    printf("test (i): worst-case size = %zu B (mvn is u16, other counters 2^32-1)\n", w.len);
}

static void test_j_init(void)
{
    battstat_rtc_t r;
    memset(&r, 0xA5, sizeof(r));
    CHECK(!bs_core_init(&r) && r.magic == BATTSTAT_MAGIC && r.sq == 0 && r.dt_ms == 0, "j: garbage zeroed");
    CHECK(r.crc == bs_core_crc(&r), "j: crc valid after zeroing");
    r.sq = 7;
    r.dt_ms = 12345;
    r.crc = bs_core_crc(&r);
    CHECK(bs_core_init(&r) && r.sq == 7 && r.dt_ms == 12345, "j: valid window kept");
    r.dt_ms ^= 1; /* corrupt without re-CRC */
    CHECK(!bs_core_init(&r) && r.dt_ms == 0 && r.sq == 0, "j: corrupt crc zeroes");
}

static void test_k_note_mv(void)
{
    battstat_rtc_t r;
    bs_ram_t ram;
    fresh(&r, &ram);
    bs_core_note_mv(&r, 0);
    bs_core_note_mv(&r, 9999);
    CHECK(r.mvn == 0, "k: 0 and 9999 ignored");
    bs_core_note_mv(&r, 3800);
    bs_core_note_mv(&r, 3900);
    CHECK(r.mvn == 3800, "k: min kept");
    bs_core_note_mv(&r, 3700);
    CHECK(r.mvn == 3700 && r.crc == bs_core_crc(&r), "k: new min, crc");
    bs_core_note_mv(&r, 1999);
    bs_core_note_mv(&r, 4501);
    CHECK(r.mvn == 3700, "k: range edges rejected");
}

int main(void)
{
    test_a_first_tick_baseline();
    test_b_awake_vs_sleep();
    test_c_cause_priority();
    test_d_us_remainder();
    test_e_commit_keeps_remainder();
    test_f_failed_publish();
    test_g_counter_backwards();
    test_h_saturation();
    test_i_encode_bytes();
    test_j_init();
    test_k_note_mv();
    if (g_failures == 0) {
        printf("PASS: battstat 11 cases, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

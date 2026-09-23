/* test_wificred.c — host test harness for main/wificred.c's pure section
 * (docs/WIFI_TASKS.md W2, docs/WIFI_DESIGN.md §4): SSID/PSK validation and
 * the replace-wholesale candidate applier.
 *
 * Coverage required by the task brief: every validation boundary
 * (0/1/32/33-byte SSID; 7/8/63/64-byte PSK; embedded NUL), 3 entries
 * refused, empty list clears. Also covers, beyond the task's own list but
 * for the same NVS str-storage reason wificred.h's own module comment gives
 * for the SSID check: an embedded NUL in the PSK is rejected too, and a
 * rejected batch never touches the caller's existing set (the
 * `sms_parse_cfg_submap()`-style "reject the whole list" contract).
 */
#include "wificred.h"

#include <stdio.h>
#include <string.h>

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

int main(void)
{
    test_ssid_boundaries();
    test_psk_boundaries();
    test_apply_full_push();
    test_apply_three_entries_refused();
    test_apply_empty_clears();
    test_apply_oversize_rejects_whole_batch();
    test_apply_two_max_is_ok();

    if (g_failures == 0) {
        printf("PASS: wificred, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

/* test_temp.c -- host test for main/temp_parse.c and the optional /status keys 23/71 encoding. */
#include "temp_parse.h"
#include "cbor.h"

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

static void ok(const char *line, int want)
{
    int v = 12345;
    bool r = temp_parse_dc(line, &v);
    CHECK(r && v == want, "\"%s\": ret %d value %d want %d", line, r, v, want);
}

static void bad(const char *line)
{
    int v = 12345;
    bool r = temp_parse_dc(line, &v);
    CHECK(!r && v == 12345, "\"%s\": should be rejected (ret %d value %d)", line, r, v);
}

/* Same optional-field pattern as modes.c build_status_cbor(): count first, then write. */
static size_t encode(uint8_t *buf, size_t cap, bool have_batt, int batt, bool have_temp, int temp)
{
    uint32_t n = 1 + (have_batt ? 1 : 0) + (have_temp ? 1 : 0);
    cbor_w_t w;
    cbor_w_init(&w, buf, cap);
    cbor_w_map(&w, n);
    cbor_w_uint(&w, 0, 1);
    if (have_batt) {
        cbor_w_uint(&w, 23, (uint64_t) batt);
    }
    if (have_temp) {
        if (temp < 0) {
            cbor_w_nint(&w, 71, temp);
        } else {
            cbor_w_uint(&w, 71, (uint64_t) temp);
        }
    }
    return w.len;
}

int main(void)
{
    ok("+SQNTMON: 1,-40,-30,85,95,23", 230);
    ok("+SQNTMON: 1,-40,-30,85,95,23.5", 235);
    ok("+SQNTMON: 1,-40,-30,85,95,-5", -50);
    ok("+SQNTMON: 1,-40,-30,85,95,-0.4", -4);
    ok("+SQNTMON: 0,-40,-30,85,95,+12.25\r\n", 122);
    ok("+SQNTMON: 1,-40,-30,85,95, 31.9 ", 319);
    ok("+SQNTMON: 1,-40,-30,85,95,0", 0);
    ok("+SQNTMON: 1,-40,-30,85,95,150", 1500);
    ok("+X: 41.2", 412);
    bad("+SQNTMON: 1,-40,-30,85,95,");
    bad("+SQNTMON: 1,-40,-30,85,95,abc");
    bad("+SQNTMON: 1,-40,-30,85,95,23.5C");
    bad("+SQNTMON: 1,-40,-30,85,95,235"); /* 235 C: modem printed tenths as an integer */
    bad("+SQNTMON: 1,-40,-30,85,95,-41");
    bad("");
    CHECK(!temp_parse_dc(NULL, NULL), "NULL line");
    CHECK(temp_parse_dc("+SQNTMON: 1,0,0,0,0,25", NULL), "NULL out is allowed");

    uint8_t a[64], b[64], c[64], d[64];
    size_t la = encode(a, sizeof(a), true, 5203, true, 235);
    size_t lb = encode(b, sizeof(b), false, 0, true, -45);
    size_t lc = encode(c, sizeof(c), true, 3900, false, 0);
    size_t ld = encode(d, sizeof(d), false, 0, false, 0);
    CHECK(la > 0 && lb > 0 && lc > 0 && ld > 0, "encode failed");
    CHECK(a[0] == 0xA3 && b[0] == 0xA2 && c[0] == 0xA2 && d[0] == 0xA1, "map counts %02x %02x %02x %02x",
          a[0], b[0], c[0], d[0]);
    /* decode a: both keys present, batt not clamped */
    cbor_r_t r;
    cbor_r_init(&r, a, la);
    uint32_t cnt = 0;
    CHECK(cbor_r_map(&r, &cnt) && cnt == 3, "decode map");
    int found_b = 0, found_t = 0;
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t key;
        uint64_t u;
        CHECK(cbor_r_key(&r, &key), "key");
        if (key == 23) {
            CHECK(cbor_r_uint(&r, &u) && u == 5203, "batt");
            found_b = 1;
        } else if (key == 71) {
            CHECK(cbor_r_uint(&r, &u) && u == 235, "temp");
            found_t = 1;
        } else {
            CHECK(cbor_r_uint(&r, &u), "v");
        }
    }
    CHECK(found_b && found_t, "both keys");
    if (g_failures == 0) {
        printf("test_temp: all passed\n");
    }
    return g_failures ? 1 : 0;
}

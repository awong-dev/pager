/* test_cbor.c — host test harness for main/cbor.c (task F3.2).
 *
 * Builds and runs with the host compiler (see Makefile), no ESP-IDF. Two
 * kinds of coverage:
 *
 *  1. A handful of direct writer/reader unit checks (overflow, a bad
 *     cbor_w_nint() argument, indefinite-length rejection).
 *  2. The task's required check: for every vector in
 *     tools/authvectors.json, decode `cbor_signed_b64` **minus its last 10
 *     bytes** (the `0x0D 0x48 <8-byte tag>` signature suffix — auth.c/F3.3's
 *     job, not this module's) and re-encode it, byte for byte identical to
 *     the input. This is a pure structural round-trip: cbor.c does not know
 *     PROTOCOL.md §10's keymap, so the "decode" side here is a small
 *     generic map/array walker (copy_kv/copy_array, below) built out of
 *     cbor_r_peek() + the typed getters, not a fixed field list.
 *
 * docs/DEVICE_PLAN.md §2.4: the publisher writes a map header whose count
 * *includes* the not-yet-written `sig` pair, then writes every other pair,
 * then appends the 10-byte tag out of band. So the map header's declared
 * pair count `n` is always one more than the number of pairs actually
 * present once the trailing 10 bytes are stripped — this file's per-vector
 * loop deliberately walks `n - 1` pairs, not `n`.
 *
 * tools/authvectors.json is read with a tiny purpose-built scanner (not a
 * general JSON parser): the file's exact `"label": "..."` /
 * `"cbor_signed_b64": "..."` formatting (json.dump(..., indent=2), see
 * S1.2) is relied on directly. This is host-test-only code; it is not
 * built for the device and the "no malloc" rule for cbor.c itself does not
 * apply here.
 */
#include "cbor.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTHVECTORS_PATH "../../tools/authvectors.json"
#define MAX_VECTORS 32
#define MAX_LABEL 32
#define MAX_CBOR 256

typedef struct {
    char label[MAX_LABEL];
    uint8_t cbor[MAX_CBOR];
    size_t cbor_len;
} vector_t;

static int g_failures = 0;

#define CHECK(cond, ...)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                      \
            printf(__VA_ARGS__);                                             \
            printf("\n");                                                    \
            g_failures++;                                                    \
        }                                                                    \
    } while (0)

/* ---------------------------------------------------------------------
 * tiny base64 decoder (host-only; no malloc-restricted device code here)
 * --------------------------------------------------------------------- */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

static bool base64_decode(const char *s, size_t len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t o = 0;
    int vals[4];
    int nv = 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (c == '=') {
            break;
        }
        int v = b64_val(c);
        if (v < 0) {
            continue;
        }
        vals[nv++] = v;
        if (nv == 4) {
            if (o + 3 > out_cap) {
                return false;
            }
            out[o++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
            out[o++] = (uint8_t)((vals[1] << 4) | (vals[2] >> 2));
            out[o++] = (uint8_t)((vals[2] << 6) | vals[3]);
            nv = 0;
        }
    }
    if (nv == 2) {
        if (o + 1 > out_cap) {
            return false;
        }
        out[o++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
    } else if (nv == 3) {
        if (o + 2 > out_cap) {
            return false;
        }
        out[o++] = (uint8_t)((vals[0] << 2) | (vals[1] >> 4));
        out[o++] = (uint8_t)((vals[1] << 4) | (vals[2] >> 2));
    }
    *out_len = o;
    return true;
}

/* ---------------------------------------------------------------------
 * tools/authvectors.json scanner
 * --------------------------------------------------------------------- */

static const char *find_after(const char *hay, size_t hay_len, size_t from, const char *needle)
{
    size_t needle_len = strlen(needle);
    if (needle_len > hay_len) {
        return NULL;
    }
    for (size_t i = from; i + needle_len <= hay_len; i++) {
        if (memcmp(hay + i, needle, needle_len) == 0) {
            return hay + i + needle_len;
        }
    }
    return NULL;
}

static int load_vectors(const char *path, vector_t *out, int max)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", path);
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        printf("FAIL %s is empty\n", path);
        return -1;
    }
    char *data = malloc((size_t)sz + 1);
    if (!data) {
        fclose(f);
        printf("FAIL out of memory reading %s\n", path);
        return -1;
    }
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    data[got] = '\0';

    int n = 0;
    size_t pos = 0;
    while (n < max) {
        const char *lbl = find_after(data, got, pos, "\"label\": \"");
        if (!lbl) {
            break;
        }
        const char *lbl_end = strchr(lbl, '"');
        if (!lbl_end) {
            break;
        }
        const char *b64 = find_after(data, got, (size_t)(lbl_end - data), "\"cbor_signed_b64\": \"");
        if (!b64) {
            break;
        }
        const char *b64_end = strchr(b64, '"');
        if (!b64_end) {
            break;
        }

        size_t lbl_len = (size_t)(lbl_end - lbl);
        if (lbl_len >= MAX_LABEL) {
            lbl_len = MAX_LABEL - 1;
        }
        memcpy(out[n].label, lbl, lbl_len);
        out[n].label[lbl_len] = '\0';

        if (!base64_decode(b64, (size_t)(b64_end - b64), out[n].cbor, MAX_CBOR, &out[n].cbor_len)) {
            printf("FAIL vector %s: cbor_signed_b64 too long for MAX_CBOR\n", out[n].label);
            free(data);
            return -1;
        }

        pos = (size_t)(b64_end - data);
        n++;
    }

    free(data);
    return n;
}

/* ---------------------------------------------------------------------
 * generic recursive map copier: reader -> writer, unchanged
 * --------------------------------------------------------------------- */

static bool copy_kv(cbor_r_t *r, cbor_w_t *w); /* forward */

/* Reads one array element and writes it back. Every array value in this
 * protocol (`c[]`, `p[]`) is itself an unkeyed map. */
static bool copy_array_element(cbor_r_t *r, cbor_w_t *w)
{
    uint32_t m;
    if (!cbor_r_map(r, &m)) {
        return false;
    }
    if (!cbor_w_map(w, m)) {
        return false;
    }
    for (uint32_t j = 0; j < m; j++) {
        if (!copy_kv(r, w)) {
            return false;
        }
    }
    return true;
}

static bool copy_kv(cbor_r_t *r, cbor_w_t *w)
{
    uint32_t key;
    if (!cbor_r_key(r, &key)) {
        return false;
    }
    switch (cbor_r_peek(r)) {
    case CBOR_T_UINT: {
        uint64_t v;
        return cbor_r_uint(r, &v) && cbor_w_uint(w, key, v);
    }
    case CBOR_T_NINT: {
        int64_t v;
        return cbor_r_nint(r, &v) && cbor_w_nint(w, key, v);
    }
    case CBOR_T_TSTR: {
        const char *s;
        size_t len;
        return cbor_r_tstr(r, &s, &len) && cbor_w_tstr(w, key, s, len);
    }
    case CBOR_T_BSTR: {
        const uint8_t *b;
        size_t len;
        return cbor_r_bstr(r, &b, &len) && cbor_w_bstr(w, key, b, len);
    }
    case CBOR_T_BOOL: {
        bool v;
        return cbor_r_bool(r, &v) && cbor_w_bool(w, key, v);
    }
    case CBOR_T_NULL:
        return cbor_r_null(r) && cbor_w_null(w, key);
    case CBOR_T_FLOAT64: {
        double v;
        return cbor_r_f64(r, &v) && cbor_w_f64(w, key, v);
    }
    case CBOR_T_MAP: {
        uint32_t n;
        if (!cbor_r_map(r, &n) || !cbor_w_map_key(w, key, n)) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (!copy_kv(r, w)) {
                return false;
            }
        }
        return true;
    }
    case CBOR_T_ARRAY: {
        uint32_t n;
        if (!cbor_r_array(r, &n) || !cbor_w_array(w, key, n)) {
            return false;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (!copy_array_element(r, w)) {
                return false;
            }
        }
        return true;
    }
    case CBOR_T_INVALID:
    default:
        return false;
    }
}

/* CHECK() as a statement can't be used inside an `if` condition; this
 * variant records the same failure and returns the condition so callers
 * can bail out of a test early without decoding garbage off a failed
 * read. */
static bool check_ok_impl(bool cond, const char *label, const char *what)
{
    if (!cond) {
        printf("FAIL %s: %s\n", label, what);
        g_failures++;
    }
    return cond;
}
#define CHECK_OK(cond, label, what) check_ok_impl((cond), (label), (what))

/* ---------------------------------------------------------------------
 * per-vector round trip
 * --------------------------------------------------------------------- */

static void test_vector(const vector_t *v)
{
    CHECK(v->cbor_len > 10, "%s: cbor_signed_b64 shorter than the 10-byte sig suffix", v->label);
    if (v->cbor_len <= 10) {
        return;
    }
    size_t body_len = v->cbor_len - 10;
    const uint8_t *body = v->cbor;

    cbor_r_t r;
    cbor_r_init(&r, body, body_len);
    uint32_t n;
    if (!CHECK_OK(cbor_r_map(&r, &n), v->label, "top-level map header")) {
        return;
    }
    CHECK(n >= 1, "%s: map header declares 0 pairs (need >= 1 for the sig slot)", v->label);

    uint8_t outbuf[MAX_CBOR];
    cbor_w_t w;
    cbor_w_init(&w, outbuf, sizeof(outbuf));
    if (!CHECK_OK(cbor_w_map(&w, n), v->label, "re-encoding top-level map header")) {
        return;
    }

    /* n includes the sig pair, which is not present once the trailing 10
     * bytes are stripped — walk n - 1 real pairs. */
    for (uint32_t i = 0; i + 1 < n; i++) {
        if (!CHECK_OK(copy_kv(&r, &w), v->label, "copy_kv")) {
            return;
        }
    }

    CHECK(w.len == body_len, "%s: re-encoded length %zu != original %zu", v->label, w.len, body_len);
    if (w.len == body_len) {
        CHECK(memcmp(outbuf, body, body_len) == 0, "%s: re-encoded bytes differ from the original", v->label);
    }
}

/* ---------------------------------------------------------------------
 * direct unit checks
 * --------------------------------------------------------------------- */

static void test_overflow(void)
{
    uint8_t buf[3];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    /* map(1) header (1 byte) + key(0) (1 byte) fits; a 4-char tstr value
     * does not (needs a 1-byte length header + 4 data bytes = 5 more). */
    CHECK(cbor_w_map(&w, 1), "overflow: map header should fit in 3 bytes");
    CHECK(!cbor_w_tstr(&w, 0, "abcd", 4), "overflow: tstr should not fit and should return false");
    CHECK(w.err, "overflow: w.err should be sticky true after the failed write");
    CHECK(!cbor_w_uint(&w, 1, 1), "overflow: sticky err should fail subsequent writes too");
}

static void test_nint_rejects_nonnegative(void)
{
    uint8_t buf[16];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(!cbor_w_nint(&w, 0, 0), "cbor_w_nint(0) must be rejected (not negative)");
    CHECK(!cbor_w_nint(&w, 0, 5), "cbor_w_nint(5) must be rejected (not negative)");
    CHECK(w.len == 0, "a rejected cbor_w_nint() must not have written anything");
}

static void test_indefinite_length_rejected(void)
{
    /* 0x9f = array, indefinite length (major 4, additional info 31). */
    static const uint8_t indefinite_array[] = {0x9f, 0x01, 0x02, 0xff};
    cbor_r_t r;
    cbor_r_init(&r, indefinite_array, sizeof(indefinite_array));
    uint32_t count;
    CHECK(!cbor_r_array(&r, &count), "indefinite-length array must be rejected");
    CHECK(cbor_r_peek(&r) == CBOR_T_INVALID, "cbor_r_peek must call an indefinite array header invalid");

    /* 0xbf = map, indefinite length (major 5, additional info 31). */
    static const uint8_t indefinite_map[] = {0xbf, 0x00, 0x01, 0xff};
    cbor_r_init(&r, indefinite_map, sizeof(indefinite_map));
    CHECK(!cbor_r_map(&r, &count), "indefinite-length map must be rejected");
}

static void test_roundtrip_scalars(void)
{
    /* {0: 1, 12: 300, 13: h'0102', 14: "hi", 15: true, 16: null, 17: -95, 18: 1.5} */
    uint8_t buf[64];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(cbor_w_map(&w, 8), "roundtrip: map header");
    CHECK(cbor_w_uint(&w, 0, 1), "roundtrip: uint");
    CHECK(cbor_w_uint(&w, 12, 300), "roundtrip: uint 2-byte form");
    static const uint8_t bstr[] = {0x01, 0x02};
    CHECK(cbor_w_bstr(&w, 13, bstr, sizeof(bstr)), "roundtrip: bstr");
    CHECK(cbor_w_tstr(&w, 14, "hi", 2), "roundtrip: tstr");
    CHECK(cbor_w_bool(&w, 15, true), "roundtrip: bool");
    CHECK(cbor_w_null(&w, 16), "roundtrip: null");
    CHECK(cbor_w_nint(&w, 17, -95), "roundtrip: nint");
    CHECK(cbor_w_f64(&w, 18, 1.5), "roundtrip: f64");
    CHECK(!w.err, "roundtrip: no overflow expected");

    cbor_r_t r;
    cbor_r_init(&r, buf, w.len);
    uint32_t n;
    CHECK(cbor_r_map(&r, &n) && n == 8, "roundtrip: read back map header");

    uint32_t key;
    uint64_t u;
    CHECK(cbor_r_key(&r, &key) && key == 0, "roundtrip: key 0");
    CHECK(cbor_r_uint(&r, &u) && u == 1, "roundtrip: value 0");

    CHECK(cbor_r_key(&r, &key) && key == 12, "roundtrip: key 12");
    CHECK(cbor_r_uint(&r, &u) && u == 300, "roundtrip: value 12");

    const uint8_t *b;
    size_t blen;
    CHECK(cbor_r_key(&r, &key) && key == 13, "roundtrip: key 13");
    CHECK(cbor_r_bstr(&r, &b, &blen) && blen == 2 && b[0] == 1 && b[1] == 2, "roundtrip: value 13");

    const char *s;
    size_t slen;
    CHECK(cbor_r_key(&r, &key) && key == 14, "roundtrip: key 14");
    CHECK(cbor_r_tstr(&r, &s, &slen) && slen == 2 && memcmp(s, "hi", 2) == 0, "roundtrip: value 14");

    bool bv;
    CHECK(cbor_r_key(&r, &key) && key == 15, "roundtrip: key 15");
    CHECK(cbor_r_bool(&r, &bv) && bv == true, "roundtrip: value 15");

    CHECK(cbor_r_key(&r, &key) && key == 16, "roundtrip: key 16");
    CHECK(cbor_r_null(&r), "roundtrip: value 16");

    int64_t iv;
    CHECK(cbor_r_key(&r, &key) && key == 17, "roundtrip: key 17");
    CHECK(cbor_r_nint(&r, &iv) && iv == -95, "roundtrip: value 17");

    double dv;
    CHECK(cbor_r_key(&r, &key) && key == 18, "roundtrip: key 18");
    CHECK(cbor_r_f64(&r, &dv) && dv == 1.5, "roundtrip: value 18");
}

static void test_skip_unknown_key(void)
{
    /* {0: 1, 99: {0: "nested", 1: [1,2,3]}, 2: "kept"} — cbor_r_skip must
     * step over the whole nested map+array under key 99 in one call. */
    uint8_t buf[64];
    cbor_w_t w;
    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(cbor_w_map(&w, 3), "skip: map header");
    CHECK(cbor_w_uint(&w, 0, 1), "skip: key 0");
    CHECK(cbor_w_map_key(&w, 99, 2), "skip: key 99 nested map header");
    CHECK(cbor_w_tstr(&w, 0, "nested", 6), "skip: nested key 0");
    CHECK(cbor_w_array(&w, 1, 3), "skip: nested key 1 array header");
    CHECK(cbor_w_map(&w, 0), "skip: array element 0 (empty map)");
    CHECK(cbor_w_map(&w, 0), "skip: array element 1 (empty map)");
    CHECK(cbor_w_map(&w, 0), "skip: array element 2 (empty map)");
    CHECK(cbor_w_tstr(&w, 2, "kept", 4), "skip: key 2");
    CHECK(!w.err, "skip: no overflow expected");

    cbor_r_t r;
    cbor_r_init(&r, buf, w.len);
    uint32_t n;
    CHECK(cbor_r_map(&r, &n) && n == 3, "skip: read back map header");

    uint32_t key;
    uint64_t u;
    CHECK(cbor_r_key(&r, &key) && key == 0, "skip: key 0");
    CHECK(cbor_r_uint(&r, &u) && u == 1, "skip: value 0");

    CHECK(cbor_r_key(&r, &key) && key == 99, "skip: key 99");
    CHECK(cbor_r_skip(&r), "skip: cbor_r_skip over the nested map");

    const char *s;
    size_t slen;
    CHECK(cbor_r_key(&r, &key) && key == 2, "skip: key 2 reached after skip");
    CHECK(cbor_r_tstr(&r, &s, &slen) && slen == 4 && memcmp(s, "kept", 4) == 0, "skip: value 2");
}

/* docs/PROTOCOL.md §10 key 50 (`link`, v0.2 §9.5 — MQTT-session generation
 * counter within a boot, modes.c's STK_LINK). No pinned /status vector
 * exists in this directory (modes.c's build_status_cbor() is not part of
 * the host build — it needs the real modem/FreeRTOS stack), so this checks
 * the one thing that is this module's job: that cbor_w_uint() encodes key
 * 50 byte-for-byte identically to relay/tests/test_devauth.py's pinned
 * vectors (`test_wirecbor_link_encodes_at_key_50`'s `cbor2.dumps({50: 1})`
 * and `test_status_envelope_with_link_example_cbor_hex`'s trailing
 * `...183203`, both re-derived here as `a1183201`/`a1183203`), so a future
 * change to the writer's key/value encoding can't silently diverge from
 * what the relay's decoder expects on the wire. */
static void test_link_key_50_matches_relay_vector(void)
{
    uint8_t buf[8];
    cbor_w_t w;

    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(cbor_w_map(&w, 1), "link=1: map header");
    CHECK(cbor_w_uint(&w, 50, 1), "link=1: key 50 = 1");
    static const uint8_t expect_link1[] = {0xa1, 0x18, 0x32, 0x01};
    CHECK(w.len == sizeof(expect_link1) && memcmp(buf, expect_link1, sizeof(expect_link1)) == 0,
          "link=1: bytes must match relay's cbor2.dumps({50: 1}) == a1183201");

    cbor_w_init(&w, buf, sizeof(buf));
    CHECK(cbor_w_map(&w, 1), "link=3: map header");
    CHECK(cbor_w_uint(&w, 50, 3), "link=3: key 50 = 3");
    static const uint8_t expect_link3[] = {0xa1, 0x18, 0x32, 0x03};
    CHECK(w.len == sizeof(expect_link3) && memcmp(buf, expect_link3, sizeof(expect_link3)) == 0,
          "link=3: bytes must match relay's cbor2.dumps({50: 3}) == a1183203, the tail of "
          "test_devauth.py's test_status_envelope_with_link_example_cbor_hex");
}

int main(void)
{
    test_overflow();
    test_nint_rejects_nonnegative();
    test_indefinite_length_rejected();
    test_roundtrip_scalars();
    test_skip_unknown_key();
    test_link_key_50_matches_relay_vector();

    vector_t vectors[MAX_VECTORS];
    int n = load_vectors(AUTHVECTORS_PATH, vectors, MAX_VECTORS);
    if (n < 0) {
        return 1;
    }
    CHECK(n > 0, "no vectors loaded from %s", AUTHVECTORS_PATH);
    printf("loaded %d vectors from %s\n", n, AUTHVECTORS_PATH);
    for (int i = 0; i < n; i++) {
        test_vector(&vectors[i]);
    }

    if (g_failures == 0) {
        printf("PASS: %d vectors + unit checks, 0 failures\n", n);
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

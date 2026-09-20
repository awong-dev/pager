/* test_auth.c — host test harness for main/auth.c (task F3.3).
 *
 * Builds and runs with the host compiler (see Makefile), no ESP-IDF; auth.c
 * itself has no ESP-IDF/mbedTLS dependency in this build (see its
 * `#ifdef ESP_PLATFORM` split) so nothing extra needs linking here.
 *
 * Coverage required by docs/DEVICE_TASKS.md F3.3:
 *  1. Every vector in tools/authvectors.json verifies, and re-signing the
 *     verified (trimmed) bytes reproduces the original signed bytes
 *     exactly.
 *  2. Flipping any single byte of a signed vector makes it fail to verify.
 *  3. The 32-wide down-window (auth_accept_down_n) accepts/rejects per
 *     docs/DEVICE_PLAN.md §2.5 / docs/PROTOCOL.md §14.2.
 * Plus direct unit checks of auth_next_up_n()'s epoch/lo split and wrap
 * signalling.
 *
 * tools/authvectors.json is read with the same tiny purpose-built scanner
 * firmware/host/test_cbor.c uses (not a general JSON parser): it relies on
 * the file's exact `"label": "..."` / `"key_b64": "..."` / `"topic": "..."`
 * / `"cbor_signed_b64": "..."` formatting (json.dump(..., indent=2), see
 * S1.2). Host-test-only code; the "no malloc" rule for auth.c itself does
 * not apply here.
 */
#include "auth.h"
#include "cbor.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AUTHVECTORS_PATH "../../tools/authvectors.json"
#define MAX_VECTORS 32
#define MAX_LABEL 32
#define MAX_TOPIC 64
#define MAX_CBOR 256

typedef struct {
    char label[MAX_LABEL];
    char topic[MAX_TOPIC];
    uint8_t key[AUTH_KDEV_LEN];
    uint8_t cbor[MAX_CBOR];
    size_t cbor_len;
} vector_t;

static int g_failures = 0;

#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

/* ---------------------------------------------------------------------
 * tiny base64 decoder (host-only; same as firmware/host/test_cbor.c's)
 * --------------------------------------------------------------------- */

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
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
            out[o++] = (uint8_t) ((vals[0] << 2) | (vals[1] >> 4));
            out[o++] = (uint8_t) ((vals[1] << 4) | (vals[2] >> 2));
            out[o++] = (uint8_t) ((vals[2] << 6) | vals[3]);
            nv = 0;
        }
    }
    if (nv == 2) {
        if (o + 1 > out_cap) {
            return false;
        }
        out[o++] = (uint8_t) ((vals[0] << 2) | (vals[1] >> 4));
    } else if (nv == 3) {
        if (o + 2 > out_cap) {
            return false;
        }
        out[o++] = (uint8_t) ((vals[0] << 2) | (vals[1] >> 4));
        out[o++] = (uint8_t) ((vals[1] << 4) | (vals[2] >> 2));
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

static bool copy_quoted_field(const char *data, size_t data_len, size_t from, const char *key,
                               char *out, size_t out_cap, size_t *next_pos)
{
    const char *v = find_after(data, data_len, from, key);
    if (!v) {
        return false;
    }
    const char *v_end = strchr(v, '"');
    if (!v_end) {
        return false;
    }
    size_t vlen = (size_t) (v_end - v);
    if (vlen >= out_cap) {
        vlen = out_cap - 1;
    }
    memcpy(out, v, vlen);
    out[vlen] = '\0';
    *next_pos = (size_t) (v_end - data);
    return true;
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
    char *data = malloc((size_t) sz + 1);
    if (!data) {
        fclose(f);
        printf("FAIL out of memory reading %s\n", path);
        return -1;
    }
    size_t got = fread(data, 1, (size_t) sz, f);
    fclose(f);
    data[got] = '\0';

    int n = 0;
    size_t pos = 0;
    while (n < max) {
        char label[MAX_LABEL];
        size_t after_label;
        if (!copy_quoted_field(data, got, pos, "\"label\": \"", label, sizeof(label), &after_label)) {
            break;
        }

        char key_b64[64];
        size_t after_key;
        if (!copy_quoted_field(data, got, after_label, "\"key_b64\": \"", key_b64, sizeof(key_b64),
                                &after_key)) {
            printf("FAIL vector %s: no key_b64\n", label);
            free(data);
            return -1;
        }

        char topic[MAX_TOPIC];
        size_t after_topic;
        if (!copy_quoted_field(data, got, after_key, "\"topic\": \"", topic, sizeof(topic),
                                &after_topic)) {
            printf("FAIL vector %s: no topic\n", label);
            free(data);
            return -1;
        }

        char cbor_b64[512];
        size_t after_cbor;
        if (!copy_quoted_field(data, got, after_topic, "\"cbor_signed_b64\": \"", cbor_b64,
                                sizeof(cbor_b64), &after_cbor)) {
            printf("FAIL vector %s: no cbor_signed_b64\n", label);
            free(data);
            return -1;
        }

        strncpy(out[n].label, label, sizeof(out[n].label) - 1);
        out[n].label[sizeof(out[n].label) - 1] = '\0';
        strncpy(out[n].topic, topic, sizeof(out[n].topic) - 1);
        out[n].topic[sizeof(out[n].topic) - 1] = '\0';

        size_t key_len;
        if (!base64_decode(key_b64, strlen(key_b64), out[n].key, sizeof(out[n].key), &key_len) ||
            key_len != AUTH_KDEV_LEN) {
            printf("FAIL vector %s: key_b64 did not decode to %d bytes\n", label, AUTH_KDEV_LEN);
            free(data);
            return -1;
        }

        if (!base64_decode(cbor_b64, strlen(cbor_b64), out[n].cbor, sizeof(out[n].cbor),
                            &out[n].cbor_len)) {
            printf("FAIL vector %s: cbor_signed_b64 too long for MAX_CBOR\n", label);
            free(data);
            return -1;
        }

        pos = after_cbor;
        n++;
    }

    free(data);
    return n;
}

/* ---------------------------------------------------------------------
 * per-vector verify + re-sign round trip, and byte-flip negative checks
 * --------------------------------------------------------------------- */

static void test_vector(const vector_t *v)
{
    CHECK(v->cbor_len > 10, "%s: cbor_signed_b64 shorter than the 10-byte sig suffix", v->label);
    if (v->cbor_len <= 10) {
        return;
    }

    auth_init(v->key);

    /* 1. verify succeeds and trims exactly 10 bytes. */
    uint8_t buf[MAX_CBOR];
    memcpy(buf, v->cbor, v->cbor_len);
    size_t len = v->cbor_len;
    bool ok = auth_verify(v->topic, buf, &len);
    CHECK(ok, "%s: auth_verify() failed on a genuine vector", v->label);
    if (!ok) {
        return;
    }
    CHECK(len == v->cbor_len - 10, "%s: auth_verify() trimmed to %zu, expected %zu", v->label, len,
          v->cbor_len - 10);
    CHECK(memcmp(buf, v->cbor, len) == 0, "%s: auth_verify() modified bytes before the sig suffix",
          v->label);

    /* 2. re-signing the verified (trimmed) bytes reproduces the original
     * signed bytes exactly (task requirement: "every vector verifies and
     * re-signs to the identical bytes"). */
    size_t resign_len = len;
    ok = auth_sign(v->topic, buf, &resign_len, sizeof(buf));
    CHECK(ok, "%s: auth_sign() failed re-signing the verified bytes", v->label);
    if (!ok) {
        return;
    }
    CHECK(resign_len == v->cbor_len, "%s: re-signed length %zu != original %zu", v->label,
          resign_len, v->cbor_len);
    if (resign_len == v->cbor_len) {
        CHECK(memcmp(buf, v->cbor, v->cbor_len) == 0, "%s: re-signed bytes differ from the original",
              v->label);
    }

    /* 3. flipping any single byte of the signed vector must fail to
     * verify (exhaustive, mirrors relay/tests/test_devauth.py). */
    for (size_t i = 0; i < v->cbor_len; i++) {
        uint8_t corrupted[MAX_CBOR];
        memcpy(corrupted, v->cbor, v->cbor_len);
        corrupted[i] ^= 0x01;
        size_t clen = v->cbor_len;
        bool cok = auth_verify(v->topic, corrupted, &clen);
        CHECK(!cok, "%s: byte %zu flip was not detected", v->label, i);
    }

    /* Wrong key must also fail. */
    uint8_t wrong_key[AUTH_KDEV_LEN];
    for (size_t i = 0; i < sizeof(wrong_key); i++) {
        wrong_key[i] = (uint8_t) (v->key[i] + 1);
    }
    auth_init(wrong_key);
    memcpy(buf, v->cbor, v->cbor_len);
    len = v->cbor_len;
    CHECK(!auth_verify(v->topic, buf, &len), "%s: verify succeeded with the wrong key", v->label);
    auth_init(v->key); /* restore for any caller running after this */
}

static void test_missing_sig_fails(void)
{
    static const uint8_t key[AUTH_KDEV_LEN] = {0};
    auth_init(key);
    uint8_t buf[4] = {0xa1, 0x00, 0x01, 0x00};
    size_t len = sizeof(buf);
    CHECK(!auth_verify("pager/pgr-0001/up", buf, &len), "payload shorter than the sig suffix must fail");
    CHECK(len == sizeof(buf), "a failed verify must not change *len");
}

static void test_sign_rejects_when_cap_too_small(void)
{
    static const uint8_t key[AUTH_KDEV_LEN] = {0};
    auth_init(key);
    uint8_t buf[12] = {0xa1, 0x00, 0x01};
    size_t len = 3;
    /* cap leaves no room for the 10-byte suffix. */
    CHECK(!auth_sign("pager/pgr-0001/up", buf, &len, 3 + 9), "auth_sign() must reject cap too small by one");
    CHECK(len == 3, "a rejected auth_sign() must not change *len");
    CHECK(auth_sign("pager/pgr-0001/up", buf, &len, 3 + 10), "auth_sign() must succeed with exactly enough room");
    CHECK(len == 13, "auth_sign() must advance *len by exactly 10 on success");
}

static void test_sign_verify_without_init_fails(void)
{
    /* Fresh process-level state is asserted only by convention here (these
     * two must run before any auth_init() call in main()); kept as the
     * first tests to run so this actually checks "never initialised". */
    uint8_t buf[16] = {0xa1, 0x00, 0x01};
    size_t len = 3;
    CHECK(!auth_sign("pager/pgr-0001/up", buf, &len, sizeof(buf)),
          "auth_sign() before auth_init() must fail");
    size_t vlen = 10;
    uint8_t vbuf[10] = {0};
    CHECK(!auth_verify("pager/pgr-0001/up", vbuf, &vlen),
          "auth_verify() before auth_init() must fail");
}

/* ---------------------------------------------------------------------
 * auth_next_up_n(): epoch/lo split and wrap signalling.
 * --------------------------------------------------------------------- */

static void test_next_up_n(void)
{
    auth_rtc_t rtc = {0};
    bool wrapped;

    uint64_t n0 = auth_next_up_n(&rtc, 0, &wrapped);
    CHECK(n0 == 0, "first n with epoch=0 must be 0, got %llu", (unsigned long long) n0);
    CHECK(!wrapped, "first call must not report a wrap");
    CHECK(rtc.up_lo == 1, "up_lo must advance to 1, got %u", (unsigned) rtc.up_lo);

    uint64_t n1 = auth_next_up_n(&rtc, 0, &wrapped);
    CHECK(n1 == 1, "second n with epoch=0 must be 1, got %llu", (unsigned long long) n1);
    CHECK(!wrapped, "second call must not report a wrap");

    uint64_t n_epoch = auth_next_up_n(&rtc, 3, NULL);
    CHECK(n_epoch == (((uint64_t) 3u << AUTH_UP_LO_BITS) | 2u),
          "epoch must shift into the high bits, got %llu", (unsigned long long) n_epoch);

    /* wrap: up_lo at AUTH_UP_LO_MASK must roll to 0 and report wrapped. */
    rtc.up_lo = AUTH_UP_LO_MASK;
    uint64_t n_before_wrap = auth_next_up_n(&rtc, 5, &wrapped);
    CHECK(n_before_wrap == (((uint64_t) 5u << AUTH_UP_LO_BITS) | AUTH_UP_LO_MASK),
          "n just before wrap must report the max lo, got %llu", (unsigned long long) n_before_wrap);
    CHECK(wrapped, "up_lo rolling from AUTH_UP_LO_MASK to 0 must report wrapped=true");
    CHECK(rtc.up_lo == 0, "up_lo must roll to 0, got %u", (unsigned) rtc.up_lo);

    /* v0.2 (docs/V02_DESIGN.md §3): epoch is now the full 32 bits of
     * ident_t.n_epoch, not masked to 12 -- a 32-bit epoch value must appear
     * in full in the high bits of a 64-bit n (>= 2^32, would have been
     * silently truncated to 0 by the old 12-bit mask and a uint32_t shift). */
    auth_rtc_t rtc2 = {0};
    uint64_t n_wide_epoch = auth_next_up_n(&rtc2, 0xFFFFFFFFu, NULL);
    CHECK(n_wide_epoch == (((uint64_t) 0xFFFFFFFFu) << AUTH_UP_LO_BITS),
          "a 32-bit epoch must appear in full in the high bits, got %llu",
          (unsigned long long) n_wide_epoch);
    CHECK(n_wide_epoch > 0xFFFFFFFFull,
          "n from a near-max epoch must itself exceed 32 bits, got %llu",
          (unsigned long long) n_wide_epoch);
}

/* ---------------------------------------------------------------------
 * auth_accept_down_n(): the 32-wide window, docs/DEVICE_PLAN.md §2.5.
 * --------------------------------------------------------------------- */

static void test_accept_down_n(void)
{
    auth_rtc_t rtc = {0};

    CHECK(!auth_accept_down_n(&rtc, 0), "n=0 must always be rejected");

    CHECK(auth_accept_down_n(&rtc, 1), "n=1 must be accepted from a fresh window");
    CHECK(rtc.down_n == 1, "down_n must advance to 1");

    CHECK(auth_accept_down_n(&rtc, 2), "n=2 must be accepted (strictly increasing)");
    CHECK(auth_accept_down_n(&rtc, 5), "n=5 must be accepted (a gap is fine going forward)");
    CHECK(rtc.down_n == 5, "down_n must advance to 5");

    /* replay of an already-seen value must be rejected without changing state. */
    uint32_t bits_before = rtc.down_bits;
    CHECK(!auth_accept_down_n(&rtc, 5), "replaying the current down_n must be rejected");
    CHECK(!auth_accept_down_n(&rtc, 2), "replaying an already-seen in-window value must be rejected");
    CHECK(rtc.down_bits == bits_before, "a rejected replay must not change down_bits");

    /* an out-of-order but not-yet-seen value inside the window is accepted
     * once, then rejected as a replay the second time. */
    CHECK(auth_accept_down_n(&rtc, 3), "n=3 (skipped earlier, in-window) must be accepted once");
    CHECK(!auth_accept_down_n(&rtc, 3), "n=3 must be rejected the second time (replay)");

    /* exactly AUTH_DOWN_WINDOW behind the current down_n (5) is still
     * in-window: gap == AUTH_DOWN_WINDOW is allowed (`gap <= AUTH_DOWN_WINDOW`). */
    auth_rtc_t rtc2 = {0};
    CHECK(auth_accept_down_n(&rtc2, 1000), "seed down_n=1000");
    uint32_t edge = 1000 - AUTH_DOWN_WINDOW;
    CHECK(auth_accept_down_n(&rtc2, edge), "n exactly AUTH_DOWN_WINDOW behind down_n must be accepted");
    uint32_t just_outside = edge - 1;
    CHECK(!auth_accept_down_n(&rtc2, just_outside),
          "n one more than AUTH_DOWN_WINDOW behind down_n must be rejected");

    /* a jump of AUTH_DOWN_WINDOW or more forward must not read
     * uninitialised/undefined bits (the shift-by->=32 guard). */
    auth_rtc_t rtc3 = {0};
    CHECK(auth_accept_down_n(&rtc3, 1), "seed down_n=1");
    CHECK(auth_accept_down_n(&rtc3, 1 + AUTH_DOWN_WINDOW + 100),
          "a big forward jump must still be accepted");
    CHECK(rtc3.down_bits == 0, "a forward jump >= AUTH_DOWN_WINDOW must reset down_bits to 0");
}

/* ---------------------------------------------------------------------
 * v0.2 (docs/V02_DESIGN.md §3): `n` >= 2^32 vectors.
 *
 * tools/authvectors.json is shared with a concurrent relay-side task that
 * is also adding large-`n` vectors there, so per this task's instructions
 * these two are self-contained here instead, to avoid an edit collision.
 * Each is a full signed CBOR envelope ({v, id, ts, ack, n}, matching the
 * shape of authvectors.json's own "ack" vector, plus `n`), generated with:
 *
 *   relay/.venv/bin/python - <<'EOF'
 *   import sys; sys.path.insert(0, "relay")
 *   from app import devauth
 *   key = bytes(range(32))
 *   topic = "pager/pgr-0001/up"
 *   obj = {"v": 1, "id": "m_7f3a2b10", "ts": 1757700100, "ack": "shown",
 *          "n": 2**32}                                    # or 2**53 - 1
 *   print(devauth.sign_cbor(key, topic, obj).hex())
 *   EOF
 *
 * i.e. the exact same sign_cbor() relay/app/devauth.py uses for every other
 * vector in tools/authvectors.json, mirroring docs/V02_DESIGN.md §3's "n
 * encodes at minimal length, so nothing grows until the epoch passes 4095"
 * -- these two exercise the two wider forms cbor.c's put_head()/get_head()
 * gain once `n` can exceed 32 bits: additional-info 27 (8-byte payload,
 * major type 0) for both 2^32 and 2^53-1 (PROTOCOL.md §3.1's "< 2^53"
 * ceiling, key 12's new range per docs/V02_DESIGN.md §7).
 * --------------------------------------------------------------------- */

typedef struct {
    const char *label;
    uint64_t expect_n;
    const uint8_t *cbor;
    size_t cbor_len;
} large_n_vector_t;

/* n = 2^32 (4294967296, 0x100000000) */
static const uint8_t k_n_2pow32[] = {
    0xa6, 0x00, 0x01, 0x01, 0x6a, 0x6d, 0x5f, 0x37, 0x66, 0x33, 0x61, 0x32,
    0x62, 0x31, 0x30, 0x02, 0x1a, 0x68, 0xc4, 0x60, 0x04, 0x05, 0x65, 0x73,
    0x68, 0x6f, 0x77, 0x6e, 0x0c, 0x1b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x0d, 0x48, 0xc8, 0xc3, 0x76, 0x5a, 0xb8, 0x13, 0x47, 0xbb,
};

/* n = 2^53 - 1 (9007199254740991, 0x1fffffffffffff — PROTOCOL.md §3.1's
 * ceiling, the largest value exact as a JSON/Firestore double/int64). */
static const uint8_t k_n_2pow53_minus1[] = {
    0xa6, 0x00, 0x01, 0x01, 0x6a, 0x6d, 0x5f, 0x37, 0x66, 0x33, 0x61, 0x32,
    0x62, 0x31, 0x31, 0x02, 0x1a, 0x68, 0xc4, 0x60, 0x68, 0x05, 0x65, 0x73,
    0x68, 0x6f, 0x77, 0x6e, 0x0c, 0x1b, 0x00, 0x1f, 0xff, 0xff, 0xff, 0xff,
    0xff, 0xff, 0x0d, 0x48, 0x22, 0xd3, 0x2e, 0x2a, 0x96, 0x21, 0x1f, 0x71,
};

static const large_n_vector_t k_large_n_vectors[] = {
    {"n_2pow32", 4294967296ull, k_n_2pow32, sizeof(k_n_2pow32)},
    {"n_2pow53_minus1", 9007199254740991ull, k_n_2pow53_minus1, sizeof(k_n_2pow53_minus1)},
};

/* Walks a decoded (sig-trimmed) envelope map looking for CBOR key 12 (`n`,
 * PROTOCOL.md §10) via the same cbor_r_uint() msg.c uses. Skips every other
 * key's value with cbor_r_skip() -- this test does not care about the
 * envelope's other fields, only that `n` round-trips. */
static bool find_n_field(const uint8_t *buf, size_t len, uint64_t *out_n)
{
    cbor_r_t r;
    cbor_r_init(&r, buf, len);
    uint32_t count;
    if (!cbor_r_map(&r, &count)) {
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            return false;
        }
        if (key == 12 /* MK_N, PROTOCOL.md §10 */) {
            return cbor_r_uint(&r, out_n);
        }
        if (!cbor_r_skip(&r)) {
            return false;
        }
    }
    return false;
}

static void test_large_n_vectors(void)
{
    static const uint8_t key[AUTH_KDEV_LEN] = {
        0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
        16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
    };
    static const char *topic = "pager/pgr-0001/up";

    for (size_t i = 0; i < sizeof(k_large_n_vectors) / sizeof(k_large_n_vectors[0]); i++) {
        const large_n_vector_t *v = &k_large_n_vectors[i];
        auth_init(key);

        uint8_t buf[64];
        CHECK(v->cbor_len <= sizeof(buf), "%s: vector longer than the scratch buffer", v->label);
        if (v->cbor_len > sizeof(buf)) {
            continue;
        }
        memcpy(buf, v->cbor, v->cbor_len);
        size_t len = v->cbor_len;

        /* 1. auth_verify() must accept a wide `n` -- nothing about it is
         * special to auth.c (it never parses `n`, only the trailing sig
         * suffix), but this pins that a bigger CBOR integer elsewhere in
         * the buffer never confuses the tag computation/comparison. */
        bool ok = auth_verify(topic, buf, &len);
        CHECK(ok, "%s: auth_verify() failed on a genuine large-n vector", v->label);
        if (!ok) {
            continue;
        }
        CHECK(len == v->cbor_len - AUTH_SIG_SUFFIX_LEN,
              "%s: auth_verify() trimmed to %zu, expected %zu", v->label, len,
              v->cbor_len - AUTH_SIG_SUFFIX_LEN);

        /* 2. re-signing the verified bytes reproduces the original exactly
         * (same round-trip contract as test_vector() above). */
        uint8_t resigned[64];
        memcpy(resigned, buf, len);
        size_t resign_len = len;
        ok = auth_sign(topic, resigned, &resign_len, sizeof(resigned));
        CHECK(ok, "%s: auth_sign() failed re-signing", v->label);
        CHECK(ok && resign_len == v->cbor_len && memcmp(resigned, v->cbor, v->cbor_len) == 0,
              "%s: re-signed bytes differ from the original", v->label);

        /* 3. cbor.c (cbor_r_uint(), the exact function msg.c's MK_N case
         * uses) must decode the wide `n` this vector carries byte-for-byte
         * as the relay's cbor2 encoded it -- the real point of this test:
         * confirming the firmware's minimal-length uint64 decode agrees
         * with the wire format a real relay/pager exchange would use. */
        uint64_t decoded_n = 0;
        CHECK(find_n_field(buf, len, &decoded_n), "%s: `n` field not found/decodable", v->label);
        CHECK(decoded_n == v->expect_n, "%s: decoded n=%llu, expected %llu", v->label,
              (unsigned long long) decoded_n, (unsigned long long) v->expect_n);

        /* 4. a corrupted tag must still fail (same defense-in-depth as
         * test_vector()'s exhaustive byte flip, spot-checked on the one
         * byte inside the wide `n`'s own encoding that is least likely to
         * be covered by any other test: its most significant byte). */
        uint8_t corrupted[64];
        memcpy(corrupted, v->cbor, v->cbor_len);
        corrupted[30] ^= 0x01; /* first byte of the 8-byte `n` payload, both vectors */
        size_t clen = v->cbor_len;
        CHECK(!auth_verify(topic, corrupted, &clen),
              "%s: corrupting a byte inside the wide n payload was not detected", v->label);
    }
}

int main(void)
{
    test_sign_verify_without_init_fails();
    test_missing_sig_fails();
    test_sign_rejects_when_cap_too_small();
    test_next_up_n();
    test_accept_down_n();
    test_large_n_vectors();

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

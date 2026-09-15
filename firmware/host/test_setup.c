/* test_setup.c — host test harness for main/setup.c's host-buildable
 * top section (task F3.5): setup_parse_code(), setup_derive(),
 * setup_decrypt_bundle().
 *
 * Builds and runs with the host compiler against the real mbedtls (see the
 * Makefile) plus main/cbor.c (this file CBOR-decodes the decrypted
 * plaintext itself, using cbor.c's own already-tested reader, to check the
 * decrypted bytes against tools/setup_code_vectors.json's fields — setup.c's
 * own CBOR-decode-into-ident_t step, decode_and_validate_bundle(), is
 * `#ifdef ESP_PLATFORM`-only and not exercised here, see setup.c's module
 * comment).
 *
 * tools/setup_code_vectors.json is read with the same tiny purpose-built
 * scanner firmware/host/test_auth.c/test_cbor.c use (not a general JSON
 * parser): it relies on the file's exact `"key": "value"` / `"key": 123`
 * formatting (json.dump(..., indent=2)). Host-test-only code; the "no
 * malloc"/"no dynamic allocation" rule for setup.c itself does not apply
 * here.
 */
#include "setup.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cbor.h"

#define VECTORS_PATH "../../tools/setup_code_vectors.json"

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
 * tiny base64 decoder (host-only; same shape as test_auth.c's/test_cbor.c's)
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
 * tools/setup_code_vectors.json scanner
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

/* Copies a quoted JSON string field, unescaping `\n \t \r \" \\ \/` (the
 * only escapes this fixture's fields use — the CA PEM's embedded newlines
 * come through as `\n`). */
static bool copy_quoted_field(const char *data, size_t data_len, size_t from, const char *key,
                               char *out, size_t out_cap, size_t *next_pos)
{
    const char *v = find_after(data, data_len, from, key);
    if (!v) {
        return false;
    }
    size_t o = 0;
    const char *p = v;
    const char *end = data + data_len;
    while (p < end && *p != '"') {
        char c = *p;
        if (c == '\\' && p + 1 < end) {
            p++;
            switch (*p) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            default: c = *p; break;
            }
        }
        if (o + 1 >= out_cap) {
            return false;
        }
        out[o++] = c;
        p++;
    }
    if (p >= end) {
        return false;
    }
    out[o] = '\0';
    *next_pos = (size_t) (p + 1 - data);
    return true;
}

static bool copy_number_field(const char *data, size_t data_len, size_t from, const char *key,
                              long *out, size_t *next_pos)
{
    const char *v = find_after(data, data_len, from, key);
    if (!v) {
        return false;
    }
    char *endp;
    long val = strtol(v, &endp, 10);
    if (endp == v) {
        return false;
    }
    *out = val;
    *next_pos = (size_t) (endp - data);
    return true;
}

typedef struct {
    char token_b64[32];
    char code[128];
    char bid[SETUP_BID_LEN];
    char bpw[SETUP_BPW_LEN];
    char bkey_b64[64];
    char exp_id[64];
    char exp_pw[128];
    char exp_k_b64[64];
    char exp_host[128];
    long exp_port;
    char exp_ca[2048];
    long exp_flags;
    char exp_label[64];
    char bundle_nonce_b64[32];
    char bundle_ct_b64[1024];
} vector_t;

static bool load_vector(const char *path, vector_t *out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        printf("FAIL cannot open %s\n", path);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) {
        fclose(f);
        printf("FAIL %s is empty\n", path);
        return false;
    }
    char *data = malloc((size_t) sz + 1);
    if (!data) {
        fclose(f);
        printf("FAIL out of memory reading %s\n", path);
        return false;
    }
    size_t got = fread(data, 1, (size_t) sz, f);
    fclose(f);
    data[got] = '\0';

    size_t pos = 0;
    bool ok = true;
    ok = ok && copy_quoted_field(data, got, pos, "\"token_b64\": \"", out->token_b64,
                                 sizeof(out->token_b64), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"code\": \"", out->code, sizeof(out->code), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"bid\": \"", out->bid, sizeof(out->bid), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"bpw\": \"", out->bpw, sizeof(out->bpw), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"bkey_b64\": \"", out->bkey_b64,
                                 sizeof(out->bkey_b64), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"id\": \"", out->exp_id, sizeof(out->exp_id), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"pw\": \"", out->exp_pw, sizeof(out->exp_pw), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"k_b64\": \"", out->exp_k_b64,
                                 sizeof(out->exp_k_b64), &pos);
    ok = ok &&
        copy_quoted_field(data, got, pos, "\"host\": \"", out->exp_host, sizeof(out->exp_host), &pos);
    ok = ok && copy_number_field(data, got, pos, "\"port\": ", &out->exp_port, &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"ca\": \"", out->exp_ca, sizeof(out->exp_ca), &pos);
    ok = ok && copy_number_field(data, got, pos, "\"flags\": ", &out->exp_flags, &pos);
    ok = ok &&
        copy_quoted_field(data, got, pos, "\"label\": \"", out->exp_label, sizeof(out->exp_label), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"bundle_nonce_b64\": \"", out->bundle_nonce_b64,
                                 sizeof(out->bundle_nonce_b64), &pos);
    ok = ok && copy_quoted_field(data, got, pos, "\"bundle_ct_b64\": \"", out->bundle_ct_b64,
                                 sizeof(out->bundle_ct_b64), &pos);

    free(data);
    if (!ok) {
        printf("FAIL could not scan every expected field out of %s\n", path);
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * tests
 * --------------------------------------------------------------------- */

static void test_parse_and_derive(const vector_t *v, uint8_t bkey_out[32])
{
    uint8_t exp_token[SETUP_TOKEN_LEN];
    size_t exp_token_len;
    CHECK(base64_decode(v->token_b64, strlen(v->token_b64), exp_token, sizeof(exp_token),
                        &exp_token_len) &&
              exp_token_len == SETUP_TOKEN_LEN,
          "vector token_b64 did not decode to %d bytes", SETUP_TOKEN_LEN);

    setup_code_t sc;
    bool ok = setup_parse_code(v->code, &sc);
    CHECK(ok, "setup_parse_code() failed on the vector's own code %s", v->code);
    if (!ok) {
        return;
    }
    CHECK(memcmp(sc.token, exp_token, SETUP_TOKEN_LEN) == 0,
          "setup_parse_code() decoded a different token than the vector's token_b64");
    /* Read straight from the vector's own `code` string
     * ("000G-40R4-0M30-EX @ abc123.ala.us-east-1.emqxsl.com"): no port or
     * ;apn=... suffix, so the default port and empty apn apply. */
    CHECK(strcmp(sc.host, "abc123.ala.us-east-1.emqxsl.com") == 0, "unexpected host '%s'", sc.host);
    CHECK(sc.port == 8883, "unexpected port %u, expected the 8883 default", (unsigned) sc.port);
    CHECK(sc.apn[0] == '\0', "unexpected apn '%s', expected empty (carrier default)", sc.apn);

    char bid[SETUP_BID_LEN];
    char bpw[SETUP_BPW_LEN];
    uint8_t bkey[32];
    setup_derive(sc.token, bid, bpw, bkey);

    CHECK(strcmp(bid, v->bid) == 0, "derived bid '%s' != vector bid '%s'", bid, v->bid);
    CHECK(strcmp(bpw, v->bpw) == 0, "derived bpw '%s' != vector bpw '%s'", bpw, v->bpw);

    uint8_t exp_bkey[32];
    size_t exp_bkey_len;
    CHECK(base64_decode(v->bkey_b64, strlen(v->bkey_b64), exp_bkey, sizeof(exp_bkey), &exp_bkey_len) &&
              exp_bkey_len == 32,
          "vector bkey_b64 did not decode to 32 bytes");
    CHECK(memcmp(bkey, exp_bkey, 32) == 0, "derived bkey != vector bkey_b64");

    memcpy(bkey_out, bkey, 32);
}

/* Corrupting a single character of the token portion must fail the Luhn
 * check character (docs/DEVICE_PLAN.md §3.1's whole reason for existing:
 * "a typo is caught on the device before any radio comes up"). */
static void test_parse_rejects_typo(const vector_t *v)
{
    char corrupted[128];
    strncpy(corrupted, v->code, sizeof(corrupted) - 1);
    corrupted[sizeof(corrupted) - 1] = '\0';
    /* v->code == "000G-40R4-0M30-EX @ ..."; flip the 2nd data character
     * (index 1, '0' -> '1') without touching the check character. */
    CHECK(corrupted[1] != '\0', "test fixture code too short to corrupt");
    corrupted[1] = (corrupted[1] == '1') ? '2' : '1';

    setup_code_t sc;
    CHECK(!setup_parse_code(corrupted, &sc), "a single-character typo must fail the check character");
}

static void test_parse_rejects_malformed(void)
{
    setup_code_t sc;
    CHECK(!setup_parse_code("no-at-sign-here", &sc), "a code with no '@' must be rejected");
    CHECK(!setup_parse_code("000G-40R4-0M30-EX @ ", &sc), "a code with no host must be rejected");
    CHECK(!setup_parse_code(NULL, &sc), "a NULL code must be rejected");
}

static void test_decrypt_and_decode(const vector_t *v, const uint8_t bkey[32])
{
    uint8_t blob[1024];
    size_t blob_len;
    bool ok = base64_decode(v->bundle_ct_b64, strlen(v->bundle_ct_b64), blob, sizeof(blob), &blob_len);
    CHECK(ok, "vector bundle_ct_b64 failed to base64-decode");
    if (!ok) {
        return;
    }

    uint8_t nonce[16];
    size_t nonce_len;
    CHECK(base64_decode(v->bundle_nonce_b64, strlen(v->bundle_nonce_b64), nonce, sizeof(nonce),
                        &nonce_len) &&
              nonce_len == 12,
          "vector bundle_nonce_b64 did not decode to 12 bytes");
    CHECK(memcmp(blob, nonce, 12) == 0,
          "bundle_ct_b64 does not start with bundle_nonce_b64 (vector shape assumption)");

    uint8_t plain[2048];
    size_t plain_len = 0;
    ok = setup_decrypt_bundle(bkey, blob, blob_len, plain, sizeof(plain), &plain_len);
    CHECK(ok, "setup_decrypt_bundle() failed on the vector's own bundle");
    if (!ok) {
        return;
    }

    /* CBOR-decode the plaintext with cbor.c's own reader and check every
     * field against the vector's bundle_plain_obj (docs/PROTOCOL.md §10
     * boot keymap: 0=v,1=id,30=pw,31=k,32=host,33=port,34=ca,35=flags,
     * 36=label). */
    cbor_r_t r;
    cbor_r_init(&r, plain, plain_len);
    uint32_t count;
    CHECK(cbor_r_map(&r, &count), "plaintext bundle is not a CBOR map");

    uint8_t exp_k[32];
    size_t exp_k_len;
    CHECK(base64_decode(v->exp_k_b64, strlen(v->exp_k_b64), exp_k, sizeof(exp_k), &exp_k_len) &&
              exp_k_len == 32,
          "vector k_b64 did not decode to 32 bytes");

    bool saw_id = false, saw_pw = false, saw_k = false, saw_host = false, saw_port = false,
        saw_ca = false, saw_flags = false, saw_label = false;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        CHECK(cbor_r_key(&r, &key), "failed to read a map key");
        switch (key) {
        case 0: { /* v */
            uint64_t val;
            CHECK(cbor_r_uint(&r, &val) && val == 1, "bundle 'v' != 1");
            break;
        }
        case 1: { /* id */
            const char *s; size_t slen;
            CHECK(cbor_r_tstr(&r, &s, &slen), "bundle 'id' is not a tstr");
            CHECK(slen == strlen(v->exp_id) && memcmp(s, v->exp_id, slen) == 0,
                  "bundle 'id' != vector id '%s'", v->exp_id);
            saw_id = true;
            break;
        }
        case 30: { /* pw */
            const char *s; size_t slen;
            CHECK(cbor_r_tstr(&r, &s, &slen), "bundle 'pw' is not a tstr");
            CHECK(slen == strlen(v->exp_pw) && memcmp(s, v->exp_pw, slen) == 0, "bundle 'pw' mismatch");
            saw_pw = true;
            break;
        }
        case 31: { /* k */
            const uint8_t *b; size_t blen;
            CHECK(cbor_r_bstr(&r, &b, &blen), "bundle 'k' is not a bstr");
            CHECK(blen == 32 && memcmp(b, exp_k, 32) == 0, "bundle 'k' != vector k_b64");
            saw_k = true;
            break;
        }
        case 32: { /* host */
            const char *s; size_t slen;
            CHECK(cbor_r_tstr(&r, &s, &slen), "bundle 'host' is not a tstr");
            CHECK(slen == strlen(v->exp_host) && memcmp(s, v->exp_host, slen) == 0,
                  "bundle 'host' != vector host '%s'", v->exp_host);
            saw_host = true;
            break;
        }
        case 33: { /* port */
            uint64_t val;
            CHECK(cbor_r_uint(&r, &val) && (long) val == v->exp_port, "bundle 'port' != %ld",
                  v->exp_port);
            saw_port = true;
            break;
        }
        case 34: { /* ca */
            const char *s; size_t slen;
            CHECK(cbor_r_tstr(&r, &s, &slen), "bundle 'ca' is not a tstr");
            CHECK(slen == strlen(v->exp_ca) && memcmp(s, v->exp_ca, slen) == 0, "bundle 'ca' mismatch");
            saw_ca = true;
            break;
        }
        case 35: { /* flags */
            uint64_t val;
            CHECK(cbor_r_uint(&r, &val) && (long) val == v->exp_flags, "bundle 'flags' != %ld",
                  v->exp_flags);
            saw_flags = true;
            break;
        }
        case 36: { /* label */
            const char *s; size_t slen;
            CHECK(cbor_r_tstr(&r, &s, &slen), "bundle 'label' is not a tstr");
            CHECK(slen == strlen(v->exp_label) && memcmp(s, v->exp_label, slen) == 0,
                  "bundle 'label' != vector label '%s'", v->exp_label);
            saw_label = true;
            break;
        }
        default:
            CHECK(cbor_r_skip(&r), "failed to skip an unexpected key %u", (unsigned) key);
            break;
        }
    }
    CHECK(saw_id && saw_pw && saw_k && saw_host && saw_port && saw_ca && saw_flags && saw_label,
          "the decoded bundle is missing at least one required field");

    /* Negative: a flipped ciphertext byte must fail the GCM tag check. */
    uint8_t corrupted[1024];
    memcpy(corrupted, blob, blob_len);
    corrupted[blob_len - 1] ^= 0x01; /* last byte of the 16-byte tag */
    uint8_t scratch[2048];
    size_t scratch_len = 0;
    CHECK(!setup_decrypt_bundle(bkey, corrupted, blob_len, scratch, sizeof(scratch), &scratch_len),
          "a corrupted tag must fail to decrypt");

    /* Negative: the wrong key must also fail. */
    uint8_t wrong_key[32];
    for (int i = 0; i < 32; i++) {
        wrong_key[i] = (uint8_t) (bkey[i] + 1);
    }
    CHECK(!setup_decrypt_bundle(wrong_key, blob, blob_len, scratch, sizeof(scratch), &scratch_len),
          "the wrong key must fail to decrypt");
}

int main(void)
{
    vector_t v;
    if (!load_vector(VECTORS_PATH, &v)) {
        return 1;
    }
    printf("loaded 1 vector from %s\n", VECTORS_PATH);

    test_parse_rejects_malformed();

    uint8_t bkey[32];
    test_parse_and_derive(&v, bkey);
    test_parse_rejects_typo(&v);
    test_decrypt_and_decode(&v, bkey);

    if (g_failures == 0) {
        printf("PASS: setup_parse_code/setup_derive/setup_decrypt_bundle, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

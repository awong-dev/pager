/* auth.c — see auth.h.
 *
 * Authority: docs/PROTOCOL.md §14, docs/DEVICE_PLAN.md §2.4/§2.5/§2.7.
 */
#include "auth.h"

#include <string.h>

/* ---------------------------------------------------------------------
 * HMAC-SHA256. mbedTLS on-device (ESP-IDF always defines ESP_PLATFORM);
 * a small self-contained fallback otherwise, so firmware/host/Makefile
 * needs no external crypto library (docs/DEVICE_TASKS.md Track F preamble:
 * "no ESP-IDF needed"). Both compute the same standard, well-defined
 * HMAC-SHA256 function — see auth.h's header note.
 * --------------------------------------------------------------------- */
#ifdef ESP_PLATFORM

#include "mbedtls/md.h"

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                         uint8_t out[32] /* full SHA-256 digest; caller truncates to AUTH_TAG_LEN */)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    /* Fixed inputs (SHA-256, 32-byte key/message buffers this module owns);
     * mbedtls_md_hmac() cannot fail for a valid info pointer, so the return
     * value is intentionally not propagated further than this file. */
    mbedtls_md_hmac(info, key, key_len, msg, msg_len, out);
}

#else /* host build: no ESP-IDF/mbedTLS available */

/* Compact public-domain-style SHA-256 (FIPS 180-4), host-test-only path. */

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t buf[64];
    size_t buflen;
} sha256_ctx_t;

static const uint32_t k_sha256_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void sha256_transform(sha256_ctx_t *ctx, const uint8_t data[64])
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t) data[i * 4] << 24) | ((uint32_t) data[i * 4 + 1] << 16) |
               ((uint32_t) data[i * 4 + 2] << 8) | ((uint32_t) data[i * 4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i - 15], 7) ^ rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = rotr32(w[i - 2], 17) ^ rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1], c = ctx->state[2], d = ctx->state[3];
    uint32_t e = ctx->state[4], f = ctx->state[5], g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + k_sha256_k[i] + w[i];
        uint32_t S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f; ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_init(sha256_ctx_t *ctx)
{
    ctx->state[0] = 0x6a09e667; ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372; ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f; ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab; ctx->state[7] = 0x5be0cd19;
    ctx->bitlen = 0;
    ctx->buflen = 0;
}

static void sha256_update(sha256_ctx_t *ctx, const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        ctx->buf[ctx->buflen++] = data[i];
        if (ctx->buflen == 64) {
            sha256_transform(ctx, ctx->buf);
            ctx->bitlen += 512;
            ctx->buflen = 0;
        }
    }
}

static void sha256_final(sha256_ctx_t *ctx, uint8_t out[32])
{
    size_t i = ctx->buflen;
    ctx->bitlen += (uint64_t) ctx->buflen * 8;

    ctx->buf[i++] = 0x80;
    if (i > 56) {
        while (i < 64) {
            ctx->buf[i++] = 0;
        }
        sha256_transform(ctx, ctx->buf);
        i = 0;
    }
    while (i < 56) {
        ctx->buf[i++] = 0;
    }
    for (int j = 7; j >= 0; j--) {
        ctx->buf[56 + (7 - j)] = (uint8_t) (ctx->bitlen >> (j * 8));
    }
    sha256_transform(ctx, ctx->buf);

    for (i = 0; i < 4; i++) {
        for (int j = 0; j < 8; j++) {
            out[j * 4 + i] = (uint8_t) (ctx->state[j] >> (24 - (int) i * 8));
        }
    }
}

static void hmac_sha256(const uint8_t *key, size_t key_len, const uint8_t *msg, size_t msg_len,
                         uint8_t out[32])
{
    uint8_t k0[64];
    memset(k0, 0, sizeof(k0));
    if (key_len > sizeof(k0)) {
        sha256_ctx_t kctx;
        sha256_init(&kctx);
        sha256_update(&kctx, key, key_len);
        sha256_final(&kctx, k0);
    } else {
        memcpy(k0, key, key_len);
    }

    uint8_t ipad[64], opad[64];
    for (size_t i = 0; i < sizeof(k0); i++) {
        ipad[i] = (uint8_t) (k0[i] ^ 0x36);
        opad[i] = (uint8_t) (k0[i] ^ 0x5c);
    }

    uint8_t inner[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, ipad, sizeof(ipad));
    sha256_update(&ctx, msg, msg_len);
    sha256_final(&ctx, inner);

    sha256_init(&ctx);
    sha256_update(&ctx, opad, sizeof(opad));
    sha256_update(&ctx, inner, sizeof(inner));
    sha256_final(&ctx, out);
}

#endif /* ESP_PLATFORM */

/* ---------------------------------------------------------------------
 * Key setup.
 * --------------------------------------------------------------------- */

static uint8_t s_kdev[AUTH_KDEV_LEN];
static bool s_kdev_set = false;

void auth_init(const uint8_t kdev[AUTH_KDEV_LEN])
{
    memcpy(s_kdev, kdev, sizeof(s_kdev));
    s_kdev_set = true;
}

/* HMAC-SHA256(K_dev, topic || 0x00 || p[0..p_len)) truncated to
 * AUTH_TAG_LEN bytes, into `out` (caller-owned, >= AUTH_TAG_LEN bytes). */
static void compute_tag(const char *topic, const uint8_t *p, size_t p_len, uint8_t out[AUTH_TAG_LEN])
{
    /* 256 bytes of topic + one 0x00 separator comfortably covers
     * PROTOCOL.md §1's device-id-bounded topic strings
     * ("pager/<=24 chars>/status" etc.); msg is hashed in two calls instead
     * of concatenated into a second buffer to avoid a second copy of `p`
     * (which can be up to the 640-byte envelope limit, PROTOCOL.md §3.3). */
    size_t topic_len = strlen(topic);
    uint8_t prefix[256 + 1];
    if (topic_len >= sizeof(prefix)) {
        topic_len = sizeof(prefix) - 1; /* defensive clamp; never expected to trigger */
    }
    memcpy(prefix, topic, topic_len);
    prefix[topic_len] = 0x00;

    /* hmac_sha256() takes one contiguous message, so build topic||0x00||p
     * into a scratch buffer sized for the largest signed envelope
     * (PROTOCOL.md §3.3's 640-byte limit) plus the topic prefix above. */
    static uint8_t scratch[256 + 1 + 640];
    size_t total = topic_len + 1;
    if (total > sizeof(scratch)) {
        memset(out, 0, AUTH_TAG_LEN); /* unreachable in practice; never matches a real tag */
        return;
    }
    memcpy(scratch, prefix, total);
    size_t copy_len = p_len;
    if (total + copy_len > sizeof(scratch)) {
        copy_len = sizeof(scratch) - total; /* defensive clamp; never expected to trigger */
    }
    memcpy(scratch + total, p, copy_len);

    uint8_t full[32];
    hmac_sha256(s_kdev, sizeof(s_kdev), scratch, total + copy_len, full);
    memcpy(out, full, AUTH_TAG_LEN);
}

/* Constant-time compare, so a mismatch takes the same time regardless of
 * where the first differing byte falls (docs/PROTOCOL.md §14.3/§14.4:
 * "MUST verify in constant time"). */
static bool tag_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff = (uint8_t) (diff | (a[i] ^ b[i]));
    }
    return diff == 0;
}

/* ---------------------------------------------------------------------
 * Sign / verify.
 * --------------------------------------------------------------------- */

bool auth_sign(const char *topic, uint8_t *buf, size_t *len, size_t cap)
{
    if (!s_kdev_set) {
        return false;
    }
    size_t p_len = *len;
    if (p_len + AUTH_SIG_SUFFIX_LEN > cap) {
        return false;
    }

    uint8_t tag[AUTH_TAG_LEN];
    compute_tag(topic, buf, p_len, tag);

    buf[p_len] = 0x0D;                 /* CBOR key 13 (sig), unsigned int, value 13 */
    buf[p_len + 1] = 0x40 | AUTH_TAG_LEN; /* CBOR bstr header, length 8 */
    memcpy(buf + p_len + 2, tag, AUTH_TAG_LEN);

    *len = p_len + AUTH_SIG_SUFFIX_LEN;
    return true;
}

bool auth_verify(const char *topic, uint8_t *buf, size_t *len)
{
    if (!s_kdev_set) {
        return false;
    }
    size_t total = *len;
    if (total < AUTH_SIG_SUFFIX_LEN) {
        return false;
    }
    size_t p_len = total - AUTH_SIG_SUFFIX_LEN;

    if (buf[p_len] != 0x0D || buf[p_len + 1] != (uint8_t) (0x40 | AUTH_TAG_LEN)) {
        return false;
    }

    uint8_t expected[AUTH_TAG_LEN];
    compute_tag(topic, buf, p_len, expected);

    if (!tag_equal(buf + p_len + 2, expected, AUTH_TAG_LEN)) {
        return false;
    }

    *len = p_len;
    return true;
}

/* ---------------------------------------------------------------------
 * Replay counters.
 * --------------------------------------------------------------------- */

uint32_t auth_next_up_n(auth_rtc_t *rtc, uint16_t epoch, bool *wrapped)
{
    uint32_t lo = rtc->up_lo & AUTH_UP_LO_MASK;
    uint32_t n = (((uint32_t) (epoch & AUTH_UP_EPOCH_MASK)) << AUTH_UP_LO_BITS) | lo;

    uint32_t next_lo = (lo + 1u) & AUTH_UP_LO_MASK;
    bool did_wrap = (next_lo == 0u); /* wrapped past AUTH_UP_LO_MASK back to 0 */
    rtc->up_lo = next_lo;

    if (wrapped) {
        *wrapped = did_wrap;
    }
    return n;
}

bool auth_accept_down_n(auth_rtc_t *rtc, uint32_t n)
{
    if (n == 0) {
        return false; /* the relay never issues n=0, see header note */
    }

    if (n > rtc->down_n) {
        uint32_t shift = n - rtc->down_n;
        uint32_t new_bits;
        if (shift >= AUTH_DOWN_WINDOW) {
            /* Every previously-seen value is now more than a window behind
             * `n`; shifting a 32-bit value by >= 32 is undefined behaviour
             * in C, so this is the explicit equivalent of what a wider
             * (e.g. 64-bit-window) shift-then-mask would produce anyway.
             * Mirrors relay/app/store/device_secrets.py's accept_up_n(),
             * including its harmless quirk of also marking the initial
             * down_n=0 sentinel as "seen" on the very first accepted
             * value — n=0 is never actually issued (see the n==0 check
             * above), so that bit is never read back as a real replay. */
            new_bits = 0;
        } else {
            new_bits = (uint32_t) ((rtc->down_bits << shift) | (1u << (shift - 1u)));
        }
        rtc->down_n = n;
        rtc->down_bits = new_bits;
        return true;
    }

    uint32_t gap = rtc->down_n - n;
    if (gap > 0 && gap <= AUTH_DOWN_WINDOW) {
        uint32_t bit = 1u << (gap - 1u);
        if (rtc->down_bits & bit) {
            return false; /* replay */
        }
        rtc->down_bits |= bit;
        return true;
    }

    return false; /* too far behind the window */
}

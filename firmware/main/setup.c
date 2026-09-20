/* setup.c — see setup.h.
 *
 * Split in two, deliberately: the top section (Crockford/Luhn decode, HKDF
 * derivation, AES-256-GCM decrypt) has no ESP-IDF dependency at all — just
 * <string.h>/<stdint.h> and mbedtls's hkdf.h/gcm.h/md.h, which
 * firmware/host/Makefile links against the *real* mbedtls (via Homebrew's
 * `mbedtls@3` / a system `libmbedcrypto`, see the Makefile's comment) rather
 * than a hand-rolled fallback the way auth.c's host-only SHA-256 is: hand-
 * writing AES-256-GCM correctly is a much larger and higher-risk undertaking
 * than HMAC-SHA256, and both ESP-IDF and the host link the same
 * well-tested mbedtls implementation of the two primitives this module
 * needs (`mbedtls_hkdf`, `mbedtls_gcm_*`), so nothing about the crypto
 * itself goes untested by using it on both sides.
 *
 * The bottom section (`#ifdef ESP_PLATFORM`) is setup_run() and everything
 * only it needs: ident.c/cbor.c (CBOR-decode + `ident_store()`), net.h (the
 * modem), ui.h (the minimal toast prompt) and esp_console glue registered
 * from main.c. None of that exists on the host build, and setup_run() would
 * have nothing to talk to there anyway (no modem, no display).
 */
#include "setup.h"

#include <string.h>

#include "mbedtls/gcm.h"
#include "mbedtls/hkdf.h"
#include "mbedtls/md.h"

/* ---------------------------------------------------------------------
 * Crockford base32 + Luhn-mod-32 check character, matching
 * relay/app/devsetup.py's `_ALPHABET`/`_VALUE_OF`/`_luhn_mod_n_validate`/
 * `_decode_token` byte-for-byte (docs/DEVICE_PLAN.md §3.1/§3.2).
 * --------------------------------------------------------------------- */

#define SETUP_TOKEN_CHARS 13         /* data characters */
#define SETUP_CODE_CHARS (SETUP_TOKEN_CHARS + 1) /* + 1 check character */

/* "0123456789ABCDEFGHJKMNPQRSTVWXYZ" (skips I, L, O, U), case-insensitive;
 * O->0, I->1, L->1 are accepted as common handwriting/typo stand-ins (same
 * as the relay), U has none and is rejected. Returns -1 for anything else. */
static int crockford_value(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char) (c - 'a' + 'A');
    }
    switch (c) {
    case '0': case 'O': return 0;
    case '1': case 'I': case 'L': return 1;
    case '2': return 2;
    case '3': return 3;
    case '4': return 4;
    case '5': return 5;
    case '6': return 6;
    case '7': return 7;
    case '8': return 8;
    case '9': return 9;
    case 'A': return 10;
    case 'B': return 11;
    case 'C': return 12;
    case 'D': return 13;
    case 'E': return 14;
    case 'F': return 15;
    case 'G': return 16;
    case 'H': return 17;
    case 'J': return 18;
    case 'K': return 19;
    case 'M': return 20;
    case 'N': return 21;
    case 'P': return 22;
    case 'Q': return 23;
    case 'R': return 24;
    case 'S': return 25;
    case 'T': return 26;
    case 'V': return 27;
    case 'W': return 28;
    case 'X': return 29;
    case 'Y': return 30;
    case 'Z': return 31;
    default: return -1;
    }
}

/* Luhn mod N (N=32), reversed-order, matching
 * relay/app/devsetup.py:_luhn_mod_n_validate exactly. `values` holds all
 * SETUP_CODE_CHARS (13 data + 1 check). */
static bool luhn_mod32_valid(const int values[SETUP_CODE_CHARS])
{
    int factor = 1;
    int total = 0;
    for (int i = SETUP_CODE_CHARS - 1; i >= 0; i--) {
        int addend = factor * values[i];
        factor = (factor == 2) ? 1 : 2;
        addend = (addend / 32) + (addend % 32);
        total += addend;
    }
    return (total % 32) == 0;
}

/* Inverse of relay/app/devsetup.py:_encode_token — 13 data values (65 bits:
 * 64 token bits + 1 trailing zero pad bit) -> 8 raw token bytes. Done in
 * fixed 64-bit arithmetic without ever materialising the 65-bit
 * intermediate: the first 12 values (60 bits) accumulate directly into a
 * uint64_t (`acc`); the 13th value's top 4 bits (its bottom bit is always
 * the padding bit being dropped) become the low 4 bits of the final 64-bit
 * value — algebraically identical to "accumulate all 65 bits then shift
 * right by 1", just without needing more than 64 bits of storage at any
 * point. */
static void decode_token_values(const int values[SETUP_TOKEN_CHARS], uint8_t token_out[SETUP_TOKEN_LEN])
{
    uint64_t acc = 0;
    for (int i = 0; i < SETUP_TOKEN_CHARS - 1; i++) {
        acc = (acc << 5) | (uint32_t) values[i];
    }
    int last = values[SETUP_TOKEN_CHARS - 1];
    uint64_t n = (acc << 4) | (uint32_t) (last >> 1);
    for (int i = 0; i < SETUP_TOKEN_LEN; i++) {
        token_out[i] = (uint8_t) (n >> (8 * (SETUP_TOKEN_LEN - 1 - i)));
    }
}

/* ---------------------------------------------------------------------
 * setup_parse_code()
 * --------------------------------------------------------------------- */

bool setup_parse_code(const char *code, setup_code_t *out)
{
    if (!code || !out) {
        return false;
    }

    const char *at = strchr(code, '@');
    if (!at) {
        return false;
    }

    /* Token portion: everything before '@', hyphens/spaces ignored,
     * case-insensitive (relay/app/devsetup.py:parse()). */
    char token_str[SETUP_CODE_CHARS + 1];
    size_t tn = 0;
    for (const char *p = code; p < at; p++) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '-') {
            continue;
        }
        if (tn >= SETUP_CODE_CHARS) {
            return false; /* too long */
        }
        token_str[tn++] = c;
    }
    if (tn != SETUP_CODE_CHARS) {
        return false; /* too short */
    }
    token_str[tn] = '\0';

    int values[SETUP_CODE_CHARS];
    for (size_t i = 0; i < SETUP_CODE_CHARS; i++) {
        int v = crockford_value(token_str[i]);
        if (v < 0) {
            return false;
        }
        values[i] = v;
    }
    if (!luhn_mod32_valid(values)) {
        return false; /* check character mismatch: a typo */
    }

    uint8_t token_tmp[SETUP_TOKEN_LEN];
    decode_token_values(values, token_tmp); /* reads values[0..SETUP_TOKEN_CHARS-1] */

    /* Host[:port][;apn=...] portion, verbatim apart from surrounding
     * whitespace (a broker hostname legitimately contains hyphens/dots). */
    const char *rest = at + 1;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    size_t rest_len = strlen(rest);
    while (rest_len > 0 &&
           (rest[rest_len - 1] == ' ' || rest[rest_len - 1] == '\t' || rest[rest_len - 1] == '\r' ||
            rest[rest_len - 1] == '\n')) {
        rest_len--;
    }
    if (rest_len == 0) {
        return false; /* missing host */
    }

    char rest_buf[SETUP_HOST_MAX + SETUP_APN_MAX + 16];
    if (rest_len >= sizeof(rest_buf)) {
        return false;
    }
    memcpy(rest_buf, rest, rest_len);
    rest_buf[rest_len] = '\0';

    char apn_buf[SETUP_APN_MAX];
    apn_buf[0] = '\0';
    char *apn_sep = strstr(rest_buf, ";apn=");
    if (apn_sep) {
        *apn_sep = '\0';
        const char *apn_val = apn_sep + 5;
        size_t apn_len = strlen(apn_val);
        if (apn_len >= sizeof(apn_buf)) {
            return false;
        }
        memcpy(apn_buf, apn_val, apn_len + 1);
    }

    uint16_t port = 8883; /* DEVICE_PLAN.md §3.1: "Port defaults to 8883." */
    char *colon = strrchr(rest_buf, ':');
    if (colon) {
        *colon = '\0';
        const char *port_str = colon + 1;
        if (*port_str == '\0') {
            return false;
        }
        long p = 0;
        for (const char *d = port_str; *d; d++) {
            if (*d < '0' || *d > '9') {
                return false;
            }
            p = p * 10 + (*d - '0');
            if (p > 65535) {
                return false;
            }
        }
        port = (uint16_t) p;
    }

    size_t hlen = strlen(rest_buf);
    while (hlen > 0 && (rest_buf[hlen - 1] == ' ' || rest_buf[hlen - 1] == '\t')) {
        rest_buf[--hlen] = '\0';
    }
    if (hlen == 0 || hlen >= sizeof(out->host)) {
        return false;
    }

    memset(out, 0, sizeof(*out));
    memcpy(out->token, token_tmp, SETUP_TOKEN_LEN);
    memcpy(out->host, rest_buf, hlen + 1);
    out->port = port;
    memcpy(out->apn, apn_buf, strlen(apn_buf) + 1);
    return true;
}

/* ---------------------------------------------------------------------
 * setup_derive() — HKDF-SHA256, one call per label
 * (docs/DEVICE_PLAN.md §3.2 / relay/app/devsetup.py:derive()).
 * --------------------------------------------------------------------- */

static void hex_encode(const uint8_t *in, size_t n, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = digits[in[i] >> 4];
        out[2 * i + 1] = digits[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
}

/* Standard base64 with the URL-safe alphabet (- and _ instead of + and /),
 * WITH padding — matching relay/app/devsetup.py:_b64url(), which is plain
 * `base64.urlsafe_b64encode` (padded). */
static void base64url_encode(const uint8_t *in, size_t n, char *out)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8) | in[i + 2];
        out[o++] = alphabet[(v >> 18) & 0x3F];
        out[o++] = alphabet[(v >> 12) & 0x3F];
        out[o++] = alphabet[(v >> 6) & 0x3F];
        out[o++] = alphabet[v & 0x3F];
    }
    size_t rem = n - i;
    if (rem == 1) {
        uint32_t v = ((uint32_t) in[i]) << 16;
        out[o++] = alphabet[(v >> 18) & 0x3F];
        out[o++] = alphabet[(v >> 12) & 0x3F];
        out[o++] = '=';
        out[o++] = '=';
    } else if (rem == 2) {
        uint32_t v = ((uint32_t) in[i] << 16) | ((uint32_t) in[i + 1] << 8);
        out[o++] = alphabet[(v >> 18) & 0x3F];
        out[o++] = alphabet[(v >> 12) & 0x3F];
        out[o++] = alphabet[(v >> 6) & 0x3F];
        out[o++] = '=';
    }
    out[o] = '\0';
}

void setup_derive(const uint8_t token[SETUP_TOKEN_LEN], char bid_out[SETUP_BID_LEN],
                  char bpw_out[SETUP_BPW_LEN], uint8_t bkey_out[32])
{
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

    /* salt=NULL/0: RFC 5869 + mbedtls_hkdf's own documented behaviour both
     * substitute a hash-length all-zero salt, the same default the relay's
     * `cryptography` HKDF(salt=None, ...) uses (verified against
     * tools/setup_code_vectors.json's vector during development). */
    uint8_t okm_id[32];
    mbedtls_hkdf(md, NULL, 0, token, SETUP_TOKEN_LEN, (const unsigned char *) "id", 2, okm_id,
                sizeof(okm_id));
    hex_encode(okm_id, 6, bid_out); /* bid = hex(...)[:12] = first 6 bytes */

    uint8_t okm_pw[32];
    mbedtls_hkdf(md, NULL, 0, token, SETUP_TOKEN_LEN, (const unsigned char *) "pw", 2, okm_pw,
                sizeof(okm_pw));
    base64url_encode(okm_pw, sizeof(okm_pw), bpw_out);

    mbedtls_hkdf(md, NULL, 0, token, SETUP_TOKEN_LEN, (const unsigned char *) "bundle", 6, bkey_out,
                32);
}

/* ---------------------------------------------------------------------
 * setup_decrypt_bundle() — AES-256-GCM, docs/DEVICE_PLAN.md §3.2 step 4:
 * blob = nonce(12) || ciphertext || tag(16), no additional authenticated
 * data.
 * --------------------------------------------------------------------- */

#define SETUP_GCM_NONCE_LEN 12
#define SETUP_GCM_TAG_LEN 16

bool setup_decrypt_bundle(const uint8_t bkey[32], const uint8_t *blob, size_t blob_len,
                          uint8_t *plain_out, size_t plain_cap, size_t *plain_len)
{
    if (!bkey || !blob || !plain_out || !plain_len) {
        return false;
    }
    if (blob_len < SETUP_GCM_NONCE_LEN + SETUP_GCM_TAG_LEN) {
        return false;
    }
    size_t ct_len = blob_len - SETUP_GCM_NONCE_LEN - SETUP_GCM_TAG_LEN;
    if (ct_len > plain_cap) {
        return false;
    }

    const uint8_t *nonce = blob;
    const uint8_t *ct = blob + SETUP_GCM_NONCE_LEN;
    const uint8_t *tag = blob + SETUP_GCM_NONCE_LEN + ct_len;

    mbedtls_gcm_context ctx;
    mbedtls_gcm_init(&ctx);
    bool ok = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, bkey, 256) == 0;
    if (ok) {
        ok = mbedtls_gcm_auth_decrypt(&ctx, ct_len, nonce, SETUP_GCM_NONCE_LEN, NULL, 0, tag,
                                      SETUP_GCM_TAG_LEN, ct, plain_out) == 0;
    }
    mbedtls_gcm_free(&ctx);
    if (!ok) {
        return false; /* wrong key or corrupted/tampered ciphertext */
    }
    *plain_len = ct_len;
    return true;
}

/* ======================================================================
 * Everything below needs the modem, NVS and the display — device only.
 * ====================================================================== */
#ifdef ESP_PLATFORM

#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"

#include "cbor.h"
#include "ident.h"
#include "net.h"
#include "ui.h"

static const char *TAG = "setup";

/* PROTOCOL.md §2: pager/boot/{bid}/... 4 kB cap, matching net.cpp's
 * PAGER_BOOT_MAX_PAYLOAD (duplicated here rather than exported through
 * net.h, since it is a wire-protocol constant this module needs on its own
 * terms, not a net.c internal). */
#define SETUP_BUNDLE_MAX 4096

/* docs/PROTOCOL.md §10 boot keymap: 0=v, 1=id, 30=pw, 31=k, 32=host,
 * 33=port, 34=ca, 35=flags, 36=label, 37=apn. */
#define BOOT_KEY_V 0
#define BOOT_KEY_ID 1
#define BOOT_KEY_PW 30
#define BOOT_KEY_K 31
#define BOOT_KEY_HOST 32
#define BOOT_KEY_PORT 33
#define BOOT_KEY_CA 34
#define BOOT_KEY_FLAGS 35
#define BOOT_KEY_LABEL 36
#define BOOT_KEY_APN 37

/* Timeouts: vTaskDelay-based polling (no busy-wait), matching net_init()'s
 * own 1s-poll attach loop style. */
#define SETUP_POLL_MS 200
#define SETUP_CONNECT_TIMEOUT_MS 30000 /* TLS handshake + CONNECTED + SUBSCRIBED */
#define SETUP_BUNDLE_TIMEOUT_MS 15000  /* the retained message "arrives immediately" once subscribed */
#define SETUP_ACK_SETTLE_MS 500        /* best-effort: give the QoS1 PUBACK a moment before disconnecting */

/* docs/PROTOCOL.md §1 dev_id regex, duplicated from ident.c's own (private,
 * non-exported) valid_dev_id() — ident.h has no exported validator and
 * ident.c is not in this task's `Files` list, so this small, stable regex
 * is kept in sync by hand rather than by a shared symbol. */
static bool valid_dev_id(const char *s, size_t len)
{
    if (len < 3 || len > 24) {
        return false;
    }
    char c0 = s[0];
    if (!((c0 >= 'a' && c0 <= 'z') || (c0 >= '0' && c0 <= '9'))) {
        return false;
    }
    for (size_t i = 1; i < len; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) {
            return false;
        }
    }
    return true;
}

/* CBOR-decode + validate the plaintext bundle into an ident_t. Every
 * required field (docs/DEVICE_PLAN.md §3.2 step 4's plaintext shape) must be
 * present and within ident.h's own field sizes; `apn`/`flags` are optional
 * (default empty/0). Computes `ca_hash` (SHA-256 of the CA PEM) for
 * ident.h's own field, which net.cpp's net_init() uses on later boots to
 * skip a redundant NVRAM rewrite (DEVICE_PLAN.md §3.3). */
static bool decode_and_validate_bundle(const uint8_t *plain, size_t plain_len, ident_t *out)
{
    memset(out, 0, sizeof(*out));

    cbor_r_t r;
    cbor_r_init(&r, plain, plain_len);
    uint32_t count;
    if (!cbor_r_map(&r, &count)) {
        return false;
    }

    bool have_id = false, have_pw = false, have_k = false, have_host = false, have_port = false,
         have_ca = false, have_label = false;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t key;
        if (!cbor_r_key(&r, &key)) {
            return false;
        }
        switch (key) {
        case BOOT_KEY_V: {
            uint64_t v;
            if (!cbor_r_uint(&r, &v)) {
                return false;
            }
            break;
        }
        case BOOT_KEY_ID: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(out->dev_id) ||
                !valid_dev_id(s, slen)) {
                return false;
            }
            memcpy(out->dev_id, s, slen);
            out->dev_id[slen] = '\0';
            have_id = true;
            break;
        }
        case BOOT_KEY_PW: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(out->mqtt_pw)) {
                return false;
            }
            memcpy(out->mqtt_pw, s, slen);
            out->mqtt_pw[slen] = '\0';
            have_pw = true;
            break;
        }
        case BOOT_KEY_K: {
            const uint8_t *b;
            size_t blen;
            if (!cbor_r_bstr(&r, &b, &blen) || blen != sizeof(out->kdev)) {
                return false;
            }
            memcpy(out->kdev, b, blen);
            have_k = true;
            break;
        }
        case BOOT_KEY_HOST: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen == 0 || slen >= sizeof(out->host)) {
                return false;
            }
            memcpy(out->host, s, slen);
            out->host[slen] = '\0';
            have_host = true;
            break;
        }
        case BOOT_KEY_PORT: {
            uint64_t v;
            if (!cbor_r_uint(&r, &v) || v == 0 || v > 65535) {
                return false;
            }
            out->port = (uint16_t) v;
            have_port = true;
            break;
        }
        case BOOT_KEY_CA: {
            const char *s;
            size_t slen;
            /* Empty is legal: the relay sends "" when it pins no CA, and the
             * production session then runs with validation off (net_init()). */
            if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(out->ca)) {
                return false;
            }
            memcpy(out->ca, s, slen);
            out->ca[slen] = '\0';
            out->ca_len = slen;
            have_ca = true;
            break;
        }
        case BOOT_KEY_FLAGS: {
            uint64_t v;
            if (!cbor_r_uint(&r, &v)) {
                return false;
            }
            out->flags = (uint32_t) v;
            break;
        }
        case BOOT_KEY_LABEL: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(out->label)) {
                return false;
            }
            memcpy(out->label, s, slen);
            out->label[slen] = '\0';
            have_label = true;
            break;
        }
        case BOOT_KEY_APN: {
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(out->apn)) {
                return false;
            }
            memcpy(out->apn, s, slen);
            out->apn[slen] = '\0';
            break;
        }
        default:
            if (!cbor_r_skip(&r)) {
                return false;
            }
            break;
        }
    }

    /* `ca` is optional (absent == empty == "pin nothing"); see BOOT_KEY_CA. */
    (void) have_ca;
    if (!(have_id && have_pw && have_k && have_host && have_port && have_label)) {
        return false;
    }

    mbedtls_sha256((const unsigned char *) out->ca, out->ca_len, out->ca_hash, 0);
    out->n_epoch = 0;
    out->claimed = 0;
    return true;
}

/* Message reception: single static slot, filled by the MQTT event task via
 * net_set_msg_cb(), drained by setup_run()'s own poll loop on its own task.
 * No lock: only ever one bootstrap fetch in flight, and setup_run() does not
 * read s_bundle_buf/s_bundle_len until it has observed s_msg_received==true,
 * which the callback sets last. */
static uint8_t s_bundle_buf[SETUP_BUNDLE_MAX];
static volatile size_t s_bundle_len = 0;
static volatile bool s_msg_received = false;

static void bundle_msg_cb(const char *topic, const char *body, uint16_t len)
{
    (void) topic; /* only one subscription is ever active during setup */
    if (s_msg_received || len > sizeof(s_bundle_buf)) {
        return;
    }
    memcpy(s_bundle_buf, body, len);
    s_bundle_len = len;
    s_msg_received = true;
}

/* Logs + best-effort toasts one of the four docs/DEVICE_PLAN.md §3.2 step 5
 * error strings; always returns false so call sites can `return
 * fail(...)`. */
static bool fail(bool ui_ok, const char *msg)
{
    ESP_LOGI(TAG, "SETUP error: %s", msg);
    if (ui_ok) {
        ui_show_toast(msg);
    }
    return false;
}

bool setup_run(const char *code)
{
    bool ui_ok = ui_init(); /* best-effort; §5.1/ui.h: no pager function may be gated on the display */
    if (ui_ok) {
        ui_show_toast("Setting up...");
    }

    setup_code_t sc;
    if (!setup_parse_code(code, &sc)) {
        return fail(ui_ok, "code damaged");
    }

    char bid[SETUP_BID_LEN];
    char bpw[SETUP_BPW_LEN];
    uint8_t bkey[32];
    setup_derive(sc.token, bid, bpw, bkey);

    char client_id[6 + SETUP_BID_LEN];
    snprintf(client_id, sizeof(client_id), "boot-%s", bid);
    char down_topic[32];
    snprintf(down_topic, sizeof(down_topic), "pager/boot/%s/down", bid);
    char up_topic[32];
    snprintf(up_topic, sizeof(up_topic), "pager/boot/%s/up", bid);

    // Power effect: modem leaves reset and attaches LTE-M (DEVICE_PLAN.md
    // §3.2 step 2); see net_bootstrap_attach()'s own doc comment.
    if (!net_bootstrap_attach(sc.apn)) {
        return fail(ui_ok, "no network");
    }
    ESP_LOGI(TAG, "SETUP network");
    if (ui_ok) {
        ui_show_toast("SETUP network");
    }

    // Power effect: one AT command (TLS profile config), no RRC of its own.
    if (!net_tls_profile_bootstrap()) {
        return fail(ui_ok, "cannot reach broker");
    }

    net_set_msg_cb(bundle_msg_cb);
    s_msg_received = false;
    s_bundle_len = 0;

    // Power effect: one TLS handshake (~5 kB, VALIDATION_NONE) plus the RRC
    // time it takes; see net_bootstrap_connect()'s own doc comment.
    if (!net_bootstrap_connect(client_id, bpw, sc.host, sc.port, down_topic)) {
        return fail(ui_ok, "cannot reach broker");
    }

    bool connected = false;
    for (int waited_ms = 0; waited_ms < SETUP_CONNECT_TIMEOUT_MS; waited_ms += SETUP_POLL_MS) {
        net_mqtt_status_t st;
        net_get_mqtt_status(&st);
        if (st.mqtt_connected) {
            connected = true;
            break;
        }
        if (st.disconnect_edge) {
            break; /* CONNECT/SUBSCRIBE refused or lost before it ever came up */
        }
        vTaskDelay(pdMS_TO_TICKS(SETUP_POLL_MS));
    }
    if (!connected) {
        net_session_down();
        return fail(ui_ok, "cannot reach broker");
    }
    ESP_LOGI(TAG, "SETUP broker");
    if (ui_ok) {
        ui_show_toast("SETUP broker");
    }

    bool got_bundle = false;
    for (int waited_ms = 0; waited_ms < SETUP_BUNDLE_TIMEOUT_MS; waited_ms += SETUP_POLL_MS) {
        if (s_msg_received) {
            got_bundle = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(SETUP_POLL_MS));
    }
    if (!got_bundle) {
        net_session_down();
        return fail(ui_ok, "no setup code waiting"); /* expired, or never issued for this token */
    }

    static uint8_t plain[SETUP_BUNDLE_MAX];
    size_t plain_len = 0;
    /* Diagnostic: the three failures below all surface as "code damaged";
     * the length tells a truncated receive apart from a wrong key. */
    ESP_LOGI(TAG, "bundle received: %u bytes", (unsigned) s_bundle_len);
    if (!setup_decrypt_bundle(bkey, s_bundle_buf, s_bundle_len, plain, sizeof(plain), &plain_len)) {
        net_session_down();
        ESP_LOGI(TAG, "bundle AES-GCM decrypt failed (bad tag: wrong key or corrupted bytes)");
        return fail(ui_ok, "code damaged"); /* bad tag: wrong key or corrupted bundle */
    }

    static ident_t id; /* static: 4 kB+ struct, keep it off the task stack (see ident_load()) */
    if (!decode_and_validate_bundle(plain, plain_len, &id)) {
        net_session_down();
        ESP_LOGI(TAG, "bundle decrypted (%u bytes) but CBOR decode/validate failed",
                 (unsigned) plain_len);
        return fail(ui_ok, "code damaged");
    }

    if (!ident_store(&id)) {
        net_session_down();
        ESP_LOGI(TAG, "bundle valid but ident_store() (NVS write) failed");
        return fail(ui_ok, "code damaged"); /* NVS write failure; no better bucket among the four */
    }

    // Power effect: one NVRAM write on the modem's own storage, no RRC; see
    // net_write_ca()'s own doc comment.
    /* No CA in the bundle: nothing to write, net_init() runs unpinned. */
    if (id.ca[0] != '\0' && !net_write_ca(id.ca)) {
        net_session_down();
        return fail(ui_ok, "cannot reach broker"); /* modem-side failure, same session as above */
    }
    ESP_LOGI(TAG, "SETUP bundle");
    if (ui_ok) {
        ui_show_toast("SETUP bundle");
    }

    uint8_t ack[16];
    cbor_w_t w;
    cbor_w_init(&w, ack, sizeof(ack));
    cbor_w_map(&w, 2);
    cbor_w_uint(&w, 0, 1);  /* v=1 */
    cbor_w_uint(&w, 29, 1); /* ok=1 */
    if (!w.err) {
        // Power effect: the RRC time for one small publish if the modem was
        // idle; ~0 extra otherwise (same class as net_publish_raw()).
        net_publish_raw(up_topic, ack, (uint16_t) w.len, 1);
    }
    /* Best-effort: give the QoS1 PUBACK a moment before disconnecting. No
     * PUBACK wait/poll exists on this one-shot bootstrap path (unlike
     * modes.c's production pending-ack bookkeeping) — a single bounded delay
     * is the documented trade-off, not a retry loop. */
    vTaskDelay(pdMS_TO_TICKS(SETUP_ACK_SETTLE_MS));

    // Power effect: one AT command, ends the bootstrap TLS/MQTT session.
    net_session_down();
    ESP_LOGI(TAG, "SETUP done");
    if (ui_ok) {
        ui_show_toast("SETUP done");
    }

    // Power effect: full ESP32 + modem power cycle; the next boot's
    // ident_load() now succeeds and modes_boot()/modes_run() takes over.
    esp_restart();
    return true; /* unreachable */
}

#endif /* ESP_PLATFORM */

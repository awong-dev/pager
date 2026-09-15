/* setup.h — Setup mode: the typed setup code -> bootstrap fetch -> ident
 * flow, docs/DEVICE_TASKS.md F3.5.
 *
 * Authority: docs/DEVICE_PLAN.md §3.1 (the setup code), §3.2 (the bootstrap
 * fetch, device side), §3.7 (no SMS path here — §3.6 is not built).
 * docs/PROTOCOL.md §2 (bootstrap topics), §6.1 (bootstrap TLS profile), §10
 * (boot keymap, keys 0/1/29-37).
 *
 * A device with no valid `ident` (main.c's `ident_load()` returned false)
 * starts an esp_console REPL (over the USB serial UART) instead of the
 * normal `modes_boot()`/`modes_run()` path. `setup <code>` is the one
 * console command it registers, wired to setup_run() below. There is no
 * on-device text-entry UI for the code yet (F6 lands the real keyboard-
 * driven screens) — per DEVICE_TASKS.md F3.5's "keep the UI minimal here",
 * the e-paper panel (ui.c) only ever shows a static prompt and the same
 * network|broker|bundle|done/error words this module logs, never takes
 * keyboard input for the code itself.
 *
 * setup_run() ends every path either by calling esp_restart() (success) or
 * by returning false (failure) after logging exactly one of the four
 * DEVICE_PLAN.md §3.2 step 5 error strings — never both. It never returns
 * true (the success path never returns at all).
 *
 * No dynamic allocation: every buffer here is a caller-owned struct or one
 * of this module's own static scratch buffers, sized once, reused. This
 * module runs once per device lifetime under normal operation (or once per
 * rotation, §3.5) — before ident exists, before auth.c has a key, before
 * modes.c's RTC struct is meaningful — so it does not touch any of those.
 */
#ifndef SETUP_H
#define SETUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* DEVICE_PLAN.md §3.1/H11: 8-byte token. */
#define SETUP_TOKEN_LEN 8
/* §3.2: bid = hex(HKDF(token,"id"))[:12] -- 12 hex chars + NUL. */
#define SETUP_BID_LEN 13
/* §3.2: bpw = base64url(HKDF(token,"pw")), 32 bytes -> 44 chars (with
 * padding) + NUL. */
#define SETUP_BPW_LEN 45
/* Host/APN reuse ident.h's own sizing so a parsed setup_code_t always fits
 * what ident_store() will eventually accept. */
#define SETUP_HOST_MAX 128 /* matches IDENT_HOST_MAX */
#define SETUP_APN_MAX 32   /* matches IDENT_APN_MAX */

typedef struct {
    uint8_t token[SETUP_TOKEN_LEN];
    char host[SETUP_HOST_MAX];
    uint16_t port;
    char apn[SETUP_APN_MAX]; /* empty = carrier default */
} setup_code_t;

/* Parses a typed setup code (docs/DEVICE_PLAN.md §3.1): `<13 Crockford data
 * chars><1 check char>[-<...>...] @ host[:port][;apn=...]`. Hyphens and
 * whitespace inside the token portion (before `@`) are ignored; the token is
 * case-insensitive; `O`/`I`/`L` are accepted as `0`/`1`/`1` per Crockford's
 * own spec. Validates the Luhn-mod-32 check character (catches every
 * single-character substitution and adjacent transposition) over all 14
 * values, matching relay/app/devsetup.py's `_decode_token` exactly (the two
 * sides must agree byte-for-byte, or a valid code from the web app would
 * fail on the device).
 *
 * Port defaults to 8883 when absent. `apn` is left as an empty string when
 * the code carries no `;apn=...` suffix (carrier default, matching
 * ident.h's own "empty = carrier default" convention).
 *
 * Returns false (leaving `*out` unspecified) on any of: missing `@`, wrong
 * token length, an invalid Crockford character, a failed check character, a
 * missing/empty host, or a malformed port. Every one of these is the single
 * "code damaged" outcome DEVICE_PLAN.md §3.2 step 5 names — this function
 * does not distinguish them further; the caller (setup_run()) is the one
 * that logs/toasts the fixed string.
 *
 * No modem or radio access; no dynamic allocation. */
bool setup_parse_code(const char *code, setup_code_t *out);

/* Derives `bid` (hex, SETUP_BID_LEN incl. NUL), `bpw` (base64url with
 * padding, SETUP_BPW_LEN incl. NUL) and `bkey` (raw 32 bytes, the AES-256-GCM
 * key) from `token`, one `mbedtls_hkdf` call per label, matching
 * relay/app/devsetup.py's `derive()` exactly: `bid =
 * hex(HKDF(token,"id"))[:12]`, `bpw = base64url(HKDF(token,"pw"))`, `bkey =
 * HKDF(token,"bundle")` (docs/DEVICE_PLAN.md §3.2). `salt` is not supplied to
 * `mbedtls_hkdf` (NULL/0), which per RFC 5869 and mbedtls's own documented
 * behaviour is equivalent to a hash-length all-zero salt — the same default
 * the relay's `cryptography.hazmat...HKDF(salt=None, ...)` uses.
 *
 * No modem or radio access; no dynamic allocation. Cannot fail (mbedtls_hkdf
 * only rejects a requested output length > 255*32 bytes, and this always
 * asks for exactly 32), so this returns void. */
void setup_derive(const uint8_t token[SETUP_TOKEN_LEN], char bid_out[SETUP_BID_LEN],
                  char bpw_out[SETUP_BPW_LEN], uint8_t bkey_out[32]);

/* AES-256-GCM-decrypts `blob` (docs/DEVICE_PLAN.md §3.2 step 4: `nonce(12) ||
 * ciphertext || tag(16)`, no additional authenticated data) under `bkey`
 * into `plain_out` (caller-owned, >= `blob_len - 28` bytes; `plain_cap` is
 * checked). Returns false — leaving `*plain_len` unset — on a too-short
 * `blob`, an oversized plaintext for `plain_cap`, or (via
 * `mbedtls_gcm_auth_decrypt`'s own tag check) a wrong `bkey` or a
 * corrupted/tampered bundle. No modem or radio access; no dynamic
 * allocation. */
bool setup_decrypt_bundle(const uint8_t bkey[32], const uint8_t *blob, size_t blob_len,
                          uint8_t *plain_out, size_t plain_cap, size_t *plain_len);

/* Runs the full docs/DEVICE_PLAN.md §3.2 device-side bootstrap flow for an
 * already-typed `code`:
 *
 *   parse -> derive -> attach -> TLS profile 3 -> MQTT connect as
 *   `boot-{bid}`/`bpw` -> subscribe `pager/boot/{bid}/down` -> wait for the
 *   retained bundle -> AES-256-GCM decrypt under `bkey` -> CBOR-decode ->
 *   validate every field -> `ident_store()` -> CA to modem slot 12 ->
 *   publish `{0:1,29:1}` on `pager/boot/{bid}/up` -> disconnect ->
 *   `esp_restart()`.
 *
 * Logs (ESP_LOGI) exactly one `SETUP network`/`SETUP broker`/`SETUP
 * bundle`/`SETUP done` line per successful stage transition (never more than
 * once each), and mirrors the same word as a `ui_show_toast()` (best-effort;
 * a missing/dead display never blocks this function, matching every other
 * module's "no pager function may be gated on the display" rule). On
 * failure, logs+toasts exactly one of the four fixed DEVICE_PLAN.md §3.2
 * step 5 strings ("no network", "cannot reach broker", "no setup code
 * waiting", "code damaged") and returns false — the caller (the `setup`
 * console command) may retry with a new `setup <code>` call.
 *
 * On success this calls esp_restart() and never returns to the caller at
 * all (the device reboots into the normal modes_boot()/modes_run() path
 * with the freshly written `ident`).
 *
 * Power effect: the full bootstrap network session (attach, one TLS
 * handshake at ~5 kB, one small publish, one NVRAM write to modem cert slot
 * 12), once per device lifetime (or once per rotation, §3.5) — see the
 * per-step comments in setup.c and net.h's net_bootstrap_*() functions for
 * the breakdown. */
bool setup_run(const char *code);

#ifdef __cplusplus
}
#endif

#endif /* SETUP_H */

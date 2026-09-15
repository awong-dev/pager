/* auth.h — device-side HMAC signing/verification and replay counters for
 * device-authenticated envelopes (`authMode: "hmac"`), per docs/PROTOCOL.md
 * §14 (normative) and docs/DEVICE_PLAN.md §2.4/§2.5/§2.7.
 *
 * Scope: this module signs/verifies the CBOR `sig` suffix only (devices
 * only ever publish CBOR on the wire, docs/DEVICE_PLAN.md §2.4 — the JSON
 * signing rule in docs/PROTOCOL.md §14.3 exists for the relay/tools side,
 * `relay/app/devauth.py`, not for firmware) and tracks the two replay
 * counters (`n` for outgoing /up,/status,/loc and the window for incoming
 * /down). It never touches NVS or the modem directly:
 *
 *  - The HMAC key (`K_dev`) is handed in once via auth_init(), copied into
 *    this module's own static storage. The real caller (a later task wires
 *    main.c/modes.c) supplies ident_get_kdev(); this module has no
 *    ident.c/NVS dependency of its own, which is what keeps it buildable
 *    and testable with a plain host compiler (firmware/host/Makefile, no
 *    ESP-IDF) per docs/DEVICE_TASKS.md's Track F preamble.
 *  - The `n_epoch` half of the /up counter (docs/DEVICE_PLAN.md §2.5) is
 *    likewise a parameter, not a call to ident_get_n_epoch() — same reason.
 *  - The RTC-resident state (`auth_rtc_t`) is a plain struct this module
 *    only reads/writes through the pointer it is given each call. It does
 *    not own the storage, the RTC_DATA_ATTR attribute, the magic/CRC pair,
 *    or any locking — modes.c embeds `auth_rtc_t` inside its own
 *    `pager_rtc_t` (docs/DEVICE_PLAN.md §2.7's "RTC changes") and owns all
 *    of that, exactly the way msg.c's `msg_rtc_t` (msg.h) works.
 *
 * HMAC-SHA256 implementation: ESP-IDF's bundled mbedTLS
 * (`mbedtls_md_hmac`) on-device, per docs/DEVICE_TASKS.md F3.3's "Do" step
 * — see the `#ifdef ESP_PLATFORM` split in auth.c. The non-ESP_PLATFORM
 * path (host test build only) uses a small self-contained SHA-256/HMAC
 * implementation local to auth.c, so firmware/host/Makefile needs no
 * external crypto library; both paths compute the same well-defined
 * HMAC-SHA256 function, so this does not weaken the host test's coverage
 * of the sign/verify *format* (docs/PROTOCOL.md §14.3), only its coverage
 * of ESP-IDF's own mbedTLS binding (which idf.py build — a plain compile,
 * no target execution — cannot exercise either).
 *
 * No dynamic allocation anywhere in this module.
 */
#ifndef AUTH_H
#define AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing constants.
 * --------------------------------------------------------------------- */

#define AUTH_KDEV_LEN 32 /* K_dev, matches ident.h's IDENT_KDEV_LEN, DEVICE_PLAN.md §2.3 */
#define AUTH_TAG_LEN 8   /* HMAC-SHA256 truncated to 64 bits, PROTOCOL.md §14.3 */

/* CBOR `sig` suffix: key 13 (0x0D) + bstr(8) header (0x48) + 8 tag bytes —
 * PROTOCOL.md §14.3 / DEVICE_PLAN.md §2.4. */
#define AUTH_SIG_SUFFIX_LEN (2 + AUTH_TAG_LEN)

/* §2.5: n = (epoch << AUTH_UP_LO_BITS) | lo, 12-bit epoch / 20-bit lo. */
#define AUTH_UP_LO_BITS 20u
#define AUTH_UP_LO_MASK ((1u << AUTH_UP_LO_BITS) - 1u) /* 0xFFFFF */
#define AUTH_UP_EPOCH_BITS 12u
#define AUTH_UP_EPOCH_MASK ((1u << AUTH_UP_EPOCH_BITS) - 1u) /* 0xFFF */

/* §2.5/§2.7: device-side /down replay window width. Deliberately narrower
 * than the relay's 64-wide /up window (relay/app/store/device_secrets.py's
 * `accept_up_n`) — asymmetric by design, not a bug to reconcile
 * (docs/DEVICE_PLAN.md §2.5's RTC sizing: `auth.down_bits` is 4 bytes). */
#define AUTH_DOWN_WINDOW 32u

/* ---------------------------------------------------------------------
 * RTC-resident sub-struct (docs/DEVICE_PLAN.md §2.7's "RTC changes": `up_lo`
 * (4), `down_n` (4), `down_bits` (4) = 12 bytes). Embedded by modes.c inside
 * its own pager_rtc_t as an `auth` field, the same pattern msg.h's
 * `msg_rtc_t` documents: this header defines the layout so both modules
 * agree on it, modes.c owns the storage, the magic/CRC pair and any
 * locking. A cold boot zeroes the whole enclosing struct (all-zero is a
 * valid starting state here: up_lo=0/down_n=0/down_bits=0), so no separate
 * auth_rtc_init() is needed.
 * --------------------------------------------------------------------- */
typedef struct {
    uint32_t up_lo;     /* low AUTH_UP_LO_BITS bits of the /up,/status,/loc counter */
    uint32_t down_n;    /* highest accepted /down n */
    uint32_t down_bits; /* AUTH_DOWN_WINDOW-wide bitmap of the values below down_n */
} auth_rtc_t;

/* ---------------------------------------------------------------------
 * Key setup.
 * --------------------------------------------------------------------- */

/* Copies `kdev` (AUTH_KDEV_LEN bytes) into this module's static storage;
 * every subsequent auth_sign()/auth_verify() call uses it until the next
 * auth_init(). No dynamic allocation. No modem or sleep-state effect. */
void auth_init(const uint8_t kdev[AUTH_KDEV_LEN]);

/* ---------------------------------------------------------------------
 * Sign / verify (CBOR `sig`, PROTOCOL.md §14.3 / DEVICE_PLAN.md §2.4).
 * --------------------------------------------------------------------- */

/* Appends the AUTH_SIG_SUFFIX_LEN-byte `sig` suffix to `buf[0..*len)` in
 * place: the tag covers `topic || 0x00 || buf[0..*len)`, i.e. the caller
 * must already have written a CBOR map header whose declared pair count
 * *includes* this not-yet-written pair (docs/DEVICE_PLAN.md §2.4). This
 * function only ever appends — "no re-serialisation" — so it never rewrites
 * bytes the caller already wrote. On success, `*len` is advanced by
 * AUTH_SIG_SUFFIX_LEN and this returns true. Returns false, leaving
 * `buf`/`*len` unchanged, if `*len + AUTH_SIG_SUFFIX_LEN` would exceed
 * `cap`, or if auth_init() has never been called. No modem or sleep-state
 * effect: pure computation over caller-owned memory. */
bool auth_sign(const char *topic, uint8_t *buf, size_t *len, size_t cap);

/* Verifies that `buf[0..*len)` ends in a well-formed `sig` suffix (the
 * `0x0D 0x48` marker followed by 8 tag bytes) and that the tag matches
 * HMAC-SHA256(K_dev, topic || 0x00 || buf[0..*len - AUTH_SIG_SUFFIX_LEN))
 * truncated to AUTH_TAG_LEN bytes, comparing in constant time
 * (docs/PROTOCOL.md §14.3/§14.4). No parsing of the rest of `buf` happens
 * before or during this call.
 *
 * On success, trims exactly the suffix (`*len -= AUTH_SIG_SUFFIX_LEN`) and
 * returns true. Per docs/DEVICE_PLAN.md §2.4's "no re-serialisation" rule,
 * this does NOT rewrite the CBOR map header: its declared pair count still
 * includes the now-absent `sig` pair. A caller that walks the trimmed
 * buffer by header count must account for that one extra (missing) pair —
 * the same convention firmware/host/test_cbor.c's vector round-trip already
 * uses. A caller that does not need to re-decode (e.g. one that only wants
 * `*len` for logging) can ignore this note entirely.
 *
 * Returns false, leaving `buf`/`*len` unchanged, on a missing/malformed
 * suffix, a bad tag, or if auth_init() has never been called — this is the
 * "malformed" outcome PROTOCOL.md §3.4 assigns: caller must not render and
 * must not ack. No modem or sleep-state effect. */
bool auth_verify(const char *topic, uint8_t *buf, size_t *len);

/* ---------------------------------------------------------------------
 * Replay counters (docs/DEVICE_PLAN.md §2.5).
 * --------------------------------------------------------------------- */

/* Computes the next 32-bit `n = (epoch << AUTH_UP_LO_BITS) | lo` for a /up,
 * /status, or /loc publish, using `rtc->up_lo` for `lo` and the caller-
 * supplied `epoch` (the real caller reads this from ident_get_n_epoch();
 * see the header note on why it is a parameter here, not a direct call).
 * Advances `rtc->up_lo` by one afterward, wrapping at AUTH_UP_LO_MASK back
 * to 0.
 *
 * If `wrapped` is non-NULL, `*wrapped` is set true exactly when `up_lo`
 * just wrapped — docs/DEVICE_PLAN.md §2.5: the caller MUST then increment
 * and persist a new `n_epoch` via ident_store() before the next call, or
 * the relay's replay window sees `n` go backwards. Set false otherwise.
 * (The other epoch-bump trigger, a cold boot / invalid RTC CRC, is
 * modes.c's own concern at boot time, before any auth_rtc_t exists to pass
 * here — not something this per-publish function can detect.)
 *
 * No modem or sleep-state effect: RTC/argument memory only. */
uint32_t auth_next_up_n(auth_rtc_t *rtc, uint16_t epoch, bool *wrapped);

/* Device-side mirror of the relay's replay window (docs/DEVICE_PLAN.md
 * §2.5), but AUTH_DOWN_WINDOW (32) wide instead of the relay's 64 — see the
 * constant's comment. Accepts `n` (updating `rtc->down_n`/`rtc->down_bits`)
 * if `n > rtc->down_n` (window shifts forward) or `rtc->down_n -
 * AUTH_DOWN_WINDOW < n <= rtc->down_n` and its bit is clear; otherwise
 * rejects as a replay/out-of-window value with no state change. `n == 0` is
 * always rejected (the relay never issues it — `next_down_n()` in
 * relay/app/store/device_secrets.py starts from 1).
 *
 * Returns true (state updated) if accepted, false (state unchanged) if
 * rejected. A rejection is the PROTOCOL.md §3.4 "malformed" outcome for a
 * /down message: caller must not render and must not ack. No modem or
 * sleep-state effect: RTC/argument memory only. */
bool auth_accept_down_n(auth_rtc_t *rtc, uint32_t n);

#ifdef __cplusplus
}
#endif

#endif /* AUTH_H */

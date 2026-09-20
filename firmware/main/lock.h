/* lock.h — passcode lock: PBKDF2 hash storage (NVS namespace "lock"),
 * auto-lock, wrong-passcode backoff, and the `cfg` `lock` map handler
 * (docs/DEVICE_TASKS.md F6.5, docs/DEVICE_PLAN.md §5.8).
 *
 * "What it protects against" / the offline-brute-force caveat: see
 * docs/DEVICE_PLAN.md §5.8's own opening paragraph — flash is unencrypted by
 * decision (H3); this module does not change that. It protects against
 * someone picking the device up and reading/replying, not against a USB
 * cable and time.
 *
 * Split the same way auth.c/setup.c already are (see their own module
 * comments for the full rationale): the pure functions below this banner —
 * lock_passcode_valid(), lock_pbkdf2(), lock_backoff_seconds(),
 * lock_parse_cfg() — have no ESP-IDF dependency and are host-tested by
 * firmware/host/test_lock.c. lock_pbkdf2() links the *real* mbedtls on the
 * host (mbedtls_pkcs5_pbkdf2_hmac, same reasoning setup.c's HKDF/AES-GCM
 * gives: both ESP-IDF and the host link the same well-tested implementation
 * of this exact primitive, so nothing about the crypto itself goes untested
 * by using it on both sides). lock_parse_cfg() only needs cbor.h, itself
 * already host-buildable (firmware/host/test_cbor.c).
 *
 * Everything else — NVS I/O, the RTC-resident locked/fail_count/
 * backoff_until_us binding, the auto-lock check (esp_timer), and
 * lock_ingest_cfg_cbor() (calls msg_mark_shown()) — is `#ifdef
 * ESP_PLATFORM`-only, device-only, mirroring msg.c/auth.c/setup.c.
 *
 * RTC ownership: same pattern as msg_rtc_t (msg.h) / auth_rtc_t (auth.h) —
 * modes.c embeds `lock_rtc_t` inside its own pager_rtc_t and owns the
 * storage, the magic/CRC pair, and the lock/unlock/save callbacks handed
 * over via lock_bind_rtc(). The same lock/unlock pair also guards this
 * module's own RAM-resident state (the NVS-cached auto_min/preview/
 * passcode-configured flag, and the one-shot admin-clear toast) — reused for
 * the same reason msg.h's own header comment gives for msg.c's RAM state:
 * lock_ingest_cfg_cbor() can run on WalterModem's _eventProcessingTask (via
 * modes.c's on_incoming_message()) while scr_device.c/scr_lock.c read/write
 * the same state from modes_run()'s task.
 */
#ifndef LOCK_H
#define LOCK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Sizing / schedule constants (docs/DEVICE_PLAN.md §5.8).
 * --------------------------------------------------------------------- */

#define LOCK_PASSCODE_MIN 4
#define LOCK_PASSCODE_MAX 16
#define LOCK_SALT_LEN 16
#define LOCK_HASH_LEN 32
#define LOCK_PBKDF2_ITERATIONS 10000u

#define LOCK_FREE_ATTEMPTS 5u   /* five free attempts before any backoff */
#define LOCK_BACKOFF_BASE_S 30u /* first backoff step */
#define LOCK_BACKOFF_CAP_S 600u /* 10-minute cap */

/* ---------------------------------------------------------------------
 * RTC-resident sub-struct — docs/DEVICE_PLAN.md §5.8's "RTC" bullet: 16 B
 * with padding (`locked` (1), `fail_count` (1), `backoff_until_us` (8) +
 * padding), docs/PROTOCOL.md §9.3's table row. Explicit `_pad` (rather than
 * relying on default struct padding) so sizeof() is exactly 16 on every ABI
 * this project builds for — same style auth_rtc_t/msg_rtc_t's own plain,
 * unambiguous field lists use. `locked`/`fail_count` are plain uint8_t, not
 * bool/bitfields, for the same reason. */
typedef struct {
    uint8_t locked; /* 0/1 */
    uint8_t fail_count;
    uint8_t _pad[6];
    int64_t backoff_until_us; /* monotonic esp_timer_get_time() deadline; 0 = no active backoff */
} lock_rtc_t;

/* ---------------------------------------------------------------------
 * Pure functions — no ESP-IDF dependency, host-tested.
 * --------------------------------------------------------------------- */

/* 4-16 printable ASCII (0x20-0x7E) characters — docs/DEVICE_PLAN.md §5.8's
 * "4-16 printable ASCII characters, so a digits-only PIN is just a short
 * passcode." No I/O, no state. */
bool lock_passcode_valid(const char *passcode, size_t len);

/* PBKDF2-HMAC-SHA256(passcode, salt, LOCK_PBKDF2_ITERATIONS) -> 32-byte
 * hash, via mbedtls_pkcs5_pbkdf2_hmac (docs/DEVICE_TASKS.md F6.5's Do).
 * Returns false (out_hash left unchanged) only if the underlying mbedTLS
 * call itself fails (should not happen with these fixed-size inputs).
 * ~50-100ms on the S3 at 10 000 iterations (UNVERIFIED,
 * docs/DEVICE_PLAN.md §5.8/§10) — no modem or sleep-state effect, pure
 * CPU-bound computation over caller-owned memory. */
bool lock_pbkdf2(const char *passcode, size_t passcode_len, const uint8_t salt[LOCK_SALT_LEN],
                  uint8_t out_hash[LOCK_HASH_LEN]);

/* Wrong-passcode backoff schedule (docs/DEVICE_PLAN.md §5.8): 0 for the
 * first LOCK_FREE_ATTEMPTS cumulative failures, then LOCK_BACKOFF_BASE_S
 * doubling with every additional failure, capped at LOCK_BACKOFF_CAP_S (10
 * minutes) — fail_count 6->30s, 7->60s, 8->120s, ... capped from 11 on.
 * Pure function of the cumulative fail count, no state, no I/O. */
uint32_t lock_backoff_seconds(uint32_t fail_count);

/* Decodes a `/down` envelope already reduced to `count` map pairs the same
 * way msg.c's own MK_* switch does — `sig_pair_present` stands in for
 * `ident_get_flags() & IDENT_FLAG_REQ_SIG` so this stays host-testable
 * (same convention auth.h documents for auth_next_up_n()'s `epoch`
 * parameter: a caller-supplied bool instead of an ident.c call). `buf` MUST
 * already have passed auth_verify() when signed, exactly like
 * msg_ingest_down_cbor()'s own contract (msg.h) — the ten trailing `sig`
 * bytes trimmed, the map header's declared pair count left untouched
 * (docs/DEVICE_PLAN.md §2.4's "no re-serialisation" rule).
 *
 * Returns false if `buf` does not decode as a well-formed `kind:"cfg"`
 * envelope carrying a `cfg.lock` map (docs/PROTOCOL.md §10: `cfg`=key 38,
 * `lock`=key 0 inside it, `clear`=key 0 / `auto`=key 1 inside *that* —
 * `CFG_KEYMAP`/`LOCK_KEYMAP` in relay/app/wirecbor.py) — this covers both
 * "not a cfg envelope at all" (some other kind, or none) and "a cfg
 * envelope but malformed", deliberately conflated: either way the caller
 * (lock_ingest_cfg_cbor() below) falls through to the normal ingest path,
 * which drops a genuinely malformed envelope as PROTOCOL.md §3.4 already
 * requires without this function needing its own malformed-counter.
 *
 * On success, fills `out_id` (NUL-terminated into the caller's buffer,
 * truncated-away silently if it doesn't fit — id is only used for acking,
 * never rendered) and `*out_have_clear`/`*out_clear`,
 * `*out_have_auto`/`*out_auto_min` for whichever `cfg.lock` sub-fields were
 * present (both may be absent, present, or both present at once). */
bool lock_parse_cfg(const uint8_t *buf, uint16_t len, bool sig_pair_present, char *out_id,
                     size_t out_id_cap, bool *out_have_clear, bool *out_clear, bool *out_have_auto,
                     uint8_t *out_auto_min);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * RTC wiring (modes.c calls this once, before lock_init()) — same pattern as
 * msg_bind_rtc()/msg_bind_auth() (msg.h).
 * --------------------------------------------------------------------- */
typedef void (*lock_rtc_lock_fn)(void);
typedef void (*lock_rtc_unlock_fn)(void);
typedef void (*lock_rtc_save_fn)(void); /* must be called with the lock already held */

void lock_bind_rtc(lock_rtc_t *rtc, lock_rtc_lock_fn lock, lock_rtc_unlock_fn unlock,
                    lock_rtc_save_fn save);

/* Loads NVS namespace "lock" (whether a passcode is configured, `auto_min`,
 * `preview`) into this module's own RAM cache. Then, regardless of
 * `rtc_was_valid`: if a passcode is configured, forces `locked = 1` —
 * docs/DEVICE_PLAN.md §5.8: "After a restart the monotonic clock is gone and
 * the device comes up locked whenever a passcode is set — the safe
 * default." This applies to BOTH a cold boot (RTC just zeroed) and a warm
 * reset (RTC survived): esp_timer's own clock resets to ~0 on every boot
 * either way (same reasoning modes.c's `active_until_us` doc comment gives
 * for why a monotonic RTC deadline is never trusted blindly across a
 * restart), so a stale "unlocked" RTC value cannot be trusted regardless of
 * which kind of reset this was.
 *
 * `fail_count` survives a warm reset unchanged (docs/DEVICE_PLAN.md §5.8:
 * "`fail_count` ... live[s] in RTC so a restart does not reset them" — a
 * battery-pull attack on the attempt counter must not work). `backoff_
 * until_us`, an absolute esp_timer_get_time() deadline from before the
 * reset, is NOT trusted verbatim against the new (reset-to-~0) clock —
 * comparing a stale large absolute value against a freshly-reset small
 * counter would leave the device locked out far longer than the 10-minute
 * cap. Instead it is recomputed fresh from *this* boot's clock using the
 * same fail_count-driven schedule (lock_backoff_seconds()), so the
 * enforced wait matches the schedule immediately after a restart rather
 * than being silently voided OR silently extended. A cold boot (RTC lost)
 * has no fail_count/backoff_until_us to preserve either way — both reset to
 * zero. No modem or sleep-state effect beyond a handful of NVS reads and
 * (on a warm reset with a nonzero fail_count) one NVS-free RTC write. */
void lock_init(bool rtc_was_valid);

bool lock_is_set(void);      /* a passcode is configured */
bool lock_is_locked(void);   /* current RTC `locked` state */
uint8_t lock_auto_min(void); /* 0 = never */
bool lock_preview(void);     /* show sender aliases on the Locked screen */

/* Sets/replaces the passcode: validates (lock_passcode_valid()), generates a
 * fresh random salt (esp_random()), computes the PBKDF2 hash
 * (lock_pbkdf2()), writes NVS namespace "lock" (`salt`, `hash`), commits.
 * Does NOT change `locked` — the caller reaching this (Device screen) is by
 * definition not currently on the Locked screen. Power effect: ~50-100ms
 * CPU-bound PBKDF2 compute (UNVERIFIED) + one NVS write; no modem/sleep
 * effect. Returns false (NVS/RAM state unchanged) on a bad passcode shape or
 * an NVS write failure. */
bool lock_set_passcode(const char *passcode, size_t len);

/* Erases `salt`/`hash` from NVS, clears the in-RAM cache, and unlocks
 * immediately (`locked`, `fail_count`, `backoff_until_us` all -> 0 — no
 * passcode left to brute-force). Power effect: one NVS erase+commit. */
void lock_clear_passcode(void);

/* Persists `auto_min` (0 = never; §5.5's cycle: 0,1,2,5,10,30,60) / the
 * sender-preview flag to NVS and updates the RAM cache. Power effect: one
 * NVS write each. */
void lock_set_auto_min(uint8_t minutes);
void lock_set_preview(bool on);

/* Checks `passcode` (len bytes) against the stored hash. If
 * lock_is_locked_out() is currently true, returns false WITHOUT computing
 * the hash or touching fail_count/backoff (caller should check
 * lock_is_locked_out()/lock_backoff_remaining_s() first and show the
 * countdown instead of even trying). If no passcode is configured, always
 * succeeds (unlocks) — nothing to check. On a real check: success unlocks
 * and resets fail_count/backoff to 0; failure increments fail_count and
 * (re)computes backoff_until_us from lock_backoff_seconds(). Power effect:
 * the same ~50-100ms PBKDF2 compute as lock_set_passcode() (skipped
 * entirely while locked out, so a rapid-fire lockout does not also burn
 * CPU/battery on wasted hashing). */
bool lock_try_passcode(const char *passcode, size_t len);

bool lock_is_locked_out(void);         /* backoff_until_us is in the future */
uint32_t lock_backoff_remaining_s(void); /* seconds left, rounded up; 0 if not locked out */

/* Auto-lock check (docs/DEVICE_PLAN.md §5.8): call on every input event and
 * every UI wake, with the current esp_timer_get_time(). Locks (RTC `locked`
 * -> 1) iff a passcode is set, `auto_min != 0`, not already locked, and
 * `now_us` is at least `auto_min` minutes past the last call's `now_us`.
 * Always updates the last-activity timestamp afterward (RAM-only, this-boot
 * monotonic — deliberately NOT an RTC field, same reasoning modes.c's own
 * non-RTC `s_last_batt_mv`/`s_last_rssi_dbm` statics give: this design never
 * deep sleeps, only light sleeps, which retain ordinary RAM). No modem
 * effect; an RTC write only on the (rare) edge that actually locks. */
void lock_check_autolock(int64_t now_us);

/* Explicit "Lock now" (Home menu, docs/DEVICE_PLAN.md §5.5). No-op if no
 * passcode is configured. */
void lock_now(void);

/* `cfg.lock` sub-map handler (docs/PROTOCOL.md §3.2/§5.8/§10). Called from
 * cfg.c's cfg_ingest_cbor() (v0.2 §4.4: a single `cfg` push can now also
 * carry `ca`, and possibly both in one envelope, so the old
 * whole-envelope-decoding lock_ingest_cfg_cbor() — which owned the
 * `kind:"cfg"`/`id` decode itself and would have silently swallowed any
 * `ca` sharing the same push — was replaced by cfg.c's own single decode
 * pass; this function only ever sees the `lock` sub-map's own raw CBOR
 * bytes, already isolated by cfg.c). `buf`/`len` are that sub-map's byte
 * span (starting at its own map header); `id` is the envelope's own id, for
 * the ack.
 *
 * Applies `clear`/`auto` (queueing an admin-clear toast via the
 * lock_take_toast() handoff below, on `clear`), then acks `shown` itself
 * (msg_mark_shown()) once applied — regardless of lock state
 * (docs/DEVICE_PLAN.md §5.8: "the lock is about the screen", not about
 * config messages; contrast with the deferred-shown rule for ordinary
 * content messages, msg.c). A malformed `lock` sub-map is logged and
 * dropped (no ack, no crash), same fail-safe rule every other malformed
 * `/down` content follows. No modem/sleep-state effect beyond the ack
 * queued for msg_pump()'s next publish and (on `clear`) the NVS erase
 * lock_clear_passcode() already documents. */
void lock_apply_cfg_submap(const uint8_t *buf, uint16_t len, const char *id);

/* Drains a toast queued by lock_ingest_cfg_cbor() (currently only "passcode
 * cleared by admin"). MUST be called only from modes_run()'s own task, same
 * discipline as modes.c's service_render_pending() (see its own comment for
 * why: ui_show_toast() renders, and rendering must never happen on the MQTT
 * event task, which is where lock_ingest_cfg_cbor() runs). Non-blocking, no
 * I/O of its own. Returns true and fills `out` (NUL-terminated into `cap`
 * bytes) if a toast was pending; false (out untouched) otherwise. */
bool lock_take_toast(char *out, size_t cap);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* LOCK_H */

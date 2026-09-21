/* catrust.h — CA trust state, TLS-fail fallback and two-phase CA apply
 * (docs/V02_DESIGN.md §4, docs/V02_DESIGN.md §4).
 *
 * Split the usual way (lock.c/loc.c/cfg.c): everything above the `#ifdef
 * ESP_PLATFORM` banner is pure C, no ESP-IDF dependency, driven by
 * caller-supplied state (a streak counter, a fake `int64_t now_us` clock —
 * never `esp_timer_get_time()` directly) so firmware/host/test_catrust.c can
 * run every policy decision without a device: trust-state derivation, the
 * "one more validated attempt, then fall back" TLS-fail policy, the
 * daily/cold-boot revalidation-while-broken gate, the `cfg.ca` sub-map
 * decode (`{url, sha}` / the `{url:""}` un-pin form), the give-up counter,
 * and the `/status` `ca_fp` formatter.
 *
 * Below the banner: the device-only trust state (backed by
 * ident_get_tls_broken()/ident_set_tls_broken(), docs/V02_DESIGN.md §4.1)
 * and the two-phase apply state machine (§4.4), driven one small step per
 * call from modes_run()'s own loop via catrust_service() — mirroring
 * loc_service()'s "never the whole attempt in one call" discipline, so a
 * slow CA fetch or a stuck reconnect trial cannot block paging. The
 * production MQTT session teardown/reconnect this needs
 * (`modes_set_ca_apply_suppress()`, modes.h) reuses the exact pattern
 * loc.c's route-2 CFUN=4 window already established for hiding a
 * *deliberate* session teardown from modes.c's own reconnect backoff/
 * watchdog/health check.
 *
 * NVS: `ident` namespace key `tls_broken` (u8, docs/V02_DESIGN.md §4.1,
 * ident.c/h) and a new `catrust` namespace, keys `pend_sha` (blob32) /
 * `pend_fail` (u8) — the give-up counter for "3 failed applies of the same
 * sha" (§4.4), persisted so a reboot mid-apply cannot reset an
 * already-bad-CA loop back to zero and retry forever; the two-phase apply's
 * own IN-PROGRESS phase is deliberately NOT persisted (a reboot mid-apply
 * simply restarts from IDLE — since ident's committed CA/hash is only ever
 * written on a successful COMMIT, the device comes back up on the OLD,
 * working CA either way, satisfying "a reboot mid-apply comes back in the
 * OLD, working state" without needing to persist the phase itself).
 */
#ifndef CATRUST_H
#define CATRUST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------
 * Trust states (docs/V02_DESIGN.md §4.1 / docs/V02_DESIGN.md §4.1).
 * --------------------------------------------------------------------- */
typedef enum {
    CATRUST_UNPINNED = 0, /* no CA in the identity; validation off */
    CATRUST_PINNED = 1,   /* CA set, last connect validated */
    CATRUST_BROKEN = 2,   /* CA set, validation failed, running with validation off */
} catrust_state_t;

/* Pure derivation: UNPINNED whenever no CA is pinned (the `broken` byte is
 * meaningless then and ignored), else PINNED/BROKEN per the persisted byte. */
catrust_state_t catrust_derive_state(bool have_ca, bool broken_flag);

/* ---------------------------------------------------------------------
 * Fallback policy (docs/V02_DESIGN.md §4.2).
 * --------------------------------------------------------------------- */
typedef enum {
    CATRUST_TLS_RETRY_VALIDATED, /* pinned, 1st TLS_FAIL: try again validated before giving up on it */
    CATRUST_TLS_FALL_BACK,       /* pinned, 2nd consecutive TLS_FAIL: reconfigure unvalidated, state -> broken */
    CATRUST_TLS_STEADY,          /* unpinned, or already broken and still failing TLS: nothing left to fall back to */
} catrust_tls_action_t;

/* `currently_broken` is catrust_derive_state(...) == CATRUST_BROKEN;
 * `have_ca` is whether a CA is pinned at all (UNPINNED also steadies, same
 * "nothing left to fall back to" reasoning). `*streak` is a caller-owned
 * counter (0 initially), reset to 0 by this function whenever it returns
 * anything other than CATRUST_TLS_RETRY_VALIDATED — the caller must also
 * reset it to 0 on any successful connect (a TLS success clears the "one
 * more try" memory). Pure, no I/O. */
catrust_tls_action_t catrust_on_tls_fail(bool currently_broken, bool have_ca, uint32_t *streak);

#define CATRUST_BROKEN_REVALIDATE_S 86400u /* 24h; V02_DESIGN.md §4.2 */

/* True iff a validated attempt is due now: the first ever call this power
 * session (`is_first_since_boot`), or at least CATRUST_BROKEN_REVALIDATE_S
 * since the last validated attempt (`last_validated_attempt_us == 0` also
 * counts as due — "never attempted this boot yet"). Interpreted as a
 * monotonic 24h interval since the last *attempt* (not wall-clock midnight):
 * V02_DESIGN.md §4.2 says "each 24h boundary", which is ambiguous between a
 * wall-clock day boundary and a rolling 24h window — this firmware uses the
 * monotonic rolling interpretation because it needs no working network
 * clock (docs/PROTOCOL.md §3.5: `ts` can read 0 indefinitely) and still
 * meets the stated intent ("heals by itself within about a day"); flagged
 * for the architect in case a wall-clock day boundary was intended
 * instead. Pure, no I/O — `now_us`/`last_validated_attempt_us` are
 * `esp_timer_get_time()`-shaped but never read directly here. */
bool catrust_due_for_validated_retry(int64_t last_validated_attempt_us, int64_t now_us,
                                     bool is_first_since_boot);

/* ---------------------------------------------------------------------
 * `cfg.ca` sub-map decode (docs/V02_DESIGN.md §7: `{url=0 tstr, sha=1
 * bstr(32)}`; un-pin is `{url=0 ""}` with NO `sha` key).
 * --------------------------------------------------------------------- */
#define CATRUST_URL_MAX 256

typedef struct {
    bool unpin;      /* url == "" and no sha key */
    bool have_url;
    char url[CATRUST_URL_MAX];
    bool have_sha;
    uint8_t sha[32];
} catrust_cfg_t;

/* `buf`/`len` are the raw CBOR bytes of the `ca` sub-map value itself
 * (cfg.h's cfg_dispatch_t.ca_off/ca_len span) — this function opens its own
 * cbor_r_t on them, it does not touch the enclosing envelope. Returns false
 * for anything that is not exactly the pin shape (`url` non-empty + a
 * 32-byte `sha`) or the un-pin shape (`url` empty, no `sha`) — e.g. an empty
 * url WITH a sha, or a non-empty url with no sha, are both rejected as
 * malformed rather than guessed at. */
bool catrust_parse_cfg_submap(const uint8_t *buf, uint16_t len, catrust_cfg_t *out);

/* "give up after 3 failed applies of the same sha" (V02_DESIGN.md §4.4).
 * Pure comparison, so the exact threshold lives in one place. */
bool catrust_give_up(uint8_t fail_count);

/* First 16 lowercase hex chars of `ca_hash` (the SHA-256 already stored in
 * ident_t/ident_get_ca_hash() — this does not hash anything itself, it only
 * formats). `out` must have room for 17 bytes (16 hex chars + NUL). */
void catrust_fingerprint_hex(const uint8_t ca_hash[32], char out[17]);

#ifdef ESP_PLATFORM
/* ---------------------------------------------------------------------
 * Device wiring.
 * --------------------------------------------------------------------- */
#include "net.h" /* net_mqtt_status_t — catrust_service()'s own parameter, below */

typedef void (*catrust_lock_fn)(void);
typedef void (*catrust_unlock_fn)(void);

/* Binds the cross-task mutex modes.c already owns (same reuse pattern
 * lock.c's own toast/RAM-cache fields document) — catrust.c's own RAM state
 * (the pending cfg request, the two-phase apply phase) is written from the
 * MQTT event task (catrust_apply_cfg_submap()) and read/advanced from
 * modes_run()'s task (catrust_service()). Call once, before catrust_init(). */
void catrust_bind(catrust_lock_fn lock, catrust_unlock_fn unlock);

/* Loads ident_get_tls_broken() and NVS namespace "catrust"
 * (`pend_sha`/`pend_fail`) into RAM. Call once from modes_boot(), after
 * ident_load(). No modem access; a couple of NVS reads. */
void catrust_init(void);

/* Derives the current state from ident_get_ca_len()>0 plus the cached
 * broken flag — a plain read, no NVS I/O of its own beyond what
 * catrust_init() already did. */
catrust_state_t catrust_get_state(void);

/* Fills `out` (17 bytes) with the pinned CA's fingerprint and returns true;
 * returns false (out untouched) when catrust_get_state() != CATRUST_PINNED/BROKEN
 * (i.e. UNPINNED) — `/status`'s `ca_fp` is "absent when unpinned"
 * (V02_DESIGN.md §4.3), and this covers both PINNED and BROKEN, since a
 * broken pin still names a fingerprint (that's the whole point of the
 * indicator: "this CA is what stopped validating"). */
bool catrust_get_ca_fp(char out[17]);

/* `/status` `tls` field text ("unpinned"/"pinned"/"broken"), for modes.c's
 * build_status_cbor(). */
const char *catrust_state_name(catrust_state_t state);

/* Called from modes.c's handle_mqtt_loss() on NET_MQTT_RC_TLS_FAIL, but NOT
 * while catrust_apply_in_progress() (a two-phase apply's own scratch-slot
 * trial failing is a different event entirely — catrust_service() resolves
 * that itself the same iteration; conflating the two would let a bad
 * *pushed* CA trial incorrectly mark the *pinned, already-working* CA
 * broken). Applies the CATRUST_TLS_FALL_BACK side effects itself
 * (net_tls_configure() back to slot 12 unvalidated, ident_set_tls_broken(true),
 * one INFO log line) and returns the action so modes.c can pick the right
 * backoff schedule (retry-validated / fall-back -> ordinary backoff so pages
 * keep flowing per §0; steady -> the pre-existing 300s "nothing left to try"
 * backoff). */
catrust_tls_action_t catrust_on_mqtt_tls_fail(void);

/* Called from modes.c's ordinary (non-apply) reconnect branch, right before
 * every net_session_up() call. No-op unless catrust_get_state() ==
 * CATRUST_BROKEN, in which case it reconfigures profile 2
 * (net_tls_configure()) validated or not per catrust_due_for_validated_retry(),
 * and remembers which one was requested for catrust_on_mqtt_connected() to
 * check afterward. */
void catrust_before_reconnect(void);

/* Called from modes.c on the "MQTT session just became usable" edge
 * (`st.mqtt_connected && !s_was_mqtt_connected`, the same edge that already
 * drives publish_status_online()). Clears the TLS-fail streak counter
 * unconditionally (any success clears "one more try" memory) and, if the
 * connect that just succeeded was a validated one requested while broken,
 * clears `tls_broken` (state -> pinned) and logs the transition. */
void catrust_on_mqtt_connected(void);

/* True while a two-phase apply (fetch, scratch-slot trial, commit/rollback)
 * is in progress — see catrust_on_mqtt_tls_fail()'s own doc comment for why
 * modes.c needs this. */
bool catrust_apply_in_progress(void);

/* `cfg.ca` intercept (cfg.c's cfg_ingest_cbor(), MQTT event task). `buf`/`len`
 * are the raw `ca` sub-map bytes (cfg_dispatch_t.ca_off/ca_len); `id` is the
 * envelope's own id, for the eventual ack. Records the request only —
 * "never from the MQTT event task beyond recording the request"
 * (V02_DESIGN.md §4.4) — the actual fetch/apply/ack all happen from
 * catrust_service(). A malformed `ca` sub-map (catrust_parse_cfg_submap()
 * returns false) is logged and dropped, exactly like any other malformed
 * `/down` content (docs/V02_DESIGN.md §2.6): no ack, no crash. Replaces any
 * previously-recorded-but-not-yet-started request (matches `cfg.lock`'s own
 * "only the newest unacked cfg is re-published" rule — an in-flight fetch
 * or reconnect trial is NOT aborted mid-flight by a second push arriving;
 * it is simply not overwritten until catrust_service() returns to IDLE). */
void catrust_apply_cfg_submap(const uint8_t *buf, uint16_t len, const char *id);

/* One non-blocking step of the pending-request / two-phase-apply state
 * machine. Call every modes_run() iteration, alongside accel_poll()/
 * loc_service(). `st` is THIS iteration's own net_get_mqtt_status() snapshot
 * (modes_run() already takes one for handle_mqtt_loss()/the reconnect
 * branch) — passed in rather than re-queried so a disconnect/connect edge
 * this iteration is seen exactly once by both handle_mqtt_loss() (which
 * acks it) and this function (which only reads the passed-in copy, never
 * calls net_ack_disconnect_edge() itself). */
void catrust_service(const net_mqtt_status_t *st);

/* `cafetch <url> <sha256hex>` debug console command (main.c,
 * PAGER_DEBUG_NO_LIGHT_SLEEP builds only): runs the fetch ONLY (no apply,
 * no NVS/ident writes) via cafetch_run_blocking(), and logs bytes received,
 * HTTP status, hash match, elapsed time, and whether the MQTT session
 * survived a second socket being opened. Blocks the CALLING task (the debug
 * console's own), never modes_run()'s — same discipline loc_debug_run()
 * documents for `gnsstest`. Returns true iff the fetch succeeded AND the
 * hash matched. */
bool catrust_debug_cafetch(const char *url, const char *sha256_hex);

#endif /* ESP_PLATFORM */

#ifdef __cplusplus
}
#endif

#endif /* CATRUST_H */

/* catrust.c — see catrust.h for the module split and every function's doc
 * comment.
 */
#include "catrust.h"

#include <string.h>

#include "cbor.h"

/* ---------------------------------------------------------------------
 * Pure functions — no ESP-IDF dependency, host-tested by
 * firmware/host/test_catrust.c.
 * --------------------------------------------------------------------- */

catrust_state_t catrust_derive_state(bool have_ca, bool broken_flag)
{
    if (!have_ca) {
        return CATRUST_UNPINNED;
    }
    return broken_flag ? CATRUST_BROKEN : CATRUST_PINNED;
}

catrust_tls_action_t catrust_on_tls_fail(bool currently_broken, bool have_ca, uint32_t *streak)
{
    if (!have_ca || currently_broken) {
        // Nothing left to fall back to: unpinned already runs unvalidated,
        // and broken already does too (this event is either the
        // daily/cold-boot revalidation attempt failing again, or an
        // ordinary unvalidated connect that still could not complete a
        // handshake at all -- V02_DESIGN.md §4.2: "-8 also fires for
        // non-certificate handshake failures").
        *streak = 0;
        return CATRUST_TLS_STEADY;
    }
    (*streak)++;
    if (*streak < 2) {
        return CATRUST_TLS_RETRY_VALIDATED;
    }
    *streak = 0;
    return CATRUST_TLS_FALL_BACK;
}

bool catrust_due_for_validated_retry(int64_t last_validated_attempt_us, int64_t now_us,
                                     bool is_first_since_boot)
{
    if (is_first_since_boot || last_validated_attempt_us == 0) {
        return true;
    }
    int64_t elapsed_s = (now_us - last_validated_attempt_us) / 1000000;
    return elapsed_s >= (int64_t) CATRUST_BROKEN_REVALIDATE_S;
}

bool catrust_parse_cfg_submap(const uint8_t *buf, uint16_t len, catrust_cfg_t *out)
{
    memset(out, 0, sizeof(*out));

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
        switch (key) {
        case 0: { /* url */
            const char *s;
            size_t slen;
            if (!cbor_r_tstr(&r, &s, &slen) || slen >= sizeof(out->url)) {
                return false;
            }
            memcpy(out->url, s, slen);
            out->url[slen] = '\0';
            out->have_url = true;
            break;
        }
        case 1: { /* sha */
            const uint8_t *b;
            size_t blen;
            if (!cbor_r_bstr(&r, &b, &blen) || blen != sizeof(out->sha)) {
                return false;
            }
            memcpy(out->sha, b, sizeof(out->sha));
            out->have_sha = true;
            break;
        }
        default:
            if (!cbor_r_skip(&r)) {
                return false;
            }
            break;
        }
    }

    if (!out->have_url) {
        return false; /* `url` is required in both the pin and un-pin shapes */
    }
    bool url_empty = out->url[0] == '\0';
    if (url_empty) {
        if (out->have_sha) {
            return false; /* an empty url with a sha makes no sense */
        }
        out->unpin = true;
        return true;
    }
    if (!out->have_sha) {
        return false; /* a non-empty url MUST carry a sha */
    }
    return true;
}

bool catrust_give_up(uint8_t fail_count)
{
    return fail_count >= 3;
}

void catrust_fingerprint_hex(const uint8_t ca_hash[32], char out[17])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        out[2 * i] = digits[ca_hash[i] >> 4];
        out[2 * i + 1] = digits[ca_hash[i] & 0x0F];
    }
    out[16] = '\0';
}

/* ======================================================================
 * Device wiring — needs NVS, esp_timer, net.h, ident.h, msg.h, cafetch.h.
 * ====================================================================== */
#ifdef ESP_PLATFORM

#include <stdio.h>

#include "cafetch.h"
#include "ident.h"
#include "modes.h"
#include "msg.h"
#include "net.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "nvs.h"

static const char *TAG = "catrust";

#define CATRUST_NVS_NS "catrust"

// ---------------------------------------------------------------------------
// Cross-task binding (same reuse pattern lock.c/loc.c document: modes.c's
// own RTC mutex, reused here for this module's small RAM state, guarding it
// against the MQTT-event-task write (catrust_apply_cfg_submap()) racing
// modes_run()'s own read/advance (catrust_service())).
// ---------------------------------------------------------------------------
static void catrust_lock_noop(void) {}
static catrust_lock_fn s_lock = catrust_lock_noop;
static catrust_unlock_fn s_unlock = catrust_lock_noop;

void catrust_bind(catrust_lock_fn lock, catrust_unlock_fn unlock)
{
    s_lock = lock;
    s_unlock = unlock;
}

// ---------------------------------------------------------------------------
// Trust state (docs/V02_DESIGN.md §4.1).
// ---------------------------------------------------------------------------

static bool s_broken_cache = false; // RAM mirror of ident_get_tls_broken(), refreshed by catrust_init()
                                    // and every write this file makes to it.

// Give-up counter (NVS "catrust": pend_sha/pend_fail), RAM-cached — declared
// here (ahead of catrust_init(), which loads them) rather than down by the
// two-phase apply code that mutates them, purely for C's "declare before
// use" ordering within this one file.
static bool s_have_pend_sha = false;
static uint8_t s_pend_sha[32];
static uint8_t s_pend_fail = 0;

void catrust_init(void)
{
    s_broken_cache = ident_get_tls_broken();

    nvs_handle_t h;
    if (nvs_open(CATRUST_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t sha[32];
        size_t sha_len = sizeof(sha);
        if (nvs_get_blob(h, "pend_sha", sha, &sha_len) == ESP_OK && sha_len == sizeof(sha)) {
            memcpy(s_pend_sha, sha, sizeof(sha));
            s_have_pend_sha = true;
        }
        uint8_t fail = 0;
        if (nvs_get_u8(h, "pend_fail", &fail) == ESP_OK) {
            s_pend_fail = fail;
        }
        nvs_close(h);
    }

    ESP_LOGI(TAG, "trust state at boot: %s%s", catrust_state_name(catrust_get_state()),
             s_have_pend_sha ? " (a give-up counter from a prior failed apply survived reboot)" : "");
}

catrust_state_t catrust_get_state(void)
{
    return catrust_derive_state(ident_get_ca_len() > 0, s_broken_cache);
}

const char *catrust_state_name(catrust_state_t state)
{
    switch (state) {
    case CATRUST_UNPINNED: return "unpinned";
    case CATRUST_PINNED: return "pinned";
    case CATRUST_BROKEN: return "broken";
    default: return "?";
    }
}

bool catrust_get_ca_fp(char out[17])
{
    if (catrust_get_state() == CATRUST_UNPINNED) {
        return false;
    }
    catrust_fingerprint_hex(ident_get_ca_hash(), out);
    return true;
}

static void set_broken(bool broken)
{
    if (broken == s_broken_cache) {
        return;
    }
    s_broken_cache = broken;
    ident_set_tls_broken(broken); // power effect: one NVS write, no modem/RRC effect
}

// ---------------------------------------------------------------------------
// Fallback (docs/V02_DESIGN.md §4.2).
// ---------------------------------------------------------------------------

static uint32_t s_validated_fail_streak = 0;
static bool s_last_attempt_validated = true; // matches net_init()'s own "validated whenever pinned" default
static bool s_attempted_validated_since_boot = false;
static int64_t s_last_validated_attempt_us = 0;

catrust_tls_action_t catrust_on_mqtt_tls_fail(void)
{
    catrust_state_t state = catrust_get_state();
    catrust_tls_action_t action =
        catrust_on_tls_fail(state == CATRUST_BROKEN, state != CATRUST_UNPINNED, &s_validated_fail_streak);
    if (action == CATRUST_TLS_FALL_BACK) {
        net_tls_configure(NET_TLS_CA_SLOT, false);
        set_broken(true);
        // docs/V02_DESIGN.md §4.3: "log SECURITY tls-broken device=... on the
        // transition into broken" is the RELAY's own log line (server-side,
        // out of this task's Files list); this is the pager's own mirror of
        // that transition, at INFO per this task's own logging convention.
        ESP_LOGI(TAG, "SECURITY tls-broken: pinned CA failed validation twice in a row - "
                      "falling back to unvalidated MQTT, state pinned -> broken");
    }
    return action;
}

void catrust_before_reconnect(void)
{
    catrust_state_t state = catrust_get_state();
    if (state != CATRUST_BROKEN) {
        s_last_attempt_validated = (state == CATRUST_PINNED);
        return; // UNPINNED/PINNED already have the right profile from net_init()/a prior commit
    }
    int64_t now = esp_timer_get_time();
    bool want_validated =
        catrust_due_for_validated_retry(s_last_validated_attempt_us, now, !s_attempted_validated_since_boot);
    net_tls_configure(NET_TLS_CA_SLOT, want_validated);
    s_last_attempt_validated = want_validated;
    if (want_validated) {
        s_attempted_validated_since_boot = true;
        s_last_validated_attempt_us = now;
        ESP_LOGI(TAG, "broken: cold-boot/24h revalidation window - attempting a validated reconnect");
    }
}

void catrust_on_mqtt_connected(void)
{
    s_validated_fail_streak = 0;
    if (catrust_get_state() == CATRUST_BROKEN && s_last_attempt_validated) {
        set_broken(false);
        ESP_LOGI(TAG, "TLS validated again: state broken -> pinned");
    }
}

// ---------------------------------------------------------------------------
// Two-phase apply (docs/V02_DESIGN.md §4.4).
// ---------------------------------------------------------------------------

typedef enum {
    CATRUST_APPLY_IDLE = 0,
    CATRUST_APPLY_FETCHING,
    CATRUST_APPLY_RECONNECT_WAIT,
} catrust_apply_phase_t;

// Pending-request handoff (MQTT event task -> modes_run()'s task), guarded
// by s_lock/s_unlock. Only one request is remembered at a time, same "only
// the newest unacked cfg is re-published" rule cfg.lock/cfg.ca already share
// server-side -- an in-flight fetch/reconnect trial is not aborted mid-way
// by a second push; it simply is not overwritten until IDLE.
static bool s_pending_present = false;
static bool s_pending_unpin = false;
static char s_pending_url[CATRUST_URL_MAX];
static uint8_t s_pending_sha[32];
static char s_pending_id[MSG_ID_MAX];

// Apply state machine (RAM-only -- see catrust.h's own module comment for
// why the phase itself is deliberately not persisted).
static catrust_apply_phase_t s_phase = CATRUST_APPLY_IDLE;
static char s_apply_id[MSG_ID_MAX];
static uint8_t s_apply_sha[32];
static char s_apply_pem[CAFETCH_PEM_MAX];
static size_t s_apply_pem_len = 0;
static bool s_apply_rollback_validated = false; // profile state to restore on a failed trial
static int64_t s_apply_reconnect_started_us = 0;
#define CATRUST_APPLY_RECONNECT_TIMEOUT_US ((int64_t) 60 * 1000000) // matches modes.c's own connect watchdog

bool catrust_apply_in_progress(void)
{
    return s_phase != CATRUST_APPLY_IDLE;
}

void catrust_apply_cfg_submap(const uint8_t *buf, uint16_t len, const char *id)
{
    catrust_cfg_t cfg;
    if (!catrust_parse_cfg_submap(buf, len, &cfg)) {
        ESP_LOGI(TAG, "malformed cfg.ca sub-map dropped (id=%s)", id ? id : "");
        return;
    }

    s_lock();
    s_pending_present = true;
    s_pending_unpin = cfg.unpin;
    if (!cfg.unpin) {
        strncpy(s_pending_url, cfg.url, sizeof(s_pending_url) - 1);
        s_pending_url[sizeof(s_pending_url) - 1] = '\0';
        memcpy(s_pending_sha, cfg.sha, sizeof(s_pending_sha));
    }
    strncpy(s_pending_id, id ? id : "", sizeof(s_pending_id) - 1);
    s_pending_id[sizeof(s_pending_id) - 1] = '\0';
    s_unlock();

    ESP_LOGI(TAG, "cfg.ca recorded (%s), id=%s - catrust_service() will act on it",
             cfg.unpin ? "un-pin" : cfg.url, id ? id : "");
}

static void persist_pend_fail(const uint8_t sha[32], uint8_t fail_count)
{
    nvs_handle_t h;
    if (nvs_open(CATRUST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, "pend_sha", sha, 32);
    nvs_set_u8(h, "pend_fail", fail_count);
    nvs_commit(h);
    nvs_close(h);
}

static void clear_pend_fail(void)
{
    s_have_pend_sha = false;
    s_pend_fail = 0;
    nvs_handle_t h;
    if (nvs_open(CATRUST_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, "pend_sha");
    nvs_erase_key(h, "pend_fail");
    nvs_commit(h);
    nvs_close(h);
}

// Bumps (or starts) the persisted give-up counter for `sha`, returns the new
// count. A push for a DIFFERENT sha than whatever was pending restarts the
// counter at 1 (a new CA, not a retry of the old one).
static uint8_t bump_pend_fail(const uint8_t sha[32])
{
    if (!s_have_pend_sha || memcmp(s_pend_sha, sha, 32) != 0) {
        memcpy(s_pend_sha, sha, 32);
        s_have_pend_sha = true;
        s_pend_fail = 0;
    }
    s_pend_fail++;
    persist_pend_fail(s_pend_sha, s_pend_fail);
    return s_pend_fail;
}

static void ack_apply(const char *id, bool shown)
{
    if (id && id[0] != '\0' && shown) {
        msg_mark_shown(id);
    }
}

static void begin_unpin(const char *id)
{
    static ident_t snap; // static: same "too big for this task's stack" reasoning modes.c's on_auth_epoch_wrap() documents
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.dev_id, ident_get_dev_id(), sizeof(snap.dev_id) - 1);
    strncpy(snap.mqtt_pw, ident_get_mqtt_pw(), sizeof(snap.mqtt_pw) - 1);
    memcpy(snap.kdev, ident_get_kdev(), sizeof(snap.kdev));
    strncpy(snap.host, ident_get_host(), sizeof(snap.host) - 1);
    snap.port = ident_get_port();
    snap.ca[0] = '\0';
    snap.ca_len = 0;
    strncpy(snap.apn, ident_get_apn(), sizeof(snap.apn) - 1);
    snap.flags = ident_get_flags();
    strncpy(snap.label, ident_get_label(), sizeof(snap.label) - 1);
    memset(snap.ca_hash, 0, sizeof(snap.ca_hash));
    snap.n_epoch = ident_get_n_epoch();
    snap.claimed = ident_get_claimed();

    if (!ident_store(&snap)) {
        ESP_LOGI(TAG, "un-pin: ident_store() failed - CA left pinned, will retry on the next push");
        return;
    }
    net_tls_configure(NET_TLS_CA_SLOT, false);
    set_broken(false);
    clear_pend_fail();
    ESP_LOGI(TAG, "CA un-pinned: state -> unpinned");
    ack_apply(id, true);
}

static void start_fetch(const char *url, const uint8_t sha[32], const char *id)
{
    if (s_have_pend_sha && memcmp(s_pend_sha, sha, 32) == 0 && catrust_give_up(s_pend_fail)) {
        ESP_LOGI(TAG, "giving up on this CA after %u failed applies of the same sha - acking without applying",
                 (unsigned) s_pend_fail);
        ack_apply(id, true);
        return;
    }
    if (!cafetch_begin(url)) {
        ESP_LOGI(TAG, "cfg.ca: could not start the fetch - will retry on the relay's next re-publish");
        uint8_t fail = bump_pend_fail(sha);
        if (catrust_give_up(fail)) {
            ack_apply(id, true);
        }
        return;
    }
    strncpy(s_apply_id, id, sizeof(s_apply_id) - 1);
    s_apply_id[sizeof(s_apply_id) - 1] = '\0';
    memcpy(s_apply_sha, sha, sizeof(s_apply_sha));
    s_phase = CATRUST_APPLY_FETCHING;
}

static void fail_current_apply(const char *why)
{
    ESP_LOGI(TAG, "cfg.ca apply failed (%s), id=%s", why, s_apply_id);
    uint8_t fail = bump_pend_fail(s_apply_sha);
    if (catrust_give_up(fail)) {
        ESP_LOGI(TAG, "giving up after %u failed applies of this sha - acking anyway", (unsigned) fail);
        ack_apply(s_apply_id, true);
    }
    // else: no ack -- "the relay re-publishes on the next online edge"
    // (V02_DESIGN.md §4.4).
    s_phase = CATRUST_APPLY_IDLE;
}

static void service_fetching(void)
{
    cafetch_status_t st = cafetch_poll(esp_timer_get_time());
    if (st == CAFETCH_PENDING) {
        return;
    }
    if (st == CAFETCH_FAILED) {
        cafetch_end();
        fail_current_apply("fetch failed (see the cafetch log line above)");
        return;
    }

    int http_status = 0;
    size_t bytes = 0;
    bool ok = cafetch_result(s_apply_sha, s_apply_pem, sizeof(s_apply_pem), &s_apply_pem_len,
                             &http_status, &bytes);
    cafetch_end();
    if (!ok) {
        ESP_LOGI(TAG, "cfg.ca fetch completed (http=%d bytes=%u) but hash/PEM validation failed",
                 http_status, (unsigned) bytes);
        fail_current_apply("hash mismatch or not exactly one PEM certificate");
        return;
    }
    ESP_LOGI(TAG, "cfg.ca fetch OK: http=%d bytes=%u - writing to scratch slot %u for a trial reconnect",
             http_status, (unsigned) bytes, (unsigned) NET_TLS_CA_SCRATCH_SLOT);

    if (!net_write_ca_slot(NET_TLS_CA_SCRATCH_SLOT, s_apply_pem)) {
        fail_current_apply("net_write_ca_slot(scratch) failed");
        return;
    }

    // Two-phase apply, step 1: point profile 2 at the scratch slot,
    // validated, and reconnect. Hidden from modes.c's own reconnect/
    // watchdog/health-check machinery, same pattern loc.c's route-2 CFUN=4
    // window already established (modes_set_loc_suppress()'s own doc
    // comment).
    s_apply_rollback_validated = (catrust_get_state() == CATRUST_PINNED);
    modes_set_ca_apply_suppress(true);
    net_session_down();
    net_tls_configure(NET_TLS_CA_SCRATCH_SLOT, true);
    net_session_up();
    s_apply_reconnect_started_us = esp_timer_get_time();
    s_phase = CATRUST_APPLY_RECONNECT_WAIT;
}

static void commit_apply(void)
{
    static ident_t snap;
    memset(&snap, 0, sizeof(snap));
    strncpy(snap.dev_id, ident_get_dev_id(), sizeof(snap.dev_id) - 1);
    strncpy(snap.mqtt_pw, ident_get_mqtt_pw(), sizeof(snap.mqtt_pw) - 1);
    memcpy(snap.kdev, ident_get_kdev(), sizeof(snap.kdev));
    strncpy(snap.host, ident_get_host(), sizeof(snap.host) - 1);
    snap.port = ident_get_port();
    strncpy(snap.ca, s_apply_pem, sizeof(snap.ca) - 1);
    snap.ca_len = strlen(snap.ca);
    strncpy(snap.apn, ident_get_apn(), sizeof(snap.apn) - 1);
    snap.flags = ident_get_flags();
    strncpy(snap.label, ident_get_label(), sizeof(snap.label) - 1);
    memcpy(snap.ca_hash, s_apply_sha, sizeof(snap.ca_hash));
    snap.n_epoch = ident_get_n_epoch();
    snap.claimed = ident_get_claimed();

    // ident.h's own field doc: "SHA-256 of the CA currently written to modem
    // slot 12" — computed over the PEM actually being stored (same
    // convention setup.c's bootstrap path now uses), not just copied from
    // the pushed cfg's own `sha` (which cafetch_result() already confirmed
    // matches the raw HTTP body, not necessarily byte-identical to the
    // extracted PEM in the rare case of extra bytes around it).
    mbedtls_sha256((const unsigned char *) snap.ca, snap.ca_len, snap.ca_hash, 0);

    bool stored = ident_store(&snap);
    net_write_ca_slot(NET_TLS_CA_SLOT, s_apply_pem); // best-effort; profile already points at the scratch
                                                     // slot's bytes for THIS session either way
    net_tls_configure(NET_TLS_CA_SLOT, true);
    set_broken(false);
    modes_set_ca_apply_suppress(false);

    if (!stored) {
        // The live session is already connected and validated against the
        // new CA (harmless to leave running), but the identity itself did
        // not persist — telling the relay this succeeded (or letting the
        // give-up counter forget about it) would be wrong. Count it as a
        // failure of THIS apply so a reboot before the next push retries
        // ident_store() from a clean fetch, and the give-up counter still
        // bounds a persistently-failing NVS write.
        ESP_LOGI(TAG, "cfg.ca apply: live session now validated on the new CA, but ident_store() "
                      "FAILED - not acking, will retry");
        fail_current_apply("ident_store() failed after a validated commit");
        return;
    }

    clear_pend_fail();
    ESP_LOGI(TAG, "cfg.ca apply COMMITTED: state -> pinned");
    ack_apply(s_apply_id, true);
    s_phase = CATRUST_APPLY_IDLE;
}

static void rollback_apply(const char *why)
{
    ESP_LOGI(TAG, "cfg.ca apply trial failed (%s) - rolling back to slot %u, %s", why,
             (unsigned) NET_TLS_CA_SLOT, s_apply_rollback_validated ? "validated" : "unvalidated");
    net_session_down();
    net_tls_configure(NET_TLS_CA_SLOT, s_apply_rollback_validated);
    net_session_up(); // modes.c's own F1/F3 backoff takes over from here if this does not immediately connect
    modes_set_ca_apply_suppress(false);
    fail_current_apply(why);
}

static void service_reconnect_wait(const net_mqtt_status_t *st)
{
    if (st->mqtt_connected) {
        commit_apply();
        return;
    }
    if (st->disconnect_edge) {
        rollback_apply("scratch-slot trial connect failed/disconnected");
        return;
    }
    if ((esp_timer_get_time() - s_apply_reconnect_started_us) >= CATRUST_APPLY_RECONNECT_TIMEOUT_US) {
        rollback_apply("scratch-slot trial connect timed out");
    }
}

void catrust_service(const net_mqtt_status_t *st)
{
    switch (s_phase) {
    case CATRUST_APPLY_IDLE: {
        // The whole pending request (not just the present/unpin flags) is
        // copied out under the lock before it is ever read again — a fresh
        // push (catrust_apply_cfg_submap(), the MQTT event task) could
        // otherwise overwrite s_pending_url/s_pending_sha/s_pending_id out
        // from under begin_unpin()/start_fetch() the instant s_pending_present
        // is cleared, since neither of those functions runs with the lock
        // held (both call ident_store()/cafetch_begin(), which must not run
        // under a lock also taken by the RTC-mutex's other, much shorter
        // critical sections elsewhere in this codebase).
        bool present, unpin;
        char id[MSG_ID_MAX];
        char url[CATRUST_URL_MAX];
        uint8_t sha[32];
        s_lock();
        present = s_pending_present;
        unpin = s_pending_unpin;
        strncpy(id, s_pending_id, sizeof(id) - 1);
        id[sizeof(id) - 1] = '\0';
        strncpy(url, s_pending_url, sizeof(url) - 1);
        url[sizeof(url) - 1] = '\0';
        memcpy(sha, s_pending_sha, sizeof(sha));
        if (present) {
            s_pending_present = false;
        }
        s_unlock();
        if (!present) {
            return;
        }
        if (unpin) {
            begin_unpin(id);
        } else {
            start_fetch(url, sha, id);
        }
        break;
    }
    case CATRUST_APPLY_FETCHING:
        service_fetching();
        break;
    case CATRUST_APPLY_RECONNECT_WAIT:
        service_reconnect_wait(st);
        break;
    }
}

// ---------------------------------------------------------------------------
// `cafetch` debug console command (main.c, PAGER_DEBUG_NO_LIGHT_SLEEP builds
// only). Fetch-only, no apply.
// ---------------------------------------------------------------------------

static bool hex_to_sha(const char *hex, uint8_t sha[32])
{
    if (strlen(hex) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1) {
            return false;
        }
        sha[i] = (uint8_t) v;
    }
    return true;
}

bool catrust_debug_cafetch(const char *url, const char *sha256_hex)
{
    uint8_t sha[32];
    if (!hex_to_sha(sha256_hex, sha)) {
        ESP_LOGI(TAG, "cafetch: sha256hex must be exactly 64 hex characters");
        return false;
    }
    if (cafetch_in_progress() || catrust_apply_in_progress()) {
        ESP_LOGI(TAG, "cafetch: a fetch or apply is already in progress");
        return false;
    }

    static char pem[CAFETCH_PEM_MAX];
    size_t pem_len = 0;
    int http_status = 0;
    size_t bytes = 0;
    uint32_t elapsed_ms = 0;
    bool mqtt_survived = false;
    bool ok = cafetch_run_blocking(url, sha, pem, sizeof(pem), &pem_len, &http_status, &bytes,
                                   &elapsed_ms, &mqtt_survived);
    ESP_LOGI(TAG,
             "cafetch %s: %s - http_status=%d bytes=%u elapsed=%ums hash_match=%d "
             "mqtt_survived_second_socket=%d (UNVERIFIED on hardware until this line is seen)",
             url, ok ? "OK" : "FAILED", http_status, (unsigned) bytes, (unsigned) elapsed_ms, (int) ok,
             (int) mqtt_survived);
    return ok;
}

#endif /* ESP_PLATFORM */

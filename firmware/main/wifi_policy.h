/* wifi_policy.h — WiFi transport selection policy (docs/WIFI_DESIGN.md §2).
 *
 * Split the same way coverage.h is (see that header's own module comment):
 * pure C, no ESP-IDF dependency, no I/O, no logging, driven entirely by a
 * caller-supplied snapshot of the world (`wifi_policy_in_t`) and a
 * caller-supplied clock (`int64_t now_us`, esp_timer_get_time()-shaped but
 * never called directly from here) — so firmware/host/test_wifi_policy.c can
 * run every transition without a device. The device-only callers (wifi_sta.c,
 * xport_wifi.c, a later task) own every actual association/MQTT/NVS call
 * this policy's returned action implies; this module only decides WHEN,
 * never calls anything itself.
 *
 * Design (docs/WIFI_DESIGN.md §2):
 *  - Default is LTE. WiFi is never preferred unless `user_enabled` is true,
 *    `have_creds` is true, AND `ca_pinned` is true (§5: WiFi requires a
 *    pinned CA, no plaintext fallback, ever) — these three gates are
 *    checked on every wifi_policy_step() call, not just once.
 *  - Once associated (`sta_associated`) with `rssi_dbm >= WIFI_RSSI_ENTER_DBM`,
 *    the policy issues WIFI_ACT_MQTT_UP exactly once per attempt cycle.
 *  - Falls back to LTE (WIFI_ACT_DROP_TO_LTE) on: 3 consecutive
 *    assoc/DHCP/TLS/CONNECT failures (wifi_policy_note_failure());
 *    2 MQTT disconnects within WIFI_FLAP_WINDOW_S (wifi_policy_note_disconnect());
 *    rssi below WIFI_RSSI_EXIT_DBM held continuously for WIFI_RSSI_EXIT_HOLD_S;
 *    or loss of association held continuously for WIFI_ASSOC_LOST_S once a
 *    WiFi MQTT session had been brought up.
 *  - Hysteresis: entry needs WIFI_RSSI_ENTER_DBM (-70), exit needs
 *    WIFI_RSSI_EXIT_DBM (-80) held WIFI_RSSI_EXIT_HOLD_S (30s) — a 10 dB gap
 *    plus a dwell time, so a single noisy sample near the boundary never
 *    flaps the transport.
 *  - After any fallback, the next WiFi attempt is gated by a backoff ladder
 *    (WIFI_RETRY_BACKOFF_S: 10/30/60 min, capped at the last entry), which
 *    escalates one step on each fallback and resets to the first step only
 *    on a WIFI_STABLE_S (10 min) unbroken good session or an explicit
 *    wifi_policy_reset() (a user toggle).
 *
 * `wifi_policy_t` is a plain struct (not opaque), same convention
 * coverage_policy_t uses, so firmware/host/test_wifi_policy.c can inspect
 * fields directly where that is the clearest way to pin a transition (e.g.
 * `p.backoff_level`) rather than only asserting on wifi_policy_step()'s
 * return value.
 */
#ifndef WIFI_POLICY_H
#define WIFI_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WIFI_XPORT_LTE = 0, WIFI_XPORT_WIFI } wifi_xport_t;

typedef enum {
    WIFI_ACT_NONE = 0,   /* nothing to do this call */
    WIFI_ACT_ASSOCIATE,  /* caller must: start associating with the cached credentials */
    WIFI_ACT_MQTT_UP,    /* caller must: net_xport_switch(NET_XPORT_WIFI), bring the MQTT client up */
    WIFI_ACT_DROP_TO_LTE, /* caller must: tear the WiFi MQTT session down, net_xport_switch(NET_XPORT_LTE) */
    WIFI_ACT_SCAN,       /* reserved for W11's trigger-driven discovery scan (docs/WIFI_DESIGN.md §3b);
                          * never returned by this phase-1 implementation. */
} wifi_policy_action_t;

typedef struct {
    bool user_enabled;   /* console `wifi on`, device menu, or cfg.wifi.en */
    bool have_creds;     /* wificred_count() > 0 */
    bool ca_pinned;      /* catrust_get_state() == CATRUST_PINNED (§5: no other state may use WiFi) */
    bool sta_associated; /* esp_wifi's current association state */
    bool mqtt_up;        /* the WiFi esp-mqtt client is SUBSCRIBED (net.h's "SUBSCRIBED, not just CONNECTED" rule) */
    int rssi_dbm;        /* meaningless unless sta_associated; from wifi_ap_record_t once associated */
} wifi_policy_in_t;

/* ---------------------------------------------------------------------
 * Constants (docs/WIFI_TASKS.md W1, docs/WIFI_DESIGN.md §2). Named #defines
 * so they can be retuned without touching the logic above.
 * --------------------------------------------------------------------- */
#define WIFI_RSSI_ENTER_DBM (-70)
#define WIFI_RSSI_EXIT_DBM (-80)
#define WIFI_RSSI_EXIT_HOLD_S 30u
#define WIFI_FAIL_MAX 3u
#define WIFI_FLAP_WINDOW_S 120u
#define WIFI_FLAP_MAX 2u
#define WIFI_ASSOC_LOST_S 20u
#define WIFI_RETRY_BACKOFF_LEVELS 3u
#define WIFI_RETRY_BACKOFF_S \
    { 600u, 1800u, 3600u } /* 10 / 30 / 60 min, capped at the last entry */
#define WIFI_STABLE_S 600u /* 10 min unbroken good session resets the backoff ladder */

typedef struct {
    wifi_xport_t desired; /* the transport this policy currently wants active */

    /* Edge-trigger memory for the two "issue exactly once per cycle" actions. */
    bool associate_issued;
    bool mqtt_up_issued;

    /* Consecutive assoc/DHCP/TLS/CONNECT failure count (wifi_policy_note_failure()). */
    uint32_t fail_count;

    /* MQTT-disconnect flap window (wifi_policy_note_disconnect()). */
    bool have_last_disconnect;
    int64_t last_disconnect_us;
    uint32_t flap_count;

    /* Set by note_failure()/note_disconnect() when a threshold trips; consumed
     * (one-shot) by the next wifi_policy_step() call, which is the only place
     * that actually emits WIFI_ACT_DROP_TO_LTE and arms the backoff timer --
     * same "event sets a flag, step() surfaces the action" split coverage.c's
     * own module comment documents. */
    bool drop_pending;

    /* RSSI exit hysteresis (held continuously below WIFI_RSSI_EXIT_DBM). */
    bool rssi_bad_active;
    int64_t rssi_bad_since_us;

    /* Association-loss hold, only armed once a WiFi MQTT session has actually
     * been brought up (mqtt_up_issued) -- losing association while merely
     * attempting to associate is not a "fallback", it is just still waiting. */
    bool assoc_lost_active;
    int64_t assoc_lost_since_us;

    /* Backoff ladder: index into WIFI_RETRY_BACKOFF_S, capped at
     * WIFI_RETRY_BACKOFF_LEVELS-1. backoff_until_us is 0 until the first
     * fallback ever happens, which is always in the past relative to any
     * real now_us, so a fresh policy never gates the first attempt. */
    uint32_t backoff_level;
    int64_t backoff_until_us;

    /* Stable-session tracking: WIFI_STABLE_S of unbroken (sta_associated &&
     * mqtt_up) resets backoff_level to 0. */
    bool up_since_active;
    int64_t up_since_us;
} wifi_policy_t;

void wifi_policy_init(wifi_policy_t *p);

/* One decision per call, driven by the current snapshot `in` and clock
 * `now_us`. Returns the ONE action the caller must perform before the next
 * call (same discipline as coverage_step()) -- WIFI_ACT_NONE the
 * overwhelming majority of calls. Never returns WIFI_ACT_SCAN in this phase-1
 * implementation (see wifi_policy_action_t's own doc comment). */
wifi_policy_action_t wifi_policy_step(wifi_policy_t *p, const wifi_policy_in_t *in, int64_t now_us);

/* The transport this policy currently wants active — LTE until a
 * WIFI_ACT_MQTT_UP has been issued (or the caller reports sta_associated &&
 * mqtt_up directly), LTE again from the instant a WIFI_ACT_DROP_TO_LTE (or
 * the hard `user_enabled`/`have_creds`/`ca_pinned` gate) fires. For
 * net_xport_active()'s eventual use (a later task) — this module has no
 * knowledge of net.h itself. */
wifi_xport_t wifi_policy_desired(const wifi_policy_t *p);

/* Call on any of: association failure, DHCP failure, TLS handshake failure,
 * MQTT CONNECT failure (docs/WIFI_DESIGN.md §2's "3 consecutive failures of
 * {association, DHCP, TLS handshake, MQTT CONNECT}"). Increments the
 * consecutive-failure count; at WIFI_FAIL_MAX, arms a one-shot
 * WIFI_ACT_DROP_TO_LTE for the next wifi_policy_step() call and resets the
 * count. `now_us` is accepted for interface symmetry with
 * wifi_policy_note_disconnect() but not currently used by the failure
 * count (no time window on consecutive failures, unlike the disconnect
 * flap window below). */
void wifi_policy_note_failure(wifi_policy_t *p, int64_t now_us);

/* Call on every MQTT disconnect while the WiFi transport is active. Two
 * disconnects no more than WIFI_FLAP_WINDOW_S apart arm a one-shot
 * WIFI_ACT_DROP_TO_LTE for the next wifi_policy_step() call. A gap strictly
 * greater than WIFI_FLAP_WINDOW_S restarts the count at 1 rather than
 * accumulating across unrelated, well-separated disconnects. */
void wifi_policy_note_disconnect(wifi_policy_t *p, int64_t now_us);

/* User toggle (console `wifi on`/`wifi off`, device menu, or a `cfg.wifi`
 * push that changes `en` or the credential set): resets every piece of
 * this policy's memory, including the backoff ladder (docs/WIFI_DESIGN.md
 * §2: "reset ... by any user toggle") — equivalent to wifi_policy_init(). */
void wifi_policy_reset(wifi_policy_t *p);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_POLICY_H */

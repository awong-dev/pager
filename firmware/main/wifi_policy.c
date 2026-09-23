// wifi_policy.c — see wifi_policy.h for the policy, the constants and the
// design rationale (docs/WIFI_DESIGN.md §2).
//
// Pure C, no ESP-IDF dependency (host-tested by
// firmware/host/test_wifi_policy.c) — wifi_sta.c/xport_wifi.c (a later task)
// are the only callers and own every actual association/MQTT/net_xport_switch()
// call this policy's returned action implies.

#include "wifi_policy.h"

#include <string.h>

#define US_PER_S ((int64_t) 1000000)

void wifi_policy_init(wifi_policy_t *p) { memset(p, 0, sizeof(*p)); }

void wifi_policy_reset(wifi_policy_t *p) { wifi_policy_init(p); }

wifi_xport_t wifi_policy_desired(const wifi_policy_t *p) { return p->desired; }

// Clears every piece of "we are attempting/using WiFi right now" memory
// without touching the failure/flap/backoff bookkeeping, which is owned by
// the drop/backoff logic in wifi_policy_step() itself.
static void enter_lte(wifi_policy_t *p)
{
    p->desired = WIFI_XPORT_LTE;
    p->associate_issued = false;
    p->mqtt_up_issued = false;
    p->rssi_bad_active = false;
    p->assoc_lost_active = false;
    p->up_since_active = false;
}

// Arms the backoff timer at the CURRENT ladder level, then escalates the
// level for the NEXT fallback (capped at the last entry) — docs/WIFI_DESIGN.md
// §2: "10 / 30 / 60 minutes, capped ... reset by a successful 10-minute WiFi
// session or by any user toggle."
static void arm_backoff(wifi_policy_t *p, int64_t now_us)
{
    static const uint32_t ladder[] = WIFI_RETRY_BACKOFF_S;
    uint32_t idx = p->backoff_level;
    if (idx >= WIFI_RETRY_BACKOFF_LEVELS) {
        idx = WIFI_RETRY_BACKOFF_LEVELS - 1;
    }
    p->backoff_until_us = now_us + (int64_t) ladder[idx] * US_PER_S;
    if (p->backoff_level + 1 < WIFI_RETRY_BACKOFF_LEVELS) {
        p->backoff_level++;
    }
}

void wifi_policy_note_failure(wifi_policy_t *p, int64_t now_us)
{
    (void) now_us; // no time window on consecutive failures, unlike the disconnect flap window
    p->fail_count++;
    if (p->fail_count >= WIFI_FAIL_MAX) {
        p->drop_pending = true;
        p->fail_count = 0;
    }
}

void wifi_policy_note_disconnect(wifi_policy_t *p, int64_t now_us)
{
    if (p->have_last_disconnect && (now_us - p->last_disconnect_us) <= (int64_t) WIFI_FLAP_WINDOW_S * US_PER_S) {
        p->flap_count++;
    } else {
        p->flap_count = 1;
    }
    p->have_last_disconnect = true;
    p->last_disconnect_us = now_us;
    if (p->flap_count >= WIFI_FLAP_MAX) {
        p->drop_pending = true;
        p->flap_count = 0;
    }
}

wifi_policy_action_t wifi_policy_step(wifi_policy_t *p, const wifi_policy_in_t *in, int64_t now_us)
{
    // Hard gates (docs/WIFI_DESIGN.md §2 and §5): WiFi is never preferred,
    // and never stays preferred, without ALL THREE of these -- checked every
    // call, not just on entry. A CA that stops being pinned mid-session (a
    // `cfg.ca` un-pin, or a TLS-fail fallback to broken) must drop WiFi
    // immediately, same as the user turning it off.
    if (!in->user_enabled || !in->have_creds || !in->ca_pinned) {
        bool was_active = (p->desired == WIFI_XPORT_WIFI) || p->associate_issued;
        enter_lte(p);
        return was_active ? WIFI_ACT_DROP_TO_LTE : WIFI_ACT_NONE;
    }

    // A threshold tripped by note_failure()/note_disconnect() since the last
    // call -- surfaced here, one-shot, with the backoff timer armed.
    if (p->drop_pending) {
        p->drop_pending = false;
        enter_lte(p);
        arm_backoff(p, now_us);
        return WIFI_ACT_DROP_TO_LTE;
    }

    // RSSI exit hysteresis: held continuously below WIFI_RSSI_EXIT_DBM for
    // WIFI_RSSI_EXIT_HOLD_S. Only meaningful once actually associated (rssi_dbm
    // is meaningless otherwise, per wifi_policy_in_t's own doc comment).
    if (in->sta_associated && in->rssi_dbm < WIFI_RSSI_EXIT_DBM) {
        if (!p->rssi_bad_active) {
            p->rssi_bad_active = true;
            p->rssi_bad_since_us = now_us;
        } else if (now_us - p->rssi_bad_since_us >= (int64_t) WIFI_RSSI_EXIT_HOLD_S * US_PER_S) {
            enter_lte(p);
            arm_backoff(p, now_us);
            return WIFI_ACT_DROP_TO_LTE;
        }
    } else {
        p->rssi_bad_active = false;
    }

    // Association-loss hold: only armed once a WiFi MQTT session has actually
    // been brought up -- losing association while still attempting to
    // associate is not a fallback, it is just still waiting (the ASSOCIATE
    // branch below handles that case). While this hold is counting down (or
    // has just tripped), return here unconditionally rather than falling
    // through to the "not associated -> ASSOCIATE" branch below, which would
    // otherwise treat this same !sta_associated sample as "start a fresh
    // attempt cycle" instead of "still within the loss-of-association grace
    // window".
    if (p->mqtt_up_issued && !in->sta_associated) {
        if (!p->assoc_lost_active) {
            p->assoc_lost_active = true;
            p->assoc_lost_since_us = now_us;
            return WIFI_ACT_NONE;
        }
        if (now_us - p->assoc_lost_since_us >= (int64_t) WIFI_ASSOC_LOST_S * US_PER_S) {
            enter_lte(p);
            arm_backoff(p, now_us);
            return WIFI_ACT_DROP_TO_LTE;
        }
        return WIFI_ACT_NONE; // still within the grace window, waiting to see if it recovers
    }
    p->assoc_lost_active = false;

    // Backoff gate: no new attempt cycle until it clears. backoff_until_us
    // starts at 0 (wifi_policy_init()'s memset), always in the past relative
    // to any real now_us, so a fresh policy is never gated on its first try.
    if (now_us < p->backoff_until_us) {
        return WIFI_ACT_NONE;
    }

    if (!in->sta_associated) {
        if (!p->associate_issued) {
            p->associate_issued = true;
            return WIFI_ACT_ASSOCIATE;
        }
        return WIFI_ACT_NONE; // already issued this cycle, still waiting
    }

    if (!in->mqtt_up) {
        if (in->rssi_dbm >= WIFI_RSSI_ENTER_DBM) {
            if (!p->mqtt_up_issued) {
                p->mqtt_up_issued = true;
                p->desired = WIFI_XPORT_WIFI;
                p->fail_count = 0; // a good connect clears the failure streak
                return WIFI_ACT_MQTT_UP;
            }
            return WIFI_ACT_NONE; // already issued, MQTT_UP still in flight
        }
        return WIFI_ACT_NONE; // associated, but not yet above the entry threshold
    }

    // Associated AND mqtt_up: a usable WiFi session. Derived from the current
    // inputs directly (not just edge-triggered memory) so a caller reporting
    // an already-up session (e.g. this exact state handed in synthetically)
    // is still recognised, not just a session this policy itself brought up.
    p->desired = WIFI_XPORT_WIFI;
    p->mqtt_up_issued = true;
    if (!p->up_since_active) {
        p->up_since_active = true;
        p->up_since_us = now_us;
    } else if (now_us - p->up_since_us >= (int64_t) WIFI_STABLE_S * US_PER_S) {
        p->backoff_level = 0; // docs/WIFI_DESIGN.md §2: a 10-minute good session resets the ladder
    }
    return WIFI_ACT_NONE;
}

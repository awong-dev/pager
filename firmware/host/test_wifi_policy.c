/* test_wifi_policy.c — host test harness for main/wifi_policy.c
 * (docs/WIFI_TASKS.md W1, docs/WIFI_DESIGN.md §2): the pure WiFi transport
 * selection state machine.
 *
 * Coverage required by the task brief:
 *  - never leaves LTE when user_enabled is false
 *  - never leaves LTE when ca_pinned is false (design §5)
 *  - enters WiFi at -70 dBm and not at -71 dBm
 *  - does not leave at -75 dBm (hysteresis)
 *  - leaves after -81 dBm held 30s but not at 29s
 *  - three consecutive failures drop to LTE
 *  - two disconnects in 120s drop to LTE, two in 121s do not
 *  - the 10/30/60 min backoff ladder and its cap
 *  - a 10-minute good session resets the ladder
 *  - wifi_policy_reset() clears everything
 *
 * Plus one extra beyond the task's own list (the design's own §2 also names
 * it as a fallback trigger, so it is worth pinning even though the task's
 * Verify section does not call it out by name):
 *  - loss of association held 20s (once a WiFi MQTT session was up) drops to LTE
 */
#include "wifi_policy.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                          \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

#define US_PER_S ((int64_t) 1000000)

static wifi_policy_in_t base_in(void)
{
    wifi_policy_in_t in;
    memset(&in, 0, sizeof(in));
    in.user_enabled = true;
    in.have_creds = true;
    in.ca_pinned = true;
    in.sta_associated = false;
    in.mqtt_up = false;
    in.rssi_dbm = 0;
    return in;
}

/* ---------------------------------------------------------------------
 * Hard gates: never leave LTE without user_enabled/have_creds/ca_pinned.
 * --------------------------------------------------------------------- */
static void test_never_leaves_lte_without_user_enabled(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();
    in.user_enabled = false;
    in.sta_associated = true; /* even a great signal must not matter */
    in.rssi_dbm = -40;

    for (int i = 0; i < 5; i++) {
        wifi_policy_action_t a = wifi_policy_step(&p, &in, i * US_PER_S);
        CHECK(a != WIFI_ACT_ASSOCIATE && a != WIFI_ACT_MQTT_UP,
              "call %d: user_enabled=false must never issue ASSOCIATE/MQTT_UP, got %d", i, (int) a);
    }
    CHECK(wifi_policy_desired(&p) == WIFI_XPORT_LTE, "desired must stay LTE");
}

static void test_never_leaves_lte_without_ca_pinned(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();
    in.ca_pinned = false;
    in.sta_associated = true;
    in.rssi_dbm = -40;

    for (int i = 0; i < 5; i++) {
        wifi_policy_action_t a = wifi_policy_step(&p, &in, i * US_PER_S);
        CHECK(a != WIFI_ACT_ASSOCIATE && a != WIFI_ACT_MQTT_UP,
              "call %d: ca_pinned=false must never issue ASSOCIATE/MQTT_UP, got %d", i, (int) a);
    }
    CHECK(wifi_policy_desired(&p) == WIFI_XPORT_LTE, "desired must stay LTE");
}

/* ---------------------------------------------------------------------
 * RSSI entry threshold: -70 enters, -71 does not.
 * --------------------------------------------------------------------- */
static void test_enters_at_70_not_71(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();
    in.sta_associated = true;
    in.mqtt_up = false;
    in.rssi_dbm = -71;

    wifi_policy_action_t a = wifi_policy_step(&p, &in, 0);
    CHECK(a != WIFI_ACT_MQTT_UP, "-71 dBm must not clear the entry threshold");

    wifi_policy_init(&p);
    in.rssi_dbm = -70;
    a = wifi_policy_step(&p, &in, 0);
    CHECK(a == WIFI_ACT_MQTT_UP, "-70 dBm must clear the entry threshold, got %d", (int) a);
    CHECK(wifi_policy_desired(&p) == WIFI_XPORT_WIFI, "desired must become WIFI on MQTT_UP");
}

/* ---------------------------------------------------------------------
 * RSSI exit hysteresis: -75 (between the two thresholds) never drops;
 * -81 held 30s drops, -81 held only 29s does not.
 * --------------------------------------------------------------------- */
static void test_hysteresis_no_drop_at_75(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();
    in.sta_associated = true;
    in.mqtt_up = true;
    in.rssi_dbm = -75;

    for (int i = 0; i < 5; i++) {
        wifi_policy_action_t a = wifi_policy_step(&p, &in, i * 10 * US_PER_S);
        CHECK(a != WIFI_ACT_DROP_TO_LTE, "call %d: -75 dBm must never trigger a drop", i);
    }
    CHECK(wifi_policy_desired(&p) == WIFI_XPORT_WIFI, "must still want WiFi at -75 dBm");
}

static void test_drop_after_81_held_30s_not_29s(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();
    in.sta_associated = true;
    in.mqtt_up = true;
    in.rssi_dbm = -81;

    int64_t t0 = 1000 * US_PER_S;
    wifi_policy_action_t a = wifi_policy_step(&p, &in, t0);
    CHECK(a == WIFI_ACT_NONE, "first bad-rssi sample must not drop immediately");

    a = wifi_policy_step(&p, &in, t0 + (WIFI_RSSI_EXIT_HOLD_S - 1) * US_PER_S);
    CHECK(a == WIFI_ACT_NONE, "must not drop 1s before the hold elapses (29s)");

    a = wifi_policy_step(&p, &in, t0 + (int64_t) WIFI_RSSI_EXIT_HOLD_S * US_PER_S);
    CHECK(a == WIFI_ACT_DROP_TO_LTE, "must drop exactly once the 30s hold elapses");
    CHECK(wifi_policy_desired(&p) == WIFI_XPORT_LTE, "desired must revert to LTE after the drop");
}

/* ---------------------------------------------------------------------
 * Three consecutive assoc/DHCP/TLS/CONNECT failures drop to LTE.
 * --------------------------------------------------------------------- */
static void test_three_failures_drop(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();

    wifi_policy_note_failure(&p, 0);
    wifi_policy_note_failure(&p, 1 * US_PER_S);
    /* Not yet at WIFI_FAIL_MAX -- must not drop. (The very first step() call
     * on a fresh policy is also the initial ASSOCIATE, sta_associated still
     * being false, so this deliberately checks "not a drop" rather than
     * "no action at all".) */
    wifi_policy_action_t a = wifi_policy_step(&p, &in, 1 * US_PER_S);
    CHECK(a != WIFI_ACT_DROP_TO_LTE, "two failures alone must not drop yet");

    wifi_policy_note_failure(&p, 2 * US_PER_S);
    a = wifi_policy_step(&p, &in, 2 * US_PER_S);
    CHECK(a == WIFI_ACT_DROP_TO_LTE, "the third consecutive failure must drop on the next step()");
}

/* ---------------------------------------------------------------------
 * Two MQTT disconnects within 120s drop to LTE; 121s apart do not.
 * --------------------------------------------------------------------- */
static void test_two_disconnects_120s_drop(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();

    wifi_policy_note_disconnect(&p, 0);
    /* Same "not a drop" rather than "no action" caveat as
     * test_three_failures_drop() -- the first step() on a fresh policy is
     * also the initial ASSOCIATE. */
    wifi_policy_action_t a = wifi_policy_step(&p, &in, 0);
    CHECK(a != WIFI_ACT_DROP_TO_LTE, "one disconnect alone must not drop");

    wifi_policy_note_disconnect(&p, (int64_t) WIFI_FLAP_WINDOW_S * US_PER_S);
    a = wifi_policy_step(&p, &in, (int64_t) WIFI_FLAP_WINDOW_S * US_PER_S);
    CHECK(a == WIFI_ACT_DROP_TO_LTE, "two disconnects exactly 120s apart must drop");
}

static void test_two_disconnects_121s_no_drop(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();

    wifi_policy_note_disconnect(&p, 0);
    wifi_policy_action_t a = wifi_policy_step(&p, &in, 0);
    CHECK(a != WIFI_ACT_DROP_TO_LTE, "one disconnect alone must not drop");

    int64_t gap = ((int64_t) WIFI_FLAP_WINDOW_S + 1) * US_PER_S;
    wifi_policy_note_disconnect(&p, gap);
    a = wifi_policy_step(&p, &in, gap);
    CHECK(a != WIFI_ACT_DROP_TO_LTE, "two disconnects 121s apart must NOT drop (outside the flap window)");
}

/* ---------------------------------------------------------------------
 * Backoff ladder: 10 / 30 / 60 min, capped at 60.
 * --------------------------------------------------------------------- */
static void test_backoff_ladder_and_cap(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();

    static const uint32_t expect_s[] = { 600, 1800, 3600, 3600, 3600 };
    int64_t now = 0;

    for (size_t i = 0; i < sizeof(expect_s) / sizeof(expect_s[0]); i++) {
        /* Drive a fallback via three consecutive failures. */
        wifi_policy_note_failure(&p, now);
        wifi_policy_note_failure(&p, now);
        wifi_policy_note_failure(&p, now);
        wifi_policy_action_t a = wifi_policy_step(&p, &in, now);
        CHECK(a == WIFI_ACT_DROP_TO_LTE, "step %zu: three failures must drop", i);

        int64_t expect_until = now + (int64_t) expect_s[i] * US_PER_S;
        CHECK(p.backoff_until_us == expect_until,
              "step %zu: expected backoff_until_us=%lld, got %lld", i, (long long) expect_until,
              (long long) p.backoff_until_us);

        /* Must not associate one second before the backoff clears. */
        a = wifi_policy_step(&p, &in, expect_until - 1);
        CHECK(a == WIFI_ACT_NONE, "step %zu: must not associate before the backoff clears", i);

        /* Backoff clears: re-attempt (ASSOCIATE), which sets up the next
         * fallback's own failures. */
        now = expect_until;
        a = wifi_policy_step(&p, &in, now);
        CHECK(a == WIFI_ACT_ASSOCIATE, "step %zu: must re-attempt exactly once the backoff clears", i);
    }
}

/* ---------------------------------------------------------------------
 * A 10-minute good (associated + mqtt_up) session resets the ladder.
 * --------------------------------------------------------------------- */
static void test_stable_session_resets_ladder(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();

    /* Elevate the ladder with two fallbacks first (10 min, then 30 min). */
    int64_t now = 0;
    wifi_policy_note_failure(&p, now);
    wifi_policy_note_failure(&p, now);
    wifi_policy_note_failure(&p, now);
    wifi_policy_step(&p, &in, now);
    CHECK(p.backoff_level == 1, "test setup: one fallback must advance the ladder to level 1");
    now = p.backoff_until_us;
    wifi_policy_step(&p, &in, now); /* re-attempt (ASSOCIATE) */
    wifi_policy_note_failure(&p, now);
    wifi_policy_note_failure(&p, now);
    wifi_policy_note_failure(&p, now);
    wifi_policy_step(&p, &in, now);
    CHECK(p.backoff_level == 2, "test setup: a second fallback must advance the ladder to level 2");

    /* Now hold a good session for WIFI_STABLE_S. */
    now = p.backoff_until_us;
    in.sta_associated = true;
    in.mqtt_up = true;
    in.rssi_dbm = -40;
    wifi_policy_action_t a = wifi_policy_step(&p, &in, now);
    CHECK(a == WIFI_ACT_NONE, "an already-up session must not re-issue MQTT_UP");
    CHECK(p.backoff_level == 2, "the ladder must not reset before the stable window elapses");

    now += (int64_t) WIFI_STABLE_S * US_PER_S;
    wifi_policy_step(&p, &in, now);
    CHECK(p.backoff_level == 0, "a %us unbroken good session must reset the ladder to level 0",
          WIFI_STABLE_S);

    /* Prove the reset actually took effect: the NEXT fallback must use the
     * first ladder step (600s) again, not continue escalating from level 2. */
    wifi_policy_in_t in2 = base_in();
    wifi_policy_note_failure(&p, now);
    wifi_policy_note_failure(&p, now);
    wifi_policy_note_failure(&p, now);
    a = wifi_policy_step(&p, &in2, now);
    CHECK(a == WIFI_ACT_DROP_TO_LTE, "test: a fresh fallback after the reset must still drop");
    CHECK(p.backoff_until_us == now + 600 * US_PER_S,
          "the next fallback after a ladder reset must use the FIRST step (600s) again, got until=%lld "
          "(now=%lld)",
          (long long) p.backoff_until_us, (long long) now);
}

/* ---------------------------------------------------------------------
 * wifi_policy_reset() clears everything.
 * --------------------------------------------------------------------- */
static void test_reset_clears_everything(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();

    /* Drive some state: a fallback (elevates the ladder, sets desired/LTE
     * already, but touches backoff/fail bookkeeping), then a good session. */
    wifi_policy_note_failure(&p, 0);
    wifi_policy_note_failure(&p, 0);
    wifi_policy_note_failure(&p, 0);
    wifi_policy_step(&p, &in, 0);
    CHECK(p.backoff_level > 0, "test setup: backoff_level must be nonzero before reset");

    in.sta_associated = true;
    in.mqtt_up = true;
    in.rssi_dbm = -40;
    wifi_policy_step(&p, &in, p.backoff_until_us);

    wifi_policy_reset(&p);

    wifi_policy_t zero;
    wifi_policy_init(&zero);
    CHECK(memcmp(&p, &zero, sizeof(p)) == 0, "wifi_policy_reset() must clear every field");
    CHECK(wifi_policy_desired(&p) == WIFI_XPORT_LTE, "desired must be LTE after reset");
}

/* ---------------------------------------------------------------------
 * Beyond the task's own Verify list, but named in docs/WIFI_DESIGN.md §2:
 * loss of association held 20s (once a WiFi session was up) drops to LTE.
 * --------------------------------------------------------------------- */
static void test_assoc_loss_20s_drops(void)
{
    wifi_policy_t p;
    wifi_policy_init(&p);
    wifi_policy_in_t in = base_in();
    in.sta_associated = true;
    in.mqtt_up = true;
    in.rssi_dbm = -40;

    int64_t t0 = 5000 * US_PER_S;
    wifi_policy_action_t a = wifi_policy_step(&p, &in, t0);
    CHECK(a == WIFI_ACT_NONE, "test setup: a good session must not itself act");

    /* The hold is measured from the first sample the policy itself observes
     * as !sta_associated (t_loss below), not from t0 above. */
    in.sta_associated = false; /* association dropped, MQTT layer has not yet noticed */
    int64_t t_loss = t0 + 1 * US_PER_S;
    a = wifi_policy_step(&p, &in, t_loss);
    CHECK(a == WIFI_ACT_NONE, "association loss must not drop immediately");

    a = wifi_policy_step(&p, &in, t_loss + ((int64_t) WIFI_ASSOC_LOST_S - 1) * US_PER_S);
    CHECK(a == WIFI_ACT_NONE, "must not drop 1s before the 20s hold elapses");

    a = wifi_policy_step(&p, &in, t_loss + (int64_t) WIFI_ASSOC_LOST_S * US_PER_S);
    CHECK(a == WIFI_ACT_DROP_TO_LTE, "must drop once association has been lost for 20s");
}

int main(void)
{
    test_never_leaves_lte_without_user_enabled();
    test_never_leaves_lte_without_ca_pinned();
    test_enters_at_70_not_71();
    test_hysteresis_no_drop_at_75();
    test_drop_after_81_held_30s_not_29s();
    test_three_failures_drop();
    test_two_disconnects_120s_drop();
    test_two_disconnects_121s_no_drop();
    test_backoff_ladder_and_cap();
    test_stable_session_resets_ladder();
    test_reset_clears_everything();
    test_assoc_loss_20s_drops();

    if (g_failures == 0) {
        printf("PASS: wifi_policy, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

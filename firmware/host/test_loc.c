/* test_loc.c — host test harness for main/loc.c's pure functions
 * (docs/V02_DESIGN.md §5 location).
 *
 * Builds with the plain host compiler, no ESP-IDF (loc.c's pure section has
 * no ESP-IDF dependency — same `#ifdef ESP_PLATFORM` split as lock.c/auth.c/
 * msg.c; see firmware/host/Makefile).
 *
 * Coverage required by the task brief:
 *  - backoff sequence 5,10,...,720 min cap
 *  - reset by success
 *  - reset by trigger but the 10-minute floor since the last attempt still
 *    enforced
 *  - cell-change debounce
 *  - motion classifier (>=60s span within a 3min window)
 *  - battery floor
 *  - cached answer only from this power session
 *  - a request arriving mid-attempt shares the result and gets its own
 *    /loc with its own req
 *  - first-attempt 40s vs 20s budget
 *  - the /loc CBOR encoding for a fix and for no_fix, byte-compared against
 *    bytes produced by relay/.venv/bin/python + relay/app/wirecbor.py (see
 *    test_loc_build_cbor_vs_relay() below for the exact command used to
 *    generate the two expected hex strings).
 */
#include "loc.h"
#include "cbor.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

#define US_PER_S ((int64_t) 1000000)

/* ---------------------------------------------------------------------
 * loc_next_backoff_s(): 5,10,20,40,80,160,320,640,720(cap) minutes.
 * --------------------------------------------------------------------- */
static void test_backoff_sequence(void)
{
    static const uint32_t expect_min[] = { 5, 10, 20, 40, 80, 160, 320, 640, 720, 720 };
    uint32_t backoff = 0;
    for (size_t i = 0; i < sizeof(expect_min) / sizeof(expect_min[0]); i++) {
        backoff = loc_next_backoff_s(backoff);
        CHECK(backoff == expect_min[i] * 60u, "step %zu: expected %u min (%u s), got %u s", i,
              expect_min[i], expect_min[i] * 60u, backoff);
    }
}

/* ---------------------------------------------------------------------
 * A full request/attempt/answer cycle drives loc_on_request()'s decision
 * table, the backoff growth/reset, and the trigger floor together — the
 * shape every other scenario below reuses.
 * --------------------------------------------------------------------- */

static void test_reset_by_success_and_backoff_growth(void)
{
    loc_policy_t p;
    loc_policy_init(&p);

    int64_t now = 1000 * US_PER_S;
    loc_decision_t d = loc_on_request(&p, now, 3700, "l_req0001");
    CHECK(d == LOC_ANSWER_START_ATTEMPT, "first-ever request must start an attempt");

    /* Fail the attempt: backoff should advance to the first step (5 min). */
    loc_on_attempt_done(&p, false, 0, 0, false, 0, 0, LOC_SRC_GNSS);
    CHECK(p.backoff_s == 300, "first failure -> 300s backoff, got %u", p.backoff_s);

    /* A request during the backoff must be answered from cache/no_fix
     * without starting a new attempt. */
    now += 60 * US_PER_S; /* only 60s later, well inside the 300s backoff */
    d = loc_on_request(&p, now, 3700, "l_req0002");
    CHECK(d == LOC_ANSWER_CACHED, "a request inside the backoff must be answered from cache");

    /* Advance past the backoff and succeed. */
    now += 300 * US_PER_S;
    d = loc_on_request(&p, now, 3700, "l_req0003");
    CHECK(d == LOC_ANSWER_START_ATTEMPT, "a request after the backoff expires must attempt again");
    loc_on_attempt_done(&p, true, 37.7, -122.4, true, 12, 1700000000, LOC_SRC_GNSS);
    CHECK(p.backoff_s == 0, "a successful fix must reset the backoff to zero");
    CHECK(p.have_fix, "a successful fix must be cached");

    /* Immediately after a success, with no trigger involved, there is no
     * floor -- a new request may attempt again right away. */
    d = loc_on_request(&p, now + 1, 3700, "l_req0004");
    CHECK(d == LOC_ANSWER_START_ATTEMPT,
          "success resets backoff to zero with NO floor (only a trigger reset adds one)");
    loc_on_attempt_done(&p, false, 0, 0, false, 0, 0, LOC_SRC_GNSS); /* leave it failed for the next test */
}

static void test_reset_by_trigger_respects_attempt_floor(void)
{
    loc_policy_t p;
    loc_policy_init(&p);

    int64_t t_attempt = 1000 * US_PER_S;
    CHECK(loc_on_request(&p, t_attempt, 3700, "l_a") == LOC_ANSWER_START_ATTEMPT, "setup: first attempt");
    loc_on_attempt_done(&p, false, 0, 0, false, 0, 0, LOC_SRC_GNSS); /* backoff -> 300s */
    CHECK(p.backoff_s == 300, "setup: backoff must be 300s after one failure");

    /* A trigger arrives 30s after the attempt: resets backoff to 0, but a
     * request must still be refused until 10 minutes after t_attempt. */
    int64_t t_trigger = t_attempt + 30 * US_PER_S;
    bool reset = loc_trigger_cell_change(&p, t_trigger, "100:1");
    /* First call for cell.have_cell_key ever seen is a learn, not a
     * "change" -- seed it first, then trigger a real change. */
    CHECK(!reset, "the very first cell key observed must not itself be a trigger");
    reset = loc_trigger_cell_change(&p, t_trigger, "200:2");
    CHECK(reset, "a genuine cell change (outside the debounce) must reset the backoff");
    CHECK(p.backoff_s == 0, "a trigger reset must zero the backoff");

    /* Just short of the 10-minute floor since t_attempt: still refused. */
    int64_t t_before_floor = t_attempt + 599 * US_PER_S;
    loc_decision_t d = loc_on_request(&p, t_before_floor, 3700, "l_b");
    CHECK(d == LOC_ANSWER_CACHED,
          "a request inside the 10-minute post-trigger floor must still be answered from cache");

    /* At/after the floor: allowed again. */
    int64_t t_after_floor = t_attempt + 600 * US_PER_S;
    d = loc_on_request(&p, t_after_floor, 3700, "l_c");
    CHECK(d == LOC_ANSWER_START_ATTEMPT, "a request at/after the 10-minute floor must attempt again");
}

/* ---------------------------------------------------------------------
 * Cell-change debounce: repeated changes inside 10 minutes of the last
 * *accepted* one must be ignored.
 * --------------------------------------------------------------------- */
static void test_cell_change_debounce(void)
{
    loc_policy_t p;
    loc_policy_init(&p);
    int64_t t = 0;

    CHECK(!loc_trigger_cell_change(&p, t, "A"), "first-ever cell key: learn only, not a change");

    t += 60 * US_PER_S;
    CHECK(loc_trigger_cell_change(&p, t, "B"), "A->B is a genuine, undebounced change");

    /* Flap back and forth well inside the 10-minute debounce window. */
    t += 60 * US_PER_S;
    CHECK(!loc_trigger_cell_change(&p, t, "A"), "B->A within 10min of the last accepted change: debounced");
    t += 60 * US_PER_S;
    CHECK(!loc_trigger_cell_change(&p, t, "B"), "A->B again, still within the debounce: debounced");

    /* Past the 10-minute debounce, measured from the last *accepted*
     * change (not from the ignored flaps in between). */
    t = 60 * US_PER_S + 600 * US_PER_S; /* 10min after the B change above */
    CHECK(loc_trigger_cell_change(&p, t, "A"), "a change 10min after the last accepted one is accepted");

    /* Repeating the SAME key is never a "change" regardless of timing. */
    CHECK(!loc_trigger_cell_change(&p, t + 1, "A"), "reporting the same cell key again is not a change");
}

/* ---------------------------------------------------------------------
 * Motion classifier: sustained = interrupts spanning >=60s within a
 * trailing 3-minute window.
 * --------------------------------------------------------------------- */
static void test_motion_classifier(void)
{
    loc_policy_t p;
    loc_policy_init(&p);
    int64_t t = 0;

    /* A single brief jolt (well under 60s span) must never fire. */
    CHECK(!loc_trigger_motion_event(&p, t), "one event alone can never span >=60s");
    t += 10 * US_PER_S;
    CHECK(!loc_trigger_motion_event(&p, t), "a 10s span is not yet sustained");
    t += 20 * US_PER_S; /* 30s span so far */
    CHECK(!loc_trigger_motion_event(&p, t), "a 30s span is not yet sustained");

    /* Cross the 60s threshold. */
    t += 35 * US_PER_S; /* 65s span since the first event */
    CHECK(loc_trigger_motion_event(&p, t), "a 65s span within the 3min window must fire");

    /* Immediately after firing, the ring was cleared -- a lone new event
     * must not re-fire until it has its own 60s span. */
    t += 1 * US_PER_S;
    CHECK(!loc_trigger_motion_event(&p, t), "right after firing, a single new event must not re-fire");

    /* Events more than 3 minutes apart never accumulate into one span. */
    loc_policy_t p2;
    loc_policy_init(&p2);
    int64_t t2 = 0;
    CHECK(!loc_trigger_motion_event(&p2, t2), "setup: first event of the stale-window case");
    t2 += 200 * US_PER_S; /* > 180s window: the first event has fallen out */
    CHECK(!loc_trigger_motion_event(&p2, t2),
          "an event outside the trailing 3-minute window must not combine with a stale one");
}

/* ---------------------------------------------------------------------
 * Battery floor.
 * --------------------------------------------------------------------- */
static void test_battery_floor(void)
{
    CHECK(!loc_battery_ok(LOC_BATTERY_FLOOR_MV - 1), "one mV below the floor must fail");
    CHECK(loc_battery_ok(LOC_BATTERY_FLOOR_MV), "exactly the floor must pass");
    CHECK(loc_battery_ok(4200), "4200mV must pass the floor");

    loc_policy_t p;
    loc_policy_init(&p);
    loc_decision_t d = loc_on_request(&p, 0, LOC_BATTERY_FLOOR_MV - 1, "l_lowbatt");
    CHECK(d == LOC_ANSWER_CACHED, "a request below the battery floor must never start GNSS");
}

/* This task: modes.c's modes_get_batt_mv() returns a fixed "unknown"
 * placeholder (PAGER_BATT_MV_UNKNOWN_PLACEHOLDER, modes.c) before the first
 * good AT+SQNVMON reading this boot -- chosen to sit safely clear of
 * LOC_BATTERY_FLOOR_MV rather than coincidentally on it (as the pre-3-Oct
 * LiFePO4-era 3300 mV placeholder did against the then-3300 mV floor), so a
 * caller that fed that placeholder straight into loc_battery_ok()/
 * loc_on_request() without distinguishing it from a real reading cannot
 * accidentally pass by coincidence. modes.c is expected to pass
 * LOC_BATTERY_UNKNOWN_MV instead (see loc.h's own doc comment and loc.c's
 * loc_ingest_req_cbor()); this must always pass the floor, and must never
 * be confused with an ordinary low reading. */
static void test_battery_unknown_sentinel(void)
{
    CHECK(loc_battery_ok(LOC_BATTERY_UNKNOWN_MV), "an unknown battery reading must pass the floor");

    loc_policy_t p;
    loc_policy_init(&p);
    loc_decision_t d = loc_on_request(&p, 0, LOC_BATTERY_UNKNOWN_MV, "l_unknownbatt");
    CHECK(d == LOC_ANSWER_START_ATTEMPT, "an unknown battery reading must not block a GNSS attempt");
}

/* ---------------------------------------------------------------------
 * Cached answer only from this power session: a fresh policy (as a cold
 * boot / reset produces, since the cache is deliberately RAM-only) has no
 * cached fix at all.
 * --------------------------------------------------------------------- */
static void test_cached_only_this_session(void)
{
    loc_policy_t p;
    loc_policy_init(&p);
    double lat, lon;
    bool have_acc;
    int32_t acc_m;
    int64_t fix_ts;
    uint8_t src;
    CHECK(!loc_get_cached(&p, &lat, &lon, &have_acc, &acc_m, &fix_ts, &src),
          "a freshly-initialised policy (cold boot/reset) must have no cached fix");

    loc_on_attempt_done(&p, true, 1.0, 2.0, true, 5, 100, LOC_SRC_GNSS);
    CHECK(loc_get_cached(&p, &lat, &lon, &have_acc, &acc_m, &fix_ts, &src),
          "after a successful attempt this power session, the fix must be cached");

    /* Re-initialising (what a reset does) must drop it again. */
    loc_policy_init(&p);
    CHECK(!loc_get_cached(&p, &lat, &lon, &have_acc, &acc_m, &fix_ts, &src),
          "loc_policy_init() (reset) must clear any previously cached fix");
}

/* ---------------------------------------------------------------------
 * A request arriving mid-attempt shares the result and gets its own /loc
 * with its own req id.
 * --------------------------------------------------------------------- */
static void test_mid_attempt_requests_share_result(void)
{
    loc_policy_t p;
    loc_policy_init(&p);

    CHECK(loc_on_request(&p, 0, 3700, "l_first") == LOC_ANSWER_START_ATTEMPT, "first request starts the attempt");
    CHECK(loc_on_request(&p, 1, 3700, "l_second") == LOC_ANSWER_NONE,
          "a second request while the attempt is in flight must not start another one");
    CHECK(loc_on_request(&p, 2, 3700, "l_third") == LOC_ANSWER_NONE,
          "a third request must also just queue behind the same in-flight attempt");

    loc_on_attempt_done(&p, true, 10.0, 20.0, true, 8, 42, LOC_SRC_GNSS);

    char id[LOC_ID_MAX];
    int seen = 0;
    int have_first = 0, have_second = 0, have_third = 0;
    while (loc_take_queued_id(&p, id, sizeof(id))) {
        seen++;
        if (strcmp(id, "l_first") == 0) have_first = 1;
        if (strcmp(id, "l_second") == 0) have_second = 1;
        if (strcmp(id, "l_third") == 0) have_third = 1;
    }
    CHECK(seen == 3, "all three requesters must each get their own queued answer, got %d", seen);
    CHECK(have_first && have_second && have_third, "every one of the three ids must be present exactly once");
}

/* ---------------------------------------------------------------------
 * First-attempt 40s vs 20s budget.
 * --------------------------------------------------------------------- */
static void test_attempt_budget(void)
{
    loc_policy_t p;
    loc_policy_init(&p); /* cold boot: long_budget_next starts true */
    CHECK(loc_attempt_budget_s(&p) == LOC_FIRST_ATTEMPT_S, "cold boot must get the 40s budget");

    loc_on_attempt_done(&p, true, 0, 0, true, 1, 0, LOC_SRC_GNSS); /* consumes long_budget_next */
    CHECK(loc_attempt_budget_s(&p) == LOC_ATTEMPT_S, "a later ordinary attempt must get the 20s budget");

    loc_policy_request_long_budget(&p); /* e.g. an assistance-data refresh just ran */
    CHECK(loc_attempt_budget_s(&p) == LOC_FIRST_ATTEMPT_S,
          "requesting the long budget again (assistance refresh) must give 40s");
}

/* ---------------------------------------------------------------------
 * /loc CBOR encoding, byte-compared against relay/app/wirecbor.py.
 *
 * Expected bytes generated with:
 *   cd /Users/albert/src/pager && relay/.venv/bin/python - <<'EOF'
 *   import sys; sys.path.insert(0, "relay")
 *   from app import wirecbor
 *   fix = {"v":1,"id":"l_3c9a11f0","ts":1757700000,
 *          "loc":{"lat":37.774929,"lon":-122.419416,"acc":14,"fix_ts":1757699991},
 *          "req":"m_7f3a2b10"}
 *   print(wirecbor.encode(fix).hex())
 *   nofix = {"v":1,"id":"l_deadbeef","ts":1757700000,"loc":None,
 *            "req":"m_7f3a2b10","err":"no_fix"}
 *   print(wirecbor.encode(nofix).hex())
 *   EOF
 * (run 2026-09-20; both wirecbor.py and cbor.c encode CBOR maps/floats the
 * same way -- definite-length headers, minimal-length integers, float64 for
 * every float -- so this only needs to be regenerated if either encoder's
 * wire format changes, not on every edit).
 * --------------------------------------------------------------------- */
static void hex_decode(const char *hex, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t n = strlen(hex) / 2;
    if (n > out_cap) {
        n = out_cap;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t) v;
    }
    *out_len = n;
}

static void test_loc_build_cbor_vs_relay(void)
{
    static const char *FIX_HEX =
        "a50001016a6c5f3363396131316630021a68c45fa008a400fb4042e330df9bdc6a01fbc05e9ad7b634dad3020e"
        "031a68c45f97096a6d5f3766336132623130";
    static const char *NOFIX_HEX =
        "a60001016a6c5f6465616462656566021a68c45fa008f6096a6d5f37663361326231300b666e6f5f666978";

    uint8_t want[128];
    size_t want_len;

    /* Fix: lat=37.774929 lon=-122.419416 acc=14 fix_ts=1757699991, req set,
     * no `cached` (default false, omitted), src omitted (default "gnss"). */
    uint8_t got[192];
    size_t got_len;
    bool ok = loc_build_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "l_3c9a11f0",
                             1757700000, /*have_fix=*/true, 37.774929, -122.419416,
                             /*have_acc=*/true, 14, 1757699991, /*src_cell=*/false, "m_7f3a2b10",
                             /*cached=*/false, NULL, /*cell=*/NULL, /*why=*/NULL);
    CHECK(ok, "loc_build_cbor() must succeed for the fix vector");
    hex_decode(FIX_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len, "fix vector length mismatch: got %zu want %zu", got_len, want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "fix vector bytes must match relay/app/wirecbor.py's own CBOR encoding exactly");

    /* no_fix: loc:null, err:"no_fix", req set. */
    ok = loc_build_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "l_deadbeef", 1757700000,
                        /*have_fix=*/false, 0, 0, /*have_acc=*/false, 0, 0, /*src_cell=*/false,
                        "m_7f3a2b10", /*cached=*/false, "no_fix", /*cell=*/NULL, /*why=*/NULL);
    CHECK(ok, "loc_build_cbor() must succeed for the no_fix vector");
    hex_decode(NOFIX_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len, "no_fix vector length mismatch: got %zu want %zu", got_len, want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "no_fix vector bytes must match relay/app/wirecbor.py's own CBOR encoding exactly");

    /* Contract check: have_fix and a non-NULL err must never both hold (or
     * both be absent). */
    CHECK(!loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_bad", 0, true, 0, 0, false, 0, 0,
                          false, NULL, false, "no_fix", NULL, NULL),
          "have_fix=true with a non-NULL err must be rejected");
    CHECK(!loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_bad", 0, false, 0, 0, false, 0, 0,
                          false, NULL, false, NULL, NULL, NULL),
          "have_fix=false with no err must be rejected");
}

/* ---------------------------------------------------------------------
 * `cell` sub-map (PROTOCOL.md §13.2 key 49, this task), byte-compared
 * against relay/app/wirecbor.py the same way test_loc_build_cbor_vs_relay()
 * above does.
 *
 * FIX_NOFIX_CELL_HEX is byte-identical to the vector the relay pins in
 * relay/tests/test_devauth.py (test_loc_envelope_with_cell_example_cbor_hex):
 * key 49 is the two bytes 0x18 0x31, directly after the text "no_fix".
 * Generated from the real relay encoder with:
 *   cd /Users/albert/src/pager && relay/.venv/bin/python - <<'EOF'
 *   import sys; sys.path.insert(0, "relay")
 *   from app import wirecbor
 *   nofix_cell = {"v":1,"id":"l_3c9a11f0","ts":1757700000,"loc":None,
 *                 "req":"m_7f3a2b10","err":"no_fix",
 *                 "cell":{"mcc":"310","mnc":"410","tac":12345,"ci":87654321,"rsrp":-95}}
 *   print(wirecbor.encode(nofix_cell).hex())
 *   nofix_cell_pad = {"v":1,"id":"l_3c9a11f0","ts":1757700000,"loc":None,
 *                      "req":None,"err":"no_fix",
 *                      "cell":{"mcc":"234","mnc":"07","tac":1,"ci":1}}
 *   print(wirecbor.encode(nofix_cell_pad).hex())
 *   EOF
 * (run 2026-09-21.)
 * --------------------------------------------------------------------- */
static void test_loc_build_cbor_cell_vs_relay(void)
{
    static const char *NOFIX_CELL_HEX =
        "a70001016a6c5f3363396131316630021a68c45fa008f6096a6d5f37663361326231300b666e6f5f66697818"
        "31a50063333130016334313002193039031a05397fb104385e";
    /* Leading-zero MNC ("07"), rsrp absent, req:null -- the two edge cases
     * the task brief's own test list calls out by name. */
    static const char *NOFIX_CELL_PAD_HEX =
        "a70001016a6c5f3363396131316630021a68c45fa008f609f60b666e6f5f6669781831a4006332333401623037"
        "02010301";

    uint8_t got[192];
    size_t got_len;
    uint8_t want[192];
    size_t want_len;

    loc_cell_t cell = { .mcc = "310", .mnc = "410", .tac = 12345, .ci = 87654321,
                        .have_rsrp = true, .rsrp = -95 };
    bool ok = loc_build_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "l_3c9a11f0",
                             1757700000, /*have_fix=*/false, 0, 0, /*have_acc=*/false, 0, 0,
                             /*src_cell=*/false, "m_7f3a2b10", /*cached=*/false, "no_fix", &cell,
                             /*why=*/NULL);
    CHECK(ok, "loc_build_cbor() must succeed for the no_fix+cell vector");
    hex_decode(NOFIX_CELL_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "no_fix+cell vector bytes must match relay/app/wirecbor.py's own CBOR encoding exactly "
          "(got %zu bytes, want %zu)",
          got_len, want_len);

    loc_cell_t cell_pad = { .mcc = "234", .mnc = "07", .tac = 1, .ci = 1, .have_rsrp = false };
    ok = loc_build_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "l_3c9a11f0", 1757700000,
                        /*have_fix=*/false, 0, 0, /*have_acc=*/false, 0, 0, /*src_cell=*/false,
                        /*req=*/NULL, /*cached=*/false, "no_fix", &cell_pad, /*why=*/NULL);
    CHECK(ok, "loc_build_cbor() must succeed for the leading-zero-mnc/no-rsrp vector");
    hex_decode(NOFIX_CELL_PAD_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "leading-zero-mnc vector must preserve the leading zero and omit rsrp (got %zu bytes, "
          "want %zu)",
          got_len, want_len);

    /* A malformed cell (bad mcc width) must be silently treated as absent --
     * §13.2's own "malformed cell is treated as absent" tolerance, applied
     * defensively on the encode side (net.cpp should never actually produce
     * one, but loc_build_cbor() must not trust that blindly). */
    loc_cell_t bad_cell = { .mcc = "31", .mnc = "410", .tac = 1, .ci = 1 };
    ok = loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_3c9a11f0", 1757700000, false, 0, 0,
                        false, 0, 0, false, NULL, false, "no_fix", &bad_cell, /*why=*/NULL);
    CHECK(ok, "a malformed cell must not fail the whole build");
    size_t got_len_nocell;
    uint8_t got_nocell[192];
    CHECK(loc_build_cbor(got_nocell, sizeof(got_nocell), &got_len_nocell, false, 0, "l_3c9a11f0",
                         1757700000, false, 0, 0, false, 0, 0, false, NULL, false, "no_fix", NULL,
                         /*why=*/NULL),
          "test setup: the no-cell control build must succeed");
    CHECK(got_len == got_len_nocell && memcmp(got, got_nocell, got_len) == 0,
          "a malformed cell must encode identically to no cell at all");
}

/* ---------------------------------------------------------------------
 * loc_parse_req_cbor(): the kind:"loc_req" recogniser.
 * --------------------------------------------------------------------- */
static void test_parse_req_cbor(void)
{
    uint8_t buf[128];
    cbor_w_t w;

    /* A well-formed loc_req: v,id,ts,kind,from,ack. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 6);
    cbor_w_uint(&w, 0, 1);
    cbor_w_tstr(&w, 1, "m_7f3a2b10", 10);
    cbor_w_uint(&w, 2, 1757700000);
    cbor_w_tstr(&w, 6, "loc_req", 7);
    cbor_w_tstr(&w, 3, "mom", 3);
    cbor_w_null(&w, 5);
    CHECK(!w.err, "test setup: encoding the loc_req fixture must not overflow");

    char id[LOC_ID_MAX];
    CHECK(loc_parse_req_cbor(buf, (uint16_t) w.len, false, id, sizeof(id)),
          "a well-formed kind:\"loc_req\" envelope must be recognised");
    CHECK(strcmp(id, "m_7f3a2b10") == 0, "the parsed id must be the request's own id, got %s", id);

    /* An ordinary msg (no kind, or kind:"msg") must not be mistaken for one. */
    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 5);
    cbor_w_uint(&w, 0, 1);
    cbor_w_tstr(&w, 1, "m_abc", 5);
    cbor_w_uint(&w, 2, 1757700000);
    cbor_w_tstr(&w, 3, "mom", 3);
    cbor_w_tstr(&w, 4, "hi", 2);
    CHECK(!w.err, "test setup: encoding the plain-msg fixture must not overflow");
    CHECK(!loc_parse_req_cbor(buf, (uint16_t) w.len, false, id, sizeof(id)),
          "a plain content message must not be accepted as loc_req");
}

/* =======================================================================
 * Location tracking (docs/LOCATION_TRACKING_DESIGN.md, task F1; T0's own
 * bullet list): STILL/MOVING transitions incl. no-LIS3DH; the flap ring;
 * the report scheduler; the GNSS schedule; the daily cap; the transition-
 * only backoff reset; the 2h assistance rule; plus the two server-architect
 * course corrections (26 Sep 2026): the P1 120s floor on every unsolicited
 * report, and the §13.3 web-request GNSS budget.
 * ======================================================================= */

/* ---------------------------------------------------------------------
 * STILL <-> MOVING, with a LIS3DH fitted: accel sustained-motion fires
 * STILL->MOVING; MOVING->STILL needs BOTH no accel edge for 300s AND no
 * new cell for 900s.
 * --------------------------------------------------------------------- */
static void test_track_still_moving_with_lis3dh(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    CHECK(loc_track_state(&t) == LOC_MSTATE_STILL, "a freshly-initialised tracker starts STILL");

    /* Seed a first cell BEFORE entering MOVING -- the very first cell this
     * power session only seeds the ring (loc_track_on_cell()'s own doc
     * comment, same carve-out loc_trigger_cell_change() uses), so it must
     * not itself count as the "recent new cell" the next step needs. */
    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "100:1"), "setup: learn the first cell (not itself \"new\")");

    CHECK(!loc_track_on_motion(&t, now), "one edge alone can never span >=60s");
    now += 65 * US_PER_S;
    CHECK(loc_track_on_motion(&t, now), "a 65s span within the 3min window must enter MOVING");
    CHECK(loc_track_state(&t) == LOC_MSTATE_MOVING, "state must now read MOVING");

    /* Neither condition alone is enough to leave MOVING. loc_track_tick()
     * runs the MOVING->STILL check as a side effect regardless of what (if
     * anything) it decides to report -- these tests only care about the
     * resulting loc_track_state(), so the return value is ignored. */
    now += LOC_STILL_MOTION_GAP_S * US_PER_S; /* no edge for 300s, but a genuinely new cell 1s ago */
    CHECK(loc_track_on_cell(&t, now - 1 * US_PER_S, "200:2") == false,
          "a second, genuinely new cell (1 of 2 needed to re-enter MOVING) is recorded");
    loc_track_tick(&t, now);
    CHECK(loc_track_state(&t) == LOC_MSTATE_MOVING,
          "no accel edge but a recent new cell must NOT yet leave MOVING");

    now += LOC_STILL_CELL_GAP_S * US_PER_S; /* now also >=900s since that cell */
    loc_track_tick(&t, now);
    CHECK(loc_track_state(&t) == LOC_MSTATE_STILL,
          "no accel edge for 300s AND no new cell for 900s must leave MOVING");
}

/* ---------------------------------------------------------------------
 * Without a LIS3DH, accel input never transitions anything (§1: "cell-
 * change is the ONLY motion signal") -- only "2 new cells within 15 min"
 * can enter MOVING, and only "no new cell for 900s" can leave it (the
 * accel half of the MOVING->STILL rule is vacuously satisfied).
 * --------------------------------------------------------------------- */
static void test_track_no_lis3dh_cell_only(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/false);

    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_motion(&t, now), "an accel edge must never fire without a LIS3DH");
    CHECK(!loc_track_on_motion(&t, now + 65 * US_PER_S),
          "a whole sustained span of accel edges must still never fire without a LIS3DH");
    CHECK(loc_track_state(&t) == LOC_MSTATE_STILL, "state must still read STILL");

    CHECK(!loc_track_on_cell(&t, now, "100:1"), "first-ever cell: learn only, not a change");
    now += 60 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "200:2"), "one new cell alone must not yet enter MOVING");
    now += 60 * US_PER_S;
    CHECK(loc_track_on_cell(&t, now, "300:3"), "a second new cell within 15min must enter MOVING");
    CHECK(loc_track_state(&t) == LOC_MSTATE_MOVING, "state must now read MOVING");

    now += LOC_STILL_CELL_GAP_S * US_PER_S;
    loc_track_tick(&t, now);
    CHECK(loc_track_state(&t) == LOC_MSTATE_STILL,
          "no new cell for 900s must leave MOVING even with zero accel signal");
}

/* ---------------------------------------------------------------------
 * The flap ring: a cell flapping A/B/A within the 60-minute membership
 * window must never look like 2 *new* cells, so it must never enter
 * MOVING on its own (§1's own worked example, T0's "flap ring" bullet).
 * --------------------------------------------------------------------- */
static void test_track_flap_ring_never_moving(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/false);
    int64_t now = 1000 * US_PER_S;

    CHECK(!loc_track_on_cell(&t, now, "A"), "first-ever cell: learn only");
    now += 5 * 60 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "B"), "A->B is a genuine new cell (1 of 2 needed)");
    now += 5 * 60 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "A"),
          "B->A within 60min of A's own last sighting is NOT a new cell (flap ring membership)");
    now += 5 * 60 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "B"),
          "A->B again, B still a ring member too -- still not new");
    CHECK(loc_track_state(&t) == LOC_MSTATE_STILL,
          "A/B/A/B flapping inside the 60min ring must never enter MOVING");
}

/* ---------------------------------------------------------------------
 * Report scheduler (§2.1): hourly cell fix while STILL, a debounced cell-
 * change report, a periodic refresh while MOVING, and the "stop" report's
 * own "only if it says something new" gate.
 * --------------------------------------------------------------------- */
static void test_track_report_scheduler(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);

    /* Hourly cell, STILL: the very first tick (last_loc_us==0) is due at
     * once; the next one is not due until LOC_HOURLY_CELL_S later. */
    int64_t now = 1000 * US_PER_S;
    CHECK(loc_track_on_cell(&t, now, "100:1") == false, "setup: learn the first cell");
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STILL, "first-ever tick must report `still` at once");
    loc_track_note_report_sent(&t, now, LOC_REPORT_STILL, false);
    CHECK(loc_track_tick(&t, now + 1) == LOC_REPORT_NONE, "right after a report, nothing is due");
    now += (LOC_HOURLY_CELL_S - 1) * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_NONE, "1s short of the hourly gate: not yet due");
    now += 1 * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STILL, "at the hourly gate: `still` is due again");
    loc_track_note_report_sent(&t, now, LOC_REPORT_STILL, false);

    /* Cell-change report: needs to have served >=60s AND differ from the
     * last *reported* cell (not just the last observed one) AND the 120s
     * gate below to actually publish. */
    now += 200 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "200:2"), "a genuine new cell (1 of 2) is recorded");
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_NONE, "a cell that has served 0s must not report yet");
    now += (LOC_CELL_REPORT_SERVING_S - 1) * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_NONE, "1s short of the serving debounce: not yet");
    now += 1 * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_CELL, "served >=60s and differs from last reported");
    loc_track_note_report_sent(&t, now, LOC_REPORT_CELL, false);

    /* Move report: only once MOVING, and only after LOC_MOVE_REFRESH_S
     * since the last /loc of any kind. Enter MOVING via a second new cell,
     * let ITS OWN cell-change report fire and be acknowledged first --
     * otherwise the freshly-changed cell would still be a pending `cell`
     * report of its own and out-rank `move` (`cell` > `move` priority). */
    now += 60 * US_PER_S;
    CHECK(loc_track_on_cell(&t, now, "300:3"), "second new cell (2 of 2) enters MOVING");
    CHECK(loc_track_state(&t) == LOC_MSTATE_MOVING, "setup: must now be MOVING");
    now += LOC_CELL_REPORT_SERVING_S * US_PER_S; /* "300:3" now old enough to report too */
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_CELL,
          "setup: the new MOVING-triggering cell earns its own `cell` report first");
    loc_track_note_report_sent(&t, now, LOC_REPORT_CELL, false);

    now += (LOC_MOVE_REFRESH_S - 1) * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_NONE, "1s short of the move-refresh gate: not yet");
    now += 1 * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_MOVE, "at the move-refresh gate: `move` is due");
    loc_track_note_report_sent(&t, now, LOC_REPORT_MOVE, false);

    /* Stop report: MOVING -> STILL is silent unless the cell differs from
     * the last one reported, or a GNSS fix went out this episode. */
    now += LOC_STILL_CELL_GAP_S * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_NONE,
          "a MOVING->STILL transition with the SAME cell as last reported, and no GNSS fix, "
          "must be silent");
    CHECK(loc_track_state(&t) == LOC_MSTATE_STILL, "the transition itself must still have happened");
}

/* Stop report DOES fire when the cell differs from the last reported one. */
static void test_track_stop_report_on_cell_change(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "100:1"), "setup: learn the first cell");
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STILL, "setup: initial still report");
    loc_track_note_report_sent(&t, now, LOC_REPORT_STILL, false);

    now += 10 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "200:2"), "setup: first new cell");
    now += 10 * US_PER_S;
    CHECK(loc_track_on_cell(&t, now, "300:3"), "setup: second new cell -> MOVING");
    CHECK(loc_track_state(&t) == LOC_MSTATE_MOVING, "setup: must be MOVING");

    now += LOC_STILL_CELL_GAP_S * US_PER_S + LOC_STILL_MOTION_GAP_S * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STOP,
          "MOVING->STILL with a different cell than last reported must report `stop`");
}

/* Stop report also fires when a GNSS fix was published this episode, even
 * if the cell happens to match the last reported one. */
static void test_track_stop_report_on_gnss_published(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "100:1"), "setup: learn the first cell");
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STILL, "setup: initial still report");
    loc_track_note_report_sent(&t, now, LOC_REPORT_STILL, false);

    CHECK(!loc_track_on_motion(&t, now), "setup: first edge alone cannot span >=60s");
    now += 65 * US_PER_S;
    CHECK(loc_track_on_motion(&t, now), "setup: sustained motion -> MOVING");
    now += 120 * US_PER_S; /* clear the 120s gate from the setup `still` report above */
    loc_track_note_report_sent(&t, now, LOC_REPORT_GNSS, /*gnss_fix=*/true);
    CHECK(loc_track_state(&t) == LOC_MSTATE_MOVING, "setup: must still be MOVING");

    now += LOC_STILL_MOTION_GAP_S * US_PER_S + LOC_STILL_CELL_GAP_S * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STOP,
          "MOVING->STILL after a GNSS fix this episode must report `stop` even with the same cell");
}

/* ---------------------------------------------------------------------
 * P1 course correction (server-architect review, 26 Sep 2026): a 120s
 * minimum gap applies to EVERY unsolicited report; one that falls due
 * early waits for the gate rather than being dropped, and a higher-
 * priority report can supersede a lower one still waiting.
 * --------------------------------------------------------------------- */
static void test_track_unsolicited_min_gap_not_dropped(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "100:1"), "setup: learn the first cell");
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STILL, "setup: initial still report");
    loc_track_note_report_sent(&t, now, LOC_REPORT_STILL, false);

    /* A cell-change report becomes "wanted" only 61s after the report
     * above (well inside the 120s gate). */
    now += 1 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "200:2"), "setup: a genuine new cell");
    now += LOC_CELL_REPORT_SERVING_S * US_PER_S; /* served long enough, but gate still open */
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_NONE,
          "a report due before the 120s gate clears must NOT publish yet");

    /* It must not be lost: once the gate clears, the SAME report fires,
     * with no new trigger needed. */
    now = 1000 * US_PER_S + LOC_UNSOLICITED_MIN_GAP_S * US_PER_S; /* exactly 120s after `still` */
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_CELL,
          "the latched cell-change report must fire the instant the 120s gate opens");
}

static void test_track_unsolicited_min_gap_superseded_by_higher_priority(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "100:1"), "setup: learn the first cell");
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_STILL, "setup: initial still report");
    loc_track_note_report_sent(&t, now, LOC_REPORT_STILL, false);

    now += 1 * US_PER_S;
    CHECK(!loc_track_on_cell(&t, now, "200:2"), "setup: a genuine new cell (1 of 2)");
    now += LOC_CELL_REPORT_SERVING_S * US_PER_S;
    CHECK(loc_track_tick(&t, now) == LOC_REPORT_NONE, "the cell report is latched, gated");

    /* A second new cell (2 of 2) enters MOVING; still gated, nothing to see yet. */
    now += 1 * US_PER_S;
    CHECK(loc_track_on_cell(&t, now, "300:3"), "setup: second new cell -> MOVING");
    /* Sustained motion then holds long enough that MOVING->STILL with a
     * differing cell becomes due -- a `stop` outranks the still-latched
     * `cell`, so once the gate opens, `stop` (not `cell`) must win. */
    now += LOC_STILL_CELL_GAP_S * US_PER_S + LOC_STILL_MOTION_GAP_S * US_PER_S;
    loc_report_t got = loc_track_tick(&t, now);
    CHECK(got == LOC_REPORT_STOP, "a higher-priority `stop` becoming due must supersede a "
                                  "still-latched `cell`, got %d",
          (int) got);
}

/* ---------------------------------------------------------------------
 * GNSS-while-moving schedule (§2.2): first attempt one full interval after
 * MOVING begins; the exit-tail edge/cell-recency guard; independence from
 * `locmove`=0 (off).
 * --------------------------------------------------------------------- */
static void test_track_gnss_schedule(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_motion(&t, now), "setup: first edge alone cannot span >=60s");
    now += 65 * US_PER_S;
    CHECK(loc_track_on_motion(&t, now), "setup: sustained motion -> MOVING");
    int64_t moving_started = now;

    CHECK(!loc_track_gnss_due(&t, now, LOC_MOVE_GNSS_DEFAULT_S),
          "no GNSS attempt is due the instant MOVING begins");
    CHECK(!loc_track_gnss_due(&t, now, 0), "locmove=0 must always mean off");

    now = moving_started + (LOC_MOVE_GNSS_DEFAULT_S - 1) * US_PER_S;
    CHECK(!loc_track_gnss_due(&t, now, LOC_MOVE_GNSS_DEFAULT_S),
          "1s short of one full interval into motion: not yet due");

    now = moving_started + LOC_MOVE_GNSS_DEFAULT_S * US_PER_S;
    CHECK(!loc_track_gnss_due(&t, now, LOC_MOVE_GNSS_DEFAULT_S),
          "at the interval, but with no recent accel edge: the exit-tail guard must refuse it");
    CHECK(!loc_track_on_motion(&t, now),
          "already MOVING: this edge cannot itself re-transition, but it does satisfy the guard");
    CHECK(loc_track_gnss_due(&t, now, LOC_MOVE_GNSS_DEFAULT_S),
          "one full interval into motion, with a recent edge, the first attempt must be due");

    loc_track_gnss_attempt_started(&t, now);
    loc_track_gnss_attempt_done(&t, /*success=*/false);
    CHECK(!loc_track_gnss_due(&t, now + 1, LOC_MOVE_GNSS_DEFAULT_S),
          "right after a failed attempt, the next one must not be due immediately");
}

/* ---------------------------------------------------------------------
 * Daily cap: 48 scheduled attempts, then refused; a rolling 24h window
 * (not calendar-day), so it self-heals once the oldest entries age out.
 * --------------------------------------------------------------------- */
static void test_track_gnss_daily_cap(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    int64_t now = 1000 * US_PER_S;

    for (int i = 0; i < LOC_GNSS_DAILY_CAP; i++) {
        CHECK(!loc_track_gnss_cap_reached(&t, now), "attempt %d/%d must still be under the cap", i,
              LOC_GNSS_DAILY_CAP);
        loc_track_gnss_attempt_started(&t, now);
        now += 601 * US_PER_S; /* clear of the 600s floor between attempts */
    }
    CHECK(loc_track_gnss_cap_reached(&t, now), "the 49th attempt today must be refused");

    /* 24h after the FIRST attempt (recorded at the loop's starting `now`,
     * 1000s), that one ages out of the rolling window, making room for one
     * more. */
    int64_t past_24h = 1000 * US_PER_S + ((int64_t) LOC_GNSS_DAY_S + 1) * US_PER_S;
    CHECK(!loc_track_gnss_cap_reached(&t, past_24h),
          "once the oldest attempt is >=24h old, the cap must allow another one");
}

/* ---------------------------------------------------------------------
 * Transition-only backoff reset (§1/§2.3): gnss_backoff_s only resets to
 * zero on STILL->MOVING -- NOT on every subsequent motion/cell event while
 * already MOVING (the whole point of "the loc_trigger_* calls become state
 * inputs", loc.h's own module comment).
 * --------------------------------------------------------------------- */
static void test_track_transition_only_backoff_reset(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    int64_t now = 1000 * US_PER_S;
    CHECK(!loc_track_on_motion(&t, now), "setup: first edge alone cannot span >=60s");
    now += 65 * US_PER_S;
    CHECK(loc_track_on_motion(&t, now), "setup: sustained motion -> MOVING");
    CHECK(t.gnss_backoff_s == 0, "backoff must be zero right after the transition");

    /* Fail a scheduled attempt: backoff advances. */
    loc_track_gnss_attempt_started(&t, now);
    loc_track_gnss_attempt_done(&t, false);
    CHECK(t.gnss_backoff_s == 300, "one failure must step the backoff to 300s, got %u",
          (unsigned) t.gnss_backoff_s);

    /* Further motion/cell events while STILL MOVING must NOT reset it. */
    now += 30 * US_PER_S;
    CHECK(!loc_track_on_motion(&t, now), "a lone edge alone must not re-fire the sustained classifier");
    CHECK(t.gnss_backoff_s == 300, "an ordinary motion edge while already MOVING must not touch backoff");

    now += 40 * US_PER_S; /* a fresh 65s-span edge, still while MOVING */
    CHECK(!loc_track_on_motion(&t, now), "already MOVING: a new sustained span must not re-transition");
    CHECK(t.gnss_backoff_s == 300, "a re-sustained motion event while already MOVING must not reset "
                                    "the backoff either");
}

/* ---------------------------------------------------------------------
 * 2h assistance rule (§2.2): due when never refreshed this session, or
 * when the last refresh is >=2h old; not due right after a fresh refresh.
 * --------------------------------------------------------------------- */
static void test_track_gnss_assist_2h_rule(void)
{
    loc_track_t t;
    loc_track_init(&t, /*have_lis3dh=*/true);
    CHECK(loc_track_gnss_assist_due(&t, 0), "never refreshed this session: due at once");

    int64_t now = 1000 * US_PER_S;
    loc_track_gnss_assist_refreshed(&t, now);
    CHECK(!loc_track_gnss_assist_due(&t, now + 1), "right after a refresh: not due");

    now += (LOC_GNSS_ASSIST_MAX_AGE_S - 1) * US_PER_S;
    CHECK(!loc_track_gnss_assist_due(&t, now), "1s short of 2h: still not due");
    now += 1 * US_PER_S;
    CHECK(loc_track_gnss_assist_due(&t, now), "at 2h since the last refresh: due again");
}

/* ---------------------------------------------------------------------
 * Web-request GNSS budget (server-architect review, 26 Sep 2026, PROTOCOL.md
 * §13.3 item 2): min(base_budget_s, 60 - assist_elapsed_s), floored at 0.
 * --------------------------------------------------------------------- */
static void test_loc_web_gnss_budget(void)
{
    CHECK(loc_web_gnss_budget_s(LOC_ATTEMPT_S, 0) == LOC_ATTEMPT_S,
          "no assistance refresh spent: the ordinary 20s budget is untouched");
    CHECK(loc_web_gnss_budget_s(LOC_FIRST_ATTEMPT_S, 0) == LOC_FIRST_ATTEMPT_S,
          "no assistance refresh spent: the cold-boot 40s budget is untouched");
    CHECK(loc_web_gnss_budget_s(LOC_FIRST_ATTEMPT_S, 25) == 35,
          "a 25s refresh must cap a 40s budget down to the 35s remaining inside the 60s bound");
    CHECK(loc_web_gnss_budget_s(LOC_ATTEMPT_S, 50) == 10,
          "a 50s refresh leaves only 10s remaining inside the 60s bound, tighter than the "
          "ordinary 20s budget, so 10 wins");
    CHECK(loc_web_gnss_budget_s(LOC_FIRST_ATTEMPT_S, 60) == 0,
          "a refresh that alone reached the 60s bound must leave a zero GNSS-wait budget");
    CHECK(loc_web_gnss_budget_s(LOC_FIRST_ATTEMPT_S, 90) == 0,
          "a refresh that overran the 60s bound must also floor at zero, not underflow");
}

/* ---------------------------------------------------------------------
 * `why` field, byte-compared against a minimal independent CBOR encoder
 * (RFC 7049 definite-length map, same rules cbor.c/relay/app/wirecbor.py
 * all share). relay/app/wirecbor.py does not map key 60/61 yet (S2, relay-
 * side, out of scope for this firmware task) -- see this task's own report
 * for that gap. Generated with a small scratch script implementing exactly
 * those rules (uint/tstr/map/null headers, float64), not cbor2 or
 * wirecbor.py, since neither has this key.
 * --------------------------------------------------------------------- */
static void test_loc_build_cbor_why_vs_reference(void)
{
    static const char *STILL_WHY_HEX =
        "a70001016a6c5f3363396131316630021a68c45fa008f609f60b666e6f5f666978183c657374696c6c";
    static const char *CELL_WHY_HEX =
        "a80001016a6c5f3363396131316630021a68c45fa008f609f60b666e6f5f6669781831a50063333130016334"
        "313002193039031a05397fb104385e183c6463656c6c";
    static const char *GNSS_WHY_HEX =
        "a60001016a6c5f3363396131316630021a68c45fa008a300fb4042e330df9bdc6a01fbc05e9ad7b634dad3031a"
        "68c45f9709f6183c64676e7373";

    uint8_t got[192], want[192];
    size_t got_len, want_len;

    bool ok = loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_3c9a11f0", 1757700000,
                             /*have_fix=*/false, 0, 0, false, 0, 0, false, /*req=*/NULL,
                             /*cached=*/false, "no_fix", /*cell=*/NULL, "still");
    CHECK(ok, "loc_build_cbor() must succeed for the still+why vector");
    hex_decode(STILL_WHY_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "still+why vector must match the reference encoder exactly (got %zu bytes, want %zu)",
          got_len, want_len);

    loc_cell_t cell = { .mcc = "310", .mnc = "410", .tac = 12345, .ci = 87654321, .have_rsrp = true,
                        .rsrp = -95 };
    ok = loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_3c9a11f0", 1757700000, false, 0, 0,
                        false, 0, 0, false, NULL, false, "no_fix", &cell, "cell");
    CHECK(ok, "loc_build_cbor() must succeed for the cell+why vector");
    hex_decode(CELL_WHY_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "cell+why vector must match the reference encoder exactly (got %zu bytes, want %zu)",
          got_len, want_len);

    ok = loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_3c9a11f0", 1757700000,
                        /*have_fix=*/true, 37.774929, -122.419416, /*have_acc=*/false, 0, 1757699991,
                        /*src_cell=*/false, NULL, false, NULL, NULL, "gnss");
    CHECK(ok, "loc_build_cbor() must succeed for the gnss+why vector");
    hex_decode(GNSS_WHY_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "gnss+why vector must match the reference encoder exactly (got %zu bytes, want %zu)",
          got_len, want_len);
}

/* docs/GNSS_DISABLE_DESIGN.md D1: cfg.loc sub-map parser. */
static void test_parse_cfg_submap(void)
{
    uint8_t buf[64];
    cbor_w_t w;
    loc_cfg_t c;

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_bool(&w, 0, false);
    CHECK(loc_parse_cfg_submap(buf, (uint16_t) w.len, &c) && c.have_gnss && !c.gnss, "{0:false}");

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_bool(&w, 0, true);
    CHECK(loc_parse_cfg_submap(buf, (uint16_t) w.len, &c) && c.have_gnss && c.gnss, "{0:true}");

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 0);
    CHECK(loc_parse_cfg_submap(buf, (uint16_t) w.len, &c) && !c.have_gnss, "{} -> have_gnss false");

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 3);
    cbor_w_uint(&w, 7, 42);
    cbor_w_bool(&w, 0, false);
    cbor_w_tstr(&w, 9, "x", 1);
    CHECK(loc_parse_cfg_submap(buf, (uint16_t) w.len, &c) && c.have_gnss && !c.gnss,
          "unknown keys are ignored");

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_uint(&w, 0, 1); /* a bare uint, not a map */
    CHECK(!loc_parse_cfg_submap(buf, (uint16_t) w.len, &c), "non-map rejected");

    cbor_w_init(&w, buf, sizeof(buf));
    cbor_w_map(&w, 1);
    cbor_w_uint(&w, 0, 1); /* gnss must be a bool */
    CHECK(!loc_parse_cfg_submap(buf, (uint16_t) w.len, &c), "non-bool gnss rejected");
}

int main(void)
{
    test_parse_cfg_submap();
    test_backoff_sequence();
    test_reset_by_success_and_backoff_growth();
    test_reset_by_trigger_respects_attempt_floor();
    test_cell_change_debounce();
    test_motion_classifier();
    test_battery_floor();
    test_battery_unknown_sentinel();
    test_cached_only_this_session();
    test_mid_attempt_requests_share_result();
    test_attempt_budget();
    test_loc_build_cbor_vs_relay();
    test_loc_build_cbor_cell_vs_relay();
    test_parse_req_cbor();

    test_track_still_moving_with_lis3dh();
    test_track_no_lis3dh_cell_only();
    test_track_flap_ring_never_moving();
    test_track_report_scheduler();
    test_track_stop_report_on_cell_change();
    test_track_stop_report_on_gnss_published();
    test_track_unsolicited_min_gap_not_dropped();
    test_track_unsolicited_min_gap_superseded_by_higher_priority();
    test_track_gnss_schedule();
    test_track_gnss_daily_cap();
    test_track_transition_only_backoff_reset();
    test_track_gnss_assist_2h_rule();
    test_loc_web_gnss_budget();
    test_loc_build_cbor_why_vs_reference();

    if (g_failures == 0) {
        printf("PASS: 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

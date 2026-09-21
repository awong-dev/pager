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
    CHECK(!loc_battery_ok(3299), "3299mV must fail the 3.3V floor");
    CHECK(loc_battery_ok(3300), "3300mV must pass the 3.3V floor");
    CHECK(loc_battery_ok(4200), "4200mV must pass the 3.3V floor");

    loc_policy_t p;
    loc_policy_init(&p);
    loc_decision_t d = loc_on_request(&p, 0, 3299, "l_lowbatt");
    CHECK(d == LOC_ANSWER_CACHED, "a request below the battery floor must never start GNSS");
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
                             /*cached=*/false, NULL, /*cell=*/NULL);
    CHECK(ok, "loc_build_cbor() must succeed for the fix vector");
    hex_decode(FIX_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len, "fix vector length mismatch: got %zu want %zu", got_len, want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "fix vector bytes must match relay/app/wirecbor.py's own CBOR encoding exactly");

    /* no_fix: loc:null, err:"no_fix", req set. */
    ok = loc_build_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "l_deadbeef", 1757700000,
                        /*have_fix=*/false, 0, 0, /*have_acc=*/false, 0, 0, /*src_cell=*/false,
                        "m_7f3a2b10", /*cached=*/false, "no_fix", /*cell=*/NULL);
    CHECK(ok, "loc_build_cbor() must succeed for the no_fix vector");
    hex_decode(NOFIX_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len, "no_fix vector length mismatch: got %zu want %zu", got_len, want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "no_fix vector bytes must match relay/app/wirecbor.py's own CBOR encoding exactly");

    /* Contract check: have_fix and a non-NULL err must never both hold (or
     * both be absent). */
    CHECK(!loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_bad", 0, true, 0, 0, false, 0, 0,
                          false, NULL, false, "no_fix", NULL),
          "have_fix=true with a non-NULL err must be rejected");
    CHECK(!loc_build_cbor(got, sizeof(got), &got_len, false, 0, "l_bad", 0, false, 0, 0, false, 0, 0,
                          false, NULL, false, NULL, NULL),
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
                             /*src_cell=*/false, "m_7f3a2b10", /*cached=*/false, "no_fix", &cell);
    CHECK(ok, "loc_build_cbor() must succeed for the no_fix+cell vector");
    hex_decode(NOFIX_CELL_HEX, want, sizeof(want), &want_len);
    CHECK(got_len == want_len && memcmp(got, want, want_len) == 0,
          "no_fix+cell vector bytes must match relay/app/wirecbor.py's own CBOR encoding exactly "
          "(got %zu bytes, want %zu)",
          got_len, want_len);

    loc_cell_t cell_pad = { .mcc = "234", .mnc = "07", .tac = 1, .ci = 1, .have_rsrp = false };
    ok = loc_build_cbor(got, sizeof(got), &got_len, /*signed_env=*/false, 0, "l_3c9a11f0", 1757700000,
                        /*have_fix=*/false, 0, 0, /*have_acc=*/false, 0, 0, /*src_cell=*/false,
                        /*req=*/NULL, /*cached=*/false, "no_fix", &cell_pad);
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
                        false, 0, 0, false, NULL, false, "no_fix", &bad_cell);
    CHECK(ok, "a malformed cell must not fail the whole build");
    size_t got_len_nocell;
    uint8_t got_nocell[192];
    CHECK(loc_build_cbor(got_nocell, sizeof(got_nocell), &got_len_nocell, false, 0, "l_3c9a11f0",
                         1757700000, false, 0, 0, false, 0, 0, false, NULL, false, "no_fix", NULL),
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

int main(void)
{
    test_backoff_sequence();
    test_reset_by_success_and_backoff_growth();
    test_reset_by_trigger_respects_attempt_floor();
    test_cell_change_debounce();
    test_motion_classifier();
    test_battery_floor();
    test_cached_only_this_session();
    test_mid_attempt_requests_share_result();
    test_attempt_budget();
    test_loc_build_cbor_vs_relay();
    test_loc_build_cbor_cell_vs_relay();
    test_parse_req_cbor();

    if (g_failures == 0) {
        printf("PASS: 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

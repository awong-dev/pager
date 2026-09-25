/* test_clock.c — host test harness for main/clockfmt.c (TASK_clock.md,
 * owner request 24 Sep ~11pm PDT): the status bar's pure live-clock
 * formatter and minute-change detector.
 *
 * Coverage required by the task brief (Do #5):
 *  - ui_status_clock_text() equivalent (clockfmt_status_text()): "--:--" /
 *    "HH:MM"
 *  - the minute-change detector (clockfmt_due()) fires once per minute
 *    change
 *
 * TASK_ui_round2.md Do #7: local (Pacific) time, not UTC, for every on-glass
 * HH:MM. clockfmt.c itself never sees an epoch (it only formats hh/mm ints
 * the caller already converted — ui.c's compute_status_clock_text()/
 * ui_format_hhmm() do that conversion device-side via localtime_r(), and
 * ui.c is not host-buildable, driver/i2c.h/esp_log.h/FreeRTOS). What IS
 * host-testable, and is the actual thing Do #7 changed, is the TZ rule
 * itself — main.c's app_main() sets `setenv("TZ", "PST8PDT,M3.2.0,M11.1.0",
 * 1); tzset();` once at boot, and every localtime_r() call after that
 * (ESP-IDF/newlib on-device, or plain glibc/BSD libc here on the host) reads
 * the same POSIX TZ string. test_tz_rule_fixed_epoch_pdt() below sets that
 * exact string via the host's own libc and checks two known epochs — one in
 * PST (winter), one in PDT (summer, so the DST rule itself is exercised too)
 * — against their expected wall-clock HH:MM, which is what ui.c's own
 * localtime_r() calls are relying on this TZ string to do correctly on
 * device.
 */
#include "clockfmt.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

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

/* ---------------------------------------------------------------------
 * clockfmt_status_text(): "--:--" / "HH:MM".
 * --------------------------------------------------------------------- */

static void test_not_in_use_is_dashes(void)
{
    char out[6];
    clockfmt_status_text(false, true, 14, 2, out, sizeof(out));
    CHECK(strcmp(out, "--:--") == 0, "not in use must show --:--, got \"%s\"", out);
}

static void test_not_seeded_is_dashes(void)
{
    char out[6];
    clockfmt_status_text(true, false, 14, 2, out, sizeof(out));
    CHECK(strcmp(out, "--:--") == 0, "unseeded clock must show --:--, got \"%s\"", out);
}

static void test_not_in_use_and_not_seeded_is_dashes(void)
{
    char out[6];
    clockfmt_status_text(false, false, 0, 0, out, sizeof(out));
    CHECK(strcmp(out, "--:--") == 0, "neither in-use nor seeded must show --:--, got \"%s\"", out);
}

static void test_in_use_and_seeded_is_hhmm(void)
{
    char out[6];
    clockfmt_status_text(true, true, 14, 2, out, sizeof(out));
    CHECK(strcmp(out, "14:02") == 0, "in use + seeded must show HH:MM, got \"%s\"", out);
}

static void test_hhmm_zero_padded(void)
{
    char out[6];
    clockfmt_status_text(true, true, 0, 0, out, sizeof(out));
    CHECK(strcmp(out, "00:00") == 0, "midnight must zero-pad, got \"%s\"", out);

    clockfmt_status_text(true, true, 9, 5, out, sizeof(out));
    CHECK(strcmp(out, "09:05") == 0, "single-digit hh/mm must zero-pad, got \"%s\"", out);

    clockfmt_status_text(true, true, 23, 59, out, sizeof(out));
    CHECK(strcmp(out, "23:59") == 0, "last minute of the day, got \"%s\"", out);
}

/* ---------------------------------------------------------------------
 * clockfmt_due(): fires once per minute change (or in-use/seeded change),
 * and records what it just compared against so the next identical call is
 * a no-op.
 * --------------------------------------------------------------------- */

static void test_due_fires_once_per_minute_change(void)
{
    char last[6];
    memset(last, 0, sizeof(last));
    char cur[6];

    // First call ever: `last` starts as "" (all zero bytes), which never
    // equals a freshly-formatted "--:--"/"HH:MM" (both always fill all 6
    // bytes, no embedded NULs) - so the very first render is always due,
    // same as draw_status_bar()'s own s_status_clock_last starting "".
    clockfmt_status_text(true, true, 14, 2, cur, sizeof(cur));
    CHECK(clockfmt_due(cur, last, sizeof(last)), "first-ever call must be due");
    CHECK(strcmp(last, "14:02") == 0, "due call must record what it compared, got \"%s\"", last);

    // Same minute again: not due.
    clockfmt_status_text(true, true, 14, 2, cur, sizeof(cur));
    CHECK(!clockfmt_due(cur, last, sizeof(last)), "same minute again must not be due");

    // Repeated calls within the same minute: still not due, every time -
    // "fires once per minute change", not once and then forgotten.
    clockfmt_status_text(true, true, 14, 2, cur, sizeof(cur));
    CHECK(!clockfmt_due(cur, last, sizeof(last)), "repeated same-minute calls must stay not due");
    clockfmt_status_text(true, true, 14, 2, cur, sizeof(cur));
    CHECK(!clockfmt_due(cur, last, sizeof(last)), "repeated same-minute calls must stay not due (2)");

    // Minute rolls over: due exactly once.
    clockfmt_status_text(true, true, 14, 3, cur, sizeof(cur));
    CHECK(clockfmt_due(cur, last, sizeof(last)), "minute rollover must be due");
    CHECK(strcmp(last, "14:03") == 0, "due call must record the new minute, got \"%s\"", last);

    clockfmt_status_text(true, true, 14, 3, cur, sizeof(cur));
    CHECK(!clockfmt_due(cur, last, sizeof(last)), "the same new minute again must not be due");

    // Hour rollover (14:59 -> 15:00) is just another minute-change: due.
    clockfmt_status_text(true, true, 14, 59, cur, sizeof(cur));
    CHECK(clockfmt_due(cur, last, sizeof(last)), "14:59 must be due (still a minute change)");
    clockfmt_status_text(true, true, 15, 0, cur, sizeof(cur));
    CHECK(clockfmt_due(cur, last, sizeof(last)), "hour rollover 14:59 -> 15:00 must be due");
    clockfmt_status_text(true, true, 15, 0, cur, sizeof(cur));
    CHECK(!clockfmt_due(cur, last, sizeof(last)), "15:00 again must not be due");
}

static void test_due_fires_on_in_use_edge(void)
{
    char last[6];
    memset(last, 0, sizeof(last));
    char cur[6];

    clockfmt_status_text(true, true, 10, 0, cur, sizeof(cur));
    CHECK(clockfmt_due(cur, last, sizeof(last)), "first call must be due");

    // The attentive window lapses (in_use -> false) with the minute
    // unchanged: this is TASK_clock.md Do #4's "--:--" edge, and it must
    // count as due (the text on screen changes from "10:00" to "--:--").
    clockfmt_status_text(false, true, 10, 0, cur, sizeof(cur));
    CHECK(clockfmt_due(cur, last, sizeof(last)), "in-use -> not-in-use edge must be due");
    CHECK(strcmp(last, "--:--") == 0, "not-in-use must record --:--, got \"%s\"", last);

    // Staying not-in-use, even across a "minute change" that nobody sees,
    // must not be due again - it is already showing --:--.
    clockfmt_status_text(false, true, 10, 1, cur, sizeof(cur));
    CHECK(!clockfmt_due(cur, last, sizeof(last)),
          "still not in use must not be due even if the hidden minute changed");

    // Coming back into use must be due again (Do #2's "if a key/button
    // follows within the window, the clock comes back on the next render").
    clockfmt_status_text(true, true, 10, 1, cur, sizeof(cur));
    CHECK(clockfmt_due(cur, last, sizeof(last)), "not-in-use -> in-use edge must be due");
    CHECK(strcmp(last, "10:01") == 0, "back in use must record the real time, got \"%s\"", last);
}

/* ---------------------------------------------------------------------
 * TASK_ui_round2.md Do #7: the TZ rule main.c's app_main() sets once at
 * boot, exercised directly against fixed epochs — see this file's own
 * module comment above for why this is the right level to test it at
 * (clockfmt.c itself never sees an epoch).
 * --------------------------------------------------------------------- */

static void test_tz_rule_fixed_epoch_pdt(void)
{
    // Same string main.c's app_main() sets; "hardcoded per owner 25 Sep
    // 2026; a cfg field later" (main.c's own comment).
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();

    // 2025-01-15 12:00:00 UTC -> winter -> PST (UTC-8) -> 04:00 local.
    {
        time_t t = (time_t) 1736942400;
        struct tm tmv;
        localtime_r(&t, &tmv);
        CHECK(tmv.tm_hour == 4 && tmv.tm_min == 0,
              "winter epoch %lld must localtime_r() to 04:00 PST, got %02d:%02d",
              (long long) t, tmv.tm_hour, tmv.tm_min);
    }

    // 2025-07-15 12:00:00 UTC -> summer -> PDT (UTC-7) -> 05:00 local — the
    // DST half of the M3.2.0/M11.1.0 rule, not just the fixed UTC-8 offset.
    {
        time_t t = (time_t) 1752580800;
        struct tm tmv;
        localtime_r(&t, &tmv);
        CHECK(tmv.tm_hour == 5 && tmv.tm_min == 0,
              "summer epoch %lld must localtime_r() to 05:00 PDT, got %02d:%02d",
              (long long) t, tmv.tm_hour, tmv.tm_min);
    }

    // epoch_s == 0 (no network clock yet, PROTOCOL.md §3.5) is handled by
    // ui_format_hhmm()'s own explicit `epoch_s == 0` guard (ui.c), never by
    // localtime_r() at all — nothing to check about the TZ rule for that
    // case here.

    // Round 4 regression vector (PATCHES.md 1.21 post-mortem): the exact
    // epoch a live boot log seeded (build/bench-logs/full-boot.log, "clock
    // seeded from network: epoch=1790328096"), independently confirmed as
    // 2026-09-25 09:21:36 UTC. This is *this file's own* TZ mechanism
    // (setenv+tzset+localtime_r) that ui.c's on-glass formatters rely on —
    // it was never the actual bug (net.cpp's WalterModem::getClock() call
    // was, fixed in the vendored component, PATCHES.md 1.21) — kept here as
    // a regression guard against ever breaking this half again.
    {
        time_t t = (time_t) 1790328096;
        struct tm tmv;
        localtime_r(&t, &tmv);
        CHECK(tmv.tm_hour == 2 && tmv.tm_min == 21,
              "round-4 epoch %lld (09:21:36 UTC) must localtime_r() to 02:21 PDT, got %02d:%02d",
              (long long) t, tmv.tm_hour, tmv.tm_min);
    }
}

int main(void)
{
    test_not_in_use_is_dashes();
    test_not_seeded_is_dashes();
    test_not_in_use_and_not_seeded_is_dashes();
    test_in_use_and_seeded_is_hhmm();
    test_hhmm_zero_padded();
    test_due_fires_once_per_minute_change();
    test_due_fires_on_in_use_edge();
    test_tz_rule_fixed_epoch_pdt();

    if (g_failures == 0) {
        printf("PASS: clock, 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

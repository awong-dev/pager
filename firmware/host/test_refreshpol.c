/* test_refreshpol.c -- host test for main/refreshpol.c's pure core. No ESP-IDF. */
#include "refreshpol.h"

#include <stdio.h>

static int g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

#define S 1000000LL
#define MS 1000LL
#define T0 (100 * S)

static void partials(refreshpol_t *p, int n, int64_t now)
{
    for (int i = 0; i < n; i++) refreshpol_on_partial(p, now);
}

int main(void)
{
    refreshpol_t p;

    /* defaults */
    refreshpol_init(&p);
    CHECK(p.floor == 20 && p.ceiling == 60 && p.idle_s == 6 && p.gap_ms == 1500 && p.presleep_min == 6, "defaults");

    /* no full below FLOOR, however idle, nor on transition */
    partials(&p, 19, T0);
    refreshpol_on_key(&p, T0);
    CHECK(!refreshpol_want_full(&p, T0 + 600 * S, false, false), "idle below floor");
    CHECK(!refreshpol_want_full(&p, T0 + 600 * S, true, false), "transition below floor");

    /* idle gap above FLOOR */
    refreshpol_on_partial(&p, T0); /* dirty 20 */
    CHECK(!refreshpol_want_full(&p, T0 + 5 * S, false, false), "5 s < idle");
    CHECK(refreshpol_want_full(&p, T0 + 6 * S, false, false), "6 s idle");
    CHECK(p.reason == REFRESHPOL_REASON_IDLE, "reason idle");

    /* no full mid-burst, even above ceiling */
    refreshpol_init(&p);
    partials(&p, 100, T0);
    refreshpol_on_key(&p, T0);
    CHECK(!refreshpol_want_full(&p, T0 + 1499 * MS, false, false), "mid-burst above ceiling");
    CHECK(p.reason == REFRESHPOL_REASON_OTHER, "reason cleared on false");

    /* ceiling forces at the next GAP, well before IDLE_S */
    CHECK(refreshpol_want_full(&p, T0 + 1500 * MS, false, false), "ceiling at gap");
    CHECK(p.reason == REFRESHPOL_REASON_CEILING, "reason ceiling");

    /* between floor and ceiling the gap alone does not trigger */
    refreshpol_init(&p);
    partials(&p, 59, T0);
    refreshpol_on_key(&p, T0);
    CHECK(!refreshpol_want_full(&p, T0 + 2 * S, false, false), "floor<dirty<ceiling at 2 s");

    /* transition above FLOOR (key just pressed: exempt from burst guard) */
    refreshpol_init(&p);
    partials(&p, 20, T0);
    refreshpol_on_key(&p, T0);
    CHECK(refreshpol_want_full(&p, T0 + 10 * MS, true, false), "transition at floor");
    CHECK(p.reason == REFRESHPOL_REASON_TRANSITION, "reason transition");
    refreshpol_init(&p);
    partials(&p, 19, T0);
    CHECK(!refreshpol_want_full(&p, T0, true, false), "transition at floor-1");

    /* pre-sleep: below PRESLEEP_MIN no, at it yes, dirty==0 no; even mid-burst */
    refreshpol_init(&p);
    CHECK(p.presleep_min == 6, "presleep default");
    CHECK(!refreshpol_want_full(&p, T0, false, true), "presleep dirty 0");
    partials(&p, 5, T0);
    refreshpol_on_key(&p, T0);
    CHECK(!refreshpol_want_full(&p, T0 + 1 * MS, false, true), "presleep dirty 5 < min");
    refreshpol_on_partial(&p, T0);
    CHECK(refreshpol_want_full(&p, T0 + 1 * MS, false, true), "presleep dirty 6 == min");
    CHECK(p.reason == REFRESHPOL_REASON_PRESLEEP, "reason presleep");
    p.presleep_min = 0; /* 0 must still never fire on a clean screen */
    refreshpol_on_full(&p);
    CHECK(!refreshpol_want_full(&p, T0, false, true), "presleep min 0, dirty 0");
    refreshpol_on_partial(&p, T0);
    CHECK(refreshpol_want_full(&p, T0, false, true), "presleep min 0, dirty 1");

    /* reset after full */
    refreshpol_init(&p);
    partials(&p, 70, T0);
    refreshpol_on_full(&p);
    CHECK(p.dirty == 0, "dirty reset");
    CHECK(!refreshpol_want_full(&p, T0 + 600 * S, false, false), "nothing after full");
    CHECK(!refreshpol_want_full(&p, T0 + 600 * S, true, false), "no transition after full");
    CHECK(!refreshpol_want_full(&p, T0 + 600 * S, false, true), "no presleep after full");

    /* no key ever seen, dirty>=floor: idle rule applies */
    refreshpol_init(&p);
    partials(&p, 20, T0);
    CHECK(refreshpol_want_full(&p, T0, false, false), "no key ever");

    /* runtime knobs honoured */
    refreshpol_init(&p);
    p.floor = 2; p.idle_s = 1; p.gap_ms = 100; p.ceiling = 3;
    partials(&p, 2, T0);
    refreshpol_on_key(&p, T0);
    CHECK(refreshpol_want_full(&p, T0 + 1 * S, false, false), "custom idle");
    refreshpol_on_partial(&p, T0);
    CHECK(refreshpol_want_full(&p, T0 + 100 * MS, false, false), "custom ceiling gap");

    if (g_failures) {
        printf("test_refreshpol: %d FAILED\n", g_failures);
        return 1;
    }
    printf("test_refreshpol: ok\n");
    return 0;
}

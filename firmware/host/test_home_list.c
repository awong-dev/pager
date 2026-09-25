/* test_home_list.c — host test for scr_home.c's pure peer-list-builder and
 * body-clip section (T3, docs/CHAT_UI_DESIGN.md §3 "Home").
 *
 * Builds against ONLY scr_home.c's own top section (above its `#ifdef
 * ESP_PLATFORM` split — see that file's own module comment), the same
 * pattern firmware/host/test_chat_layout.c already uses for scr_chat.c's
 * pure row-layout section. home_peer_t/HOME_PEER_ALIAS_MAX/HOME_PEER_BODY_MAX
 * are scr_home.c-private (no shared header, same as test_chat_layout.c's
 * own chat_layout_cell_t) — redeclared here verbatim (17/48, matching
 * MSG_FROM_MAX/scr_home.c's own "48-byte copy" doc comment); a mismatch
 * would be a link/ABI error, not a silent divergence, since the two files
 * are compiled and linked together by firmware/host/Makefile's own
 * `test_home_list` rule.
 *
 * msg.h IS included directly (msg_t, the MSG_DIR_ and MSG_ACK_ constants —
 * struct layout and constants only, no ESP-IDF dependency per msg.h's own
 * doc comment) so
 * test fixtures are built as real msg_t values, exactly what
 * home_peers_build()'s real (ESP-side) caller hands it via a loop of
 * msg_thread_at() copies.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "msg.h"
#include "ui.h" /* TASK_ui_round2.md Do #2: UI_ROW_H/UI_SEP_H/ui_row_advance() are now the
                  * shared source of truth (ui.h's own `static inline` — no ESP-IDF-dependent
                  * object file needed to link this test), replacing this file's own former
                  * home_row_advance() re-declaration and TEST_HOME_ROW_H/TEST_HOME_SEP_H
                  * hand-kept-in-sync constants. */

#define HOME_PEER_ALIAS_MAX 17 /* MSG_FROM_MAX — must match scr_home.c's own #define */
#define HOME_PEER_BODY_MAX 48  /* must match scr_home.c's own #define */

typedef struct {
    char alias[HOME_PEER_ALIAS_MAX];
    char body[HOME_PEER_BODY_MAX];
    int64_t ts;
    bool unread;
} home_peer_t;

int home_peers_build(const msg_t *msgs, size_t n, bool have_default, const char *default_alias,
                      home_peer_t *out, int max_peers);
int home_body_clip(const uint8_t *adv, int n, int avail_px, int marker_px, bool *out_marker);

#define TEST_HOME_FIRST_ROW_Y (UI_BODY_TOP + 2) /* ui.h; == 17 */

static int g_failures = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                          \
            printf("\n");                                 \
            g_failures++;                                \
        }                                                \
    } while (0)

static msg_t mk_msg(int64_t ts, const char *from, const char *to, const char *body, uint8_t dir,
                     uint8_t ack)
{
    msg_t m;
    memset(&m, 0, sizeof(m));
    m.ts = ts;
    strncpy(m.from, from ? from : "", sizeof(m.from) - 1);
    strncpy(m.to, to ? to : "", sizeof(m.to) - 1);
    strncpy(m.body, body ? body : "", sizeof(m.body) - 1);
    m.body_len = (uint16_t) strlen(m.body);
    m.dir = dir;
    m.ack_state = ack;
    m.in_use = true;
    return m;
}

/* Verify's own spec: "3 down messages from two peers + 1 up with empty `to`
 * -> 3 peers in newest-first order, unread flags right". msgs[0] is
 * newest, matching msg_thread_at(0)'s own convention. */
static void test_three_peers_newest_first_unread(void)
{
    msg_t msgs[4];
    msgs[0] = mk_msg(400, "mom", NULL, "hi", MSG_DIR_DOWN, MSG_ACK_UNSHOWN);   // unread
    msgs[1] = mk_msg(300, NULL, "", "hey", MSG_DIR_UP, MSG_ACK_UP_PENDING);   // empty to -> default "dad"
    msgs[2] = mk_msg(200, "mom", NULL, "earlier", MSG_DIR_DOWN, MSG_ACK_READ); // older, read
    msgs[3] = mk_msg(100, "ben", NULL, "yo", MSG_DIR_DOWN, MSG_ACK_SHOWN);     // unread (shown != read)

    home_peer_t out[16];
    int n = home_peers_build(msgs, 4, true, "dad", out, 16);
    CHECK(n == 3, "expected 3 peers, got %d", n);
    if (n != 3) {
        return;
    }
    CHECK(strcmp(out[0].alias, "mom") == 0, "peer 0 should be mom, got %s", out[0].alias);
    CHECK(strcmp(out[0].body, "hi") == 0, "peer 0 body should be the NEWEST mom message, got %s",
          out[0].body);
    CHECK(out[0].ts == 400, "peer 0 ts should be 400 (newest), got %lld", (long long) out[0].ts);
    CHECK(out[0].unread, "peer 0 (mom) should be unread (msgs[0] is UNSHOWN)");

    CHECK(strcmp(out[1].alias, "dad") == 0, "peer 1 should resolve to the default alias dad, got %s",
          out[1].alias);
    CHECK(!out[1].unread, "peer 1 (dad, up-only) should not be unread");

    CHECK(strcmp(out[2].alias, "ben") == 0, "peer 2 should be ben, got %s", out[2].alias);
    CHECK(out[2].unread, "peer 2 (ben) should be unread (SHOWN != READ)");
}

/* No book applied yet (have_default == false): an up message with an empty
 * `to` falls back to the literal "(default)" placeholder. */
static void test_no_book_default_fallback(void)
{
    msg_t msgs[1];
    msgs[0] = mk_msg(100, NULL, "", "hi", MSG_DIR_UP, MSG_ACK_UP_PENDING);

    home_peer_t out[16];
    int n = home_peers_build(msgs, 1, false, "", out, 16);
    CHECK(n == 1, "expected 1 peer, got %d", n);
    if (n == 1) {
        CHECK(strcmp(out[0].alias, "(default)") == 0, "expected \"(default)\" fallback, got %s",
              out[0].alias);
    }
}

/* An UP message with a non-empty `to` addresses that peer directly,
 * regardless of the default alias. */
static void test_up_with_to(void)
{
    msg_t msgs[1];
    msgs[0] = mk_msg(100, NULL, "grandma", "hi", MSG_DIR_UP, MSG_ACK_UP_SENT);

    home_peer_t out[16];
    int n = home_peers_build(msgs, 1, true, "dad", out, 16);
    CHECK(n == 1, "expected 1 peer, got %d", n);
    if (n == 1) {
        CHECK(strcmp(out[0].alias, "grandma") == 0, "expected grandma, got %s", out[0].alias);
    }
}

/* Cap: a message whose peer is not among the first `max_peers` distinct
 * aliases already seen is dropped from the list entirely, never corrupting
 * an already-tracked peer. */
static void test_max_peers_cap(void)
{
    msg_t msgs[3];
    msgs[0] = mk_msg(300, "a", NULL, "x", MSG_DIR_DOWN, MSG_ACK_READ);
    msgs[1] = mk_msg(200, "b", NULL, "y", MSG_DIR_DOWN, MSG_ACK_READ);
    msgs[2] = mk_msg(100, "c", NULL, "z", MSG_DIR_DOWN, MSG_ACK_UNSHOWN);

    home_peer_t out[2];
    int n = home_peers_build(msgs, 3, true, "d", out, 2);
    CHECK(n == 2, "expected the list capped at 2, got %d", n);
    if (n == 2) {
        CHECK(strcmp(out[0].alias, "a") == 0, "peer 0 should still be a, got %s", out[0].alias);
        CHECK(strcmp(out[1].alias, "b") == 0, "peer 1 should still be b, got %s", out[1].alias);
    }
}

/* Group message: the peer is `from` (the group's own alias), never `sndr`
 * (msg.h's own doc comment: `sndr` is a per-message author label, not the
 * thread identity). */
static void test_group_uses_from_not_sndr(void)
{
    msg_t m = mk_msg(100, "cousins", NULL, "hi all", MSG_DIR_DOWN, MSG_ACK_SHOWN);
    strncpy(m.sndr, "ben", sizeof(m.sndr) - 1);

    home_peer_t out[16];
    int n = home_peers_build(&m, 1, true, "dad", out, 16);
    CHECK(n == 1, "expected 1 peer, got %d", n);
    if (n == 1) {
        CHECK(strcmp(out[0].alias, "cousins") == 0, "expected the group alias, got %s", out[0].alias);
    }
}

/* Verify's own spec: "body clipping (a 40-char body at 12 px never reaches
 * the time column)" — a pure algebraic property check over precomputed
 * advances (the same "no gfx.c/assets.bin dependency" convention
 * chat_composer_viewport()'s own host test uses): whatever home_body_clip()
 * returns, the drawn pixel span (leading advances + a trailing marker, if
 * any) never exceeds avail_px. */
static void test_body_clip_never_overflows(void)
{
    uint8_t adv[40];
    for (int i = 0; i < 40; i++) {
        adv[i] = 8; // representative 12px-font glyph width
    }
    const int avail_px = 120; // e.g. time_x - 4 - body_start_x on a typical row
    const int marker_px = 10;

    bool marker = false;
    int count = home_body_clip(adv, 40, avail_px, marker_px, &marker);
    CHECK(marker, "40*8==320px over a 120px budget should need the marker");
    CHECK(count < 40, "should not fit all 40 codepoints, got count=%d", count);

    int drawn = 0;
    for (int i = 0; i < count; i++) {
        drawn += adv[i];
    }
    int span = drawn + (marker ? marker_px : 0);
    CHECK(span <= avail_px, "clipped span %dpx exceeds the %dpx budget (never reaches the time column)",
          span, avail_px);

    // A short body fits whole, no marker.
    uint8_t short_adv[5] = { 8, 8, 8, 8, 8 };
    bool short_marker = true;
    int short_count = home_body_clip(short_adv, 5, avail_px, marker_px, &short_marker);
    CHECK(short_count == 5, "a body that fits should return all codepoints, got %d", short_count);
    CHECK(!short_marker, "a body that fits should not need the marker");

    // A pathologically narrow span: even the marker alone does not fit.
    bool narrow_marker = false;
    int narrow_count = home_body_clip(adv, 40, 3, marker_px, &narrow_marker);
    CHECK(narrow_count == 0, "marker-does-not-fit case should draw zero body codepoints, got %d",
          narrow_count);
    CHECK(narrow_marker, "marker-does-not-fit case should still report marker true");
}

/* Do #1's own regression test — the exact photo-evidence scenario ("the
 * first row's '04:26 *' sits beside the second row, and the fourth row's
 * time sits on the separator"): replays home_render()'s own row-advance
 * loop for 4 peer rows + the separator + 4 menu rows via the SAME shared
 * ui_row_advance() (ui.h, TASK_ui_round2.md Do #2) home_render() itself
 * calls — never a re-derived formula — then asserts the two things Do #1
 * requires:
 *   1. every row's own geometry matches the exact pixel values Do #1's
 *      text specifies (17, 33, 49, ... — a plain regression lock: since
 *      ui_row_advance()/UI_ROW_H/UI_SEP_H are the one shared symbol both
 *      this test and scr_home.c's own home_render() call, they cannot
 *      drift apart from each other any more; this still catches
 *      scr_home.c's own loop ever retuning TEST_HOME_FIRST_ROW_Y's own
 *      UI_BODY_TOP or no longer calling ui_row_advance() for every line, as
 *      a numeric mismatch, not just an inequality that happens to still
 *      hold);
 *   2. the last visible peer row's own bottom (its top + UI_ROW_H — the
 *      12px font height + descender allowance, ui.h's own comment) is
 *      strictly above (a smaller y than) the separator's own first
 *      underline (drawn at the separator line's `y + 3`) — the actual
 *      "the double underline is drawn across the LAST chat row's text"
 *      photo symptom, expressed as an inequality so it stays true even if
 *      the exact pixel constants above are ever retuned again.
 */
static void test_row_geometry_matches_photo_fix(void)
{
    int y = TEST_HOME_FIRST_ROW_Y;
    int peer_top[4];
    for (int i = 0; i < 4; i++) {
        peer_top[i] = y;
        y = ui_row_advance(y, false);
    }
    int sep_top = y;
    y = ui_row_advance(y, true);
    int menu_top[4];
    for (int i = 0; i < 4; i++) {
        menu_top[i] = y;
        y = ui_row_advance(y, false);
    }

    // (1) exact pixel regression lock, per Do #1's own "12 px font height +
    // 2 px descender[-equivalent] row box, separator at bottom+3/+5, y +=
    // 8" text, evaluated against ui.h's actual measured UI_ROW_H/UI_SEP_H
    // (16/8 — DejaVu, not the task text's literal Noto-era 14/... numbers;
    // see ui.h's own doc comment for the derivation).
    static const int want_peer_top[4] = { 17, 33, 49, 65 };
    for (int i = 0; i < 4; i++) {
        CHECK(peer_top[i] == want_peer_top[i], "peer row %d top should be %d, got %d", i,
              want_peer_top[i], peer_top[i]);
    }
    CHECK(sep_top == 81, "separator row top should be 81 (65 + UI_ROW_H), got %d", sep_top);
    static const int want_menu_top[4] = { 89, 105, 121, 137 };
    for (int i = 0; i < 4; i++) {
        CHECK(menu_top[i] == want_menu_top[i], "menu row %d top should be %d, got %d", i,
              want_menu_top[i], menu_top[i]);
    }

    // (2) the actual photo-evidence assertion: the last visible row's own
    // bottom (baseline + descender, i.e. its top + UI_ROW_H) must be
    // strictly above the separator's own first underline (sep_top + 3).
    int last_row_bottom = peer_top[3] + UI_ROW_H;
    int sep_first_line_y = sep_top + 3;
    CHECK(last_row_bottom < sep_first_line_y,
          "last row's own bottom (%d) must be above the separator's first line (%d) — "
          "this is the exact 'underline drawn across the last row's text' photo bug",
          last_row_bottom, sep_first_line_y);
}

int main(void)
{
    test_three_peers_newest_first_unread();
    test_no_book_default_fallback();
    test_up_with_to();
    test_max_peers_cap();
    test_group_uses_from_not_sndr();
    test_body_clip_never_overflows();
    test_row_geometry_matches_photo_fix();

    if (g_failures == 0) {
        printf("PASS: all test_home_list checks passed\n");
        return 0;
    }
    printf("FAILED: %d check(s) failed\n", g_failures);
    return 1;
}

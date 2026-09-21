/* test_chat_layout.c — host test for scr_chat.c's pure row-layout section
 * (owner task 2026-09-20, "wrap long messages in the chat screen").
 *
 * Builds against ONLY scr_chat.c's own top section (above its `#ifdef
 * ESP_PLATFORM` split — see that file's own module comment), the same
 * pattern firmware/host/test_msg.c already uses for msg.c's composer
 * section. No shared header exists for these declarations (scr_chat.c has
 * no scr_chat.h of its own, unlike msg.c/msg.h), so they are re-declared
 * here verbatim, matching scr_chat.c's real (non-static, extern-linkage)
 * definitions exactly — the two files are compiled and linked together by
 * firmware/host/Makefile's own `test_chat_layout` rule, so a mismatch here
 * would be a link error, not a silent divergence.
 *
 * Coverage:
 *  1. All-single-row messages: behaves like the pre-wrap implementation's
 *     message-granular scrolling (a sanity check that the new line-based
 *     model subsumes the old one when nothing wraps).
 *  2. A message spanning several rows: header row at the TOP of its own
 *     block, continuation rows below it, in reading order; newest message
 *     pinned to the bottom at scroll=0.
 *  3. Every row of a message taller than the whole viewport is reachable
 *     by varying scroll (the owner's explicit scrolling requirement).
 *  4. A short thread leaves blank (msg_index == -1) rows at the TOP, never
 *     silently drops a message.
 *  5. chat_layout_is_last_row_visible(): true only once a message's LAST
 *     row is actually within the visible window.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    int msg_index;
    int row_in_msg;
} chat_layout_cell_t;

int chat_layout_max_scroll(const int *msg_rows, int count, int rows);
int chat_layout(const int *msg_rows, int count, int rows, int scroll, chat_layout_cell_t *out_cells);
bool chat_layout_is_last_row_visible(const int *msg_rows, int count, int rows, int scroll,
                                     int msg_index);

static int g_failures = 0;

#define CHECK(cond, ...)                                \
    do {                                                 \
        if (!(cond)) {                                   \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
            printf(__VA_ARGS__);                         \
            printf("\n");                                \
            g_failures++;                                \
        }                                                \
    } while (0)

/* Five single-row messages, a 3-row viewport: behaves exactly like the
 * pre-wrap message-granular model. */
static void test_single_row_messages_like_old_model(void)
{
    int msg_rows[5] = { 1, 1, 1, 1, 1 };
    int rows = 3;
    CHECK(chat_layout_max_scroll(msg_rows, 5, rows) == 2, "max_scroll == %d, want 2",
          chat_layout_max_scroll(msg_rows, 5, rows));

    chat_layout_cell_t cells[3];
    chat_layout(msg_rows, 5, rows, 0, cells);
    /* scroll=0: bottom (cells[2]) is msg 0 (newest), then msg 1, msg 2 at top. */
    CHECK(cells[2].msg_index == 0 && cells[2].row_in_msg == 0, "cells[2] should be msg 0's row 0");
    CHECK(cells[1].msg_index == 1 && cells[1].row_in_msg == 0, "cells[1] should be msg 1's row 0");
    CHECK(cells[0].msg_index == 2 && cells[0].row_in_msg == 0, "cells[0] should be msg 2's row 0");

    chat_layout(msg_rows, 5, rows, 2, cells); /* max scroll: oldest 3 messages */
    CHECK(cells[2].msg_index == 2, "at max scroll, cells[2] should be msg 2");
    CHECK(cells[1].msg_index == 3, "at max scroll, cells[1] should be msg 3");
    CHECK(cells[0].msg_index == 4, "at max scroll, cells[0] should be msg 4 (oldest)");
}

/* A short thread (2 one-row messages) in a 4-row viewport: the extra rows
 * at the TOP are blank, never a dropped or duplicated message. */
static void test_short_thread_leaves_top_blank(void)
{
    int msg_rows[2] = { 1, 1 };
    int rows = 4;
    CHECK(chat_layout_max_scroll(msg_rows, 2, rows) == 0, "a short thread should never scroll");

    chat_layout_cell_t cells[4];
    chat_layout(msg_rows, 2, rows, 0, cells);
    CHECK(cells[3].msg_index == 0, "bottom row should be the newest message");
    CHECK(cells[2].msg_index == 1, "second-from-bottom should be the older message");
    CHECK(cells[1].msg_index == -1, "row above the oldest message should be blank");
    CHECK(cells[0].msg_index == -1, "top row should be blank for a 2-message thread");
}

/* Message 0 (newest) wraps to 3 rows; message 1 (older) is 1 row. 4-row
 * viewport shows everything at scroll=0: message 0's header row must be
 * ABOVE its own continuation rows (reading order), and its LAST row must
 * be the bottom-most screen row (newest message pinned to the bottom). */
static void test_wrapped_message_row_order(void)
{
    int msg_rows[2] = { 3, 1 };
    int rows = 4;
    chat_layout_cell_t cells[4];
    int n = chat_layout(msg_rows, 2, rows, 0, cells);
    CHECK(n == 4, "chat_layout returned %d, want 4", n);

    CHECK(cells[3].msg_index == 0 && cells[3].row_in_msg == 2,
          "bottom row should be msg 0's LAST row (row_in_msg 2), got msg %d row %d",
          cells[3].msg_index, cells[3].row_in_msg);
    CHECK(cells[2].msg_index == 0 && cells[2].row_in_msg == 1,
          "second-from-bottom should be msg 0's middle row");
    CHECK(cells[1].msg_index == 0 && cells[1].row_in_msg == 0,
          "third-from-bottom should be msg 0's HEADER row (row_in_msg 0)");
    CHECK(cells[0].msg_index == 1 && cells[0].row_in_msg == 0,
          "top row should be msg 1 (the older, single-row message)");
}

/* A single message far taller than the viewport: every one of its rows
 * must be reachable by SOME scroll value (the owner's scrolling
 * requirement — "every line of every message reachable"). */
static void test_every_row_of_a_tall_message_reachable(void)
{
    int msg_rows[1] = { 10 };
    int rows = 4;
    int max_scroll = chat_layout_max_scroll(msg_rows, 1, rows);
    CHECK(max_scroll == 6, "max_scroll == %d, want 6 (10 rows - 4 visible)", max_scroll);

    bool seen[10] = { false };
    for (int scroll = 0; scroll <= max_scroll; scroll++) {
        chat_layout_cell_t cells[4];
        chat_layout(msg_rows, 1, rows, scroll, cells);
        for (int r = 0; r < rows; r++) {
            CHECK(cells[r].msg_index == 0, "every cell should belong to the only message");
            CHECK(cells[r].row_in_msg >= 0 && cells[r].row_in_msg < 10, "row_in_msg out of range");
            seen[cells[r].row_in_msg] = true;
        }
    }
    for (int i = 0; i < 10; i++) {
        CHECK(seen[i], "row_in_msg %d of the tall message was never reachable by any scroll", i);
    }

    /* scroll=0 (default, auto-follow): the message's LAST row (its tail —
     * "showing its last rows is acceptable" for a message that does not
     * fully fit) is pinned to the very bottom of the screen. */
    chat_layout_cell_t cells[4];
    chat_layout(msg_rows, 1, rows, 0, cells);
    CHECK(cells[3].row_in_msg == 9, "at scroll=0 the bottom row should be the message's last row");

    /* At max scroll, the message's HEADER row (its first/topmost line) is
     * reachable at the top of the screen. */
    chat_layout(msg_rows, 1, rows, max_scroll, cells);
    CHECK(cells[0].row_in_msg == 0, "at max scroll the top row should be the message's header row");
}

static void test_last_row_visible(void)
{
    /* msg 0: 3 rows (newest, at the bottom); msg 1: 1 row (older, above it). */
    int msg_rows[2] = { 3, 1 };
    int rows = 2; /* deliberately smaller than msg 0 alone */

    /* scroll=0: bottom 2 rows are msg 0's rows 1 and 2 (its last row is
     * row_in_msg==2, which IS on screen) -> msg 0's last row visible. */
    CHECK(chat_layout_is_last_row_visible(msg_rows, 2, rows, 0, 0),
          "msg 0's last row should be visible at scroll=0");
    /* msg 1 is entirely off-screen (scrolled past) at scroll=0. */
    CHECK(!chat_layout_is_last_row_visible(msg_rows, 2, rows, 0, 1),
          "msg 1 should not be visible at scroll=0 (msg 0 alone fills the viewport)");

    /* Scroll up by 1: window now shows msg 0's row 0 (header) and msg 1's
     * row 0 (its only, and therefore last, row) -> msg 1 now visible, msg
     * 0's last row (row_in_msg 2) is no longer on screen. */
    CHECK(chat_layout_is_last_row_visible(msg_rows, 2, rows, 2, 1),
          "msg 1's last row should be visible after scrolling to include it");
    CHECK(!chat_layout_is_last_row_visible(msg_rows, 2, rows, 2, 0),
          "msg 0's last row should no longer be visible after scrolling past it");
}

int main(void)
{
    test_single_row_messages_like_old_model();
    test_short_thread_leaves_top_blank();
    test_wrapped_message_row_order();
    test_every_row_of_a_tall_message_reachable();
    test_last_row_visible();

    if (g_failures == 0) {
        printf("PASS: scr_chat.c chat_layout() (row ordering, blank-row handling, full "
               "scroll reachability, last-row visibility), 0 failures\n");
        return 0;
    }
    printf("FAIL: %d failure(s)\n", g_failures);
    return 1;
}

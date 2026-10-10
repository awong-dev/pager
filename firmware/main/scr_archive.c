// scr_archive.c — Home's "Archive Chat" (owner request, 10 Oct 2026,
// docs/CHAT_UI_DESIGN.md "Archive"). Lists Home's visible peers (same order,
// same display names); Enter toggles a check mark at the right edge; the last
// row "Archive Selected" archives every checked peer, toasts, and pops to
// Home. Esc pops with no change. List screens stay GFX_FONT_NORMAL
// regardless of the body text-size setting (same as Home/Pick).

#include "ui.h"
#include "archive.h"
#include "book.h"

#include <stdio.h>
#include <string.h>

#define ARCH_VISIBLE_ROWS 5 /* same derivation as scr_pick.c's PICK_VISIBLE_ROWS */
#define ARCH_MAX_PEERS 16   /* == scr_home.c's HOME_MAX_PEERS */

static char s_alias[ARCH_MAX_PEERS][17];
static int64_t s_ts[ARCH_MAX_PEERS];
static bool s_checked[ARCH_MAX_PEERS];
static int s_n = 0;
static int s_sel = 0; /* 0..s_n-1 = peers, s_n = "Archive Selected" */

static void archive_on_event(ui_evt_t evt)
{
    if (evt != UI_EVT_ENTER) {
        return;
    }
    s_n = scr_home_visible_peers(s_alias, s_ts, ARCH_MAX_PEERS);
    memset(s_checked, 0, sizeof(s_checked));
    s_sel = 0;
}

static void archive_on_key(input_key_t key)
{
    switch (key.type) {
    case INPUT_KEY_UP:
        if (s_sel > 0) {
            s_sel--;
        }
        break;
    case INPUT_KEY_DOWN:
        if (s_sel < s_n) {
            s_sel++;
        }
        break;
    case INPUT_KEY_ENTER:
        if (s_sel < s_n) {
            s_checked[s_sel] = !s_checked[s_sel];
        } else {
            int done = 0;
            for (int i = 0; i < s_n; i++) {
                if (s_checked[i]) {
                    archive_set(s_alias[i], s_ts[i]); // NVS write if changed
                    done++;
                }
            }
            ui_pop();
            char t[24];
            if (done > 0) {
                snprintf(t, sizeof(t), "Archived %d", done);
            } else {
                snprintf(t, sizeof(t), "Nothing selected");
            }
            ui_show_toast(t);
        }
        break;
    case INPUT_KEY_ESC:
        ui_pop();
        break;
    default:
        break;
    }
}

static void archive_render(void)
{
    const int sz = GFX_FONT_NORMAL;
    int total = s_n + 1;
    int scroll_top = 0;
    if (s_sel >= ARCH_VISIBLE_ROWS) {
        scroll_top = s_sel - ARCH_VISIBLE_ROWS + 1;
    }
    int max_top = (total > ARCH_VISIBLE_ROWS) ? (total - ARCH_VISIBLE_ROWS) : 0;
    if (scroll_top > max_top) {
        scroll_top = max_top;
    }

    int y = UI_BODY_TOP + 2;
    for (int r = scroll_top; r < total && r < scroll_top + ARCH_VISIBLE_ROWS; r++) {
        if (r == s_sel) {
            gfx_text(0, y, sz, ">");
        }
        if (r < s_n) {
            char nick[BOOK_NICK_MAX];
            gfx_text(10, y, sz, book_display_name(s_alias[r], nick, sizeof(nick)));
            if (s_checked[r]) {
                gfx_icon(GFX_SCREEN_W - GFX_ICON_W, y + 1, GFX_ICON_CHECK);
            }
        } else {
            gfx_text(10, y, sz, "Archive Selected");
        }
        y = ui_row_advance(y, false);
    }
    gfx_text(0, UI_FOOTER_Y, sz, "enter select   esc back");
}

const ui_screen_t g_scr_archive = {
    .name = "archive",
    .render = archive_render,
    .on_key = archive_on_key,
    .on_event = archive_on_event,
};

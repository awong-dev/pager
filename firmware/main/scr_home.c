// scr_home.c — Home screen: active chats + the fixed menu (docs/DEVICE_TASKS.md
// F6.3, docs/DEVICE_PLAN.md §5.5 "Home").
//
// Scope note (book.c does not exist yet, F7.1): §5.5's Home mockup shows one
// row per peer alias ("mom", "dad", "grandma", "system", ...), sorted by
// newest message. msg.c (unchanged by this task — not in F6.3's Files list)
// has no peer/`to` concept yet (that's F6.4's "ring 32, `to`, per-peer
// iteration" — the very next task), so there is exactly one merged thread,
// same as the pre-F6.3 ui.c rendered. This screen shows that one thread as a
// single conversation row (labelled from the most recent down message's
// `from`, or "(default)" before any arrives) rather than guessing at a
// multi-peer grouping the data doesn't support yet — faithful to what the
// current message store actually holds, not a simulation of F6.4/F7.1.
//
// "New message" and "Address book" need book.c (F7.1, does not exist);
// "Lock now" needs lock.c (F6.5, does not exist). All three are rendered as
// real, navigable, but non-functional rows (Enter shows a "not built yet"
// toast) rather than removed outright, so the menu shape already matches
// §5.5 and only needs re-wiring once those modules land.

#include "ui.h"
#include "msg.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    HROW_NEWMSG = 0,
    HROW_BOOK,
    HROW_DEVICE,
    HROW_LOCK,
    HROW_FIXED_COUNT,
} home_fixed_row_t;

static int s_sel = 0;

static bool has_convo(void) { return msg_thread_count() > 0; }
static int row_count(void) { return (has_convo() ? 1 : 0) + HROW_FIXED_COUNT; }

// -1 == the (single, merged-thread) conversation row; else a home_fixed_row_t.
static int row_kind(int sel)
{
    int off = has_convo() ? 1 : 0;
    if (off && sel == 0) {
        return -1;
    }
    return sel - off;
}

// README R6 (open, not in F6.3's Files list): copies out promptly, same
// mitigation pattern used elsewhere in this task's new code.
static bool find_peer_label(char *out, size_t cap)
{
    size_t n = msg_thread_count();
    for (size_t i = 0; i < n; i++) {
        const msg_t *m = msg_thread_at(i);
        if (m && m->dir == (uint8_t) MSG_DIR_DOWN) {
            strncpy(out, m->from, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

static const char *fixed_label(home_fixed_row_t k)
{
    switch (k) {
    case HROW_NEWMSG: return "New message (needs address book)";
    case HROW_BOOK: return "Address book (needs address book)";
    case HROW_DEVICE: return "Device";
    case HROW_LOCK: return "Lock now (needs passcode lock)";
    default: return "?";
    }
}

static void home_on_event(ui_evt_t evt)
{
    if (evt != UI_EVT_ENTER) {
        return;
    }
    int rc = row_count();
    if (s_sel >= rc) {
        s_sel = rc - 1;
    }
    if (s_sel < 0) {
        s_sel = 0;
    }
}

static void home_on_key(input_key_t key)
{
    switch (key.type) {
    case INPUT_KEY_UP:
        if (s_sel > 0) {
            s_sel--;
        }
        break;
    case INPUT_KEY_DOWN:
        if (s_sel < row_count() - 1) {
            s_sel++;
        }
        break;
    case INPUT_KEY_ENTER: {
        int kind = row_kind(s_sel);
        if (kind == -1) {
            // User-initiated open: §5.5's general "opening a chat" rule
            // (marks every rendered down message read) applies here, unlike
            // ui_incoming()'s steal path.
            ui_push(&g_scr_chat);
            scr_chat_mark_visible_read();
            break;
        }
        switch ((home_fixed_row_t) kind) {
        case HROW_NEWMSG:
        case HROW_BOOK:
            ui_show_toast("needs address book (not built yet)");
            break;
        case HROW_DEVICE:
            ui_push(&g_scr_device);
            break;
        case HROW_LOCK:
            ui_show_toast("needs passcode lock (not built yet)");
            break;
        default:
            break;
        }
        break;
    }
    default:
        break; // esc/other: "esc nothing", docs/DEVICE_PLAN.md §5.5
    }
}

static void home_render(void)
{
    const int sz = GFX_FONT_NORMAL; // list screen: stays compact regardless of the body text-size setting
    int y = UI_BODY_TOP + 2;
    bool convo = has_convo();
    int off = convo ? 1 : 0;

    if (convo) {
        char label[MSG_FROM_MAX];
        if (!find_peer_label(label, sizeof(label))) {
            strncpy(label, "(default)", sizeof(label) - 1);
            label[sizeof(label) - 1] = '\0';
        }
        const msg_t *m0 = msg_thread_at(0);
        char body[48] = "";
        char ts[6] = "--:--";
        bool unread_marker = false;
        if (m0) {
            strncpy(body, m0->body, sizeof(body) - 1);
            body[sizeof(body) - 1] = '\0';
            ui_format_hhmm(m0->ts, ts, sizeof(ts));
            unread_marker = (m0->dir == (uint8_t) MSG_DIR_DOWN && m0->ack_state != MSG_ACK_READ);
        }
        if (s_sel == 0) {
            gfx_text(0, y, sz, ">");
        }
        int x = gfx_text(10, y, sz, label);
        x = gfx_text(x + 4, y, sz, body);
        (void) x;
        char tsbuf[16];
        snprintf(tsbuf, sizeof(tsbuf), unread_marker ? "%s *" : "%s", ts);
        int tw = gfx_text_width(sz, tsbuf);
        gfx_text(GFX_SCREEN_W - tw, y, sz, tsbuf);
        y += 12;
    } else {
        gfx_text(10, y, sz, "(no messages yet)");
        y += 12;
    }

    gfx_hline(0, GFX_SCREEN_W - 1, y);
    y += 3;

    for (int k = 0; k < HROW_FIXED_COUNT; k++) {
        int idx = off + k;
        if (idx == s_sel) {
            gfx_text(0, y, sz, ">");
        }
        gfx_text(10, y, sz, fixed_label((home_fixed_row_t) k));
        y += 12;
    }

    gfx_text(0, GFX_SCREEN_H - 9, sz, "up/down move  enter open  hold=home");
}

const ui_screen_t g_scr_home = {
    .name = "home",
    .render = home_render,
    .on_key = home_on_key,
    .on_event = home_on_event,
};

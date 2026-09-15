// scr_chat.c — Chat screen: message history + inline composer
// (docs/DEVICE_TASKS.md F6.3, docs/DEVICE_PLAN.md §5.5 "Chat").
//
// Scope note (msg.c unchanged by this task — F6.4 is the very next task and
// adds `to`/per-peer iteration): this renders the one merged thread msg.c
// currently holds (same data the pre-F6.3 ui.c's thread view rendered), not
// a per-peer filtered view — see scr_home.c's own scope note for the same
// limitation. The composer reuses msg.c's existing 160-byte/ASCII-oriented
// msg_composer_*() API unchanged (F6.4 is what moves it to the 320-byte/
// 160-codepoint UTF-8 caps docs/DEVICE_PLAN.md §5.2 specifies).
//
// IME hook (docs/DEVICE_PLAN.md §5.3, ime.h): wired here per F6.3's brief —
// this is "where a chat composer would wire it up". The only IME built so
// far is the identity one (ASCII straight through), so this does not change
// observable behaviour yet, but the composer now goes through ime_feed()
// rather than pushing raw bytes directly, so a real IME drops in later
// without touching this file's key-dispatch structure.

#include "ui.h"
#include "msg.h"
#include "ime.h"

#include <stdio.h>
#include <string.h>

static int s_scroll = 0; // 0 = pinned to the newest messages (auto-follow), docs/DEVICE_PLAN.md §5.5

static int visible_rows(void)
{
    // §5.2: "at 2x the body shows 4 rows of 24 columns" for the *whole*
    // body; this screen also reserves one row for the composer, so message
    // rows are one fewer than the body's own row budget at each size.
    return (ui_text_size() == GFX_FONT_LARGE) ? 4 : 6;
}

// README R6 (open, not in F6.3's Files list): copies fields out promptly
// per index rather than holding a raw msg_t* across any work.
static void mark_visible_read(void)
{
    size_t n = msg_thread_count();
    for (size_t i = 0; i < n; i++) {
        const msg_t *m = msg_thread_at(i);
        if (!m) {
            continue;
        }
        if (m->dir == (uint8_t) MSG_DIR_DOWN && m->ack_state != MSG_ACK_READ) {
            char id[MSG_ID_MAX];
            strncpy(id, m->id, sizeof(id) - 1);
            id[sizeof(id) - 1] = '\0';
            // PROTOCOL.md §4.1 rule 2: `read` without a prior `shown`
            // promotes and back-fills shown_ts — the correct, sanctioned
            // path for a message that arrived via ui_incoming()'s
            // toast-only branch (never marked `shown`) and is only now
            // actually being looked at.
            msg_mark_read(id);
        }
    }
}

void scr_chat_mark_visible_read(void) { mark_visible_read(); }

static void chat_on_event(ui_evt_t evt)
{
    if (evt == UI_EVT_ENTER) {
        s_scroll = 0;
        msg_composer_reset();
    }
}

static void clamp_scroll(void)
{
    int count = (int) msg_thread_count();
    int rows = visible_rows();
    int max_scroll = (count > rows) ? (count - rows) : 0;
    if (s_scroll > max_scroll) {
        s_scroll = max_scroll;
    }
    if (s_scroll < 0) {
        s_scroll = 0;
    }
}

static void try_send(void)
{
    uint16_t len = msg_composer_len();
    if (len == 0) {
        return; // nothing to send; §5.5 doesn't define a beep-on-empty-enter, just do nothing
    }
    if (msg_queue_reply(msg_composer_text(), len)) {
        msg_composer_reset();
        s_scroll = 0; // auto-follow back to the newest (the reply itself)
    } else {
        ui_show_toast("reply too long or send queue full");
    }
}

static void chat_on_key(input_key_t key)
{
    mark_visible_read(); // §5.5: "...or pressing any key while it is on screen"

    switch (key.type) {
    case INPUT_KEY_CHAR: {
        ime_t ime = ime_identity();
        ime_result_t r = ime.feed(&ime, key);
        if (r.consumed) {
            for (const char *p = r.commit_utf8; *p != '\0'; p++) {
                if (!msg_composer_push_char(*p)) {
                    ui_show_toast("reply full (160 chars)");
                    break;
                }
            }
        }
        break;
    }
    case INPUT_KEY_BACKSPACE:
        msg_composer_backspace();
        break;
    case INPUT_KEY_ENTER:
        try_send();
        break;
    case INPUT_KEY_ESC:
        if (msg_composer_len() > 0) {
            msg_composer_reset(); // "esc clears it"
        } else {
            ui_pop(); // "esc on an empty composer goes back"
        }
        break;
    case INPUT_KEY_UP:
        s_scroll++;
        clamp_scroll();
        break;
    case INPUT_KEY_DOWN:
        s_scroll--;
        clamp_scroll();
        break;
    case INPUT_KEY_LEFT:
        s_scroll += 3; // "left/right a page"
        clamp_scroll();
        break;
    case INPUT_KEY_RIGHT:
        s_scroll -= 3;
        clamp_scroll();
        break;
    default:
        break;
    }
}

static void chat_render(void)
{
    gfx_font_t sz = ui_text_size();
    int pitch = (sz == GFX_FONT_LARGE) ? 16 : 12;
    int rows = visible_rows();
    clamp_scroll();
    size_t count = msg_thread_count();

    const msg_t *nu = msg_newest_unread();
    char newest_unread_id[MSG_ID_MAX] = "";
    if (nu) {
        strncpy(newest_unread_id, nu->id, sizeof(newest_unread_id) - 1);
    }

    int y = UI_BODY_TOP + 2;
    // Screen row r (0 = top) shows thread index (s_scroll + rows-1-r):
    // msg_thread_at(0) is newest (msg.h), and the mockup wants newest at
    // the bottom, so the index *decreases* going down the screen.
    for (int r = 0; r < rows; r++) {
        int idx = s_scroll + (rows - 1 - r);
        if (idx >= 0 && (size_t) idx < count) {
            const msg_t *m = msg_thread_at(idx);
            if (m) {
                char id[MSG_ID_MAX];
                strncpy(id, m->id, sizeof(id) - 1);
                id[sizeof(id) - 1] = '\0';
                char from[MSG_FROM_MAX];
                strncpy(from, m->from, sizeof(from) - 1);
                from[sizeof(from) - 1] = '\0';
                char body[80];
                strncpy(body, m->body, sizeof(body) - 1);
                body[sizeof(body) - 1] = '\0';
                char ts[6];
                ui_format_hhmm(m->ts, ts, sizeof(ts));
                uint8_t dir = m->dir;
                uint8_t ack = m->ack_state;
                uint8_t flags = m->flags;

                const char *who = (dir == (uint8_t) MSG_DIR_UP) ? "you" : from;
                int x = gfx_text(0, y, sz, who);
                x = gfx_text(x, y, sz, " ");
                x = gfx_text(x, y, sz, ts);
                x = gfx_text(x, y, sz, " ");
                gfx_text(x, y, sz, body);

                const char *tag = NULL;
                if (dir == (uint8_t) MSG_DIR_UP) {
                    if (ack == MSG_ACK_UP_SENT) {
                        tag = "sent";
                    } else if (ack == MSG_ACK_UP_FAILED || (flags & MSG_F_SEND_FAILED)) {
                        tag = "FAILED";
                    } else {
                        tag = "...";
                    }
                } else if (newest_unread_id[0] != '\0' && strcmp(id, newest_unread_id) == 0) {
                    tag = "NEW";
                }
                if (tag) {
                    int tw = gfx_text_width(sz, tag);
                    gfx_text(GFX_SCREEN_W - tw, y, sz, tag);
                }
            }
        }
        y += pitch;
    }

    gfx_hline(0, GFX_SCREEN_W - 1, y);
    y += 2;

    // Composer row — always drawn (docs/DEVICE_PLAN.md §5.5's mockup shows
    // it as a permanent bottom line, not something that only appears once
    // typing starts). Single line, not wrapped: the mockup shows exactly
    // one line and the 296px body only fits ~45 normal-size characters, so
    // a reply near the 160-byte cap runs off the visible edge (gfx_text()'s
    // own edge clip) while remaining fully intact in msg.c's buffer and
    // fully sent — a cosmetic viewport limitation, not a data limitation.
    char counter[16];
    snprintf(counter, sizeof(counter), "%u/160", (unsigned) msg_composer_len());
    int cw = gfx_text_width(GFX_FONT_NORMAL, counter);
    int x = gfx_text(0, y, GFX_FONT_NORMAL, "> ");
    gfx_text(x, y, GFX_FONT_NORMAL, msg_composer_text());
    gfx_text(GFX_SCREEN_W - cw, y, GFX_FONT_NORMAL, counter);

    gfx_text(0, GFX_SCREEN_H - 9, GFX_FONT_NORMAL, "enter send  esc back  ^v history");
}

const ui_screen_t g_scr_chat = {
    .name = "chat",
    .render = chat_render,
    .on_key = chat_on_key,
    .on_event = chat_on_event,
};

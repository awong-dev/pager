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
//
// F7.3 (docs/DEVICE_TASKS.md, docs/DEVICE_PLAN.md §5.5 "Sending from a
// chat"/"Nicknames"): this screen still renders the one merged thread (the
// scope note above is unchanged by this task — there is still no per-peer
// Chat view for scr_pick.c/scr_home.c to open "into"), so the only way a
// message sent from here can target a specific peer is the composer's own
// leading `@nick`/`@alias` word, exactly as the Nicknames paragraph
// describes: "the composer also accepts `@nick` or `@alias` as the first
// word to pick the recipient from the keyboard without the picker". Without
// a leading `@word`, `to` is empty (the default recipient, wire unchanged).
// Resolution here reuses book.h's existing public accessors
// (book_contact_count()/book_contact_at()/book_get_default_alias()) — no new
// book.h entry point was needed, so book.c/h stays untouched by this task.

#include "ui.h"
#include "msg.h"
#include "book.h"
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

// §5.5 Nicknames paragraph: a leading `@word` (first char '@', word = up to
// the next space or end of text) resolves against the approved contacts'
// nickname first, then alias — matching either "@nick" or "@alias" per the
// same paragraph. Returns true and fills `alias_out` on a match; false (and
// leaves `alias_out` untouched) if the composer has no leading `@word` at
// all (nothing to resolve — not an error) OR if the sentinel `*is_at_word`
// is set true but no contact matched (the "unknown" case the caller toasts
// on). `*word_len` is the byte length of the word (used by the caller to
// strip "@word" plus one following space from the body).
static bool resolve_at_word(const char *text, bool *is_at_word, size_t *word_len,
                             char *alias_out, size_t alias_cap)
{
    *is_at_word = false;
    *word_len = 0;
    if (text[0] != '@') {
        return false;
    }
    const char *p = text + 1;
    size_t wlen = 0;
    while (p[wlen] != '\0' && p[wlen] != ' ') {
        wlen++;
    }
    if (wlen == 0) {
        return false; // bare "@" with nothing after it — send literally, not a reference
    }
    *is_at_word = true;
    *word_len = wlen;

    size_t n = book_contact_count();
    for (size_t i = 0; i < n; i++) {
        book_contact_t c;
        if (!book_contact_at(i, &c)) {
            continue;
        }
        bool nick_match = c.nickname[0] != '\0' && strncmp(c.nickname, p, wlen) == 0 &&
                           c.nickname[wlen] == '\0';
        bool alias_match = strncmp(c.alias, p, wlen) == 0 && c.alias[wlen] == '\0';
        if (nick_match || alias_match) {
            strncpy(alias_out, c.alias, alias_cap - 1);
            alias_out[alias_cap - 1] = '\0';
            return true;
        }
    }
    return false; // *is_at_word stays true — caller toasts "unknown"
}

static void try_send(void)
{
    uint16_t len = msg_composer_len();
    if (len == 0) {
        return; // nothing to send; §5.5 doesn't define a beep-on-empty-enter, just do nothing
    }
    const char *text = msg_composer_text();

    bool is_at_word = false;
    size_t wlen = 0;
    char resolved_alias[BOOK_ALIAS_MAX] = "";
    bool matched = resolve_at_word(text, &is_at_word, &wlen, resolved_alias, sizeof(resolved_alias));

    if (is_at_word && !matched) {
        // §5.5: "picker error toast if unknown" — same ui_show_toast() path
        // every other composer error uses; the composer is left exactly as
        // typed so the student can fix the typo, same as "reply too long".
        char toast[48];
        int shown = (wlen > 16) ? 16 : (int) wlen;
        snprintf(toast, sizeof(toast), "unknown @%.*s - use New message", shown, text + 1);
        ui_show_toast(toast);
        return;
    }

    const char *body = text;
    uint16_t body_len = len;
    char to[BOOK_ALIAS_MAX] = "";

    if (is_at_word) {
        // Strip "@word" and one following space (if present) from the body.
        size_t skip = 1 + wlen;
        if (text[skip] == ' ') {
            skip++;
        }
        body = text + skip;
        body_len = (uint16_t) (len - skip);

        // §5.5: "except the default recipient, where `to` is omitted so the
        // wire stays identical to today's common case" — compare the
        // resolved alias against the book's own default, not the literal
        // `@word` typed (an `@alias` for the default recipient must also
        // omit `to`, not just an unqualified send).
        char default_alias[BOOK_ALIAS_MAX] = "";
        bool have_default = book_get_default_alias(default_alias, sizeof(default_alias));
        if (!(have_default && strcmp(default_alias, resolved_alias) == 0)) {
            strncpy(to, resolved_alias, sizeof(to) - 1);
        }
    }

    if (body_len == 0) {
        return; // "@word" consumed the whole composer — nothing left to send
    }

    if (msg_queue_reply(to, body, body_len)) {
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

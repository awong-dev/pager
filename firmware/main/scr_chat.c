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
#include "sms.h"

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

// §5.5 Nicknames paragraph, extended by v0.2 §6 (docs/V02_DESIGN.md, SMS
// contacts): a leading `@word` (first char '@', word = up to the next space
// or end of text) resolves against the approved contacts' nickname/alias
// first, then against sms.c's own SMS-contact allow-list by name — matching
// "@nick"/"@alias" (book) or "@name" (SMS). A word that matches BOTH a book
// contact and an SMS contact is deliberately never silently resolved to
// either one (V02_DESIGN.md §6: "must be disambiguated, not silently
// shadowed") — AT_AMBIGUOUS, same toast-and-leave-the-composer-alone
// treatment as AT_UNKNOWN, pointing the student at New message (scr_pick.c)
// instead, where both entries are visible as separate, distinctly tagged
// rows.
typedef enum {
    AT_NONE,      // no leading "@word" at all — nothing to resolve, not an error
    AT_BOOK,      // resolved to a book contact; `alias_out` filled
    AT_SMS,       // resolved to an SMS contact; `sms_out` filled
    AT_AMBIGUOUS, // matched both a book contact and an SMS contact
    AT_UNKNOWN,   // "@word" present but nothing matched
} at_word_result_t;

static at_word_result_t resolve_at_word(const char *text, size_t *word_len, char *alias_out,
                                         size_t alias_cap, sms_contact_t *sms_out)
{
    *word_len = 0;
    if (text[0] != '@') {
        return AT_NONE;
    }
    const char *p = text + 1;
    size_t wlen = 0;
    while (p[wlen] != '\0' && p[wlen] != ' ') {
        wlen++;
    }
    if (wlen == 0) {
        return AT_NONE; // bare "@" with nothing after it — send literally, not a reference
    }
    *word_len = wlen;

    bool book_matched = false;
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
            book_matched = true;
            break;
        }
    }

    bool sms_matched = sms_find_by_name(p, wlen, sms_out) >= 0;

    if (book_matched && sms_matched) {
        return AT_AMBIGUOUS;
    }
    if (book_matched) {
        return AT_BOOK;
    }
    if (sms_matched) {
        return AT_SMS;
    }
    return AT_UNKNOWN;
}

// Render-time-only peek at the composer's own leading `@word` (no side
// effects, never the authoritative resolution — try_send() below is) so
// chat_render() can show the tighter SMS limit while composing, per
// V02_DESIGN.md §6: "the composer shows the tighter limit ... when the
// chosen recipient is an SMS contact". Deliberately reports false (no SMS
// limit shown) for an ambiguous word — try_send() will reject it anyway,
// and showing either limit for a word that resolves to two different
// things would be misleading.
static bool composer_targets_sms(const char *text, size_t *out_skip, sms_contact_t *out)
{
    size_t wlen = 0;
    char alias_scratch[BOOK_ALIAS_MAX];
    at_word_result_t r = resolve_at_word(text, &wlen, alias_scratch, sizeof(alias_scratch), out);
    if (r != AT_SMS) {
        return false;
    }
    size_t skip = 1 + wlen;
    if (text[skip] == ' ') {
        skip++;
    }
    *out_skip = skip;
    return true;
}

static void try_send(void)
{
    uint16_t len = msg_composer_len();
    if (len == 0) {
        return; // nothing to send; §5.5 doesn't define a beep-on-empty-enter, just do nothing
    }
    const char *text = msg_composer_text();

    size_t wlen = 0;
    char resolved_alias[BOOK_ALIAS_MAX] = "";
    sms_contact_t sms_target;
    at_word_result_t at = resolve_at_word(text, &wlen, resolved_alias, sizeof(resolved_alias),
                                          &sms_target);

    if (at == AT_UNKNOWN || at == AT_AMBIGUOUS) {
        // §5.5: "picker error toast if unknown" — same ui_show_toast() path
        // every other composer error uses; the composer is left exactly as
        // typed so the student can fix the typo, same as "reply too long".
        char toast[48];
        int shown = (wlen > 16) ? 16 : (int) wlen;
        snprintf(toast, sizeof(toast), "%s @%.*s - use New message",
                 at == AT_AMBIGUOUS ? "ambiguous" : "unknown", shown, text + 1);
        ui_show_toast(toast);
        return;
    }

    const char *body = text;
    uint16_t body_len = len;
    char to[BOOK_ALIAS_MAX] = "";
    bool is_at_word = (at == AT_BOOK || at == AT_SMS);

    if (is_at_word) {
        // Strip "@word" and one following space (if present) from the body.
        size_t skip = 1 + wlen;
        if (text[skip] == ' ') {
            skip++;
        }
        body = text + skip;
        body_len = (uint16_t) (len - skip);
    }

    if (body_len == 0) {
        return; // "@word" consumed the whole composer — nothing left to send
    }

    if (at == AT_SMS) {
        // v0.2 §6: goes out via smsSend(), NOT `/down`/`/up` — sms.c's own
        // sms_queue_send() inserts the PENDING thread row and queues the
        // actual send for sms_service() (modes_run()'s task) to perform;
        // "send first, log second" also means the sms_log audit entry is
        // written by sms.c once the send attempt itself completes, not
        // here. The tighter GSM-7/UCS-2 limit is enforced here (not just
        // shown, chat_render()'s own composer_targets_sms() peek) so a
        // message that grew past the limit after the `@name` was typed
        // cannot silently overflow into truncation.
        sms_measure_t m;
        sms_measure(body, body_len, sms_get_charset_mode(), &m);
        if (sms_decide_encoding(&m) == SMS_ENC_TOO_LONG) {
            ui_show_toast("message too long for SMS");
            return;
        }
        if (sms_queue_send(&sms_target, body, body_len)) {
            msg_composer_reset();
            s_scroll = 0;
        } else {
            ui_show_toast("SMS unavailable or send queue full");
        }
        return;
    }

    if (at == AT_BOOK) {
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
    // Glyph height plus 2 px of leading (owner decision 2026-09-20, from real
    // hardware: rows set solid at the glyph height read as cramped). Budget
    // at 128 px: body starts at y=17; normal 6 x 14 = 84, large 4 x 18 = 72;
    // then the rule (2 px) and the 12 px composer row end at 115 / 103.
    int pitch = ((sz == GFX_FONT_LARGE) ? 16 : 12) + 2;
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
    // v0.2 §6: tighter GSM-7/UCS-2 counter while the composer's leading
    // `@word` resolves (unambiguously) to an SMS contact — a render-time-only
    // peek (composer_targets_sms()), never the authoritative encoding
    // decision (try_send() re-measures at send time).
    char counter[24];
    sms_contact_t sms_peek;
    size_t sms_skip = 0;
    if (composer_targets_sms(msg_composer_text(), &sms_skip, &sms_peek)) {
        const char *counted = msg_composer_text();
        size_t counted_len = msg_composer_len();
        if (sms_skip <= counted_len) {
            counted += sms_skip;
            counted_len -= sms_skip;
        }
        sms_measure_t m;
        sms_measure(counted, counted_len, sms_get_charset_mode(), &m);
        sms_encoding_t enc = sms_decide_encoding(&m);
        size_t limit = (enc == SMS_ENC_UCS2) ? SMS_UCS2_MAX_UNITS : SMS_GSM7_MAX_SEPTETS;
        size_t used = (enc == SMS_ENC_UCS2) ? m.ucs2_units : m.gsm7_septets;
        snprintf(counter, sizeof(counter), "%u/%u sms", (unsigned) used, (unsigned) limit);
    } else {
        snprintf(counter, sizeof(counter), "%u/160", (unsigned) msg_composer_len());
    }
    int cw = gfx_text_width(GFX_FONT_NORMAL, counter);
    int x = gfx_text(0, y, GFX_FONT_NORMAL, "> ");
    gfx_text(x, y, GFX_FONT_NORMAL, msg_composer_text());
    gfx_text(GFX_SCREEN_W - cw, y, GFX_FONT_NORMAL, counter);
    // No key-hint footer (owner decision 2026-09-20): the space goes to the
    // message rows' leading instead.
}

const ui_screen_t g_scr_chat = {
    .name = "chat",
    .render = chat_render,
    .on_key = chat_on_key,
    .on_event = chat_on_event,
};

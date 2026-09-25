// scr_home.c — Home screen: peer list + the fixed menu (docs/DEVICE_TASKS.md
// F6.3, docs/DEVICE_PLAN.md §5.5 "Home"; superseded by T3,
// docs/CHAT_UI_DESIGN.md §3 "Home"/§4/§5, which is the authority for this
// file's actual row shape, scroll, toast position and Pick/Book wiring —
// read that document first.

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "msg.h" // msg_t, MSG_DIR_*/MSG_ACK_READ, MSG_FROM_MAX — struct/consts only, no ESP-IDF dep

// ---------------------------------------------------------------------------
// Peer-list model (T3 Do #1, docs/CHAT_UI_DESIGN.md §3) — pure index/string
// arithmetic over a caller-supplied array of msg_t COPIES (never a live
// msg.c pointer, same README R6 discipline every other caller of
// msg_thread_at() already follows), no gfx.h/book.h/ESP-IDF dependency, same
// split/host-test pattern scr_chat.c's own pure row-layout section uses
// (that file's own module comment). home_render() (the ESP-only section
// below) is what actually calls msg_thread_at() in a loop to fill the array
// this takes, and book.h's accessors to resolve a nickname/the default
// alias — this section only ever sees plain strings and msg_t fields.
//
// T4 (docs/CHAT_UI_DESIGN.md §3 "Chat"): the actual peer-attribution rule
// (which alias a given message belongs to) moved to msg.c's msg_peer_of()
// so Chat's own per-peer row source (msg_iter_peer()) uses the exact same
// rule and the two screens can never disagree — msg.h has no ESP-IDF
// dependency of its own (msg_t is a plain struct), so calling it here does
// not pull anything new into this file's host-test build beyond msg.o
// (firmware/host/Makefile's test_home_list rule).
// ---------------------------------------------------------------------------

#define HOME_MAX_PEERS 16       /* Do #1: "Cap 16 peers" */
#define HOME_PEER_ALIAS_MAX MSG_FROM_MAX /* 17 — matches msg_t.from/to's own cap */
#define HOME_PEER_BODY_MAX 48            /* Do #1: "newest body (48-byte copy)" */

typedef struct {
    char alias[HOME_PEER_ALIAS_MAX];
    char body[HOME_PEER_BODY_MAX];
    int64_t ts;  /* newest message's ts; HH:MM formatting is a render-time
                  * concern (ui_format_hhmm(), ui.c) — kept raw here so this
                  * section stays free of that module's ESP-IDF-linked build. */
    bool unread;
} home_peer_t;

/* Walks `msgs[0..n-1]` (newest-first — msgs[0] == msg_thread_at(0), the
 * same order the caller must supply) and collects up to `max_peers`
 * distinct peer aliases, in newest-activity-first order:
 *   - a DOWN message's peer is `from` (the group alias for a group message
 *     — msg.h's own doc comment: `sndr` is a per-message AUTHOR label, not
 *     the thread identity, so it is never used here);
 *   - an UP message's peer is `to`, or, when `to` is empty: `default_alias`
 *     if `have_default` is true, else the literal fallback "(default)"
 *     (no book applied yet — same fallback the pre-T3 single-merged-row
 *     Home used).
 * Each peer's `body`/`ts` are its FIRST-ENCOUNTERED (= newest, since `msgs`
 * is newest-first) message's; `unread` is true iff ANY down message from
 * that peer — not just its newest — has ack_state != MSG_ACK_READ, so an
 * older unread page under an already-read newer reply from the same peer
 * still shows the `*` marker. A message whose peer isn't among the first
 * `max_peers` distinct aliases seen is simply not counted towards a 17th+
 * peer (never corrupts an already-collected peer's own fields). Returns
 * the number of peers written to `out[0..N-1]`. */
int home_peers_build(const msg_t *msgs, size_t n, bool have_default, const char *default_alias,
                      home_peer_t *out, int max_peers)
{
    int count = 0;
    for (size_t i = 0; i < n; i++) {
        const msg_t *m = &msgs[i];
        char alias[HOME_PEER_ALIAS_MAX];
        // T4 (docs/CHAT_UI_DESIGN.md §3): this rule now lives in msg.c's
        // msg_peer_of(), shared with msg_iter_peer() (Chat's own per-peer
        // row source), so the two screens can never disagree — see that
        // function's own doc comment for the exact rule.
        msg_peer_of(m, have_default, default_alias, alias, sizeof(alias));

        int idx = -1;
        for (int j = 0; j < count; j++) {
            if (strcmp(out[j].alias, alias) == 0) {
                idx = j;
                break;
            }
        }
        if (idx < 0) {
            if (count >= max_peers) {
                continue; // 17th+ distinct peer: not tracked, see doc comment above
            }
            idx = count++;
            strncpy(out[idx].alias, alias, HOME_PEER_ALIAS_MAX - 1);
            out[idx].alias[HOME_PEER_ALIAS_MAX - 1] = '\0';
            strncpy(out[idx].body, m->body, HOME_PEER_BODY_MAX - 1);
            out[idx].body[HOME_PEER_BODY_MAX - 1] = '\0';
            out[idx].ts = m->ts;
            out[idx].unread = false;
        }
        if (m->dir == (uint8_t) MSG_DIR_DOWN && m->ack_state != MSG_ACK_READ) {
            out[idx].unread = true;
        }
    }
    return count;
}

/* T3 Do #2 (docs/CHAT_UI_DESIGN.md §3/§4 — "Peer row: ... body clipped to
 * time_x - 4"): the mirror image of scr_chat.c's own chat_composer_viewport()
 * — that one keeps the TAIL of a composer's in-progress text visible
 * (marker at the FRONT); a peer row instead shows the HEAD of its newest
 * body, with the "…" marker at the END once it doesn't fit. Same
 * "precomputed per-codepoint advance array" input convention (host-testable
 * without gfx.c/assets.bin — home_render() below is what actually calls
 * gfx_glyph_advance() to fill `adv`). Returns how many LEADING codepoints
 * of `adv` fit inside `avail_px`; *out_marker is set true iff not all `n`
 * fit, in which case `marker_px` (the "…" glyph's own width) is reserved
 * out of `avail_px` before counting — the caller's own "stay >= 4px left of
 * the time column" margin is folded into `avail_px` before this function
 * ever sees it. n <= 0 -> 0, no marker. A marker that alone does not fit
 * returns 0 with marker true (draw nothing but the marker), same
 * pathological-narrow-span handling chat_composer_viewport() documents. */
int home_body_clip(const uint8_t *adv, int n, int avail_px, int marker_px, bool *out_marker)
{
    if (n <= 0) {
        if (out_marker) {
            *out_marker = false;
        }
        return 0;
    }
    int total = 0;
    for (int i = 0; i < n; i++) {
        total += adv[i];
    }
    if (total <= avail_px) {
        if (out_marker) {
            *out_marker = false;
        }
        return n;
    }
    if (out_marker) {
        *out_marker = true;
    }
    int budget = avail_px - marker_px;
    if (budget <= 0) {
        return 0;
    }
    int sum = 0;
    int count = 0;
    for (int i = 0; i < n; i++) {
        int next = sum + adv[i];
        if (next > budget) {
            break;
        }
        sum = next;
        count++;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Row geometry: moved to ui.h/ui.c as UI_ROW_H/ui_row_advance()/
// ui_draw_row_separator() (TASK_ui_round2.md Do #2) — see ui.h's own module
// comment on those for the full derivation (Do #1, owner photo evidence 24
// Sep 22:35 PDT: the HH:MM/`*` column drawn one row below its own chat row,
// and the double-underline separator drawn across the LAST chat row's own
// text; DejaVu Sans's measured baseline=13 + 3px descent for g/y/j/p/q at
// the 12px size). home_render() below (the ESP-only section) now calls
// ui_row_advance()/ui_draw_row_separator() directly via ui.h, shared with
// scr_pick.c/scr_book.c/scr_device.c's own list rows — this file no longer
// has a private copy of this math. The host test (firmware/host/
// test_home_list.c) gets the same UI_ROW_H/ui_row_advance() by including
// ui.h directly (it is `static inline` there specifically so no
// ESP-IDF-dependent object file needs linking for that).
// ---------------------------------------------------------------------------

#ifdef ESP_PLATFORM

#include "ui.h"
#include "book.h"
#include "lock.h" /* F6.5: "Lock now" (docs/DEVICE_PLAN.md §5.8) */

#include <stdio.h>

typedef enum {
    HROW_NEWMSG = 0,
    HROW_BOOK,
    HROW_DEVICE,
    HROW_LOCK,
    HROW_FIXED_COUNT,
} home_fixed_row_t;

static int s_sel = 0;

static msg_t s_msg_scratch[MSG_THREAD_DEPTH];
static home_peer_t s_peers[HOME_MAX_PEERS];
static int s_peer_count = 0;
static char s_default_alias[BOOK_ALIAS_MAX];
static bool s_have_default = false;

// Rebuilds s_peers/s_peer_count/s_default_alias/s_have_default from the
// live thread + book — cheap (<=MSG_THREAD_DEPTH msg_t copies via
// msg_thread_at()'s own README R6 per-index copy-out contract; book.h's
// accessors are O(book size), capped at BOOK_MAX_CONTACTS). Called at the
// top of home_on_event()/home_on_key()/home_render() so all three agree on
// the same list within one call, matching scr_device.c's own
// build-fresh-every-call convention (no cross-call cache to go stale).
static void refresh_peers(void)
{
    size_t n = msg_thread_count();
    if (n > MSG_THREAD_DEPTH) {
        n = MSG_THREAD_DEPTH;
    }
    for (size_t i = 0; i < n; i++) {
        const msg_t *m = msg_thread_at(i);
        if (!m) {
            n = i;
            break;
        }
        s_msg_scratch[i] = *m;
    }
    s_have_default = book_get_default_alias(s_default_alias, sizeof(s_default_alias));
    s_peer_count =
        home_peers_build(s_msg_scratch, n, s_have_default, s_default_alias, s_peers, HOME_MAX_PEERS);
}

static int row_count(void) { return s_peer_count + HROW_FIXED_COUNT; }

static void clamp_sel(void)
{
    int rc = row_count();
    if (s_sel >= rc) {
        s_sel = rc - 1;
    }
    if (s_sel < 0) {
        s_sel = 0;
    }
}

static const char *fixed_label(home_fixed_row_t k)
{
    switch (k) {
    case HROW_NEWMSG: return "New message";
    case HROW_BOOK: return "Address book";
    case HROW_DEVICE: return "Device";
    case HROW_LOCK: return lock_is_set() ? "Lock now" : "Lock now (no passcode set)";
    default: return "?";
    }
}

static void home_on_event(ui_evt_t evt)
{
    if (evt != UI_EVT_ENTER) {
        return;
    }
    refresh_peers();
    clamp_sel();
}

static void home_on_key(input_key_t key)
{
    refresh_peers();
    clamp_sel();
    int rc = row_count();
    switch (key.type) {
    case INPUT_KEY_UP:
        if (s_sel > 0) {
            s_sel--;
        }
        break;
    case INPUT_KEY_DOWN:
        if (s_sel < rc - 1) {
            s_sel++;
        }
        break;
    case INPUT_KEY_ENTER:
        if (s_sel < s_peer_count) {
            const home_peer_t *p = &s_peers[s_sel];
            // T4 (docs/CHAT_UI_DESIGN.md §3 "Chat"): opens THIS peer's own
            // filtered chat (scr_chat_open_peer(), ui.h) — no more `@alias`
            // composer prefill trick (scr_chat_open_with_prefix(), removed):
            // the per-peer view itself is what makes a plain Enter in Chat
            // now address the right peer.
            scr_chat_open_peer(p->alias);
        } else {
            switch ((home_fixed_row_t) (s_sel - s_peer_count)) {
            case HROW_NEWMSG:
                ui_push(&g_scr_pick);
                break;
            case HROW_BOOK:
                ui_push(&g_scr_book);
                break;
            case HROW_DEVICE:
                ui_push(&g_scr_device);
                break;
            case HROW_LOCK:
                // F6.5 (docs/DEVICE_PLAN.md §5.8): lock_now() is a no-op if
                // no passcode is configured; the actual screen-stack
                // transition happens on the very next modes_run() iteration
                // (lock_screen_sync()).
                if (lock_is_set()) {
                    lock_now();
                } else {
                    ui_show_toast("no passcode set (Device screen)");
                }
                break;
            default:
                break;
            }
        }
        break;
    case INPUT_KEY_CHAR:
        // Bench finding (22 Sep): people start typing on Home, where letters
        // did nothing, and the text was lost. Typing here opens the chat and
        // hands the key on, so the first character is not dropped. T4
        // (docs/CHAT_UI_DESIGN.md §3 Do #5): the newest chat's peer (Home's
        // own row 0, already newest-activity-first), or the default peer
        // when there are no chats yet.
        scr_chat_open_peer(s_peer_count > 0 ? s_peers[0].alias : NULL);
        if (g_scr_chat.on_key) {
            g_scr_chat.on_key(key);
        }
        break;
    default:
        break; // esc/other: "esc nothing", docs/DEVICE_PLAN.md §5.5
    }
}

// Decodes one UTF-8 codepoint starting at `p`, returning a pointer to the
// next codepoint's start — byte-for-byte identical to scr_chat.c's own
// composer_utf8_next() (that file's own comment on why this is duplicated
// rather than shared: gfx.c's utf8_next() is private, same reason sms.c has
// its own copy too).
static const char *home_utf8_next(const char *p, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t *) p;
    uint8_t b0 = u[0];
    int extra;
    uint32_t v;
    if (b0 < 0x80) {
        *cp = b0;
        return p + 1;
    } else if ((b0 & 0xE0) == 0xC0) {
        extra = 1;
        v = b0 & 0x1F;
    } else if ((b0 & 0xF0) == 0xE0) {
        extra = 2;
        v = b0 & 0x0F;
    } else if ((b0 & 0xF8) == 0xF0) {
        extra = 3;
        v = b0 & 0x07;
    } else {
        *cp = 0xFFFD;
        return p + 1;
    }
    for (int i = 1; i <= extra; i++) {
        uint8_t bi = u[i];
        if (bi == 0 || (bi & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return p + 1;
        }
        v = (v << 6) | (bi & 0x3F);
    }
    *cp = v;
    return p + 1 + extra;
}

static bool nickname_for_alias(const char *alias, char *out, size_t cap)
{
    size_t n = book_contact_count();
    for (size_t i = 0; i < n; i++) {
        book_contact_t c;
        if (!book_contact_at(i, &c)) {
            continue;
        }
        if (strcmp(c.alias, alias) == 0 && c.nickname[0] != '\0') {
            strncpy(out, c.nickname, cap - 1);
            out[cap - 1] = '\0';
            return true;
        }
    }
    return false;
}

// One line of Home's single scrollable list (docs/CHAT_UI_DESIGN.md §3):
// peer rows, then (if any peers at all) the double-underline separator, or
// the "(no chats yet)" placeholder when there are none, then the fixed menu
// rows — built fresh every render into a bounded stack array (no heap),
// same shape scr_device.c's device_render_normal() builds its own
// info+menu lines into, but tagged with a `kind` since this screen's lines
// aren't all the same shape.
typedef enum { HLINE_PEER, HLINE_SEP, HLINE_PLACEHOLDER, HLINE_MENU } home_line_kind_t;

typedef struct {
    home_line_kind_t kind;
    int idx;         /* HLINE_PEER: index into s_peers; HLINE_MENU: home_fixed_row_t */
    bool selectable;
    int sel_index;    /* valid iff selectable — matches s_sel's own numbering */
} home_line_t;

// Do #1 fix: was 7, sized against a 12px row (no descender allowance) —
// see ui.h's UI_ROW_H comment for why that undercounted a real row's own
// ink span and let it run into the next row/the separator.
// Recomputed against the real UI_ROW_H (16, not 12): (UI_FOOTER_Y -
// (UI_BODY_TOP+2)) / UI_ROW_H == (110-17)/16 == 93/16 == 5.8, floored to
// 5 — the worst case (peer_count >= HOME_VISIBLE_ROWS, so the visible
// window is all HLINE_PEER rows, no separator's own cheaper UI_SEP_H
// slot in view at all) is HOME_VISIBLE_ROWS * UI_ROW_H == 80 <= the real
// 93px budget, same "row-granular scroll, never overflows past the
// footer" conservatism scr_device.c's own device_render_normal() uses.
#define HOME_VISIBLE_ROWS 5

static void home_render(void)
{
    const int sz = GFX_FONT_NORMAL; // list screen: stays compact regardless of the body text-size setting
    refresh_peers();
    clamp_sel();

    home_line_t lines[HOME_MAX_PEERS + 1 + HROW_FIXED_COUNT];
    int nlines = 0;
    if (s_peer_count > 0) {
        for (int i = 0; i < s_peer_count; i++) {
            lines[nlines].kind = HLINE_PEER;
            lines[nlines].idx = i;
            lines[nlines].selectable = true;
            lines[nlines].sel_index = i;
            nlines++;
        }
        lines[nlines].kind = HLINE_SEP;
        lines[nlines].idx = 0;
        lines[nlines].selectable = false;
        lines[nlines].sel_index = -1;
        nlines++;
    } else {
        lines[nlines].kind = HLINE_PLACEHOLDER;
        lines[nlines].idx = 0;
        lines[nlines].selectable = false;
        lines[nlines].sel_index = -1;
        nlines++;
    }
    for (int k = 0; k < HROW_FIXED_COUNT; k++) {
        lines[nlines].kind = HLINE_MENU;
        lines[nlines].idx = k;
        lines[nlines].selectable = true;
        lines[nlines].sel_index = s_peer_count + k;
        nlines++;
    }

    int sel_line = 0;
    for (int i = 0; i < nlines; i++) {
        if (lines[i].selectable && lines[i].sel_index == s_sel) {
            sel_line = i;
            break;
        }
    }
    int scroll_top = 0;
    if (sel_line >= HOME_VISIBLE_ROWS) {
        scroll_top = sel_line - HOME_VISIBLE_ROWS + 1;
    }
    int max_top = (nlines > HOME_VISIBLE_ROWS) ? (nlines - HOME_VISIBLE_ROWS) : 0;
    if (scroll_top > max_top) {
        scroll_top = max_top;
    }

    int y = UI_BODY_TOP + 2;
    for (int i = scroll_top; i < nlines && i < scroll_top + HOME_VISIBLE_ROWS; i++) {
        const home_line_t *ln = &lines[i];
        bool sel = ln->selectable && ln->sel_index == s_sel;
        switch (ln->kind) {
        case HLINE_PEER: {
            // Do #1: alias/body and the HH:MM/`*` column below are both
            // drawn at this SAME `y` — one row's single pen position, never
            // a `y` recomputed partway through — so they can never land on
            // different rows relative to each other; only the row-box
            // HEIGHT (UI_ROW_H, ui.h) needed fixing.
            const home_peer_t *p = &s_peers[ln->idx];
            if (sel) {
                gfx_text(0, y, sz, ">");
            }
            char nick[BOOK_NICK_MAX];
            const char *alias_disp = nickname_for_alias(p->alias, nick, sizeof(nick)) ? nick : p->alias;
            int x = gfx_text(10, y, sz, "[");
            x = gfx_text(x, y, sz, alias_disp);
            x = gfx_text(x, y, sz, "] ");

            char ts[6];
            ui_format_hhmm(p->ts, ts, sizeof(ts));
            char tsbuf[8];
            snprintf(tsbuf, sizeof(tsbuf), p->unread ? "%s *" : "%s", ts);
            int tw = gfx_text_width(sz, tsbuf);
            int time_x = GFX_SCREEN_W - tw;
            int avail = time_x - 4 - x;
            if (avail < 0) {
                avail = 0;
            }

            uint8_t adv[HOME_PEER_BODY_MAX];
            const char *cp_start[HOME_PEER_BODY_MAX];
            int n = 0;
            const char *cur = p->body;
            while (*cur && n < HOME_PEER_BODY_MAX) {
                cp_start[n] = cur;
                uint32_t cp;
                cur = home_utf8_next(cur, &cp);
                adv[n] = (uint8_t) gfx_glyph_advance(sz, cp);
                n++;
            }
            static const char MARKER[] = "…";
            int marker_w = gfx_text_width(sz, MARKER);
            bool marker;
            int count = home_body_clip(adv, n, avail, marker_w, &marker);
            if (count > 0) {
                const char *end = (count < n) ? cp_start[count] : cur;
                char clipped[HOME_PEER_BODY_MAX];
                size_t blen = (size_t) (end - p->body);
                if (blen >= sizeof(clipped)) {
                    blen = sizeof(clipped) - 1;
                }
                memcpy(clipped, p->body, blen);
                clipped[blen] = '\0';
                x = gfx_text(x, y, sz, clipped);
            }
            if (marker) {
                x = gfx_text(x, y, sz, MARKER);
            }
            (void) x;
            gfx_text(time_x, y, sz, tsbuf);
            break;
        }
        case HLINE_SEP:
            // Do #1: drawn at y_last_row_bottom + 3 / + 5 — `y` here IS
            // already y_last_row_bottom, since the peer row directly above
            // advanced by UI_ROW_H (ui_row_advance(), below) before this
            // iteration started, so the double underline can never land on
            // that row's own descenders (UI_ROW_H already reaches them;
            // ui_draw_row_separator() is 3px clear of that). Shared helper,
            // TASK_ui_round2.md Do #2 — see ui.h's own doc comment.
            ui_draw_row_separator(y);
            break;
        case HLINE_PLACEHOLDER:
            gfx_text(10, y, sz, "(no chats yet)");
            break;
        case HLINE_MENU:
            if (sel) {
                gfx_text(0, y, sz, ">");
            }
            gfx_text(10, y, sz, fixed_label((home_fixed_row_t) ln->idx));
            break;
        default:
            break;
        }
        y = ui_row_advance(y, ln->kind == HLINE_SEP);
    }

    gfx_text(0, UI_FOOTER_Y, sz, "up/down move  enter open  hold=home");
}

const ui_screen_t g_scr_home = {
    .name = "home",
    .render = home_render,
    .on_key = home_on_key,
    .on_event = home_on_event,
};

#endif /* ESP_PLATFORM */

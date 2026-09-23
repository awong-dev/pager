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

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ---------------------------------------------------------------------------
// Chat row layout (owner request 2026-09-20: "wrap long messages in the
// chat screen") — pure index arithmetic, no gfx.h/msg.h/ESP-IDF dependency,
// host-tested by firmware/host/test_chat_layout.c the same way msg.c splits
// its composer section above its own `#ifdef ESP_PLATFORM` (this whole
// block sits above THIS file's split, below). Everything that actually
// needs to measure text (gfx_text_width()/gfx_text_wrap()) or read the
// thread (msg_thread_at()) lives in the ESP-only section below and feeds
// this section nothing but small int arrays.
//
// Model: each message occupies some number of consecutive "lines" — row 0
// is its header row (the "who HH:MM " prefix + first chunk of body + the
// right-aligned tag, if any), rows 1.. are wrapped continuation rows. Lay
// these out as one flat, newest-at-the-bottom line sequence: message index
// 0 (msg_thread_at(0), the newest) occupies the BOTTOM-most msg_rows[0]
// lines of that sequence, its own row 0 (header) being the topmost of
// *those*; message index 1 occupies the next msg_rows[1] lines up, and so
// on. `scroll` is a LINE offset into this flat sequence (0 = pinned to the
// very bottom = the newest message's last row), not a message offset —
// unlike the pre-wrap implementation's message-granular s_scroll, a line
// granularity is what makes every row of a message taller than the whole
// viewport still individually reachable (owner's scrolling requirement).
// ---------------------------------------------------------------------------

typedef struct {
    int msg_index;  // index into the caller's msg_rows[]/message list, or -1 = no message on this screen row (blank; only ever the TOP rows, when the whole thread is shorter than the viewport)
    int row_in_msg; // which row of that message (0 = its header row), meaningful only when msg_index >= 0
} chat_layout_cell_t;

// Sum of every (positive) entry in msg_rows[0..count-1] — the total number
// of lines in the flat sequence described above. A non-positive entry
// (should not happen; every real message wraps to at least 1 row) is
// treated as 0 rather than corrupting the running total.
static int chat_layout_total_lines(const int *msg_rows, int count)
{
    int total = 0;
    for (int i = 0; i < count; i++) {
        if (msg_rows[i] > 0) {
            total += msg_rows[i];
        }
    }
    return total;
}

// The largest `scroll` (line offset) that still shows real content in every
// screen row — 0 if the whole thread already fits in `rows` lines (nothing
// to scroll). Mirrors the pre-wrap implementation's
// `(count > rows) ? (count - rows) : 0`, just in lines instead of messages.
int chat_layout_max_scroll(const int *msg_rows, int count, int rows)
{
    int total = chat_layout_total_lines(msg_rows, count);
    return (total > rows) ? (total - rows) : 0;
}

// Fills out_cells[0..rows-1] (out_cells[rows - 1] is the BOTTOM/newest-most
// screen row, out_cells[0] the top one — matching how chat_render() paints
// top-to-bottom) with which message/row belongs on each screen row at line
// offset `scroll`. A screen row whose flat line index falls outside
// [0, total_lines) is left blank (msg_index = -1); this only ever happens
// at the TOP of the screen, when the whole thread is shorter than the
// viewport — see this file's own module comment for why a message that
// does not fully fit is otherwise always shown from its LAST rows first,
// never silently dropped.
int chat_layout(const int *msg_rows, int count, int rows, int scroll, chat_layout_cell_t *out_cells)
{
    if (rows <= 0 || !out_cells) {
        return 0;
    }
    int total = chat_layout_total_lines(msg_rows, count);
    for (int r = 0; r < rows; r++) {
        int flat = scroll + (rows - 1 - r);
        chat_layout_cell_t cell;
        cell.msg_index = -1;
        cell.row_in_msg = 0;
        if (flat >= 0 && flat < total) {
            int cum = 0;
            for (int i = 0; i < count; i++) {
                int ri = (msg_rows[i] > 0) ? msg_rows[i] : 0;
                if (flat < cum + ri) {
                    cell.msg_index = i;
                    cell.row_in_msg = ri - 1 - (flat - cum);
                    break;
                }
                cum += ri;
            }
        }
        out_cells[r] = cell;
    }
    return rows;
}

// True iff message `msg_index`'s LAST row (its highest row_in_msg, i.e. the
// row closest to the newer messages below it) is currently on screen at
// line offset `scroll` — the owner's mark-as-read rule: "a message counts
// as seen only if at least its last row is on screen". A message's last
// row is always the BOTTOM-most line of its own block in the flat sequence
// above, i.e. the line at flat index `cum` (the running total of every
// newer message's row count) — so this is a one-comparison check once
// `cum` is known, no need to build the full out_cells[] mapping first.
bool chat_layout_is_last_row_visible(const int *msg_rows, int count, int rows, int scroll,
                                     int msg_index)
{
    if (msg_index < 0 || msg_index >= count || rows <= 0) {
        return false;
    }
    int cum = 0;
    for (int i = 0; i < msg_index; i++) {
        if (msg_rows[i] > 0) {
            cum += msg_rows[i];
        }
    }
    return cum >= scroll && cum <= scroll + rows - 1;
}

// ---------------------------------------------------------------------------
// Composer viewport (v0.3 task 1.1, docs/V03_PLAN.md §1: "the reply buffer
// is fine ... the problem is purely the viewport") — pure index arithmetic,
// no gfx.h/msg.h/ESP-IDF dependency, same split/host-test pattern as the
// chat row layout block above. The composer render (chat_render(), in the
// ESP-only section below) measures each codepoint's own advance with
// gfx_glyph_advance() into an adv[] array and hands it here; this only
// works out which slice of that array is visible.
// ---------------------------------------------------------------------------

// Given per-codepoint advances adv[0..n-1] (px), the span available for
// text (px), and the width of the "…" marker (px), return the index of the
// first codepoint to draw and whether the marker is needed. Draws the
// longest suffix that fits; when everything fits, start = 0 and no marker.
// n == 0 -> start 0, no marker. If the marker itself does not fit in
// avail_px (a pathologically narrow span), returns n with marker true — the
// renderer then draws only the marker, no text.
int chat_composer_viewport(const uint8_t *adv, int n, int avail_px, int marker_px, bool *marker)
{
    if (n <= 0) {
        if (marker) {
            *marker = false;
        }
        return 0;
    }

    int total = 0;
    for (int i = 0; i < n; i++) {
        total += adv[i];
    }
    if (total <= avail_px) {
        // Exact fit (total == avail_px) or plenty of room: the whole text is
        // visible, no marker needed.
        if (marker) {
            *marker = false;
        }
        return 0;
    }

    if (marker) {
        *marker = true;
    }
    int budget = avail_px - marker_px;
    if (budget <= 0) {
        return n; // marker alone does not fit either -- draw only it
    }

    // Walk from the end accumulating advances; the first (further-left)
    // codepoint that would push the running sum past `budget` stops the
    // walk -- `start` is left at the index just past it, i.e. the longest
    // suffix that still fits.
    int sum = 0;
    int start = n;
    for (int i = n - 1; i >= 0; i--) {
        int next_sum = sum + adv[i];
        if (next_sum > budget) {
            break;
        }
        sum = next_sum;
        start = i;
    }
    return start;
}

#ifdef ESP_PLATFORM

#include "ui.h"
#include "msg.h"
#include "book.h"
#include "ime.h"
#include "sms.h"

#include <stdio.h>
#include <string.h>

// Small indent that marks a continuation row as "still part of the message
// above it" (owner request: "continuation rows should be visibly
// continuation") without needing a second font or a bullet glyph.
#define CHAT_CONT_INDENT_PX 8

// Owner's floor so a pathological case (a huge tag string, or a `who` name
// at the 16-byte cap, leaving almost no room for body text) never hands
// gfx_text_wrap() a zero/negative width — gfx.c's own wrap loop already
// refuses to drop text even at 1px (emits one codepoint per line rather
// than nothing), so this is just a sane lower bound, not a correctness
// requirement.
#define CHAT_MIN_WRAP_WIDTH_PX 24

// Upper bound on how many rows a single message can be counted as, chosen
// generously above any layout this project's fonts/screen width can
// realistically produce for a 320-byte/160-codepoint body (docs/PROTOCOL.md
// §3.1): even the narrowest realistic wrap width (CHAT_MIN_WRAP_WIDTH_PX)
// fits several Latin glyphs or a couple of CJK ones per line, so 64 rows is
// a wide safety margin, not the expected case. A message that somehow still
// needs more than this many rows has its LAST CHAT_MAX_ROWS_PER_MSG rows
// permanently unreachable by scrolling (gfx_text_wrap()'s own "refuse
// rather than silently drop" contract still holds for everything up to
// this cap) — an accepted, documented edge case rather than an unbounded
// static buffer.
#define CHAT_MAX_ROWS_PER_MSG 64

// visible_rows() is always <= 6 (see its own body below); this is the cell
// array chat_render()/mark_visible_read()/clamp_scroll() lay out into.
#define CHAT_MAX_VISIBLE_ROWS 6

static int s_scroll = 0; // 0 = pinned to the newest messages (auto-follow), docs/DEVICE_PLAN.md §5.5

static int visible_rows(void)
{
    // §5.2: "at 2x the body shows 4 rows of 24 columns" for the *whole*
    // body; this screen also reserves one row for the composer, so message
    // rows are one fewer than the body's own row budget at each size.
    return (ui_text_size() == GFX_FONT_LARGE) ? 4 : 6;
}

// ---------------------------------------------------------------------------
// Row source + wrapping (owner request 2026-09-20). One entry per thread
// message (msg_thread_at() index order, 0 = newest): the strings
// chat_render()'s row 0 needs (who/ts/tag) plus the full, untruncated body
// (up to MSG_RAM_BODY_MAX-1 bytes — the pre-wrap implementation's `char
// body[80]` truncation is gone: a message that wraps needs its whole body,
// not just what used to fit clipped on one row) and, in the parallel
// s_rows[] array, how many rows gfx_text_wrap() says that body needs.
//
// Both arrays are file-static and re-filled by chat_build_rows() on every
// call (from clamp_scroll(), mark_visible_read() and chat_render() alike)
// rather than cached across calls: at most MSG_THREAD_DEPTH (32)
// gfx_text_wrap() calls, done only in response to a keypress or a screen
// render, never in a sleep/wake power path, and keeping this data
// file-static rather than on the stack matches this project's existing
// rule for anything this size (firmware/GOTCHAS.md: "must be static: it
// overflowed the main task's stack").
// ---------------------------------------------------------------------------

typedef struct {
    char who[MSG_FROM_MAX];
    char ts[6];
    char body[MSG_RAM_BODY_MAX];
    const char *tag; // NULL, or a pointer to one of this file's own string literals
    char id[MSG_ID_MAX];
} chat_row_src_t;

static chat_row_src_t s_src[MSG_THREAD_DEPTH];
static int s_rows[MSG_THREAD_DEPTH];

// One reusable scratch buffer for wrap CONTENT — sized for the worst-case
// row count (CHAT_MAX_ROWS_PER_MSG) but only ever holding ONE message's
// lines at a time, whichever message chat_render() is currently drawing a
// row of (get_wrapped_line() below re-wraps on a cache miss). Row-COUNTING
// (chat_build_rows()) also reuses it as pure scratch — its content is
// discarded there, only the returned count matters.
static char s_wrap_scratch[CHAT_MAX_ROWS_PER_MSG][64];
static int s_wrap_cached_msg = -1; // which s_src[] index s_wrap_scratch currently holds lines for

static void load_row_src(const msg_t *m, const char *newest_unread_id, chat_row_src_t *out)
{
    strncpy(out->id, m->id, sizeof(out->id) - 1);
    out->id[sizeof(out->id) - 1] = '\0';
    strncpy(out->body, m->body, sizeof(out->body) - 1);
    out->body[sizeof(out->body) - 1] = '\0';
    ui_format_hhmm(m->ts, out->ts, sizeof(out->ts));

    uint8_t dir = m->dir;
    if (dir == (uint8_t) MSG_DIR_UP) {
        strncpy(out->who, "you", sizeof(out->who) - 1);
    } else {
        strncpy(out->who, m->from, sizeof(out->who) - 1);
    }
    out->who[sizeof(out->who) - 1] = '\0';

    out->tag = NULL;
    if (dir == (uint8_t) MSG_DIR_UP) {
        if (m->ack_state == MSG_ACK_UP_SENT) {
            out->tag = "sent";
        } else if (m->ack_state == MSG_ACK_UP_FAILED || (m->flags & MSG_F_SEND_FAILED)) {
            out->tag = "FAILED";
        } else {
            out->tag = "...";
        }
    } else if (strncmp(out->id, "x_", 2) == 0) {
        // v0.2 §6/msg.h: "x_" is msg_insert_sms_in()'s own synthetic id
        // namespace, used for nothing else — the one reliable way to tell
        // "this down row arrived by SMS, not through the relay" apart from
        // an ordinary down message, hence the owner's requested "sms" tag.
        out->tag = "sms";
    } else if (newest_unread_id[0] != '\0' && strcmp(out->id, newest_unread_id) == 0) {
        out->tag = "NEW";
    }
}

// The pixel width available for row 0's body text: the full screen minus
// the "who HH:MM " prefix (measured, not estimated, so a long `who` name
// or a wide font size is accounted for exactly) minus the tag (plus a
// small gap), if any — this is what keeps row 0's body from ever running
// under the right-aligned tag (owner requirement).
static int wrap_width_row0(gfx_font_t sz, const chat_row_src_t *src)
{
    char prefix[MSG_FROM_MAX + 8];
    snprintf(prefix, sizeof(prefix), "%s %s ", src->who, src->ts);
    int avail = GFX_SCREEN_W - gfx_text_width(sz, prefix);
    if (src->tag) {
        avail -= gfx_text_width(sz, src->tag) + 4;
    }
    return avail;
}

// gfx_text_wrap() wraps a whole string at ONE width; row 0's budget (above)
// and a continuation row's budget (full width minus the small indent)
// differ, so this file wraps every message at the NARROWER of the two —
// every produced line then safely fits either role. The cost is that
// continuation rows do not use the full available width when row 0's
// budget is the narrower one (the common case: a "who HH:MM "+tag prefix
// easily costs more than CHAT_CONT_INDENT_PX) — a few more, slightly
// short, continuation rows rather than a second wrap pass with a
// per-line-varying width gfx_text_wrap() has no way to express. Documented
// trade-off, not a bug: every byte of the body is still shown, just
// possibly over one more row than the tightest possible packing.
static int wrap_width_conservative(gfx_font_t sz, const chat_row_src_t *src)
{
    int row0 = wrap_width_row0(sz, src);
    int cont = GFX_SCREEN_W - CHAT_CONT_INDENT_PX;
    int w = (row0 < cont) ? row0 : cont;
    return (w < CHAT_MIN_WRAP_WIDTH_PX) ? CHAT_MIN_WRAP_WIDTH_PX : w;
}

static int compute_row_count(gfx_font_t sz, const chat_row_src_t *src)
{
    int width = wrap_width_conservative(sz, src);
    int n = gfx_text_wrap(sz, src->body, width, s_wrap_scratch, CHAT_MAX_ROWS_PER_MSG);
    s_wrap_cached_msg = -1; // scratch content is now this call's, not any s_src[] index's
    if (n < 0) {
        return CHAT_MAX_ROWS_PER_MSG; // needs more rows than the cap — see its own comment
    }
    return (n < 1) ? 1 : n; // gfx_text_wrap() always returns >=1 for a real call; defensive floor
}

// Fills s_src[]/s_rows[] for every message currently in the thread (capped
// at MSG_THREAD_DEPTH, msg.h's own ring depth) and returns how many. Shared
// by clamp_scroll(), mark_visible_read() and chat_render() so all three
// agree on exactly the same row counts every time.
static size_t chat_build_rows(gfx_font_t sz)
{
    size_t count = msg_thread_count();
    if (count > MSG_THREAD_DEPTH) {
        count = MSG_THREAD_DEPTH; // defensive; msg_thread_count() never actually exceeds this
    }

    const msg_t *nu = msg_newest_unread();
    char newest_unread_id[MSG_ID_MAX] = "";
    if (nu) {
        strncpy(newest_unread_id, nu->id, sizeof(newest_unread_id) - 1);
        newest_unread_id[sizeof(newest_unread_id) - 1] = '\0';
    }

    for (size_t i = 0; i < count; i++) {
        const msg_t *m = msg_thread_at(i);
        if (!m) {
            // Should not happen for i < count (msg_thread_at()/
            // msg_thread_count() are both under the same lock, msg.h's own
            // README R6 doc comment) — fail closed to a harmless 1-row
            // blank entry rather than an uninitialised s_src[i]/s_rows[i].
            memset(&s_src[i], 0, sizeof(s_src[i]));
            s_rows[i] = 1;
            continue;
        }
        load_row_src(m, newest_unread_id, &s_src[i]);
        s_rows[i] = compute_row_count(sz, &s_src[i]);
    }
    return count;
}

// Returns the wrapped text of `s_src[msg_index]`'s row `row_in_msg`,
// re-wrapping into s_wrap_scratch only on a cache miss (chat_render() asks
// for a message's rows in increasing row_in_msg order, one screen row at a
// time, so consecutive calls for the SAME message — the common case, a
// wrapped message spanning several screen rows — hit the cache).
static const char *get_wrapped_line(gfx_font_t sz, int msg_index, int row_in_msg)
{
    if (msg_index < 0 || msg_index >= MSG_THREAD_DEPTH || row_in_msg < 0 ||
        row_in_msg >= CHAT_MAX_ROWS_PER_MSG) {
        return "";
    }
    if (s_wrap_cached_msg != msg_index) {
        int width = wrap_width_conservative(sz, &s_src[msg_index]);
        gfx_text_wrap(sz, s_src[msg_index].body, width, s_wrap_scratch, CHAT_MAX_ROWS_PER_MSG);
        s_wrap_cached_msg = msg_index;
    }
    return s_wrap_scratch[row_in_msg];
}

// Owner request: "a message counts as seen only if at least its last row is
// on screen" — now checked precisely via chat_layout_is_last_row_visible()
// instead of the pre-wrap implementation's blind sweep of the ENTIRE
// thread (up to all 32 messages, on- or off-screen) on every keypress.
// README R6 (open, not in F6.3's Files list): copies fields out promptly
// per index rather than holding a raw msg_t* across any work.
static void mark_visible_read(void)
{
    gfx_font_t sz = ui_text_size();
    size_t count = chat_build_rows(sz);
    int rows = visible_rows();
    int max_scroll = chat_layout_max_scroll(s_rows, (int) count, rows);
    int scroll = s_scroll;
    if (scroll > max_scroll) {
        scroll = max_scroll;
    }
    if (scroll < 0) {
        scroll = 0;
    }

    for (size_t i = 0; i < count; i++) {
        const msg_t *m = msg_thread_at(i);
        if (!m) {
            continue;
        }
        if (m->dir == (uint8_t) MSG_DIR_DOWN && m->ack_state != MSG_ACK_READ &&
            chat_layout_is_last_row_visible(s_rows, (int) count, rows, scroll, (int) i)) {
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
    gfx_font_t sz = ui_text_size();
    size_t count = chat_build_rows(sz);
    int rows = visible_rows();
    int max_scroll = chat_layout_max_scroll(s_rows, (int) count, rows);
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

    // Owner request 2026-09-20 ("wrap long messages in the chat screen"):
    // build every visible-candidate message's row count, then hand scroll
    // position + row counts to the pure chat_layout() (this file's own
    // module comment above has the full model) to learn exactly which
    // message/row belongs on each of the `rows` screen rows. clamp_scroll()
    // itself calls chat_build_rows() again — a second, identical
    // recomputation — rather than trusting a value already computed
    // elsewhere this frame; simpler to read, and cheap (msg_thread_count()
    // is at most MSG_THREAD_DEPTH == 32) next to a full e-paper refresh.
    size_t count = chat_build_rows(sz);
    clamp_scroll();

    chat_layout_cell_t cells[CHAT_MAX_VISIBLE_ROWS];
    chat_layout(s_rows, (int) count, rows, s_scroll, cells);
    s_wrap_cached_msg = -1; // fresh wrap cache for this render's draw pass

    int y = UI_BODY_TOP + 2;
    // Screen row r (0 = top) draws cells[r] — chat_layout() already placed
    // the newest message's rows at the bottom (docs/DEVICE_PLAN.md §5.5's
    // "newest at the bottom" mockup) and, for a message spanning several
    // rows, its header (row_in_msg == 0) above its own continuation rows,
    // in reading order.
    for (int r = 0; r < rows; r++) {
        chat_layout_cell_t cell = cells[r];
        if (cell.msg_index >= 0) {
            const chat_row_src_t *src = &s_src[cell.msg_index];
            if (cell.row_in_msg == 0) {
                int x = gfx_text(0, y, sz, src->who);
                x = gfx_text(x, y, sz, " ");
                x = gfx_text(x, y, sz, src->ts);
                x = gfx_text(x, y, sz, " ");
                gfx_text(x, y, sz, get_wrapped_line(sz, cell.msg_index, 0));

                // Tag is ALWAYS on row 0, regardless of how many rows this
                // message wraps to (owner requirement) — wrap_width_row0()
                // already reserved room for it, so this never overlaps the
                // body text drawn just above.
                if (src->tag) {
                    int tw = gfx_text_width(sz, src->tag);
                    gfx_text(GFX_SCREEN_W - tw, y, sz, src->tag);
                }
            } else {
                // Continuation row: small indent, no who/ts/tag — visibly
                // "still part of the message above it" (owner requirement).
                gfx_text(CHAT_CONT_INDENT_PX, y, sz,
                         get_wrapped_line(sz, cell.msg_index, cell.row_in_msg));
            }
        }
        // cell.msg_index < 0: no message reaches this far up the screen
        // (a short thread doesn't fill the viewport) — leave the row
        // blank, same as the pre-wrap implementation's `idx >= count` case.
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

#endif /* ESP_PLATFORM */

// scr_pick.c — "New message -> pick recipient" screen (docs/DEVICE_TASKS.md
// F7.2, docs/DEVICE_PLAN.md §5.5 "New message → pick recipient").
//
// T3 (docs/CHAT_UI_DESIGN.md §3 "Pick"): reachable from Home's "New
// message" row (scr_home.c). g_scr_pick is declared in ui.h (not a new
// scr_pick.h) and registered in firmware/main/CMakeLists.txt's SRCS, same
// precedent F6.5 set for g_scr_lock.
//
// Scope note: `enter` on a contact opens the per-peer chat via
// scr_chat_open_with_prefix() (ui.h), which seeds the composer's own
// `@alias`/`@name` word — the SAME resolution path (scr_chat.c's
// resolve_at_word()) an approved contact's alias or an SMS contact's name
// already goes through when typed by hand, just typed here on the
// student's behalf. The Chat screen itself still renders the one merged
// thread (per-peer Chat is T4) — this is real, targeted navigation for
// *sending*, not yet a filtered *view* of just that peer's messages.
//
// v0.2 §6 (docs/V02_DESIGN.md, docs/DEVICE_PLAN.md §4.4): SMS contacts
// (sms.c's own parent-managed allow-list, entirely separate from the
// address book above) are listed as additional selectable rows after the
// book's own approved contacts, each tagged "sms" in the same trailing
// column book contacts already use for their `type` (web/sms/chat) —
// visually identical presentation, different underlying list; see sms.h's
// own module comment for why the two "sms" concepts (a book contact
// reachable via the relay's SMS gateway vs. a device-direct SMS contact)
// are unrelated data even though this column happens to render the same
// three letters for both.

#include "ui.h"
#include "book.h"
#include "sms.h"

#include <stdio.h>
#include <string.h>

#define PICK_VISIBLE_ROWS 8 /* same 12px-pitch budget as scr_device.c's menu */

static int s_sel = 0; /* index into (book contacts ++ sms contacts), 0-based */

static size_t selectable_count(void) { return book_contact_count() + sms_contact_count(); }

static void pick_on_event(ui_evt_t evt)
{
    if (evt != UI_EVT_ENTER) {
        return;
    }
    size_t n = selectable_count();
    if (s_sel < 0) {
        s_sel = 0;
    }
    if (n == 0) {
        s_sel = 0;
    } else if ((size_t) s_sel >= n) {
        s_sel = (int) n - 1;
    }
}

static void pick_on_key(input_key_t key)
{
    size_t n = selectable_count();
    switch (key.type) {
    case INPUT_KEY_UP:
        if (s_sel > 0) {
            s_sel--;
        }
        break;
    case INPUT_KEY_DOWN:
        if (n > 0 && (size_t) s_sel < n - 1) {
            s_sel++;
        }
        break;
    case INPUT_KEY_ENTER: {
        // pending/rejected rows (book_request_at()) are not part of s_sel's
        // range at all (§5.5: "greyed and are not selectable") — nothing
        // else to check here.
        if (n == 0) {
            break; // nothing to choose — esc is the only way out
        }
        // T3 (docs/CHAT_UI_DESIGN.md §3 "Pick"): opens the per-peer chat
        // with the picked contact's `@word` prefilled (scr_chat_open_with_prefix(),
        // ui.h) — replaces the old "just open the merged chat" stopgap this
        // screen's own module comment above documented (now stale for this
        // path; the module comment itself is left as historical context for
        // the `@name` composer resolution it describes, which is unchanged).
        // A book contact's alias and an SMS contact's name are exactly the
        // two things resolve_at_word() (scr_chat.c) already knows how to
        // resolve, same as if the student had typed the `@word` by hand.
        size_t n_book = book_contact_count();
        if ((size_t) s_sel < n_book) {
            book_contact_t c;
            if (book_contact_at((size_t) s_sel, &c)) {
                scr_chat_open_with_prefix(c.alias);
            }
        } else {
            sms_contact_t sc;
            if (sms_contact_at((size_t) s_sel - n_book, &sc)) {
                scr_chat_open_with_prefix(sc.name);
            }
        }
        break;
    }
    case INPUT_KEY_ESC:
        ui_pop();
        break;
    default:
        break;
    }
}

static void pick_render(void)
{
    size_t n_book = book_contact_count();
    size_t n_sms = sms_contact_count();
    size_t n_contacts = n_book + n_sms; /* selectable rows: book contacts, then sms contacts */
    size_t n_requests = book_request_count();

    int y = UI_BODY_TOP + 2;

    if (n_contacts == 0 && book_get_bv() == 0) {
        // §5.5: "If the book is empty (no `book` received yet) the screen
        // says so and offers *Re-sync* on the Device screen." book_get_bv()
        // == 0 is book.h's own documented "never applied a book yet" signal
        // (book_init()'s doc comment), used here rather than n_contacts == 0
        // alone, since a book with bv > 0 but zero approved contacts (e.g.
        // every request still pending) is a different, valid state that
        // should fall through to the ordinary (empty-contacts, greyed
        // pending rows) rendering below instead of this one-time message.
        gfx_text(0, y, GFX_FONT_NORMAL, "No address book yet.");
        y += 12;
        gfx_text(0, y, GFX_FONT_NORMAL, "Re-sync from the Device screen.");
        gfx_text(0, UI_FOOTER_Y, GFX_FONT_NORMAL, "esc back");
        return;
    }

    // Scroll-into-view over the combined contacts+requests list (up to
    // BOOK_MAX_CONTACTS + BOOK_MAX_REQUESTS = 14 rows, more than the 8-row
    // viewport) — same fresh-each-render scroll_top computation
    // scr_device.c's device_render_normal() uses, adapted to this screen's
    // two-section (contacts then requests) layout. s_sel indexes contacts
    // only (0..n_contacts-1, requests are never selectable), which also
    // happens to equal the row index directly since contacts are always
    // the first section.
    int total_n = (int) (n_contacts + n_requests);
    int sel_abs = s_sel;
    int scroll_top = 0;
    if (sel_abs >= PICK_VISIBLE_ROWS) {
        scroll_top = sel_abs - PICK_VISIBLE_ROWS + 1;
    }
    int max_top = (total_n > PICK_VISIBLE_ROWS) ? (total_n - PICK_VISIBLE_ROWS) : 0;
    if (scroll_top > max_top) {
        scroll_top = max_top;
    }

    char default_alias[BOOK_ALIAS_MAX] = "";
    bool have_default = book_get_default_alias(default_alias, sizeof(default_alias));

    for (int r = scroll_top; r < total_n && r < scroll_top + PICK_VISIBLE_ROWS; r++) {
        if (r < (int) n_book) {
            book_contact_t c;
            if (!book_contact_at((size_t) r, &c)) {
                continue;
            }
            bool is_default = have_default && strcmp(default_alias, c.alias) == 0;
            if (r == sel_abs) {
                gfx_text(0, y, GFX_FONT_NORMAL, ">");
            }
            const char *label = (c.nickname[0] != '\0') ? c.nickname : c.name;
            int x = gfx_text(10, y, GFX_FONT_NORMAL, label);
            if (is_default) {
                x = gfx_text(x + 4, y, GFX_FONT_NORMAL, "(default)");
            }
            (void) x;
            int tw = gfx_text_width(GFX_FONT_NORMAL, c.type);
            gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, c.type);
        } else if (r < (int) n_contacts) {
            // v0.2 §6: an SMS contact (sms.c's own allow-list) — same row
            // shape as a book contact above, tagged "sms" in the trailing
            // column (see this file's own module comment on the two
            // unrelated "sms" concepts sharing that column's rendering).
            sms_contact_t sc;
            if (!sms_contact_at((size_t) (r - (int) n_book), &sc)) {
                continue;
            }
            if (r == sel_abs) {
                gfx_text(0, y, GFX_FONT_NORMAL, ">");
            }
            gfx_text(10, y, GFX_FONT_NORMAL, sc.name);
            int tw = gfx_text_width(GFX_FONT_NORMAL, "sms");
            gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, "sms");
        } else {
            // Pending/rejected requests: greyed (drawn plain, no `>` marker
            // — this gfx.c has no separate "dim" ink, so "greyed and
            // unselectable" is expressed the same way scr_device.c
            // expresses "not selectable": no cursor ever lands here,
            // s_sel's range never includes these rows).
            book_request_t req;
            if (!book_request_at((size_t) (r - (int) n_contacts), &req)) {
                continue;
            }
            gfx_text(10, y, GFX_FONT_NORMAL, req.name);
            const char *status =
                (strcmp(req.status, "pend") == 0) ? "pending approval" : "not approved";
            int tw = gfx_text_width(GFX_FONT_NORMAL, status);
            gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, status);
        }
        y += 12;
    }

    gfx_text(0, UI_FOOTER_Y, GFX_FONT_NORMAL, "enter choose   esc back");
}

const ui_screen_t g_scr_pick = {
    .name = "pick",
    .render = pick_render,
    .on_key = pick_on_key,
    .on_event = pick_on_event,
};

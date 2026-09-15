// scr_book.c — Address Book screen: contacts + pending/rejected requests,
// the Add-contact form, and the per-contact Nickname form (docs/DEVICE_TASKS.md
// F7.2, docs/DEVICE_PLAN.md §5.5 "Address book", "Nicknames").
//
// Reachability note: same as scr_pick.c's own module comment — Home's
// "Address book" row (scr_home.c's HROW_BOOK) is not wired to
// ui_push(&g_scr_book) by this task (scr_home.c is not in F7.2's Files
// list); g_scr_book is declared in ui.h and registered in
// firmware/main/CMakeLists.txt's SRCS, both required for this file to
// build/link at all. Flagged in this task's own report rather than guessed.
//
// Three internal modes, one screen (mirrors scr_device.c's own
// confirm/passcode-modal pattern for a single ui_screen_t with more than one
// rendered state):
//   BOOK_MODE_LIST — the contacts + greyed requests + "Add" row.
//   BOOK_MODE_ADD  — the two-field Add-contact form (§4.2/§5.5).
//   BOOK_MODE_NICK — the one-field Nickname form (§5.5's "Nicknames"
//                    paragraph), reached by `enter` on an approved contact
//                    row in list mode (NOT scr_pick.c's `enter`, which opens
//                    the chat instead — the picker and the address-book
//                    manager deliberately give the same row a different
//                    action, matching each screen's own stated purpose).
//
// UTF-8 field buffering (Name in Add mode, Nickname in Nick mode): both
// reuse the exact byte-at-a-time, atomic-code-point-commit discipline
// msg.c's composer documents and implements (msg.h's own doc comment on
// msg_composer_push_char()) — same shape, parameterized by cap_bytes/cap_cp
// here since this file needs two different caps (48B/16cp for Name,
// 36B/12cp for Nickname, book.h's BOOK_NAME_MAX/BOOK_NICK_MAX) rather than
// msg.c's one fixed 320B/160cp composer. Not shared code with msg.c (msg.c
// exposes only its own single fixed-cap composer, not a generic reusable
// helper) — duplicated deliberately rather than widening msg.c's API for a
// second caller outside this task's Files list.
//
// Phone/alias field (Add mode's second field): ASCII-only, IME bypassed,
// same reasoning scr_device.c's factory-reset confirm field and passcode
// modal already give for the setup-code-shaped fields (docs/DEVICE_PLAN.md
// §5.3: "the setup-code and phone fields are ASCII-only by definition and
// bypass it [the IME]"). Character filter reconciles two textually adjacent
// but literally distinct instructions: docs/DEVICE_TASKS.md F7.2's Do bullet
// says the field "accepts only `+` and digits", while its own mockup
// (§5.5) shows a second line directly under it, "(or leave blank and type
// an @alias)" — which is unreachable if the filter is *only* `+`/digit.
// Read together with book.h's own book_request()'s doc comment ("a leading
// `+` means phone, anything else means alias") and §4.2's prose ("either a
// phone number ... or an alias the student already knows"), the field is
// clearly meant to carry both, mediated by its own leading character: `+`
// or a digit locks the rest of the field to E.164 digits (the Do bullet's
// literal rule, for the common case), while a leading `@` locks the rest to
// the alias charset (PROTOCOL.md §1's alias regex, `^[a-z0-9][a-z0-9_-]*$`)
// so the mockup's own hint line is actually reachable. The leading `@` is
// stripped before the value is handed to book_request() (an alias per
// book.h's own contract has no `@` in it) rather than guessed at some other
// resolution point.

#include "ui.h"
#include "book.h"

#include <stdio.h>
#include <string.h>

typedef enum {
    BOOK_MODE_LIST = 0,
    BOOK_MODE_ADD,
    BOOK_MODE_NICK,
} book_mode_t;

static book_mode_t s_mode = BOOK_MODE_LIST;
static int s_sel = 0; /* BOOK_MODE_LIST: index over contacts (0..count-1) then one more for "Add" */

/* ---------------------------------------------------------------------
 * Generic UTF-8 capped field — see this file's own module comment for why
 * this duplicates, rather than reuses, msg.c's composer shape.
 * --------------------------------------------------------------------- */
typedef struct {
    char buf[BOOK_NAME_MAX]; /* 49 bytes: big enough for both Name (48) and Nickname (36) */
    uint16_t len;            /* bytes committed */
    uint16_t cp_count;       /* code points committed */
    uint8_t pending[4];
    uint8_t pending_len;
    uint8_t pending_want;
} utf8_field_t;

static bool is_utf8_cont(uint8_t b) { return (b & 0xC0) == 0x80; }

static uint8_t utf8_seq_len(uint8_t lead)
{
    if ((lead & 0x80) == 0x00) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1; // invalid lead byte: never wedge the field, treat as its own code point
}

static void field_reset(utf8_field_t *f) { memset(f, 0, sizeof(*f)); }

static void field_set(utf8_field_t *f, const char *s)
{
    field_reset(f);
    size_t n = strlen(s);
    if (n >= sizeof(f->buf)) {
        n = sizeof(f->buf) - 1; // defensive only: every caller's source is already within-cap
    }
    memcpy(f->buf, s, n);
    f->buf[n] = '\0';
    f->len = (uint16_t) n;
    for (size_t i = 0; i < n; i++) {
        if (!is_utf8_cont((uint8_t) s[i])) {
            f->cp_count++;
        }
    }
}

/* Refuses (returns false, buffer unchanged) rather than truncates, same
 * "refuse further input" discipline docs/DEVICE_PLAN.md §5.2/§9.4 states for
 * the composer — book_name_valid()/book_set_nickname() apply the identical
 * bound at the far end, so this is belt-and-suspenders, not the only guard. */
static bool field_push_byte(utf8_field_t *f, char c, uint16_t cap_bytes, uint16_t cap_cp)
{
    uint8_t b = (uint8_t) c;

    if (f->pending_want > 0 && !is_utf8_cont(b)) {
        f->pending_want = 0;
        f->pending_len = 0;
    }
    if (f->pending_want == 0) {
        f->pending_want = utf8_seq_len(b);
        f->pending_len = 0;
    }
    f->pending[f->pending_len++] = b;

    if (f->pending_len < f->pending_want) {
        return true; // sequence still incomplete; nothing to check yet
    }

    uint8_t want = f->pending_want;
    f->pending_want = 0;

    if (f->cp_count >= cap_cp || (uint16_t) (f->len + want) > cap_bytes) {
        return false; // refuse the whole code point atomically
    }

    memcpy(&f->buf[f->len], f->pending, want);
    f->len = (uint16_t) (f->len + want);
    f->buf[f->len] = '\0';
    f->cp_count++;
    return true;
}

static bool field_backspace(utf8_field_t *f)
{
    if (f->pending_want > 0) {
        f->pending_want = 0;
        f->pending_len = 0;
        return true;
    }
    if (f->len == 0) {
        return false;
    }
    size_t cut = f->len;
    do {
        cut--;
    } while (cut > 0 && is_utf8_cont((uint8_t) f->buf[cut]));
    f->len = (uint16_t) cut;
    f->buf[f->len] = '\0';
    if (f->cp_count > 0) {
        f->cp_count--;
    }
    return true;
}

/* ---------------------------------------------------------------------
 * BOOK_MODE_ADD state.
 * --------------------------------------------------------------------- */
typedef enum { ADD_FIELD_NAME = 0, ADD_FIELD_PHONE } add_field_t;

static add_field_t s_add_field = ADD_FIELD_NAME;
static utf8_field_t s_add_name;
static char s_add_phone[BOOK_REQ_PH_MAX];
static size_t s_add_phone_len = 0;

static bool phone_char_ok(char c)
{
    if (s_add_phone_len == 0) {
        return c == '+' || c == '@' || (c >= '0' && c <= '9');
    }
    if (s_add_phone[0] == '@') {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }
    return c >= '0' && c <= '9'; // leading '+' or digit already: E.164 digits only from here
}

static void add_reset(void)
{
    s_add_field = ADD_FIELD_NAME;
    field_reset(&s_add_name);
    s_add_phone[0] = '\0';
    s_add_phone_len = 0;
}

static void add_submit(void)
{
    if (s_add_name.len == 0) {
        ui_show_toast("name required");
        return;
    }
    const char *ph_or_alias = NULL;
    char stripped[BOOK_REQ_PH_MAX];
    if (s_add_phone_len > 0) {
        if (s_add_phone[0] == '@') {
            // book.h's book_request(): "anything else [not a leading '+']
            // means alias" — the alias value itself carries no '@'.
            strncpy(stripped, s_add_phone + 1, sizeof(stripped) - 1);
        } else {
            strncpy(stripped, s_add_phone, sizeof(stripped) - 1);
        }
        stripped[sizeof(stripped) - 1] = '\0';
        ph_or_alias = stripped;
    }
    if (book_request(s_add_name.buf, ph_or_alias)) {
        ui_show_toast("sent for approval");
        s_mode = BOOK_MODE_LIST;
    } else {
        ui_show_toast("could not send request");
    }
}

static void add_on_key(input_key_t key)
{
    switch (key.type) {
    case INPUT_KEY_TAB:
        s_add_field = (s_add_field == ADD_FIELD_NAME) ? ADD_FIELD_PHONE : ADD_FIELD_NAME;
        break;
    case INPUT_KEY_CHAR:
        if (s_add_field == ADD_FIELD_NAME) {
            if (!field_push_byte(&s_add_name, key.ch, BOOK_NAME_MAX - 1, 16)) {
                ui_show_toast("name: 16 code points max");
            }
        } else {
            if (phone_char_ok(key.ch)) {
                if (s_add_phone_len + 1 < sizeof(s_add_phone)) {
                    s_add_phone[s_add_phone_len++] = key.ch;
                    s_add_phone[s_add_phone_len] = '\0';
                } else {
                    ui_show_toast("16 characters max");
                }
            }
        }
        break;
    case INPUT_KEY_BACKSPACE:
        if (s_add_field == ADD_FIELD_NAME) {
            field_backspace(&s_add_name);
        } else if (s_add_phone_len > 0) {
            s_add_phone[--s_add_phone_len] = '\0';
        }
        break;
    case INPUT_KEY_ENTER:
        add_submit();
        break;
    case INPUT_KEY_ESC:
        s_mode = BOOK_MODE_LIST;
        break;
    default:
        break;
    }
}

static void add_render(void)
{
    int y = UI_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, "Add contact");
    y += 14;

    char line[BOOK_NAME_MAX + 8];
    snprintf(line, sizeof(line), "Name   %s%s", s_add_name.buf,
             (s_add_field == ADD_FIELD_NAME) ? "_" : "");
    gfx_text(0, y, GFX_FONT_NORMAL, line);
    y += 12;

    char pline[BOOK_REQ_PH_MAX + 8];
    snprintf(pline, sizeof(pline), "Phone  %s%s", s_add_phone,
              (s_add_field == ADD_FIELD_PHONE) ? "_" : "");
    gfx_text(0, y, GFX_FONT_NORMAL, pline);
    y += 12;
    gfx_text(8, y, GFX_FONT_NORMAL, "(or leave blank and type an @alias)");

    gfx_text(0, GFX_SCREEN_H - 9, GFX_FONT_NORMAL,
             "tab next field   enter send for approval   esc cancel");
}

/* ---------------------------------------------------------------------
 * BOOK_MODE_NICK state.
 * --------------------------------------------------------------------- */
static char s_nick_alias[BOOK_ALIAS_MAX];
static utf8_field_t s_nick_field;

static void nick_enter(const book_contact_t *c)
{
    strncpy(s_nick_alias, c->alias, sizeof(s_nick_alias) - 1);
    s_nick_alias[sizeof(s_nick_alias) - 1] = '\0';
    field_set(&s_nick_field, c->nickname); // prefilled — editing an existing nickname is common
    s_mode = BOOK_MODE_NICK;
}

static void nick_on_key(input_key_t key)
{
    switch (key.type) {
    case INPUT_KEY_CHAR:
        if (!field_push_byte(&s_nick_field, key.ch, BOOK_NICK_MAX - 1, 12)) {
            ui_show_toast("12 code points max");
        }
        break;
    case INPUT_KEY_BACKSPACE:
        field_backspace(&s_nick_field);
        break;
    case INPUT_KEY_ENTER:
        // §5.5: "empty clears" — book_set_nickname("") is exactly that.
        if (book_set_nickname(s_nick_alias, s_nick_field.buf, s_nick_field.len)) {
            ui_show_toast(s_nick_field.len == 0 ? "nickname cleared" : "nickname set");
            s_mode = BOOK_MODE_LIST;
        } else {
            ui_show_toast("contact no longer approved");
            s_mode = BOOK_MODE_LIST;
        }
        break;
    case INPUT_KEY_ESC:
        s_mode = BOOK_MODE_LIST;
        break;
    default:
        break;
    }
}

static void nick_render(void)
{
    int y = UI_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, "Nickname");
    y += 14;
    char sub[40];
    snprintf(sub, sizeof(sub), "for %s", s_nick_alias);
    gfx_text(0, y, GFX_FONT_NORMAL, sub);
    y += 12;

    // Sized off sizeof(s_nick_field.buf) (BOOK_NAME_MAX, the shared utf8_field_t
    // buffer's compile-time capacity), not BOOK_NICK_MAX: the nickname
    // field's *runtime* cap is enforced by field_push_byte()'s cap_bytes=36
    // argument at set/push time, but GCC's -Wformat-truncation reasons only
    // about buf's declared array size, and warns (this build is
    // -Werror=format-truncation) if `line` is sized for the smaller runtime
    // bound instead.
    char line[sizeof(s_nick_field.buf) + 4];
    snprintf(line, sizeof(line), "> %s_", s_nick_field.buf);
    gfx_text(0, y, GFX_FONT_NORMAL, line);
    y += 12;
    char counter[16];
    snprintf(counter, sizeof(counter), "%u/12", (unsigned) s_nick_field.cp_count);
    gfx_text(0, y, GFX_FONT_NORMAL, counter);

    gfx_text(0, GFX_SCREEN_H - 9, GFX_FONT_NORMAL, "enter save   esc cancel");
}

/* ---------------------------------------------------------------------
 * BOOK_MODE_LIST state.
 * --------------------------------------------------------------------- */
#define BOOK_VISIBLE_ROWS 8

static int selectable_count(void)
{
    return (int) book_contact_count() + 1; // +1 for the trailing "Add" row
}

static void list_on_event(void)
{
    int n = selectable_count();
    if (s_sel < 0) {
        s_sel = 0;
    }
    if (s_sel > n - 1) {
        s_sel = n - 1;
    }
}

static void list_on_key(input_key_t key)
{
    int n = selectable_count();
    switch (key.type) {
    case INPUT_KEY_UP:
        if (s_sel > 0) {
            s_sel--;
        }
        break;
    case INPUT_KEY_DOWN:
        if (s_sel < n - 1) {
            s_sel++;
        }
        break;
    case INPUT_KEY_ENTER: {
        size_t n_contacts = book_contact_count();
        if ((size_t) s_sel < n_contacts) {
            book_contact_t c;
            if (book_contact_at((size_t) s_sel, &c)) {
                nick_enter(&c);
            }
        } else {
            add_reset();
            s_mode = BOOK_MODE_ADD;
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

static void list_render(void)
{
    size_t n_contacts = book_contact_count();
    size_t n_requests = book_request_count();
    int y = UI_BODY_TOP + 2;

    // Scroll-into-view over contacts + requests + the trailing "Add" row (up
    // to 10+4+1 = 15 rows, more than the 8-row viewport) — same fresh-each-
    // render scroll_top computation scr_device.c's device_render_normal()
    // and scr_pick.c's pick_render() use. s_sel (from selectable_count():
    // contacts, 0..n_contacts-1, plus one more for "Add") maps to the
    // combined row index sel_abs by skipping over the (never selectable)
    // request rows for the "Add" case.
    int total_n = (int) (n_contacts + n_requests) + 1;
    int sel_abs = (s_sel < (int) n_contacts) ? s_sel : (int) (n_contacts + n_requests);
    int scroll_top = 0;
    if (sel_abs >= BOOK_VISIBLE_ROWS) {
        scroll_top = sel_abs - BOOK_VISIBLE_ROWS + 1;
    }
    int max_top = (total_n > BOOK_VISIBLE_ROWS) ? (total_n - BOOK_VISIBLE_ROWS) : 0;
    if (scroll_top > max_top) {
        scroll_top = max_top;
    }

    for (int r = scroll_top; r < total_n && r < scroll_top + BOOK_VISIBLE_ROWS; r++) {
        if (r < (int) n_contacts) {
            book_contact_t c;
            if (!book_contact_at((size_t) r, &c)) {
                continue;
            }
            if (r == sel_abs) {
                gfx_text(0, y, GFX_FONT_NORMAL, ">");
            }
            const char *label = (c.nickname[0] != '\0') ? c.nickname : c.name;
            int x = gfx_text(10, y, GFX_FONT_NORMAL, label);
            if (c.nickname[0] != '\0') {
                // §5.5: "the alias is shown small beside it in the book so
                // the student can still see who 'gma' is" — this gfx.c has
                // one fixed 12/16px size, not a smaller third size, so
                // "small" is expressed as parentheses rather than a
                // distinct font size.
                char alias_hint[BOOK_ALIAS_MAX + 2];
                snprintf(alias_hint, sizeof(alias_hint), "(%s)", c.alias);
                x = gfx_text(x + 4, y, GFX_FONT_NORMAL, alias_hint);
            }
            int tw = gfx_text_width(GFX_FONT_NORMAL, c.type);
            gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, c.type);
            (void) x;
        } else if (r < (int) (n_contacts + n_requests)) {
            book_request_t req;
            if (!book_request_at((size_t) (r - (int) n_contacts), &req)) {
                continue;
            }
            gfx_text(10, y, GFX_FONT_NORMAL, req.name);
            const char *status =
                (strcmp(req.status, "pend") == 0) ? "pending approval" : "not approved";
            int tw = gfx_text_width(GFX_FONT_NORMAL, status);
            gfx_text(GFX_SCREEN_W - tw, y, GFX_FONT_NORMAL, status);
        } else {
            if (r == sel_abs) {
                gfx_text(0, y, GFX_FONT_NORMAL, ">");
            }
            gfx_text(10, y, GFX_FONT_NORMAL, "Add");
        }
        y += 12;
    }

    gfx_text(0, GFX_SCREEN_H - 9, GFX_FONT_NORMAL, "up/down move   enter select   esc back");
}

/* ---------------------------------------------------------------------
 * ui_screen_t glue.
 * --------------------------------------------------------------------- */
static void book_on_event(ui_evt_t evt)
{
    if (evt != UI_EVT_ENTER) {
        return;
    }
    s_mode = BOOK_MODE_LIST; // always start a fresh visit browsing the list
    list_on_event();
}

static void book_on_key(input_key_t key)
{
    switch (s_mode) {
    case BOOK_MODE_ADD:
        add_on_key(key);
        break;
    case BOOK_MODE_NICK:
        nick_on_key(key);
        break;
    case BOOK_MODE_LIST:
    default:
        list_on_key(key);
        break;
    }
}

static void book_render(void)
{
    switch (s_mode) {
    case BOOK_MODE_ADD:
        add_render();
        break;
    case BOOK_MODE_NICK:
        nick_render();
        break;
    case BOOK_MODE_LIST:
    default:
        list_render();
        break;
    }
}

const ui_screen_t g_scr_book = {
    .name = "book",
    .render = book_render,
    .on_key = book_on_key,
    .on_event = book_on_event,
};

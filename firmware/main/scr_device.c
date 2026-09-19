// scr_device.c — Device screen: read-mostly status + settings menu
// (docs/DEVICE_TASKS.md F6.3, docs/DEVICE_PLAN.md §5.5 "Device").
//
// Scope note — what's real vs. stubbed, per the orchestrator's instruction
// to stub/defer lock.c/book.c-dependent bullets and document it here:
//   - "Re-sync address book" [REAL]: §5.5 says this bullet IS just
//     "publishes a /status now (it carries bv), which is the sync trigger"
//     — that doesn't need book.c at all, so it is wired for real via
//     modes_publish_status_now(). `bv` itself is hardcoded to 0 in
//     modes.c's build_status_cbor() until F7.1 tracks a real book version
//     (already true before this task; unchanged here).
//   - "Text size: normal/large" [REAL]: ui_text_size()/ui_toggle_text_size()
//     (ui.c), NVS-backed.
//   - "Passcode / Auto-lock / Show senders when locked" [REAL, F6.5]:
//     lock.c now exists. "Passcode" cycles set -> change -> off per
//     docs/DEVICE_PLAN.md §5.8 ("asks for the current passcode first" for
//     both change and off) as: not-yet-set -> straight to a "new passcode"
//     prompt; already-set -> a "current passcode" prompt first, and only on
//     a correct current passcode does a second "new passcode" prompt open,
//     where submitting it non-empty changes the passcode and submitting it
//     EMPTY turns the passcode off — this is the one interaction detail
//     the mockups don't fully spell out (no dedicated "change passcode"
//     screen is drawn anywhere in §5.5), so this is a deliberate, minimal
//     reading of "cycles set -> change -> off" that keeps every one of the
//     three actions reachable and keeps "asks for the current passcode
//     first" true for both change and off, without inventing a new mockup.
//     "Auto-lock" cycles 0,1,2,5,10,30,60 minutes independently (drawn as
//     its own selectable line rather than sharing "Passcode"'s mockup row,
//     for the same reason: two independently-actionable settings need two
//     selectable rows in this line-based menu, however the schematic
//     mockup happens to lay them out visually). "Show senders when locked"
//     toggles lock_preview().
//   - "Set up again" [STUBBED — see this file's own note below `MROW_SETUP_AGAIN`
//     for why this is a documented blocker, not a simple omission]: setup.c's
//     own module comment states its no-live-session-overlap assumption holds
//     only because main.c calls setup_run() *before* modes_boot() ever runs
//     (before net_init()/net_session_up()). Calling setup_run() from a
//     screen reachable only *after* modes_boot() (this one) would run it
//     against an already-live modem/MQTT session that setup.c's own globals
//     (s_down_topic, its MQTT event handler registration, WalterModem::begin()
//     re-entry) are not documented to tolerate — net.h's own module comment
//     repeats the same "these functions never run in the same power cycle as
//     net_init()" assumption. Making this safe needs setup.c/net.cpp changes
//     (a session teardown before the bootstrap hop, or a "pending setup code"
//     flag main.c checks before calling modes_boot() at all), neither of
//     which is in this task's Files list. Flagged rather than guessed, per
//     this task's own instructions on modem-state ambiguity.
//   - "Factory reset" [REAL]: ident_erase() + esp_restart() has none of the
//     above hazard — it reboots unconditionally, and main.c's ident_load()
//     failing on the next boot is exactly the existing, working "no ident"
//     path (Setup console). `book`'s own NVS namespace (F7.1, not built)
//     has nothing to erase yet; a one-line TODO marks where that goes.

#include "ui.h"
#include "modes.h"
#include "ident.h"
#include "lock.h" /* F6.5: passcode/auto-lock/senders rows, docs/DEVICE_PLAN.md §5.8 */

#include <stdio.h>
#include <string.h>

#include "esp_system.h" /* esp_restart() */

typedef enum {
    MROW_RESYNC = 0,
    MROW_TEXTSIZE,
    MROW_PASSCODE,
    MROW_AUTOLOCK,
    MROW_SENDERS,
    MROW_SETUP_AGAIN,
    MROW_FACTORY_RESET,
    MROW_COUNT,
} device_row_t;

/* §5.5's cycle: 0 (never), 1, 2, 5, 10, 30, 60 minutes. */
static const uint8_t k_autolock_steps[] = {0, 1, 2, 5, 10, 30, 60};
#define AUTOLOCK_STEPS_N (sizeof(k_autolock_steps) / sizeof(k_autolock_steps[0]))

static int s_sel = 0;
static bool s_confirm_active = false;
static char s_confirm_buf[IDENT_DEV_ID_MAX];
static size_t s_confirm_len = 0;

/* F6.5: the passcode set/change/off modal — see this file's own module
 * comment above for the exact interaction this implements. */
typedef enum {
    PW_MODAL_NONE = 0,
    PW_MODAL_CURRENT, /* asking for the CURRENT passcode (change/off path) */
    PW_MODAL_NEW,     /* asking for the NEW passcode (or empty = off, if s_pw_allow_off) */
} pw_modal_t;

static pw_modal_t s_pw_modal = PW_MODAL_NONE;
static char s_pw_buf[LOCK_PASSCODE_MAX + 1];
static size_t s_pw_len = 0;
static bool s_pw_allow_off = false;

static void device_on_event(ui_evt_t evt)
{
    if (evt != UI_EVT_ENTER) {
        return;
    }
    s_confirm_active = false; // always start a fresh visit in normal browse mode
    s_pw_modal = PW_MODAL_NONE;
    s_pw_len = 0;
    s_pw_buf[0] = '\0';
    if (s_sel < 0) {
        s_sel = 0;
    }
    if (s_sel > MROW_COUNT - 1) {
        s_sel = MROW_COUNT - 1;
    }
}

static void device_on_key_confirm(input_key_t key)
{
    switch (key.type) {
    case INPUT_KEY_CHAR:
        // ASCII-only field, IME bypassed (docs/DEVICE_PLAN.md §5.3: "the
        // setup-code and phone fields are ASCII-only by definition and
        // bypass it" — this confirm field is the same kind of field).
        if (s_confirm_len + 1 < sizeof(s_confirm_buf)) {
            s_confirm_buf[s_confirm_len++] = key.ch;
            s_confirm_buf[s_confirm_len] = '\0';
        }
        break;
    case INPUT_KEY_BACKSPACE:
        if (s_confirm_len > 0) {
            s_confirm_buf[--s_confirm_len] = '\0';
        }
        break;
    case INPUT_KEY_ENTER:
        if (strcmp(s_confirm_buf, ident_get_dev_id()) == 0) {
            // TODO(F7.1): also erase the `book` NVS namespace once it exists.
            ident_erase();
            // Power effect: full ESP32 reset. The modem is left exactly as
            // it was (a separate chip over UART) until the next boot's
            // ident_load() fails and main.c's existing "IDENT missing"
            // Setup-console path takes over — the same reset-recovery
            // shape this codebase already uses elsewhere (F4's
            // net_recover_modem(), watchdog resets), not a new hazard.
            esp_restart();
        } else {
            ui_show_toast("doesn't match");
            s_confirm_len = 0;
            s_confirm_buf[0] = '\0';
        }
        break;
    case INPUT_KEY_ESC:
        s_confirm_active = false;
        break;
    default:
        break;
    }
}

// F6.5: the passcode set/change/off modal's own key handler — masked field,
// IME bypassed (same "ASCII-only field" reasoning device_on_key_confirm()
// above already uses for the factory-reset confirm field).
static void device_on_key_pw(input_key_t key)
{
    switch (key.type) {
    case INPUT_KEY_CHAR:
        if (s_pw_len + 1 < sizeof(s_pw_buf)) {
            s_pw_buf[s_pw_len++] = key.ch;
            s_pw_buf[s_pw_len] = '\0';
        }
        break;
    case INPUT_KEY_BACKSPACE:
        if (s_pw_len > 0) {
            s_pw_buf[--s_pw_len] = '\0';
        }
        break;
    case INPUT_KEY_ESC:
        s_pw_modal = PW_MODAL_NONE;
        s_pw_len = 0;
        s_pw_buf[0] = '\0';
        break;
    case INPUT_KEY_ENTER:
        if (s_pw_modal == PW_MODAL_CURRENT) {
            // §5.8: "asks for the current passcode first" — verified via
            // the same lock_try_passcode() the Locked screen itself uses
            // (shares its fail_count/backoff, deterring brute-forcing from
            // either surface with one shared schedule).
            bool ok = lock_try_passcode(s_pw_buf, s_pw_len);
            s_pw_len = 0;
            s_pw_buf[0] = '\0';
            if (ok) {
                s_pw_modal = PW_MODAL_NEW;
                s_pw_allow_off = true;
            } else {
                ui_show_toast(lock_is_locked_out() ? "wrong - locked out" : "wrong passcode");
                s_pw_modal = PW_MODAL_NONE;
            }
        } else if (s_pw_modal == PW_MODAL_NEW) {
            if (s_pw_len == 0) {
                if (s_pw_allow_off) {
                    lock_clear_passcode();
                    ui_show_toast("passcode off");
                } // else: nothing typed on a first-time set — treat as cancelled, no-op
            } else if (lock_set_passcode(s_pw_buf, s_pw_len)) {
                ui_show_toast("passcode set");
            } else {
                ui_show_toast("4-16 characters");
            }
            s_pw_len = 0;
            s_pw_buf[0] = '\0';
            s_pw_modal = PW_MODAL_NONE;
        }
        break;
    default:
        break;
    }
}

static void device_on_key(input_key_t key)
{
    if (s_confirm_active) {
        device_on_key_confirm(key);
        return;
    }
    if (s_pw_modal != PW_MODAL_NONE) {
        device_on_key_pw(key);
        return;
    }

    switch (key.type) {
    case INPUT_KEY_UP:
        if (s_sel > 0) {
            s_sel--;
        }
        break;
    case INPUT_KEY_DOWN:
        if (s_sel < MROW_COUNT - 1) {
            s_sel++;
        }
        break;
    case INPUT_KEY_ENTER:
        switch ((device_row_t) s_sel) {
        case MROW_RESYNC:
            ui_show_toast(modes_publish_status_now() ? "re-sync requested" : "not connected");
            break;
        case MROW_TEXTSIZE:
            ui_toggle_text_size();
            break;
        case MROW_PASSCODE:
            s_pw_len = 0;
            s_pw_buf[0] = '\0';
            if (lock_is_set()) {
                s_pw_modal = PW_MODAL_CURRENT;
                s_pw_allow_off = false; // set once PW_MODAL_CURRENT succeeds, above
            } else {
                s_pw_modal = PW_MODAL_NEW;
                s_pw_allow_off = false; // first-time set: empty = cancel, not "off"
            }
            break;
        case MROW_AUTOLOCK: {
            uint8_t cur = lock_auto_min();
            size_t idx = 0;
            for (size_t i = 0; i < AUTOLOCK_STEPS_N; i++) {
                if (k_autolock_steps[i] == cur) {
                    idx = i;
                    break;
                }
            }
            idx = (idx + 1) % AUTOLOCK_STEPS_N;
            lock_set_auto_min(k_autolock_steps[idx]);
            break;
        }
        case MROW_SENDERS:
            lock_set_preview(!lock_preview());
            break;
        case MROW_SETUP_AGAIN:
            ui_show_toast("use the USB console: setup <code> (see F6.3 report)");
            break;
        case MROW_FACTORY_RESET:
            s_confirm_active = true;
            s_confirm_len = 0;
            s_confirm_buf[0] = '\0';
            break;
        default:
            break;
        }
        break;
    case INPUT_KEY_ESC:
        ui_pop();
        break;
    default:
        break;
    }
}

static void device_render_confirm(void)
{
    int y = UI_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, "Factory reset");
    y += 14;
    char msg[64];
    snprintf(msg, sizeof(msg), "type device id to confirm:");
    gfx_text(0, y, GFX_FONT_NORMAL, msg);
    y += 12;
    gfx_text(0, y, GFX_FONT_NORMAL, ident_get_dev_id());
    y += 16;
    char line[40];
    snprintf(line, sizeof(line), "> %s", s_confirm_buf);
    gfx_text(0, y, GFX_FONT_NORMAL, line);

    gfx_text(0, UI_FOOTER_Y, GFX_FONT_NORMAL, "enter confirm  esc cancel");
}

// F6.5: the passcode modal's own render — masked, never the actual
// characters (same reasoning scr_lock.c's Locked-screen field gives).
static void device_render_pw(void)
{
    int y = UI_BODY_TOP + 2;
    const char *title = (s_pw_modal == PW_MODAL_CURRENT) ? "Current passcode" : "New passcode";
    gfx_text(0, y, GFX_FONT_NORMAL, title);
    y += 14;
    if (s_pw_modal == PW_MODAL_NEW && s_pw_allow_off) {
        gfx_text(0, y, GFX_FONT_NORMAL, "(empty = turn off)");
        y += 12;
    }
    char mask[LOCK_PASSCODE_MAX + 4]; /* "> " + up to LOCK_PASSCODE_MAX '*' + '_' + NUL */
    size_t i = 0;
    mask[i++] = '>';
    mask[i++] = ' ';
    for (size_t j = 0; j < s_pw_len; j++) {
        mask[i++] = '*';
    }
    mask[i++] = '_';
    mask[i] = '\0';
    gfx_text(0, y, GFX_FONT_NORMAL, mask);

    gfx_text(0, UI_FOOTER_Y, GFX_FONT_NORMAL, "enter submit  esc cancel");
}

// Info lines (not selectable) followed by the menu rows (selectable, in
// device_row_t order) — built fresh each render into a static buffer (no
// heap; single render task per README R5, so no reentrancy hazard) and
// drawn with a simple scroll-into-view so the whole thing "scrolls"
// (docs/DEVICE_PLAN.md §5.5: "Read-mostly, one screen, scrolls").
#define DEVICE_MAX_LINES 16
#define DEVICE_LINE_LEN 72
#define DEVICE_VISIBLE_LINES 8 /* 12px pitch between UI_BODY_TOP and the footer */

static void device_render_normal(void)
{
    static char lines[DEVICE_MAX_LINES][DEVICE_LINE_LEN];
    bool selectable[DEVICE_MAX_LINES];
    int n = 0;

    snprintf(lines[n], DEVICE_LINE_LEN, "id %s  fw %s", ident_get_dev_id(), modes_get_fw_version());
    selectable[n++] = false;
    snprintf(lines[n], DEVICE_LINE_LEN, "owner %s  claimed %s",
             ident_get_label()[0] ? ident_get_label() : "-", ident_get_claimed() ? "yes" : "no");
    selectable[n++] = false;
    snprintf(lines[n], DEVICE_LINE_LEN, "broker %s:%u  sig %s", ident_get_host(),
             (unsigned) ident_get_port(), (ident_get_flags() & IDENT_FLAG_REQ_SIG) ? "on" : "off");
    selectable[n++] = false;
    snprintf(lines[n], DEVICE_LINE_LEN, "signal %d dBm  batt %d mV", modes_get_rssi_dbm(),
             modes_get_batt_mv());
    selectable[n++] = false;
    snprintf(lines[n], DEVICE_LINE_LEN, "session %s  book v- (not synced, needs book.c)",
             modes_get_session_id());
    selectable[n++] = false;
    snprintf(lines[n], DEVICE_LINE_LEN, "counters memfull %u  drops %u  resets %u",
             (unsigned) modes_get_memfull_count(), (unsigned) modes_get_oversize_drop_count(),
             (unsigned) modes_get_modem_resets());
    selectable[n++] = false;
    snprintf(lines[n], DEVICE_LINE_LEN, "tofu glyphs %u", (unsigned) gfx_get_tofu_count());
    selectable[n++] = false;

    int menu_start = n;
    snprintf(lines[n], DEVICE_LINE_LEN, "Re-sync address book");
    selectable[n++] = true;
    snprintf(lines[n], DEVICE_LINE_LEN, "Text size: %s",
             ui_text_size() == GFX_FONT_LARGE ? "large" : "normal");
    selectable[n++] = true;
    snprintf(lines[n], DEVICE_LINE_LEN, "Passcode: %s", lock_is_set() ? "set" : "not set");
    selectable[n++] = true;
    uint8_t auto_min = lock_auto_min();
    if (auto_min == 0) {
        snprintf(lines[n], DEVICE_LINE_LEN, "Auto-lock: never");
    } else {
        snprintf(lines[n], DEVICE_LINE_LEN, "Auto-lock: %u min", (unsigned) auto_min);
    }
    selectable[n++] = true;
    snprintf(lines[n], DEVICE_LINE_LEN, "Show senders when locked: %s",
             lock_preview() ? "on" : "off");
    selectable[n++] = true;
    snprintf(lines[n], DEVICE_LINE_LEN, "Set up again (see report)");
    selectable[n++] = true;
    snprintf(lines[n], DEVICE_LINE_LEN, "Factory reset");
    selectable[n++] = true;

    int sel_abs = menu_start + s_sel;
    int scroll_top = 0;
    if (sel_abs >= DEVICE_VISIBLE_LINES) {
        scroll_top = sel_abs - DEVICE_VISIBLE_LINES + 1;
    }
    int max_top = (n > DEVICE_VISIBLE_LINES) ? (n - DEVICE_VISIBLE_LINES) : 0;
    if (scroll_top > max_top) {
        scroll_top = max_top;
    }

    int y = UI_BODY_TOP + 2;
    for (int i = scroll_top; i < n && i < scroll_top + DEVICE_VISIBLE_LINES; i++) {
        if (selectable[i] && i == sel_abs) {
            gfx_text(0, y, GFX_FONT_NORMAL, ">");
        }
        gfx_text(10, y, GFX_FONT_NORMAL, lines[i]);
        y += 12;
    }

    gfx_text(0, UI_FOOTER_Y, GFX_FONT_NORMAL, "up/down move  enter select  esc back");
}

static void device_render(void)
{
    if (s_confirm_active) {
        device_render_confirm();
    } else if (s_pw_modal != PW_MODAL_NONE) {
        device_render_pw();
    } else {
        device_render_normal();
    }
}

const ui_screen_t g_scr_device = {
    .name = "device",
    .render = device_render,
    .on_key = device_on_key,
    .on_event = device_on_event,
};

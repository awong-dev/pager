// scr_setup.c — Setup screen: typed-code entry + the §3.2 progress line
// (docs/DEVICE_TASKS.md F6.3, docs/DEVICE_PLAN.md §5.5 "Setup").
//
// UNREACHABLE THIS TASK — flagged rather than silently built as dead code.
// §5.5: "Shown when ident is missing or fails validation, and by *Set up
// again*." Both triggers are out of reach from here:
//   - "ident missing": main.c's app_main() checks ident_load() and, on
//     failure, starts start_setup_console() (a USB-serial esp_console REPL)
//     in an infinite vTaskDelay() loop *without ever calling modes_boot()/
//     modes_run()* — the screen stack this file plugs into does not exist
//     yet at that point. Wiring this screen in means editing main.c, which
//     is not in F6.3's Files list.
//   - "Set up again" (Device screen): stubbed in scr_device.c with a
//     documented reason (setup_run()'s no-live-session-overlap assumption)
//     — see that file's own comment.
// So this screen is real, compiles, and is exercised by
// firmware/host's fixture-style render (see render_png.c's own comment on
// why it draws an equivalent mockup by hand instead of linking this file),
// but nothing in this codebase pushes it onto the stack yet. Wiring either
// trigger is a follow-on task's job (touching main.c and/or setup.c/net.cpp).
//
// The code field is ASCII-only and bypasses the IME (docs/DEVICE_PLAN.md
// §5.3: "the setup-code ... fields are ASCII-only by definition and bypass
// it"), same as scr_device.c's factory-reset confirm field.

#include "ui.h"

#include <stdio.h>
#include <string.h>

#define SETUP_CODE_BUF 40 /* generous over a typical "<14 chars> @ host[:port]" code */

static char s_code[SETUP_CODE_BUF];
static size_t s_len = 0;

static void setup_on_event(ui_evt_t evt)
{
    if (evt == UI_EVT_ENTER) {
        s_len = 0;
        s_code[0] = '\0';
    }
}

static void setup_on_key(input_key_t key)
{
    switch (key.type) {
    case INPUT_KEY_CHAR:
        if (s_len + 1 < sizeof(s_code)) {
            s_code[s_len++] = key.ch;
            s_code[s_len] = '\0';
        }
        break;
    case INPUT_KEY_BACKSPACE:
        if (s_len > 0) {
            s_code[--s_len] = '\0';
        }
        break;
    case INPUT_KEY_ENTER:
        // Not wired to setup_run() this task — see this file's own module
        // comment and scr_device.c's MROW_SETUP_AGAIN comment for why.
        ui_show_toast("on-device entry not wired yet; use USB console: setup <code>");
        break;
    case INPUT_KEY_ESC:
        if (s_len > 0) {
            s_len = 0;
            s_code[0] = '\0';
        } else {
            ui_pop();
        }
        break;
    default:
        break;
    }
}

static void setup_render(void)
{
    int y = UI_BODY_TOP + 2;
    gfx_text(0, y, GFX_FONT_NORMAL, "Setup");
    y += 16;
    gfx_text(0, y, GFX_FONT_NORMAL, "type your setup code:");
    y += 12;
    char line[48];
    snprintf(line, sizeof(line), "> %s", s_code);
    gfx_text(0, y, GFX_FONT_NORMAL, line);
    y += 16;

    // Static progress legend (docs/DEVICE_PLAN.md §3.2's four words) — not
    // driven by any real state on this path, see this file's module
    // comment; shown so the layout matches §5.5's mockup.
    gfx_text(0, y, GFX_FONT_NORMAL, "network . broker . bundle . done");

    gfx_text(0, UI_FOOTER_Y, GFX_FONT_NORMAL, "enter submit  esc clear/back");
}

const ui_screen_t g_scr_setup = {
    .name = "setup",
    .render = setup_render,
    .on_key = setup_on_key,
    .on_event = setup_on_event,
};

// scr_greeting.c — TASK_ui_finish.md Do #3 (owner list, 24 Sep 22:30 PDT):
// the per-name "Hi May! Hi Hannah! Hi Colin!" boot splash is gone. This
// screen is a plain, single-line centered STATUS screen: one string at
// 16px in the middle of the body ("booting"/"sim missing"/
// "type: setup <code>"/"shutting down"). The name shuffle, the Fisher-Yates
// helper, and the "reshuffle only on push, never on repaint" e-paper
// ghosting fix this file used to need for THAT bug are all gone with it —
// a single fixed status string repainted several times in a row cannot
// show overlapping remnants of different shuffles, there is nothing to
// reshuffle any more.
//
// Lifecycle, revised 25 Sep 2026 (owner report: the device sat on "booting"
// for ~100s after a reboot because modes_boot() used to push this
// unconditionally, ahead of the synchronous net_init()/net_session_up()
// bring-up, and nothing replaced it until modes_run()'s loop took its first
// iteration): modes_boot() (modes.c) no longer pushes this screen on a
// normal boot at all — it pushes the real Lock/Home frame directly, since
// nothing about Lock-vs-Home depends on the network being up. This screen
// now survives only for main.c's own pre-provisioning (IDENT-missing) Setup
// mode splash (main.c's own call site, before modes_boot() is ever reached),
// where it walks "booting" -> "sim missing"/"type: setup <code>" ->
// "shutting down" as before. There is no longer a modes.c-side sync
// function that pops or replaces it — a Setup-mode boot without a valid
// identity never reaches modes_boot()/modes_run() at all (main.c loops in
// the Setup console instead), so this screen has no boot-time successor to
// hand off to any more.
//
// GREETING_HELLO is kept as the one surviving mode value (ui.h's
// scr_greeting_mode_t) purely so main.c's existing
// scr_greeting_set_mode(GREETING_HELLO) call site did not need touching —
// there is only ever one mode now, so on_event() no longer branches on it.

#include "ui.h"

#include <stdio.h>
#include <string.h>

static char s_status[32] = "";

void scr_greeting_set_mode(scr_greeting_mode_t mode) { (void) mode; /* only GREETING_HELLO exists */ }

void scr_greeting_set_status(const char *status)
{
    if (!status) {
        s_status[0] = '\0';
        return;
    }
    snprintf(s_status, sizeof(s_status), "%s", status);
}

static void render(void)
{
    // Do #3: "a single centered string at 16px in the middle of the body."
    // Falls back to "booting" if a caller ever pushes this screen without
    // first calling scr_greeting_set_status() (should not happen via either
    // call site today — defensive only, mirrors the old render()'s own
    // "shuffle_line() called before any on_event(ENTER)" defensive branch).
    const char *text = s_status[0] != '\0' ? s_status : "booting";

    const int sz = GFX_FONT_LARGE;
    int w = gfx_text_width(sz, text);
    int x = (GFX_SCREEN_W - w) / 2;
    // Owner, 24 Sep 2026: centered text at 16px sat visibly a little low —
    // shift up half a character height (8px). scr_lock.c's centered
    // "locked"/"password:" line uses the identical formula and gets the
    // same shift, so the two screens' vertical centering still agree.
    int y = UI_BODY_TOP + (GFX_SCREEN_H - UI_BODY_TOP - 16) / 2 - 8;
    gfx_text(x, y, sz, text);
}

static void on_key(input_key_t key)
{
    (void) key;
    ui_pop();
}

const ui_screen_t g_scr_greeting = {
    .name = "greeting",
    .render = render,
    .on_key = on_key,
    .on_event = NULL, // Do #3: nothing left to reset per-push (no shuffle) — see this file's module comment
};

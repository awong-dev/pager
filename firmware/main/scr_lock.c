// scr_lock.c — Locked screen (docs/DEVICE_TASKS.md F6.5, docs/DEVICE_PLAN.md
// §5.5 "Locked" mockup, §5.8).
//
// Reached by auto-lock, "Lock now" (scr_home.c), or a restart while a
// passcode is set (modes.c). Status bar as usual (drawn by ui_render()
// itself); nothing else on this screen is reachable except the masked
// passcode field — ui_on_button_short()/ui_on_button_long() (ui.c) are
// gated on lock_is_locked() so the button truly does nothing while locked,
// matching this screen's own "btn hold = nothing" footer.
//
// The screen shows nothing about the waiting messages: no count, no sender
// names (owner decision, 24 Sep 2026 — a leak, and redundant with the status
// bar). Unlocking still lands on the newest unread chat.

#include "ui.h"
#include "lock.h"
#include "msg.h"

#include <stdio.h>
#include <string.h>

#define LOCK_INPUT_MAX (LOCK_PASSCODE_MAX + 1)
static char s_buf[LOCK_INPUT_MAX];
static size_t s_len = 0;

static void lock_on_event(ui_evt_t evt)
{
    if (evt == UI_EVT_ENTER) {
        s_buf[0] = '\0';
        s_len = 0;
    }
}

static void try_unlock(void)
{
    // No retry lockout (owner decision, 2026-09-23, docs/DEVICE_PLAN.md
    // §5.8): PBKDF2's own ~50-100ms per attempt already deters guessing, so
    // a wrong code just clears the entry and shows a toast — no wait.
    bool ok = lock_try_passcode(s_buf, s_len);
    s_buf[0] = '\0';
    s_len = 0;

    if (!ok) {
        ui_show_toast("wrong passcode");
        return;
    }

    // docs/DEVICE_PLAN.md §5.8: "the newest-unread chat opens, the refresh
    // completes, and the deferred shown acks go out through the normal
    // pending-ack queue." ui_replace()/ui_go_home() here only update the
    // screen stack — the actual e-paper refresh happens moments later, in
    // this SAME modes_run() wake-and-drain iteration, when it calls
    // ui_render() after draining every input event (modes.c). Queuing the
    // deferred acks here (rather than only after that render, the way
    // ui_incoming() insists on) is safe specifically because both this call
    // and that render run on modes_run()'s own task, in program order —
    // there is no cross-task race to guard against, unlike ui_incoming()'s
    // MQTT-event-task caller. msg_pump() (the actual network publish of the
    // now-queued acks) always runs still later in that same iteration, after
    // ui_render() — see modes.c's modes_run() ordering.
    const msg_t *u = msg_newest_unread();
    if (u) {
        ui_replace(&g_scr_chat);
    } else {
        ui_go_home();
    }
    msg_mark_all_unshown();
}

static void lock_on_key(input_key_t key)
{
    // "the button and every key other than the passcode field do nothing" —
    // only CHAR/BACKSPACE/ENTER touch the passcode field; everything else
    // (esc, tab, arrows) is a deliberate no-op, not just "unhandled".
    switch (key.type) {
    case INPUT_KEY_CHAR:
        if (s_len + 1 < sizeof(s_buf)) {
            s_buf[s_len++] = key.ch;
            s_buf[s_len] = '\0';
        }
        break;
    case INPUT_KEY_BACKSPACE:
        if (s_len > 0) {
            s_buf[--s_len] = '\0';
        }
        break;
    case INPUT_KEY_ENTER:
        try_unlock();
        break;
    default:
        break;
    }
}

static void lock_render(void)
{
    const int sz = GFX_FONT_NORMAL;
    int y = UI_BODY_TOP + 6;

    // Owner's beta request, verbatim: "the lock screen should show the words
    // 'screen locked' somewhere" — the literal phrase, not just "Locked"
    // (docs/DEVICE_PLAN.md §5.8's mockup updated to match).
    const char *title = "screen locked";
    int tw = gfx_text_width(sz, title);
    gfx_text((GFX_SCREEN_W - tw) / 2, y, sz, title);
    y += 16;

    // No unread count or sender names here (owner, 24 Sep 2026 beta
    // feedback): it leaks who is writing to a locked pager, and the status
    // bar's unread indicator already says there is something waiting.

    // Owner's beta request: "when typing a pass code, it should show * or
    // similar" — one '*' per digit typed so far, nothing else about the
    // code (no length number, no cursor glyph past the last '*').
    char line[64];
    char mask[LOCK_INPUT_MAX + 1];
    size_t i = 0;
    for (; i < s_len; i++) {
        mask[i] = '*';
    }
    mask[i] = '\0';
    snprintf(line, sizeof(line), "passcode  %s", mask);
    gfx_text(8, y, sz, line);

    gfx_text(0, UI_FOOTER_Y, sz, "enter unlock            btn hold = nothing");
}

const ui_screen_t g_scr_lock = {
    .name = "lock",
    .render = lock_render,
    .on_key = lock_on_key,
    .on_event = lock_on_event,
};

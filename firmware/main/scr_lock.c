// scr_lock.c — Locked screen (docs/DEVICE_TASKS.md F6.5, docs/DEVICE_PLAN.md
// §5.5 "Locked" mockup, §5.8).
//
// TASK_ui_finish.md Do #4/#6 (owner list, 24 Sep 22:30 PDT): rewritten to
// two states instead of one always-visible passcode field. (a) idle: the
// single centered word "locked", nothing else — no instructions, no field,
// no unread info (commit c291504's own rule still stands; this task only
// changes state (a)'s remaining "enter unlock / btn hold = nothing" footer
// text, which is gone too now — "nothing else" means nothing else). (b)
// entering, reached ONLY by an IO1 short press (Do #6: "not by typing" —
// scr_lock_start_entry(), called from ui.c's ui_on_button_short(), never
// from this file's own lock_on_key()): the single line becomes
// "password: ****", typing digits fills the mask, Enter verifies (existing
// lock_try_passcode() logic, unchanged), Esc or 30s idle returns to (a), a
// wrong code returns to (a) after the existing toast.
//
// Reached by auto-lock, "Lock now" (scr_home.c), or a restart while a
// passcode is set (modes.c). Status bar as usual (drawn by ui_render()
// itself); nothing else on this screen is reachable except the masked
// passcode field once entering — ui_on_button_long() (ui.c) is still gated
// on lock_is_locked() so a long press truly does nothing while locked, and
// ui_on_button_short() no longer opens Chat while locked either (see its
// own comment) — it starts passcode entry instead.
//
// The screen shows nothing about the waiting messages: no count, no sender
// names (owner decision, 24 Sep 2026 — a leak, and redundant with the status
// bar). Unlocking still lands on the newest unread chat.

#include "ui.h"
#include "lock.h"
#include "msg.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <string.h>

static const char *TAG = "scr_lock";

#define LOCK_INPUT_MAX (LOCK_PASSCODE_MAX + 1)

// Do #4 (b): "Esc or 30 s idle returns to state (a)".
#define LOCK_ENTRY_IDLE_TIMEOUT_US ((int64_t) 30 * 1000000)

typedef enum {
    LOCK_UI_IDLE = 0,  // Do #4 (a): "locked", nothing else
    LOCK_UI_ENTERING,  // Do #4 (b): "password:" + masked field
} lock_ui_state_t;

static lock_ui_state_t s_state = LOCK_UI_IDLE;
static char s_buf[LOCK_INPUT_MAX];
static size_t s_len = 0;
static int64_t s_last_activity_us = 0; // valid iff s_state == LOCK_UI_ENTERING

static void reset_entry(void)
{
    s_buf[0] = '\0';
    s_len = 0;
}

static void return_to_idle(void)
{
    s_state = LOCK_UI_IDLE;
    reset_entry();
}

static void lock_on_event(ui_evt_t evt)
{
    if (evt == UI_EVT_ENTER) {
        // Every fresh arrival at Locked (auto-lock, "Lock now", a locked
        // restart, or admin `cfg` `clear`-while-locked — lock_screen_sync()/
        // modes.c's own call sites) starts at state (a), never mid-entry.
        return_to_idle();
    }
}

void scr_lock_start_entry(void)
{
    // Do #4: "No-op if already entering" (see this function's own ui.h doc
    // comment) — a second IO1 short press mid-entry does not reset the
    // field or its idle timer; typing/backspacing already keeps both alive
    // (lock_on_key() below).
    if (s_state == LOCK_UI_ENTERING) {
        return;
    }
    s_state = LOCK_UI_ENTERING;
    reset_entry();
    s_last_activity_us = esp_timer_get_time();
    // Diagnostic (25 Sep bug, coordinator's round-2 request, kept in
    // release builds too): state transition idle -> entering, so the next
    // capture proves whether lock_render() ever gets a chance to draw
    // "password:" for this press.
    ESP_LOGI(TAG, "scr_lock_start_entry: idle -> entering");
}

// Do #4 (b)'s own 30s-idle half — checked once per render (this screen's
// own regular "runs every modes_run() iteration while on top" cadence,
// same as lock_check_autolock()'s call-site pattern elsewhere) rather than
// a separate timer/poll hook, so a forgotten in-progress entry does not sit
// on screen showing a partial passcode indefinitely.
static void check_idle_timeout(void)
{
    if (s_state != LOCK_UI_ENTERING) {
        return;
    }
    if (esp_timer_get_time() - s_last_activity_us >= LOCK_ENTRY_IDLE_TIMEOUT_US) {
        return_to_idle();
    }
}

static void try_unlock(void)
{
    // No retry lockout (owner decision, 2026-09-23, docs/DEVICE_PLAN.md
    // §5.8): PBKDF2's own ~50-100ms per attempt already deters guessing, so
    // a wrong code just clears the entry and shows a toast — no wait.
    bool ok = lock_try_passcode(s_buf, s_len);
    // Do #4: "wrong code returns to (a) after the existing failure
    // handling" — and a SUCCESSFUL unlock leaves this screen behind
    // entirely (ui_pop()/ui_go_home() below), so returning to idle
    // first is harmless either way and keeps this one return path for both
    // outcomes.
    return_to_idle();

    if (!ok) {
        ui_show_toast("wrong passcode");
        return;
    }

    // Round 8 fix ("relock never shows again" bug, build/bench-logs/
    // round8-sleeptest-C-wait.log): this screen is always reached as an
    // OVERLAY pushed on top of whatever was showing before (modes_boot()'s
    // boot-time `if (lock_is_locked()) ui_push(&g_scr_lock)`, or
    // lock_screen_sync()'s own `ui_push(&g_scr_lock)` on an auto-lock edge,
    // modes.c) — never the sole/bottom frame (ui_init() always establishes
    // Home at depth 1 first). The old code below used ui_replace()/
    // ui_go_home() to leave this screen, which only ever overwrites or
    // resets — neither one pops the frame THIS screen's own push added, so
    // whatever was directly underneath Lock (Home at boot, or the chat an
    // auto-lock interrupted) stayed buried on the stack, unreachable,
    // permanently: every relock+unlock cycle leaked one level of
    // UI_STACK_DEPTH (ui.h, 4). Two cycles were enough on the bench to fill
    // it; the third relock's own ui_push(&g_scr_lock) (lock_screen_sync())
    // then silently failed ("screen stack full (depth 4), dropping push of
    // lock", ui.c) every single loop iteration for the rest of that boot —
    // this screen could never be shown again, matching the reported defect
    // exactly ("whatever was on screen... stays"). ui_pop() undoes exactly
    // the push that got us here, restoring the stack to what it was before
    // Lock went up (in the auto-lock-while-chatting case, this alone
    // already reveals the correct chat again, s_peer untouched).
    //
    // docs/DEVICE_PLAN.md §5.8: "the newest-unread chat opens, the refresh
    // completes, and the deferred shown acks go out through the normal
    // pending-ack queue." This still only updates the screen stack — the
    // actual e-paper refresh happens moments later, in this SAME
    // modes_run() wake-and-drain iteration, when it calls ui_render() after
    // draining every input event (modes.c). Queuing the deferred acks here
    // (rather than only after that render, the way ui_incoming() insists
    // on) is safe specifically because both this call and that render run
    // on modes_run()'s own task, in program order — there is no cross-task
    // race to guard against, unlike ui_incoming()'s MQTT-event-task caller.
    // msg_pump() (the actual network publish of the now-queued acks) always
    // runs still later in that same iteration, after ui_render() — see
    // modes.c's modes_run() ordering.
    ui_pop();
    const msg_t *u = msg_newest_unread();
    if (u) {
        // ui_pop() above may already have revealed g_scr_chat (the
        // auto-lock-while-chatting case) — same peer-selection behaviour
        // as before this fix (s_peer, scr_chat.c, untouched either way);
        // only push if something else (Home, at boot) was revealed instead.
        if (ui_top() != &g_scr_chat) {
            ui_push(&g_scr_chat);
        }
    } else {
        ui_go_home();
    }
    msg_mark_all_unshown();
}

static void lock_on_key(input_key_t key)
{
    if (s_state == LOCK_UI_IDLE) {
        // Do #4/#6: "Keys typed in state (a) are ignored (they no longer
        // open the field)" — unlocking starts ONLY from an IO1 short press
        // (scr_lock_start_entry(), called by ui.c's ui_on_button_short()).
        return;
    }
    switch (key.type) {
    case INPUT_KEY_CHAR:
        if (s_len + 1 < sizeof(s_buf)) {
            s_buf[s_len++] = key.ch;
            s_buf[s_len] = '\0';
        }
        s_last_activity_us = esp_timer_get_time();
        break;
    case INPUT_KEY_BACKSPACE:
        if (s_len > 0) {
            s_buf[--s_len] = '\0';
        }
        s_last_activity_us = esp_timer_get_time();
        break;
    case INPUT_KEY_ENTER:
        try_unlock();
        break;
    case INPUT_KEY_ESC:
        return_to_idle(); // Do #4: "Esc ... returns to state (a)"
        break;
    default:
        break; // tab/arrows: still a deliberate no-op, not just "unhandled"
    }
}

static void lock_render(void)
{
    check_idle_timeout();

    const int sz = GFX_FONT_LARGE; // Do #4: "the single centered word 'locked' at 16 px"
    char line[LOCK_INPUT_MAX + 16];
    if (s_state == LOCK_UI_IDLE) {
        // Do #4 (a): "nothing else" — no instructions, no field, no unread
        // info, and (this task) no footer either; this one word is the
        // entire screen body.
        snprintf(line, sizeof(line), "locked");
    } else {
        // Do #4 (b): "the text becomes 'password:' followed by the masked
        // field (asterisks) on the same centered line" — one '*' per digit
        // typed so far, nothing else about the code (no length number, no
        // cursor glyph past the last '*'), same as the old always-visible
        // field's own owner-requested behaviour.
        char mask[LOCK_INPUT_MAX];
        size_t i = 0;
        for (; i < s_len; i++) {
            mask[i] = '*';
        }
        mask[i] = '\0';
        snprintf(line, sizeof(line), "password: %s", mask);
    }
    int w = gfx_text_width(sz, line);
    // Owner, 24 Sep 2026: same up-8px vertical shift as scr_greeting.c's
    // centered status line (identical formula, both centered 16px text) —
    // sat visibly a little low otherwise.
    int y = UI_BODY_TOP + (GFX_SCREEN_H - UI_BODY_TOP - 16) / 2 - 8;
    gfx_text((GFX_SCREEN_W - w) / 2, y, sz, line);
}

const ui_screen_t g_scr_lock = {
    .name = "lock",
    .render = lock_render,
    .on_key = lock_on_key,
    .on_event = lock_on_event,
};

// scr_greeting.c — boot splash ("Hi <name>! Hi <name>! Hi <name>!", order
// randomized each time it's PUSHED, not each time it's repainted -- see
// on_event()/s_line below; a real hardware bug (duplicated/garbled names,
// stray overlapping strokes) came from an earlier version that reshuffled
// inside render() itself, so a screen pushed once but partial-refreshed
// several times in a row (main.c's booting -> sim missing -> shutting down
// status sequence) painted a DIFFERENT name order each time, and the
// e-paper panel -- which only partial-refreshes the diffed region, not a
// full clear -- showed overlapping remnants of two or three shuffles at
// once) and its reused-layout "sleeping" mode.
//
// Not part of docs/DEVICE_PLAN.md §5.5's screen set — added directly at the
// user's request as a real, separate screen (Home's own conversation-row
// rendering is left untouched, noise-pattern bug and all, for a later
// pass). Two call sites own its lifecycle, both in modes.c:
//   - modes_boot() pushes it once, in GREETING_HELLO mode, right after
//     ui_init() (and before the Locked-screen push, so a locked device
//     still always ends up showing Locked on top — see modes_boot()'s own
//     comment on that ordering requirement).
//   - modes_run()'s UI-awake-window edge detection pushes it in
//     GREETING_SLEEPING mode on the awake->asleep edge, and pops it (if
//     still on top) on the asleep->awake edge — mirroring lock_screen_sync()
//     immediately above that call site.
// on_key pops unconditionally in either mode, so a stray key dispatched in
// the same tick as a wake edge and the sync-driven pop below can't double
// pop: whichever runs first leaves the other's ui_top() check/attempt a
// no-op (modes_run() is single-threaded, no cross-task race here).

#include "ui.h"

#include <stdio.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_random.h"
#endif

static scr_greeting_mode_t s_mode = GREETING_HELLO;
static char s_status[32] = "";
static char s_line[64] = ""; /* the shuffled "Hi ...!" banner, fixed for this push -- see on_event() */

void scr_greeting_set_mode(scr_greeting_mode_t mode) { s_mode = mode; }

void scr_greeting_set_status(const char *status)
{
    if (!status) {
        s_status[0] = '\0';
        return;
    }
    snprintf(s_status, sizeof(s_status), "%s", status);
}

static uint32_t next_rand(void)
{
#ifdef ESP_PLATFORM
    return esp_random();
#else
    // Host build has no hardware RNG and this screen isn't part of the
    // host PNG fixture set (unlike scr_home.c etc.) -- a fixed sequence is
    // fine, this branch exists only so firmware/host's other tests still
    // link cleanly against this file if something ever pulls it in.
    static uint32_t s_state = 0x9e3779b9u;
    s_state = s_state * 1103515245u + 12345u;
    return s_state;
#endif
}

// Fisher-Yates over 3 elements, run once per push (on_event(), below) so
// every repaint of the same push draws the identical banner -- see this
// file's module comment on why re-shuffling per-repaint corrupted the
// e-paper display for real.
static void shuffle_line(void)
{
    static const char *const names[3] = { "Colin", "May", "Hannah" };
    int order[3] = { 0, 1, 2 };
    for (int i = 2; i > 0; i--) {
        int j = (int) (next_rand() % (uint32_t) (i + 1));
        int t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    snprintf(s_line, sizeof(s_line), "Hi %s! Hi %s! Hi %s!", names[order[0]], names[order[1]],
             names[order[2]]);
}

static void on_event(ui_evt_t evt)
{
    if (evt == UI_EVT_ENTER && s_mode == GREETING_HELLO) {
        shuffle_line();
    }
}

static void render(void)
{
    const int sz = GFX_FONT_LARGE;
    int y = UI_BODY_TOP + (GFX_SCREEN_H - UI_BODY_TOP) / 2 - 10;

    if (s_mode == GREETING_SLEEPING) {
        const char *text = "sleeping";
        int w = gfx_text_width(sz, text);
        gfx_text((GFX_SCREEN_W - w) / 2, y, sz, text);
        return;
    }

    if (s_line[0] == '\0') {
        shuffle_line(); // defensive: render() called before any on_event(ENTER), shouldn't happen via ui_push()
    }
    char wrapped[2][64];
    int n = gfx_text_wrap(sz, s_line, GFX_SCREEN_W - 16, wrapped, 2);
    int total_h = n * 20;
    int wy = UI_BODY_TOP + (GFX_SCREEN_H - UI_BODY_TOP - total_h) / 2;
    for (int i = 0; i < n && i < 2; i++) {
        int w = gfx_text_width(sz, wrapped[i]);
        gfx_text((GFX_SCREEN_W - w) / 2, wy, sz, wrapped[i]);
        wy += 20;
    }

    if (s_status[0] != '\0') {
        int fw = gfx_text_width(GFX_FONT_NORMAL, s_status);
        gfx_text((GFX_SCREEN_W - fw) / 2, GFX_SCREEN_H - 9, GFX_FONT_NORMAL, s_status);
    }
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
    .on_event = on_event,
};

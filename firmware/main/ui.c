// ui.c — the screen stack, status bar, CardKB read (docs/DEVICE_TASKS.md
// F6.3, docs/DEVICE_PLAN.md §5.4/§5.5). See ui.h's module comment for the
// full scope note (what's built, what's stubbed, and why).
//
// Authority: docs/PROTOCOL.md §4 (ack ordering — see ui_incoming()),
// firmware/README.md R4 (closed: disp.c's own mutex, F6.1) / R5 (closed:
// modes.c is the only caller of ui_render()/ui_incoming(), both of which
// only ever run on modes_run()'s task) / R7 (closed: ui_incoming() returns
// whether it actually displayed the message; modes.c only marks `shown`
// when it did).
//
// All timing/current/visual claims here are PENDING_HW.

#include "ui.h"
#include "pins.h"
#include "msg.h"
#include "modes.h"
#include "net.h"
#include "disp.h"
#include "gfx.h"
#include "lock.h" /* F6.5: gates ui_on_button_short()/ui_on_button_long() below, docs/DEVICE_PLAN.md §5.8 */
#include "catrust.h" /* v0.2 §4.3: TLS trust-state padlock in draw_status_bar() below */
#include "rail.h" /* rail gate: rail_restored_us() gates ui_poll_keyboard() below, docs/ROADMAP.md */
#include "clockfmt.h" /* TASK_clock.md: pure status-bar clock formatter/minute-change detector */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "driver/gpio.h" /* ui_kb_bus_release()/ui_kb_bus_restore() below */
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

// ---------------------------------------------------------------------------
// Screen stack
// ---------------------------------------------------------------------------

static const ui_screen_t *s_stack[UI_STACK_DEPTH];
static int s_depth = 0;

static void fire_enter(const ui_screen_t *scr)
{
    if (scr && scr->on_event) {
        scr->on_event(UI_EVT_ENTER);
    }
}

void ui_push(const ui_screen_t *scr)
{
    if (!scr) {
        return;
    }
    if (s_depth >= UI_STACK_DEPTH) {
        ESP_LOGI(TAG, "screen stack full (depth %d), dropping push of %s", UI_STACK_DEPTH,
                 scr->name ? scr->name : "?");
        return;
    }
    s_stack[s_depth++] = scr;
    ESP_LOGD(TAG, "screen: -> %s (depth %d)", scr->name ? scr->name : "?", s_depth);
    fire_enter(scr);
}

void ui_pop(void)
{
    if (s_depth <= 1) {
        return; // Home is always the floor
    }
    s_depth--;
    const ui_screen_t *top = s_stack[s_depth - 1];
    ESP_LOGD(TAG, "screen: <- %s (depth %d)", top && top->name ? top->name : "?", s_depth);
    fire_enter(top);
}

void ui_replace(const ui_screen_t *scr)
{
    if (!scr) {
        return;
    }
    if (s_depth == 0) {
        ui_push(scr);
        return;
    }
    s_stack[s_depth - 1] = scr;
    ESP_LOGD(TAG, "screen: = %s (depth %d)", scr->name ? scr->name : "?", s_depth);
    fire_enter(scr);
}

void ui_go_home(void)
{
    s_depth = 0;
    s_stack[s_depth++] = &g_scr_home;
    ESP_LOGD(TAG, "screen: -> home (depth 1)");
    fire_enter(&g_scr_home);
}

const ui_screen_t *ui_top(void)
{
    return (s_depth > 0) ? s_stack[s_depth - 1] : NULL;
}

// ---------------------------------------------------------------------------
// Text size setting (docs/DEVICE_PLAN.md §5.2), NVS namespace "ui".
// ---------------------------------------------------------------------------

#define UI_NVS_NAMESPACE "ui"
#define UI_NVS_KEY_TEXTSZ "textsz"

static gfx_font_t s_text_size = GFX_FONT_NORMAL;

static void load_text_size(void)
{
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return; // never set yet; stays GFX_FONT_NORMAL
    }
    uint8_t v = 0;
    if (nvs_get_u8(h, UI_NVS_KEY_TEXTSZ, &v) == ESP_OK && v <= 1) {
        s_text_size = (gfx_font_t) v;
    }
    nvs_close(h);
}

gfx_font_t ui_text_size(void) { return s_text_size; }

void ui_toggle_text_size(void)
{
    s_text_size = (s_text_size == GFX_FONT_NORMAL) ? GFX_FONT_LARGE : GFX_FONT_NORMAL;
    // Power effect: one NVS (flash) write, negligible — no modem/sleep-state effect.
    nvs_handle_t h;
    if (nvs_open(UI_NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGI(TAG, "text size NVS open failed; new size not persisted this boot");
        return;
    }
    nvs_set_u8(h, UI_NVS_KEY_TEXTSZ, (uint8_t) s_text_size);
    nvs_commit(h);
    nvs_close(h);
}

// ---------------------------------------------------------------------------
// Timestamp formatting helper (docs/DEVICE_PLAN.md §5.5 mockups: "14:02").
// ---------------------------------------------------------------------------

void ui_format_hhmm(int64_t epoch_s, char *out, size_t out_size)
{
    if (epoch_s == 0) {
        snprintf(out, out_size, "--:--"); // PROTOCOL.md §3.5: no clock yet, never guess
        return;
    }
    time_t t = (time_t) epoch_s;
    struct tm tmv;
    // TASK_ui_round2.md Do #7: local time, not UTC — main.c's app_main()
    // sets TZ="PST8PDT,M3.2.0,M11.1.0"/tzset() once at boot (hardcoded per
    // owner 25 Sep 2026; a cfg field later), so localtime_r() here already
    // reflects it regardless of when during boot this first runs.
    localtime_r(&t, &tmv);
    snprintf(out, out_size, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

// ---------------------------------------------------------------------------
// TASK_clock.md: live status-bar clock. The pure formatter/minute-change-
// detector logic lives in clockfmt.c/clockfmt.h (host-testable,
// firmware/host/Makefile's test_clock rule — ui.c itself pulls in
// driver/i2c.h/esp_log.h/FreeRTOS and is not host-buildable, same reason
// modes.c's rssi/batt gathering is split from the pure bucket math in
// bars_from_rssi_dbm()/segs_from_batt_mv() below). This section is just the
// ESP-IDF-side glue: gathering in_use (modes_in_use())/seeded+hh/mm
// (net_get_clock()) and the small bit of state that lets modes_run() know,
// once per loop pass, whether the minute (or the in-use/seeded state) has
// actually changed since the last time draw_status_bar() drew it - see
// ui_clock_due()'s own comment below for why that split matters (modes_run()
// must not force a partial refresh every single loop pass just to ask).
// ---------------------------------------------------------------------------

void ui_status_clock_text(bool in_use, bool seeded, int hh, int mm, char *out, size_t out_size)
{
    clockfmt_status_text(in_use, seeded, hh, mm, out, out_size);
}

// Last HH:MM (or "--:--") draw_status_bar() actually drew - the "last drawn"
// half of clockfmt_due()'s "compare with the last drawn one" (TASK_clock.md
// Do #3). Updated only inside draw_status_bar() itself, so it tracks
// whatever got drawn regardless of which caller triggered the render (a
// key/button event, the cadence's full refresh, or the clock tick itself
// all go through the same paint_frame()->draw_status_bar() path).
static char s_status_clock_last[6] = "";

// TASK_clock.md Do #1/#3: epoch -> local hh/mm, reused by both
// draw_status_bar() (below) and ui_clock_due() so the two never disagree
// about what "now" formats to. TASK_ui_round2.md Do #7: local time, not
// UTC, like ui_format_hhmm() above — same TZ/tzset() set once at boot
// (main.c's app_main()).
static void compute_status_clock_text(bool in_use, char *out, size_t out_size)
{
    int64_t epoch_s = 0;
    bool seeded = net_get_clock(&epoch_s);
    int hh = 0, mm = 0;
    if (seeded) {
        time_t t = (time_t) epoch_s;
        struct tm tmv;
        localtime_r(&t, &tmv);
        hh = tmv.tm_hour;
        mm = tmv.tm_min;
    }
    clockfmt_status_text(in_use, seeded, hh, mm, out, out_size);
}

// TASK_clock.md Do #3: called from modes_run() on every loop pass (not just
// when a key/button event already forced a render) so the minute rolls over
// even through an idle stretch inside the attentive window. Only a string
// compare against the cache above plus (while in use) one net_get_clock()
// call - a RAM read (net.cpp's own doc comment: no AT round trip, just
// esp_timer_get_time() extrapolation from the last NITZ read) - never an AT
// command, never a modem/sleep-state effect of its own. Returns false
// without even formatting anything while not in use: the one-shot "--:--"
// render on the attentive window lapsing is modes_run()'s own job (the
// `attentive != s_attentive_prev` edge, modes.c), not this function's -
// otherwise this would fire a render every single idle loop pass forever
// while asleep, defeating the "one partial per minute" budget below.
// clockfmt_due() both compares AND records `cur` into the cache; that
// second half is harmless here even though draw_status_bar() below
// unconditionally re-records it too a few lines later — modes.c always
// renders (calling draw_status_bar()) in the very same iteration a true
// return here is acted on (its `if (!render_now && ui_clock_due()) ...` ->
// `if (render_now) ui_render();`), so the two writes never disagree.
bool ui_clock_due(void)
{
    if (!modes_in_use()) {
        return false;
    }
    char cur[6];
    compute_status_clock_text(true, cur, sizeof(cur));
    return clockfmt_due(cur, s_status_clock_last, sizeof(s_status_clock_last));
}

// ---------------------------------------------------------------------------
// Status bar (docs/DEVICE_PLAN.md §5.4). Bucket values are read fresh every
// call (cheap: msg.c ring scan, net_get_mqtt_status(), ident getters, and
// modes.c's own cached rssi/batt — see modes_get_rssi_dbm()/
// modes_get_batt_mv()'s doc comments for who owns the *expensive* AT-call
// cadence). "Redraw only on bucket change" (§5.4) falls out of disp.c's own
// diff-against-shadow-plane for free (disp_partial_refresh()'s "no-op if
// nothing changed" contract) — no separate bucket cache is kept here.
// ---------------------------------------------------------------------------

static int bars_from_rssi_dbm(int dbm)
{
    if (dbm >= -85) return 4;
    if (dbm >= -95) return 3;
    if (dbm >= -105) return 2;
    if (dbm >= -115) return 1;
    return 0;
}

static int segs_from_batt_mv(int mv)
{
    if (mv >= 3300) return 4;
    if (mv >= 3250) return 3;
    if (mv >= 3200) return 2;
    if (mv >= 3100) return 1;
    return 0;
}

// README R6 (open, not fixed by this task — msg_thread_at()/
// msg_newest_unread() are not in F6.3's Files list): copies fields out
// promptly per index, same mitigation modes.c's handle_ingest_result()
// already uses, rather than holding a raw msg_t* across any drawing work.
static void count_unread_unsent(int *unread, int *unsent)
{
    *unread = 0;
    *unsent = 0;
    size_t n = msg_thread_count();
    for (size_t i = 0; i < n; i++) {
        const msg_t *m = msg_thread_at(i);
        if (!m) {
            continue;
        }
        uint8_t dir = m->dir;
        uint8_t ack = m->ack_state;
        if (dir == (uint8_t) MSG_DIR_DOWN && ack != MSG_ACK_READ) {
            (*unread)++;
        } else if (dir == (uint8_t) MSG_DIR_UP && ack == MSG_ACK_UP_PENDING) {
            (*unsent)++;
        }
    }
}

// Status-bar text's own line_top_y. Icons below are drawn at y=0 and their
// ink sits roughly in rows 0-11 (icon_bars()/icon_battery()/gfx.c's
// GFX_ICON_LINK_* all bottom out around row 9-11). GFX_FONT_NORMAL's ink
// does NOT start at line_top_y -- confirmed on real hardware (text sat
// visibly lower than the icons, "off by nearly 4 pixels") and measured
// directly from the real font asset data: baseline=13, and the glyphs this
// row actually draws (digits, and the x-height letters in "new ") have
// bearing_y 7-9, so their ink top is line_top_y + (13-9)..(13-7) = +4..+6,
// and EVERY one of them bottoms out at line_top_y+12 (none has a
// descender). Drawing at y=1 (the old value) put that ink at rows 5/7..13,
// well below the icons' own rows. -3 puts it at rows 1/3..9, matching the
// icons' span instead.
#define UI_STATUS_TEXT_Y (-3)

// Icon spacing along the right-aligned group below (MQTT link, signal bars,
// battery) — same 4px gap the old adjacent-icon layout used (lock icon sat
// 4px after the "u<n>" text it followed).
#define UI_STATUS_ICON_GAP 4

// Boot crash indicator (owner request, beta feedback 2026-09-23) — see
// ui_set_crash_indicator()'s own doc comment (ui.h) for the full contract.
// Set once at boot by main.c; cleared by ui_dispatch_key() on the first key.
static bool s_crash_indicator = false;

void ui_set_crash_indicator(bool show) { s_crash_indicator = show; }

static void draw_status_bar(void)
{
    int unread = 0, unsent = 0;
    count_unread_unsent(&unread, &unsent);

    // Left: spelled-out counts, at the user's request (replacing the old
    // icon + "u<n>"/"new <n>" abbreviations split across both ends of the
    // bar).
    char buf[32];
    snprintf(buf, sizeof(buf), "new: %d  unsent: %d", unread, unsent);
    gfx_text(0, UI_STATUS_TEXT_Y, GFX_FONT_NORMAL, buf);

    // Rightmost of all, at the right screen edge (TASK_clock.md, owner
    // request 24 Sep ~11pm PDT): the live HH:MM clock, "--:--" whenever the
    // pager is not "in use" (modes_in_use(): the 120s attentive window) or
    // the clock has never been seeded from the network. Drawn before the
    // icon group below so batt_x can be shifted left by its width - see
    // draw_status_bar()'s own layout comment further down for the ordering.
    char clk[6];
    compute_status_clock_text(modes_in_use(), clk, sizeof(clk));
    int clk_w = gfx_text_width(GFX_FONT_NORMAL, clk);
    int clk_x = GFX_SCREEN_W - clk_w;
    gfx_text(clk_x, UI_STATUS_TEXT_Y, GFX_FONT_NORMAL, clk);
    strncpy(s_status_clock_last, clk, sizeof(s_status_clock_last) - 1);
    s_status_clock_last[sizeof(s_status_clock_last) - 1] = '\0';

    // Right (excluding the clock above): TLS padlock, MQTT link, signal
    // bars, battery - battery now flush against the clock text (was flush
    // against the right screen edge before the clock existed), shifted left
    // by the clock's own width + UI_STATUS_ICON_GAP (TASK_clock.md Do #1:
    // "shifting the icon group left by the text width + 4 px"). (The
    // padlock moved here from the left on 23 Sep 2026, owner's request: it
    // belongs with the other link indicators.)
    int batt_x = clk_x - UI_STATUS_ICON_GAP - GFX_ICON_W;
    gfx_icon(batt_x, 0, (gfx_icon_t) (GFX_ICON_BATTERY_0 + segs_from_batt_mv(modes_get_batt_mv())));

    int bars_x = batt_x - GFX_ICON_W - UI_STATUS_ICON_GAP;
    int bars = bars_from_rssi_dbm(modes_get_rssi_dbm());
    // Note: §5.4 also specifies a distinct "not registered -> x" bucket
    // separate from "0 bars"; net_get_rssi() (net.h) exposes only a dBm
    // reading or failure-with-fallback, no registration-state bit, so that
    // distinction collapses into "0 bars" here. Fixing it needs a new
    // net.h entry point, out of this task's Files list.
    gfx_icon(bars_x, 0, (gfx_icon_t) (GFX_ICON_SIGNAL_0 + bars));

    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    int mqtt_x = bars_x - GFX_ICON_W - UI_STATUS_ICON_GAP;
    gfx_icon(mqtt_x, 0, st.mqtt_connected ? GFX_ICON_LINK_OK : GFX_ICON_LINK_X);

    // v0.2 §4.3: TLS trust-state padlock, leftmost of the right cluster —
    // closed while pinned, broken while broken, nothing at all while
    // unpinned (docs/V02_DESIGN.md §4.3's own "none (this is the chosen
    // default, not a fault)"). Redrawn every partial refresh like every
    // other status-bar icon (disp.c's no-op-if-unchanged partial refresh
    // makes a steady state free).
    int tls_x = mqtt_x - GFX_ICON_W - UI_STATUS_ICON_GAP;
    switch (catrust_get_state()) {
    case CATRUST_PINNED:
        gfx_icon(tls_x, 0, GFX_ICON_TLS_PINNED);
        break;
    case CATRUST_BROKEN:
        gfx_icon(tls_x, 0, GFX_ICON_TLS_BROKEN);
        break;
    case CATRUST_UNPINNED:
    default:
        break;
    }

    // Boot crash indicator (owner request, beta feedback 2026-09-23):
    // immediately left of the TLS padlock slot, for the whole boot after a
    // panic / task-or-RTC watchdog / brownout reset -- see
    // ui_set_crash_indicator()'s own doc comment (ui.h) for the full
    // contract. Its own fixed slot, independent of whether the padlock
    // itself drew anything this frame (CATRUST_UNPINNED draws nothing).
    if (s_crash_indicator) {
        int crash_x = tls_x - GFX_ICON_W - UI_STATUS_ICON_GAP;
        gfx_icon(crash_x, 0, GFX_ICON_CRASH);
    }

    gfx_hline(0, GFX_SCREEN_W - 1, UI_STATUS_H);
}

// ---------------------------------------------------------------------------
// Shared list-row geometry (TASK_ui_round2.md Do #2) — see ui.h's own
// module comment on UI_ROW_H/ui_row_advance() (there, `static inline`, for
// host-testability) for the full rationale; this is only the actual drawing
// half, which needs gfx_hline() and so stays here.
// ---------------------------------------------------------------------------

void ui_draw_row_separator(int y)
{
    gfx_hline(0, GFX_SCREEN_W - 1, y + 3);
    gfx_hline(0, GFX_SCREEN_W - 1, y + 5);
}

// ---------------------------------------------------------------------------
// Lazy rail gate (TASK_ui_round2.md Do #4) — see ui.h's own ui_ensure_powered()
// doc comment for the full contract.
// ---------------------------------------------------------------------------

// Sleeptest report counter (modes.c's "rail: ... lazy_on=") — counts only
// the off->on edges THIS function drives (a render that needed the rail up
// on its own, not the wake-path's own EXT0/EXT1 rule, modes.c's separate
// "on_wakes" counter).
static uint32_t s_rail_lazy_on = 0;

uint32_t ui_rail_lazy_on_count(void) { return s_rail_lazy_on; }

void ui_ensure_powered(void)
{
    bool was_off = !rail_is_on();
    rail_on(); // power effect: see rail_on()'s own comment; a no-op if already on
    if (was_off) {
        disp_note_power_loss(); // the panel's own RAM was lost while the rail was down; force a full refresh
        s_rail_lazy_on++;
    }
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

static void paint_frame(void)
{
    if (s_depth == 0) {
        ui_go_home(); // safety net; should not happen after ui_init()
    }
    gfx_clear();
    draw_status_bar();
    const ui_screen_t *top = ui_top();
    if (top && top->render) {
        top->render();
    }
}

void ui_render(void)
{
    ui_ensure_powered(); // lazy rail gate (Do #4): a render is about to happen
    paint_frame();
    // docs/DEVICE_PLAN.md §5.4: "the full refresh is never taken on the
    // inbound-message path (README R9): it is deferred to the moment the
    // UI-awake window lapses." This is the render path modes_run() calls on
    // every wake-and-drain iteration while input_awake() - i.e. potentially
    // many times per interactive session (each keystroke/scroll) - so it
    // ALWAYS does a partial (never consults/advances the 20-partial
    // cadence counter). ui_on_awake_lapse() below is the one place that
    // cadence counter is ever consulted outside of ui_init()'s own
    // boot-time full refresh (modes_boot() calls disp_refresh_cadence()
    // directly via paint_frame()+ui_render_boot(), not this function).
    disp_partial_refresh(); // power effect: ~0.3-0.8s, PENDING_HW; no-op if nothing changed
}

// Called once, from modes_boot(), right after a successful ui_init() — the
// one call site allowed to consume disp_init()'s "force a full refresh on
// the first call" priming (disp.h), giving the panel a clean baseline
// image on power-up rather than diffing partial-refresh rows against a
// blank shadow plane.
void ui_render_boot(void)
{
    paint_frame();
    disp_refresh_cadence(); // power effect: ~2-4s (forced full on this first call), PENDING_HW
}

// Called from modes.c on the input_awake() true->false edge (the UI-awake
// window lapsing, docs/DEVICE_PLAN.md §5.4). Owner decision, 22 Sep evening
// ("stay on chat unless it's explicitly locked"): modes.c no longer pushes
// scr_greeting.c's "sleeping" screen at this edge, so the top screen is
// whatever it already was — this still repaints it (paint_frame() is cheap,
// framebuffer only, no SPI, and a no-op-looking diff when the stack didn't
// change) rather than just flushing whatever ui_render() last painted. The
// 20-partial cadence counter still decides partial vs. full, so a due full
// refresh lands here (session just ended) rather than mid-interaction or on
// the inbound-message path.
void ui_on_awake_lapse(void)
{
    paint_frame();
    disp_refresh_cadence(); // power effect: ~0.3-0.8s partial, or ~2-4s full (every 20th), PENDING_HW
}

void ui_dispatch_key(input_key_t key)
{
    if (s_crash_indicator) {
        // "the first keyboard interaction (any key event; a page arriving
        // does not clear it)" — ui_incoming() below never calls this
        // function, only modes.c's key-drain path does, so that split is
        // free here.
        s_crash_indicator = false;
        ESP_LOGI(TAG, "boot crash indicator cleared by key press");
    }
    const ui_screen_t *top = ui_top();
    if (top && top->on_key) {
        top->on_key(key);
    }
}

void ui_on_button_short(void)
{
    // TASK_ui_finish.md Do #4/#6 (owner list, 24 Sep 22:30 PDT): "Unlocking
    // is only started by the IO1 button (the wake button), not by typing."
    // A short press while the Locked screen is on top starts passcode entry
    // (scr_lock_start_entry() — no-op if already entering) instead of
    // opening Chat right over it. If, for whatever reason, the device is
    // locked but Locked is NOT on top (should not happen —
    // lock_screen_sync(), modes.c, keeps the two in sync every iteration),
    // this still does nothing further, matching the old "btn hold = nothing
    // [and short = nothing either]" fail-safe default.
    if (lock_is_locked()) {
        // Diagnostic (25 Sep bug: "lock screen never switches to
        // password:" — coordinator's round-2 request, kept in release
        // builds too): proves on the next capture whether this path is
        // even reached, and with the stack in the state it should be.
        ESP_LOGI(TAG, "ui_on_button_short: locked=1 top=%s",
                 ui_top() ? (ui_top()->name ? ui_top()->name : "?") : "(empty)");
        if (ui_top() == &g_scr_lock) {
            scr_lock_start_entry();
            // TASK_ui_round2.md Do #1: paint "password:" right now,
            // synchronously, instead of waiting on modes_run()'s own
            // render_now gate (which was found on the glass to sometimes
            // miss this edge). ui_render() itself calls ui_ensure_powered()
            // first (Do #4), so the rail is guaranteed up for this render
            // regardless of whether the EXT0 wake that got us here already
            // brought it up.
            ui_render();
        }
        return;
    }
    // docs/DEVICE_PLAN.md §5.5 (Home's Keys bullet, applies from anywhere):
    // "if any unread, open the newest unread chat, else stay." T4
    // (docs/CHAT_UI_DESIGN.md §3 Do #5): that chat is the newest unread
    // message's own peer — `from` directly, a down message's peer per
    // msg.c's msg_peer_of() rule (never needs the default-alias/`(default)`
    // fallback that rule also has, since a down message always has `from`).
    const msg_t *u = msg_newest_unread();
    if (!u) {
        return;
    }
    if (ui_top() != &g_scr_chat) {
        ui_push(&g_scr_chat);
    }
    scr_chat_set_peer(u->from);
    scr_chat_mark_visible_read(); // user-initiated open: §5.5's general "opening a chat" rule
}

void ui_on_button_long(void)
{
    // F6.5: same rationale as ui_on_button_short() above — "btn hold =
    // nothing" while Locked is on top, per docs/DEVICE_PLAN.md §5.8's own
    // Locked-screen footer text.
    if (lock_is_locked()) {
        return;
    }
    ui_go_home();
}

bool ui_incoming(const char *from, bool was_asleep)
{
    const ui_screen_t *top = ui_top();
    // Chat already on top counts as "steal" too: the new message belongs in
    // the view the person is looking at, so repaint it. It used to fall to
    // the toast branch, which on real hardware meant a burst of messages
    // painted nine toasts over the chat instead of showing them.
    bool steal = was_asleep || top == NULL || top == &g_scr_home || top == &g_scr_greeting ||
                 top == &g_scr_chat;

    if (!steal) {
        char toast[40];
        snprintf(toast, sizeof(toast), "new: %s", from ? from : "?");
        ui_show_toast(toast);
        return false;
    }

    // TASK_ui_round2.md Do #4: this steal branch always paints
    // (paint_frame()+disp_*_refresh() below, synchronously) regardless of
    // whether the wake that delivered this message already brought the rail
    // up (a timer wake with a page waiting is exactly the "nothing to draw
    // at wake time" case Do #4 describes turning into "something to draw a
    // moment later" once msg_pump() ingests it) — same rule ui_render()'s
    // own top now applies, called here directly since this path bypasses
    // ui_render() (it needs disp_full_refresh() on the was_greeting edge
    // below, not ui_render()'s own always-partial contract).
    ui_ensure_powered();

    // §5.5: "the chat for that sender is pushed with the new message at the
    // bottom, the partial refresh completes, shown is published." No
    // scr_chat_mark_visible_read() here — see this file's/ui.h's own
    // comment on why that ack stays `shown`, not `read`, on this path.
    bool was_greeting = (top == &g_scr_greeting);
    if (top == &g_scr_greeting) {
        // Replace, not push: the greeting is a sleep splash, not somewhere
        // to come back to. Popping Chat should land on Home.
        ui_replace(&g_scr_chat);
    } else if (top != &g_scr_chat) {
        ui_push(&g_scr_chat);
    }
    // T4 (docs/CHAT_UI_DESIGN.md §3: "incoming page ... opens that page's
    // peer"): switches Chat to `from`'s own filtered view. Covers the
    // `top == &g_scr_chat` steal case too (Chat already open, on ANY peer) —
    // CHAT_UI_DESIGN.md does not spell out that case, but leaving Chat
    // showing a different peer than the one that just arrived would mean
    // the incoming message is never actually rendered, which would make the
    // msg_mark_shown() the caller (modes.c) does right after this call
    // returns true a false "shown" ack (README R7: never claim `shown` for
    // a message that was not actually displayed) — so this always switches,
    // at the cost of interrupting an in-progress reply to a DIFFERENT peer
    // than the one that just paged in (scr_chat_set_peer()'s own
    // only-reset-on-an-actual-change guard means the SAME peer's
    // in-progress reply is never disturbed).
    scr_chat_set_peer(from);
    gfx_clear();
    draw_status_bar();
    if (g_scr_chat.render) {
        g_scr_chat.render();
    }
    if (was_greeting) {
        // Owner decision 2026-09-20, from real hardware: going from the
        // greeting (large "Hi <name>!" type) to Chat with only a partial
        // refresh left the greeting visibly ghosted under the message. A
        // change of screen gets a full clear-and-redraw. This overrides
        // DEVICE_PLAN.md §5.4's "the full refresh is never taken on the
        // inbound-message path", but only for the first message that wakes
        // the screen; further messages into an open Chat stay partial.
        // Power effect: ~2-4 s full refresh instead of ~0.3-0.8 s, PENDING_HW.
        //
        // Owner, 24 Sep 2026: accept ghosting on Home for a page arrival in
        // exchange for one ~455 ms partial instead of a ~3.4 s full;
        // supersedes the 20 Sep full-on-steal choice except for the
        // greeting.
        disp_full_refresh();
    } else {
        disp_partial_refresh();
    }
    return true;
}

void ui_show_toast(const char *text)
{
    if (disp_is_dead()) {
        ESP_LOGI(TAG, "toast (display dead, log only): %s", text);
        return;
    }
    // Overlay one row well above the footer; the next ui_render() overwrites
    // it — same contract the pre-F6.3 ui.c's ui_show_toast() documented.
    // docs/CHAT_UI_DESIGN.md §4 ("Home visual fixes (exact)"): the old
    // GFX_SCREEN_H - 12 (= 116) placement overlapped UI_FOOTER_Y (112)'s own
    // footer text (which reaches row 124, ui.h's UI_FOOTER_Y comment) —
    // confirmed on real hardware as the toast stomping the footer hint mid-
    // word. y=98, clearing rows 96..110 (15 rows: the font's 12 px ink band
    // plus a 1-2px margin on each side, same "a bit more than the glyph
    // height" margin UI_STATUS_H's own comment gives for the status bar)
    // stays clear of both the footer (112) and any chat/pick/book content
    // above it.
    const int toast_top = 96;
    const int toast_bottom = 110; // inclusive
    const int toast_text_y = 98;
    for (int x = 0; x < GFX_SCREEN_W; x++) {
        for (int row = toast_top; row <= toast_bottom; row++) {
            gfx_set_pixel(x, row, false);
        }
    }
    gfx_text(0, toast_text_y, GFX_FONT_NORMAL, text);
    disp_partial_refresh(); // power effect: ~0.3-0.8s, PENDING_HW
}

// ---------------------------------------------------------------------------
// CardKB I2C read (moved here from the pre-F6.3 ui.c — see ui.h's own
// compatibility note and input.h's F6.2 scope note on why input.c doesn't
// do this itself).
// ---------------------------------------------------------------------------

static int s_i2c_fail_count = 0;

// Rail gate (docs/ROADMAP.md, owner 24 Sep 10:30 pm PDT): the CardKB's own
// MCU loses power whenever rail.c's rail_off() runs and reboots on the next
// rail_on() -- it needs time to come back up before it can answer an I2C
// read. PAGER_KB_BOOT_GUARD_MS is that budget; ui_poll_keyboard() below
// withholds reads until it has passed, tracked per rail-restore edge
// (rail_restored_us()) rather than a one-shot timer so it re-arms correctly
// every time modes.c's rail gate takes the rail down and back up again
// (every non-attentive sleep). Not blocking: a skipped read just returns
// with no key, same as a NACK would, at zero I2C cost.
#define PAGER_KB_BOOT_GUARD_MS 300

// The rail_restored_us() value this module last decided the boot-guard/
// re-init state for. -1 (never equal to any real timestamp, which is >= 0
// once rail_init() has run) so the very first call always re-evaluates.
static int64_t s_kb_guard_restored_us = -1;
// Whether an I2C-driver re-init has already been attempted for the current
// rail-restore edge -- caps the re-init (and its log line) at once per
// edge instead of once per failed read, in case the keyboard is genuinely
// absent or still dead (docs task brief: "log once per wake at most").
static bool s_kb_reinit_done_this_restore = false;
// Sleeptest report counter (modes.c's "rail: ... kb_skipped_reads=").
static uint32_t s_kb_skipped_reads = 0;

// Whether the I2C driver is currently installed on I2C_NUM_0 -- tracked so
// ui_kb_bus_release() (below) only calls i2c_driver_delete() when there is
// actually something to delete (task brief: "if installed").
static bool s_i2c_installed = false;

static void i2c_kb_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PAGER_PIN_KB_SDA,
        .scl_io_num = PAGER_PIN_KB_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_NUM_0, &conf);
    i2c_driver_install(I2C_NUM_0, conf.mode, 0, 0, 0);
    s_i2c_installed = true;
}

uint32_t ui_kb_skipped_read_count(void) { return s_kb_skipped_reads; }

// Sleeptest report counter (modes.c's "rail: ... kb_bus_releases=").
static uint32_t s_kb_bus_releases = 0;

// See ui.h's own doc comment (ui_kb_bus_release()) for the full rationale.
// Power effect: removes the CardKB's/LIS3DH's phantom-power path through
// the I2C pull-ups while the rail is off (rail.c's rail_off() calls this
// before dropping the rail); no effect on the rail itself.
void ui_kb_bus_release(void)
{
    if (s_i2c_installed) {
        i2c_driver_delete(I2C_NUM_0);
        s_i2c_installed = false;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << PAGER_PIN_KB_SDA) | (1ULL << PAGER_PIN_KB_SCL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&cfg);
    // Low, not floating: floating still lets the external pull-up (tied to
    // the always-on 3V3, not this gated rail) feed the CardKB MCU through
    // its I/O protection diodes -- the phantom-power path this fix closes.
    gpio_set_level((gpio_num_t) PAGER_PIN_KB_SDA, 0);
    gpio_set_level((gpio_num_t) PAGER_PIN_KB_SCL, 0);
    // Same pattern net.cpp's net_sleep() uses for the modem RTS line: holds
    // this LOW level through every light sleep instead of floating (IDF's
    // default sleep GPIO isolation) and being re-pulled high.
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_KB_SDA);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_KB_SCL);
    s_kb_bus_releases++;
}

// See ui.h's own doc comment. Power effect: none by itself -- rail.c's
// rail_on() already drove the rail edge that powers the CardKB back up
// before calling this; this only restores the bus's I2C mode.
void ui_kb_bus_restore(void)
{
    i2c_kb_init(); // re-installs the I2C driver; sets s_i2c_installed = true
}

uint32_t ui_kb_bus_release_count(void) { return s_kb_bus_releases; }

void ui_poll_keyboard(void)
{
    // TASK_ui_round2.md Do #4: the lazy rail gate leaves the rail OFF on a
    // timer wake with nothing to draw — the CardKB is unpowered then, so an
    // I2C read here would just NACK (or worse, wedge waiting on a bus with
    // no pull-ups driven) for zero benefit. Same "skipped read" accounting
    // as the post-restore boot-guard branch below (s_kb_skipped_reads),
    // since from the caller's point of view both are "no key this call, try
    // again later" outcomes.
    if (!rail_is_on()) {
        s_kb_skipped_reads++;
        return;
    }
    int64_t restored_us = rail_restored_us();
    if (restored_us != s_kb_guard_restored_us) {
        // A new rail-restore edge (or the very first call): this module's
        // own per-edge state starts over. rail_on() itself is a no-op (no
        // new edge) while the rail was already on, so this stays untouched
        // for every wake inside the attentive window.
        s_kb_guard_restored_us = restored_us;
        s_kb_reinit_done_this_restore = false;
    }
    if (restored_us != 0 &&
        esp_timer_get_time() - restored_us < (int64_t) PAGER_KB_BOOT_GUARD_MS * 1000) {
        s_kb_skipped_reads++;
        return; // CardKB MCU has not had PAGER_KB_BOOT_GUARD_MS to boot yet
    }

    uint8_t byte = 0;
    esp_err_t err =
        i2c_master_read_from_device(I2C_NUM_0, PAGER_I2C_ADDR_CARDKB, &byte, 1, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        s_i2c_fail_count++;
        if (s_i2c_fail_count == 3) { // log once, then keep trying silently (matches pre-F6.3 tolerance)
            ESP_LOGI(TAG, "CardKB: 3 consecutive I2C failures");
        }
        // Rail gate: a read failing once the boot guard above has already
        // elapsed for this restore edge is worth one I2C-driver re-init --
        // the bus could have been left mid-transaction when the rail
        // dropped. At most once per edge (see
        // s_kb_reinit_done_this_restore's own comment); a genuinely absent
        // keyboard just keeps failing afterwards without spamming this.
        if (!s_kb_reinit_done_this_restore) {
            s_kb_reinit_done_this_restore = true;
            ESP_LOGI(TAG, "CardKB: I2C read failed post-restore; re-initializing the driver");
            i2c_driver_delete(I2C_NUM_0);
            i2c_kb_init();
        }
        return;
    }
    if (s_i2c_fail_count >= 3) {
        ESP_LOGI(TAG, "CardKB: answering again");
    }
    s_i2c_fail_count = 0;

    if (byte == 0x00) {
        return;
    }
    ESP_LOGD(TAG, "CardKB: 0x%02x", byte); // bench: the first hardware check of the decode table
    input_feed_key(byte); // arms the UI-awake window, queues INPUT_EVT_KEY (input.h)
}

// Strong definition of disp.h's weak disp_busy_idle_hook() — bench bug fix:
// each key event made modes.c render a partial refresh whose
// disp_wait_busy_fb() (disp.c) blocks the calling task for ~455ms polling
// BUSY every 10ms; ui_poll_keyboard() otherwise only runs once per
// modes_run() loop iteration (modes.c), so a key typed during that wait was
// lost outright (the CardKB only holds the single most recent unread key).
// This hook now gives that same poll a chance on every 10ms BUSY-wait tick
// too.
//
// Task-safety: ui_poll_keyboard() does one I2C read and, on a decoded byte,
// calls input_feed_key() (input.h), which only arms input.c's awake window
// and xQueueSend()s an input_event_t — no disp.c/SPI call, so no risk of
// re-entering disp.c's own mutex (already held by our caller) or its SPI
// transaction. That makes the hook itself re-entrancy safe; the remaining
// risk is calling the CardKB's I2C bus from two tasks at once, which
// modes_on_run_task() (modes.h) rules out: every disp_*_refresh() call site
// today runs on modes_run()'s own task (ui_render()/ui_on_awake_lapse()/
// service_render_pending(), see their own comments), the same task
// ui_poll_keyboard() is normally called from — modes_on_run_task() confirms
// we are still on it before touching I2C, and skips the poll otherwise
// (currently only modes_boot()'s ui_render_boot(), where the handle is not
// yet recorded and there is nothing to type yet anyway).
//
// Also gated on input_awake(): as ui_poll_keyboard()'s own doc comment
// says, there is no screen to type into unless the UI-awake window is
// armed, so polling here when it is not would just cost an I2C transaction
// for nothing.
void disp_busy_idle_hook(void)
{
    if (!input_awake() || !modes_on_run_task()) {
        return;
    }
    ui_poll_keyboard();
}

// Bounded wait for disp_pre_write_gate_hook() below: a dropped
// +SQNSMQTTONPUBLISH URC (or its OK/ERROR) must not stall rendering
// forever. 1500ms per the coordinator's own figure for this fix.
#define PAGER_UI_PUBLISH_QUIET_MAX_WAIT_MS 1500

// Strong definition of disp.h's weak disp_pre_write_gate_hook() — 23 Sep
// display-corruption field failures: three register-loss events all
// correlated with a panel SPI write starting while a pager-originated MQTT
// publish's LTE uplink was in flight (a reconnect + the `/up` ack publish
// in the 00:40 event); zero on console-driven (`disptest`) refreshes,
// which never publish. net.c tracks in-flight publishes and a short quiet
// window after each one completes (publish_quiet.h); this hook is disp.c's
// only route to that state, kept out of disp.c itself so it does not have
// to include net.h — same layering seam disp_busy_idle_hook() above uses
// for ui.h. Logs once per delayed refresh so the bench can see the gate
// working; silent (and free) when nothing is in flight, which
// net_publish_quiet_wait_ms() itself makes true without a syscall beyond
// one esp_timer_get_time() read.
void disp_pre_write_gate_hook(void)
{
    uint32_t waited_ms = net_publish_quiet_wait_ms(PAGER_UI_PUBLISH_QUIET_MAX_WAIT_MS);
    if (waited_ms >= PAGER_UI_PUBLISH_QUIET_MAX_WAIT_MS) {
        // S12 (docs/SLEEP_URC_DESIGN.md §8.3, docs/SLEEP_URC_TASKS.md S12):
        // net_publish_quiet_wait_ms() returns exactly the budget it was
        // given (not a shorter value) only when it never saw the gate clear
        // -- i.e. it spent its whole PAGER_UI_PUBLISH_QUIET_MAX_WAIT_MS and
        // is about to let the refresh start anyway. That is the 23 Sep
        // display-corruption gate failing open; today it is silent (the
        // ordinary "waited_ms > 0" line below reads identically whether the
        // gate cleared early or never cleared at all). S10 wants to know if
        // this ever fires with a publish actually still stuck -- with S11 in
        // place a publish should never be in flight this long.
        ESP_LOGI(TAG,
                 "publish-quiet gate exhausted its %u ms budget; refreshing anyway "
                 "(23 Sep corruption gate failing open)",
                 (unsigned) PAGER_UI_PUBLISH_QUIET_MAX_WAIT_MS);
    } else if (waited_ms > 0) {
        ESP_LOGI(TAG, "refresh delayed %u ms for an in-flight publish", (unsigned) waited_ms);
    }
}

// ---------------------------------------------------------------------------
// Public init/shutdown
// ---------------------------------------------------------------------------

bool ui_init(void)
{
    i2c_kb_init();
    gfx_clear();
#ifdef ESP_PLATFORM
    if (!gfx_init()) {
        ESP_LOGI(TAG, "gfx_init: no/invalid assets partition; every codepoint draws as tofu");
    }
#endif
    load_text_size();
    s_depth = 0;
    ui_go_home(); // establishes the stack even if disp_init() below fails (headless is not fatal)
    return disp_init(); // power effect: see disp_init()'s own comment
}

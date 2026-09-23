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

#include <string.h>
#include <stdio.h>
#include <time.h>

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
    gmtime_r(&t, &tmv); // UTC — this codebase has no timezone concept anywhere
    snprintf(out, out_size, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
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

    // Right, at the user's request: TLS padlock, MQTT link, signal bars,
    // battery, battery flush against the right edge. (The padlock moved
    // here from the left on 23 Sep 2026, owner's request: it belongs with
    // the other link indicators.)
    int batt_x = GFX_SCREEN_W - GFX_ICON_W;
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

    gfx_hline(0, GFX_SCREEN_W - 1, UI_STATUS_H);
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
    const ui_screen_t *top = ui_top();
    if (top && top->on_key) {
        top->on_key(key);
    }
}

void ui_on_button_short(void)
{
    // F6.5 (docs/DEVICE_PLAN.md §5.8's Locked mockup: "btn hold = nothing" —
    // applies to short press too, "the button ... do[es] nothing" while
    // locked): without this guard, a short press while Locked is on top
    // would open Chat right over the lock screen, bypassing the passcode.
    if (lock_is_locked()) {
        return;
    }
    // docs/DEVICE_PLAN.md §5.5 (Home's Keys bullet, applies from anywhere):
    // "if any unread, open the newest unread chat, else stay."
    const msg_t *u = msg_newest_unread();
    if (!u) {
        return;
    }
    if (ui_top() != &g_scr_chat) {
        ui_push(&g_scr_chat);
    }
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

    // §5.5: "the chat for that sender is pushed with the new message at the
    // bottom, the partial refresh completes, shown is published." No
    // scr_chat_mark_visible_read() here — see this file's/ui.h's own
    // comment on why that ack stays `shown`, not `read`, on this path.
    bool screen_changed = (top != &g_scr_chat);
    if (top == &g_scr_greeting) {
        // Replace, not push: the greeting is a sleep splash, not somewhere
        // to come back to. Popping Chat should land on Home.
        ui_replace(&g_scr_chat);
    } else if (top != &g_scr_chat) {
        ui_push(&g_scr_chat);
    }
    gfx_clear();
    draw_status_bar();
    if (g_scr_chat.render) {
        g_scr_chat.render();
    }
    if (screen_changed) {
        // Owner decision 2026-09-20, from real hardware: going from the
        // greeting (large "Hi <name>!" type) to Chat with only a partial
        // refresh left the greeting visibly ghosted under the message. A
        // change of screen gets a full clear-and-redraw. This overrides
        // DEVICE_PLAN.md §5.4's "the full refresh is never taken on the
        // inbound-message path", but only for the first message that wakes
        // the screen; further messages into an open Chat stay partial.
        // Power effect: ~2-4 s full refresh instead of ~0.3-0.8 s, PENDING_HW.
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
    // Overlay just the bottom text row; the next ui_render() overwrites it —
    // same contract the pre-F6.3 ui.c's ui_show_toast() documented.
    // The row is the font's full height: GFX_FONT_NORMAL is 12 px, and the
    // 8 px row this used to clear drew the toast with its lower third cut
    // off (seen on hardware).
    const int toast_h = 12;
    for (int x = 0; x < GFX_SCREEN_W; x++) {
        for (int row = 0; row < toast_h; row++) {
            gfx_set_pixel(x, GFX_SCREEN_H - toast_h + row, false);
        }
    }
    gfx_text(0, GFX_SCREEN_H - toast_h, GFX_FONT_NORMAL, text);
    disp_partial_refresh(); // power effect: ~0.3-0.8s, PENDING_HW
}

// ---------------------------------------------------------------------------
// CardKB I2C read (moved here from the pre-F6.3 ui.c — see ui.h's own
// compatibility note and input.h's F6.2 scope note on why input.c doesn't
// do this itself).
// ---------------------------------------------------------------------------

static int s_i2c_fail_count = 0;

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
}

void ui_poll_keyboard(void)
{
    uint8_t byte = 0;
    esp_err_t err =
        i2c_master_read_from_device(I2C_NUM_0, PAGER_I2C_ADDR_CARDKB, &byte, 1, pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        s_i2c_fail_count++;
        if (s_i2c_fail_count == 3) { // log once, then keep trying silently (matches pre-F6.3 tolerance)
            ESP_LOGI(TAG, "CardKB: 3 consecutive I2C failures");
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
    if (waited_ms > 0) {
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

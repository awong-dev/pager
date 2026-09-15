// ui.c — CardKB keyboard input and the thread-view/composer screens.
//
// docs/DEVICE_TASKS.md F6.1 moved the SSD1680 transport to disp.c and the
// framebuffer/text/glyph primitives to gfx.c; this file now only owns the
// CardKB I2C polling and the two screens it drew before the split (the
// "real screen-stack rewrite" that replaces these screens with the F6.3
// stack has not happened yet — see docs/DEVICE_PLAN.md §5.4).
//
// Authority: docs/PROTOCOL.md §9.4 (composer limit — now 160 code points/
// 320 bytes per docs/DEVICE_PLAN.md §5.2, not the old ASCII-160-byte
// assumption, which §5.2 withdraws). All timing/current/visual claims here
// are PENDING_HW.

#include "ui.h"
#include "pins.h"
#include "msg.h"
#include "modes.h"
#include "net.h"
#include "disp.h"
#include "gfx.h"

#include <string.h>
#include <stdio.h>

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

#define PAGER_COMPOSER_IDLE_TIMEOUT_US ((int64_t) 60 * 1000000)

static bool s_composer_open = false;
static int64_t s_composer_last_activity_us = 0;
static int s_i2c_fail_count = 0;

// ---------------------------------------------------------------------------
// Layout. Text is drawn at GFX_FONT_NORMAL (12px) throughout — these two
// screens predate DEVICE_PLAN.md §5.4's status-bar/footer/text-size-setting
// design, which F6.3 brings in along with the rest of the screen stack.
// ---------------------------------------------------------------------------

static void draw_msg_line(int y, const msg_t *m, bool show_new)
{
    const char *who = (m->dir == (uint8_t) MSG_DIR_UP) ? "you" : m->from;
    int x = gfx_text(0, y, GFX_FONT_NORMAL, who);
    x = gfx_text(x, y, GFX_FONT_NORMAL, ": ");
    gfx_text(x, y, GFX_FONT_NORMAL, m->body);
    if (show_new) {
        gfx_text(GFX_SCREEN_W - gfx_text_width(GFX_FONT_NORMAL, "NEW"), y, GFX_FONT_NORMAL, "NEW");
    }
}

static void render_thread_frame(void)
{
    gfx_clear();

    if (msg_history_lost()) {
        gfx_text(0, 0, GFX_FONT_NORMAL, "earlier messages lost (restart)");
        gfx_hline(0, GFX_SCREEN_W - 1, 9);
    }

    size_t count = msg_thread_count();
    const msg_t *newest_unread = msg_newest_unread();

    if (count > 0) {
        const msg_t *m0 = msg_thread_at(0);
        bool show_new = (newest_unread != NULL && m0 == newest_unread);
        draw_msg_line(12, m0, show_new);
        gfx_hline(0, GFX_SCREEN_W - 1, 21);
    } else {
        gfx_text(0, 12, GFX_FONT_NORMAL, "no messages yet");
    }

    for (size_t i = 1; i < count && i <= 2; i++) {
        draw_msg_line(24 + (int) (i - 1) * 12, msg_thread_at(i), false);
    }
    gfx_hline(0, GFX_SCREEN_W - 1, 49);

    char status[64];
    net_mqtt_status_t st;
    net_get_mqtt_status(&st);
    size_t unsent = 0;
    for (size_t i = 0; i < count; i++) {
        const msg_t *m = msg_thread_at(i);
        if (m->dir == (uint8_t) MSG_DIR_UP && m->ack_state == MSG_ACK_UP_PENDING) {
            unsent++;
        }
    }
    snprintf(status, sizeof(status), "%s sig:%s unsent:%u", modes_is_active() ? "active" : "sleep",
             st.mqtt_connected ? "ok" : "--", (unsigned) unsent);
    gfx_text(0, GFX_SCREEN_H - 8, GFX_FONT_NORMAL, status);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

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

bool ui_init(void)
{
    i2c_kb_init();
    gfx_clear();
#ifdef ESP_PLATFORM
    if (!gfx_init()) {
        ESP_LOGI(TAG, "gfx_init: no/invalid assets partition; every codepoint draws as tofu");
    }
#endif
    return disp_init(); // power effect: see disp_init()'s own comment
}

void ui_shutdown(void)
{
    disp_shutdown(); // power effect: see disp_shutdown()'s own comment
}

void ui_render_thread(void)
{
    if (disp_is_dead()) {
        return;
    }
    render_thread_frame();
    disp_refresh_cadence(); // power effect: ~0.3-0.8s partial, or ~2-4s every 20th call (full)
}

void ui_render_message_pane(void)
{
    if (disp_is_dead()) {
        return;
    }
    render_thread_frame();
    disp_partial_refresh(); // power effect: ~0.3-0.8s; forced partial, does not touch the cadence counter
}

void ui_show_toast(const char *text)
{
    if (disp_is_dead()) {
        ESP_LOGI(TAG, "toast (display dead, log only): %s", text);
        return;
    }
    // Overlay just the status band (bottom 8 rows); next real render
    // overwrites it.
    for (int x = 0; x < GFX_SCREEN_W; x++) {
        for (int bit = 0; bit < 8; bit++) {
            gfx_set_pixel(x, GFX_SCREEN_H - 8 + bit, false);
        }
    }
    gfx_text(0, GFX_SCREEN_H - 8, GFX_FONT_NORMAL, text);
    disp_partial_refresh(); // power effect: ~0.3-0.8s
}

void ui_composer_open(const char *reply_to_id)
{
    (void) reply_to_id;
    msg_composer_reset();
    s_composer_open = true;
    s_i2c_fail_count = 0;
    s_composer_last_activity_us = esp_timer_get_time();

    gfx_clear();
    gfx_text(0, 0, GFX_FONT_NORMAL, "reply:");
    gfx_hline(0, GFX_SCREEN_W - 1, 9);
    gfx_text(0, GFX_SCREEN_H - 8, GFX_FONT_NORMAL, "enter/hold btn=send  tap btn=cancel");
    disp_refresh_cadence(); // power effect: see disp_refresh_cadence()
}

static void render_composer_text(void)
{
    gfx_clear();
    gfx_text(0, 0, GFX_FONT_NORMAL, "reply:");
    gfx_hline(0, GFX_SCREEN_W - 1, 9);

    char lines[8][64];
    int n = gfx_text_wrap(GFX_FONT_NORMAL, msg_composer_text(), GFX_SCREEN_W, lines, 8);
    if (n < 0) {
        n = 8; // still draw the first 8 lines rather than nothing (gfx_text_wrap's contract)
    }
    for (int i = 0; i < n; i++) {
        gfx_text(0, 12 + i * 10, GFX_FONT_NORMAL, lines[i]);
    }

    char counter[16];
    snprintf(counter, sizeof(counter), "%u/160", (unsigned) msg_composer_len());
    gfx_text(0, GFX_SCREEN_H - 8, GFX_FONT_NORMAL, counter);
}

void ui_composer_close(bool sent)
{
    (void) sent;
    s_composer_open = false;
    s_i2c_fail_count = 0;
    if (disp_is_dead()) {
        return;
    }
    render_thread_frame();
    disp_full_refresh(); // power effect: ~2-4s; ui_init() and composer-close both force a full refresh
}

bool ui_composer_is_open(void) { return s_composer_open; }

ui_key_t ui_poll_keys(void)
{
    if (!s_composer_open) {
        return UI_KEY_NONE;
    }

    int64_t now = esp_timer_get_time();
    if (now - s_composer_last_activity_us > PAGER_COMPOSER_IDLE_TIMEOUT_US) {
        ESP_LOGI(TAG, "composer idle timeout (60s), auto-closing");
        ui_show_toast("composer timed out");
        ui_composer_close(false);
        return UI_KEY_NONE;
    }

    uint8_t byte = 0;
    esp_err_t err = i2c_master_read_from_device(I2C_NUM_0, PAGER_I2C_ADDR_CARDKB, &byte, 1,
                                                 pdMS_TO_TICKS(50));
    if (err != ESP_OK) {
        s_i2c_fail_count++;
        if (s_i2c_fail_count >= 3) {
            ESP_LOGI(TAG, "CardKB: 3 consecutive I2C failures, closing composer");
            ui_show_toast("keyboard not found");
            ui_composer_close(false);
            s_i2c_fail_count = 0;
        }
        return UI_KEY_NONE;
    }
    s_i2c_fail_count = 0;

    if (byte == 0x00) {
        return UI_KEY_NONE;
    }

    modes_note_activity(); // Part A bug #2
    s_composer_last_activity_us = now;

    ui_key_t key = UI_KEY_NONE;
    if (byte >= 0x20 && byte <= 0x7E) {
        if (!msg_composer_push_char((char) byte)) {
            ui_show_toast("reply full (160 chars)");
            return UI_KEY_NONE;
        }
        key.type = UI_KEYTYPE_CHAR;
        key.ch = (char) byte;
        render_composer_text();
        disp_refresh_cadence(); // power effect: see disp_refresh_cadence()
    } else if (byte == 0x08) {
        msg_composer_backspace();
        key.type = UI_KEYTYPE_BACKSPACE;
        render_composer_text();
        disp_refresh_cadence(); // power effect: see disp_refresh_cadence()
    } else if (byte == 0x0D) {
        key.type = UI_KEYTYPE_ENTER;
    }
    // Everything else (>=0x80, other control bytes) is ignored — the CardKB
    // emits one ASCII byte per press (docs/DEVICE_PLAN.md §5.2: an IME may
    // sit between keystrokes and the composer in a later task; there is
    // none yet, so non-ASCII bytes are simply dropped here as before).
    return key;
}

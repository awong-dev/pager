// ui.c — custom SSD1680 e-paper driver, CardKB keyboard, reply composer.
//
// Authority: docs/PROTOCOL.md §6 (command sequence — the 0x22 parameter
// values 0xF7/0xFF are called out there as "inferred, not verified from the
// datasheet PDF"; carried forward here unchanged, same caveat), §9.5 (frame
// buffer sizing), §9.4 (composer limit / ASCII CardKB assumption).
// firmware/README.md (every-20th-partial full refresh, this project's explicit
// requirement over the component author's "~10" suggestion).
//
// All timing/current/visual claims here are PENDING_HW.

#include "ui.h"
#include "pins.h"
#include "msg.h"
#include "modes.h"
#include "net.h"

#include <string.h>
#include <stdio.h>

#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "ui";

// ---------------------------------------------------------------------------
// Panel geometry. Native SSD1680 addressing (matches the command sequence
// below verbatim): X = 128px / 8 = 16 bytes (0..15), Y = 296 rows (0..295).
// We render a landscape (296-wide x 128-tall) thread view entirely by
// choosing which native (row, bit) a given screen pixel maps to — no change
// to the RAM-addressing commands is needed for the rotation, only to the
// pixel-plot helper below.
// ---------------------------------------------------------------------------
#define FB_ROW_BYTES 16
#define FB_ROWS 296
#define SCREEN_W FB_ROWS  // 296 — landscape width, screen x maps to native row
#define SCREEN_H (FB_ROW_BYTES * 8) // 128 — landscape height, screen y maps to native column

#define PAGER_UI_PARTIAL_FULL_EVERY 20 // firmware/README.md, explicit override of "~10"
#define PAGER_UI_BUSY_TIMEOUT_US (15 * 1000000)
#define PAGER_COMPOSER_IDLE_TIMEOUT_US ((int64_t) 60 * 1000000)

static uint8_t s_fb_new[FB_ROWS][FB_ROW_BYTES];
static uint8_t s_fb_old[FB_ROWS][FB_ROW_BYTES];

static spi_device_handle_t s_spi;
static bool s_spi_ready = false;
static bool s_display_dead = false; // logged once, then the device runs headless
static bool s_display_dead_logged = false;
static uint32_t s_partial_count = 0;

static bool s_composer_open = false;
static int64_t s_composer_last_activity_us = 0;
static int s_i2c_fail_count = 0;

// ---------------------------------------------------------------------------
// 5x7 font. Hand-authored for this project (not transcribed from any
// external font file — avoids a licensing question and a dependency).
// Each glyph is 7 row-bytes; bits 4..0 of each byte are columns 0..4
// (MSB-first), bit=1 means "draw". Legibility is UNVERIFIED (no way to
// render/photograph the framebuffer in this environment) — treat as
// PENDING_HW alongside every other visual claim in this file.
// Unsupported characters (after toupper()) draw as a small box outline.
// ---------------------------------------------------------------------------

static bool font_glyph(char c, uint8_t rows[7])
{
    if (c >= 'a' && c <= 'z') {
        c = (char) (c - 'a' + 'A');
    }
    switch (c) {
    case ' ': { static const uint8_t g[7] = { 0, 0, 0, 0, 0, 0, 0 }; memcpy(rows, g, 7); return true; }
    case '!': { static const uint8_t g[7] = { 0x04,0x04,0x04,0x04,0x04,0,0x04 }; memcpy(rows, g, 7); return true; }
    case '\'': { static const uint8_t g[7] = { 0x04,0x04,0,0,0,0,0 }; memcpy(rows, g, 7); return true; }
    case ',': { static const uint8_t g[7] = { 0,0,0,0,0,0x04,0x08 }; memcpy(rows, g, 7); return true; }
    case '-': { static const uint8_t g[7] = { 0,0,0,0x1F,0,0,0 }; memcpy(rows, g, 7); return true; }
    case '.': { static const uint8_t g[7] = { 0,0,0,0,0,0,0x04 }; memcpy(rows, g, 7); return true; }
    case '/': { static const uint8_t g[7] = { 0x01,0x02,0x04,0x04,0x08,0x10,0x10 }; memcpy(rows, g, 7); return true; }
    case '0': { static const uint8_t g[7] = { 0x0E,0x11,0x13,0x15,0x19,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case '1': { static const uint8_t g[7] = { 0x04,0x0C,0x04,0x04,0x04,0x04,0x0E }; memcpy(rows, g, 7); return true; }
    case '2': { static const uint8_t g[7] = { 0x0E,0x11,0x01,0x02,0x04,0x08,0x1F }; memcpy(rows, g, 7); return true; }
    case '3': { static const uint8_t g[7] = { 0x0E,0x11,0x01,0x06,0x01,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case '4': { static const uint8_t g[7] = { 0x02,0x06,0x0A,0x12,0x1F,0x02,0x02 }; memcpy(rows, g, 7); return true; }
    case '5': { static const uint8_t g[7] = { 0x1F,0x10,0x10,0x1E,0x01,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case '6': { static const uint8_t g[7] = { 0x06,0x08,0x10,0x1E,0x11,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case '7': { static const uint8_t g[7] = { 0x1F,0x01,0x02,0x04,0x08,0x08,0x08 }; memcpy(rows, g, 7); return true; }
    case '8': { static const uint8_t g[7] = { 0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case '9': { static const uint8_t g[7] = { 0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C }; memcpy(rows, g, 7); return true; }
    case ':': { static const uint8_t g[7] = { 0,0x04,0,0,0,0x04,0 }; memcpy(rows, g, 7); return true; }
    case '?': { static const uint8_t g[7] = { 0x0E,0x11,0x01,0x02,0x04,0,0x04 }; memcpy(rows, g, 7); return true; }
    case '%': { static const uint8_t g[7] = { 0x11,0x02,0x04,0x04,0x04,0x08,0x11 }; memcpy(rows, g, 7); return true; }
    case 'A': { static const uint8_t g[7] = { 0x04,0x0A,0x11,0x11,0x1F,0x11,0x11 }; memcpy(rows, g, 7); return true; }
    case 'B': { static const uint8_t g[7] = { 0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E }; memcpy(rows, g, 7); return true; }
    case 'C': { static const uint8_t g[7] = { 0x0F,0x10,0x10,0x10,0x10,0x10,0x0F }; memcpy(rows, g, 7); return true; }
    case 'D': { static const uint8_t g[7] = { 0x1E,0x11,0x11,0x11,0x11,0x11,0x1E }; memcpy(rows, g, 7); return true; }
    case 'E': { static const uint8_t g[7] = { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F }; memcpy(rows, g, 7); return true; }
    case 'F': { static const uint8_t g[7] = { 0x1F,0x10,0x10,0x1E,0x10,0x10,0x10 }; memcpy(rows, g, 7); return true; }
    case 'G': { static const uint8_t g[7] = { 0x0F,0x10,0x10,0x17,0x11,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case 'H': { static const uint8_t g[7] = { 0x11,0x11,0x11,0x1F,0x11,0x11,0x11 }; memcpy(rows, g, 7); return true; }
    case 'I': { static const uint8_t g[7] = { 0x0E,0x04,0x04,0x04,0x04,0x04,0x0E }; memcpy(rows, g, 7); return true; }
    case 'J': { static const uint8_t g[7] = { 0x01,0x01,0x01,0x01,0x11,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case 'K': { static const uint8_t g[7] = { 0x11,0x12,0x14,0x18,0x14,0x12,0x11 }; memcpy(rows, g, 7); return true; }
    case 'L': { static const uint8_t g[7] = { 0x10,0x10,0x10,0x10,0x10,0x10,0x1F }; memcpy(rows, g, 7); return true; }
    case 'M': { static const uint8_t g[7] = { 0x11,0x1B,0x15,0x11,0x11,0x11,0x11 }; memcpy(rows, g, 7); return true; }
    case 'N': { static const uint8_t g[7] = { 0x11,0x19,0x15,0x13,0x11,0x11,0x11 }; memcpy(rows, g, 7); return true; }
    case 'O': { static const uint8_t g[7] = { 0x0E,0x11,0x11,0x11,0x11,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case 'P': { static const uint8_t g[7] = { 0x1E,0x11,0x11,0x1E,0x10,0x10,0x10 }; memcpy(rows, g, 7); return true; }
    case 'Q': { static const uint8_t g[7] = { 0x0E,0x11,0x11,0x11,0x15,0x12,0x0D }; memcpy(rows, g, 7); return true; }
    case 'R': { static const uint8_t g[7] = { 0x1E,0x11,0x11,0x1E,0x14,0x12,0x11 }; memcpy(rows, g, 7); return true; }
    case 'S': { static const uint8_t g[7] = { 0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E }; memcpy(rows, g, 7); return true; }
    case 'T': { static const uint8_t g[7] = { 0x1F,0x04,0x04,0x04,0x04,0x04,0x04 }; memcpy(rows, g, 7); return true; }
    case 'U': { static const uint8_t g[7] = { 0x11,0x11,0x11,0x11,0x11,0x11,0x0E }; memcpy(rows, g, 7); return true; }
    case 'V': { static const uint8_t g[7] = { 0x11,0x11,0x11,0x11,0x11,0x0A,0x04 }; memcpy(rows, g, 7); return true; }
    case 'W': { static const uint8_t g[7] = { 0x11,0x11,0x11,0x15,0x15,0x1B,0x11 }; memcpy(rows, g, 7); return true; }
    case 'X': { static const uint8_t g[7] = { 0x11,0x11,0x0A,0x04,0x0A,0x11,0x11 }; memcpy(rows, g, 7); return true; }
    case 'Y': { static const uint8_t g[7] = { 0x11,0x11,0x0A,0x04,0x04,0x04,0x04 }; memcpy(rows, g, 7); return true; }
    case 'Z': { static const uint8_t g[7] = { 0x1F,0x01,0x02,0x04,0x08,0x10,0x1F }; memcpy(rows, g, 7); return true; }
    default: { static const uint8_t g[7] = { 0x1F,0x11,0x11,0x11,0x11,0x11,0x1F }; memcpy(rows, g, 7); return false; }
    }
}

// ---------------------------------------------------------------------------
// Frame buffer helpers.
// ---------------------------------------------------------------------------

static void fb_clear(void) { memset(s_fb_new, 0xFF, sizeof(s_fb_new)); } // 1 = white

static void fb_set_pixel(int x, int y, bool black)
{
    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) {
        return;
    }
    int native_row = x;
    int native_byte = y / 8;
    int bit = 7 - (y % 8);
    if (black) {
        s_fb_new[native_row][native_byte] &= (uint8_t) ~(1u << bit);
    } else {
        s_fb_new[native_row][native_byte] |= (uint8_t) (1u << bit);
    }
}

static void draw_char(int x0, int y0, char c)
{
    uint8_t rows[7];
    font_glyph(c, rows);
    for (int r = 0; r < 7; r++) {
        for (int col = 0; col < 5; col++) {
            bool bit = (rows[r] >> (4 - col)) & 1;
            if (bit) {
                fb_set_pixel(x0 + col, y0 + r, true);
            }
        }
    }
}

// Draws left-to-right, clipping (not wrapping) at max_chars. Returns the
// number of characters actually drawn.
static int draw_text_n(int x0, int y0, const char *text, int max_chars)
{
    int n = 0;
    for (const char *p = text; *p && n < max_chars; p++, n++) {
        draw_char(x0 + n * 6, y0, *p);
    }
    return n;
}

static void draw_hline(int x0, int x1, int y)
{
    for (int x = x0; x <= x1 && x < SCREEN_W; x++) {
        fb_set_pixel(x, y, true);
    }
}

// ---------------------------------------------------------------------------
// SSD1680 low-level transport.
// ---------------------------------------------------------------------------

static bool disp_wait_busy(void)
{
    int64_t start = esp_timer_get_time();
    // BUSY high = busy (common SSD1680 breakout polarity) — UNVERIFIED
    // against this exact panel's datasheet, PENDING_HW.
    while (gpio_get_level(PAGER_PIN_DISP_BUSY) == 1) {
        if (esp_timer_get_time() - start > PAGER_UI_BUSY_TIMEOUT_US) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // NEVER a tight busy-loop
    }
    return true;
}

static void disp_send_cmd(uint8_t cmd)
{
    gpio_set_level(PAGER_PIN_DISP_DC, 0);
    spi_transaction_t t = { 0 };
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi, &t);
}

static void disp_send_data(const uint8_t *data, size_t len)
{
    if (len == 0) {
        return;
    }
    gpio_set_level(PAGER_PIN_DISP_DC, 1);
    spi_transaction_t t = { 0 };
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(s_spi, &t);
}

static void disp_send_data1(uint8_t b) { disp_send_data(&b, 1); }

static void disp_power_on(void)
{
    // Power effect: enables the panel's VCC rail (active-low P-MOSFET gate).
    gpio_set_level(PAGER_PIN_DISP_VCC_EN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void disp_power_off(void)
{
    // Power effect: the single largest display-side saving in this driver —
    // panel VCC is gated off between refreshes (PROTOCOL.md §8.4: "Display
    // (gated off via IO15) ~0 mA").
    gpio_set_level(PAGER_PIN_DISP_VCC_EN, 1);
}

static void disp_hw_reset(void)
{
    gpio_set_level(PAGER_PIN_DISP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PAGER_PIN_DISP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

static void disp_set_ram_window(uint16_t y_start, uint16_t y_end)
{
    // X range is always the full 16 bytes (128px) — only Y (the landscape
    // "row band" in our rotation) is ever windowed for a partial refresh.
    uint8_t xr[2] = { 0x00, 0x0F };
    disp_send_cmd(0x44);
    disp_send_data(xr, 2);

    uint8_t yr[4] = { (uint8_t) (y_start & 0xFF), (uint8_t) (y_start >> 8),
                       (uint8_t) (y_end & 0xFF), (uint8_t) (y_end >> 8) };
    disp_send_cmd(0x45);
    disp_send_data(yr, 4);

    disp_send_cmd(0x4E);
    disp_send_data1(0x00);
    uint8_t yc[2] = { (uint8_t) (y_start & 0xFF), (uint8_t) (y_start >> 8) };
    disp_send_cmd(0x4F);
    disp_send_data(yc, 2);
}

// Runs the SSD1680 init sequence per docs/PROTOCOL.md §6. Assumes VCC is
// already on and a hardware reset has just completed.
static bool disp_run_init_sequence(void)
{
    disp_send_cmd(0x12); // SW reset
    if (!disp_wait_busy()) {
        return false;
    }

    uint8_t mux[3] = { 0x27, 0x01, 0x00 };
    disp_send_cmd(0x01);
    disp_send_data(mux, 3);

    disp_send_cmd(0x11);
    disp_send_data1(0x03); // X increment, Y increment

    disp_set_ram_window(0, 295);

    disp_send_cmd(0x3C);
    disp_send_data1(0x05); // border follows LUT

    uint8_t duc1[2] = { 0x00, 0x80 };
    disp_send_cmd(0x21);
    disp_send_data(duc1, 2);

    disp_send_cmd(0x18);
    disp_send_data1(0x80); // internal temperature sensor

    disp_send_cmd(0x4E);
    disp_send_data1(0x00);
    uint8_t yc0[2] = { 0x00, 0x00 };
    disp_send_cmd(0x4F);
    disp_send_data(yc0, 2);

    return disp_wait_busy();
}

static void mark_display_dead(void)
{
    s_display_dead = true;
    if (!s_display_dead_logged) {
        ESP_LOGI(TAG, "display BUSY timeout after reset+re-init retry — marking dead for "
                      "this boot; device continues headless (network/replies/acks unaffected)");
        s_display_dead_logged = true;
    }
}

// ---------------------------------------------------------------------------
// Refresh primitives (PROTOCOL.md §6). Full: write to 0x24 AND 0x26 (syncs
// the shadow plane), border 0x05, update-mode byte 0xF7 (flagged in
// PROTOCOL.md as inferred/unverified — carried forward unchanged). Partial:
// only the changed row band, border 0x80 (HiZ), update-mode byte 0xFF (same
// unverified caveat), then re-mirror those rows into the shadow plane.
// ---------------------------------------------------------------------------

static void full_refresh(void)
{
    if (s_display_dead) {
        return;
    }
    disp_set_ram_window(0, 295);
    disp_send_cmd(0x24);
    for (int r = 0; r < FB_ROWS; r++) {
        disp_send_data(s_fb_new[r], FB_ROW_BYTES);
    }
    disp_send_cmd(0x26);
    for (int r = 0; r < FB_ROWS; r++) {
        disp_send_data(s_fb_new[r], FB_ROW_BYTES);
    }
    disp_send_cmd(0x3C);
    disp_send_data1(0x05);
    disp_send_cmd(0x22);
    disp_send_data1(0xF7); // PROTOCOL.md §6: inferred, not datasheet-verified
    disp_send_cmd(0x20);
    if (!disp_wait_busy()) {
        ESP_LOGI(TAG, "full refresh BUSY timeout; attempting one reset+re-init");
        disp_hw_reset();
        if (!disp_run_init_sequence() || !disp_wait_busy()) {
            mark_display_dead();
            return;
        }
    }
    memcpy(s_fb_old, s_fb_new, sizeof(s_fb_new));
    s_partial_count = 0;
}

static void partial_refresh(void)
{
    if (s_display_dead) {
        return;
    }
    int first = -1, last = -1;
    for (int r = 0; r < FB_ROWS; r++) {
        if (memcmp(s_fb_new[r], s_fb_old[r], FB_ROW_BYTES) != 0) {
            if (first < 0) {
                first = r;
            }
            last = r;
        }
    }
    if (first < 0) {
        return; // nothing changed, not worth a refresh or a cadence tick
    }

    disp_set_ram_window((uint16_t) first, (uint16_t) last);
    disp_send_cmd(0x24);
    for (int r = first; r <= last; r++) {
        disp_send_data(s_fb_new[r], FB_ROW_BYTES);
    }
    disp_send_cmd(0x3C);
    disp_send_data1(0x80); // HiZ border for partial, PROTOCOL.md §6
    disp_send_cmd(0x22);
    disp_send_data1(0xFF); // inferred, not datasheet-verified
    disp_send_cmd(0x20);
    if (!disp_wait_busy()) {
        ESP_LOGI(TAG, "partial refresh BUSY timeout; attempting one reset+re-init");
        disp_hw_reset();
        if (!disp_run_init_sequence() || !disp_wait_busy()) {
            mark_display_dead();
            return;
        }
        // Re-init invalidates the shadow plane's validity; force the next
        // caller onto a full refresh rather than risk desync.
        s_partial_count = PAGER_UI_PARTIAL_FULL_EVERY;
        return;
    }

    disp_set_ram_window((uint16_t) first, (uint16_t) last);
    disp_send_cmd(0x26);
    for (int r = first; r <= last; r++) {
        disp_send_data(s_fb_new[r], FB_ROW_BYTES);
    }
    for (int r = first; r <= last; r++) {
        memcpy(s_fb_old[r], s_fb_new[r], FB_ROW_BYTES);
    }
    s_partial_count++;
}

static void refresh_cadence(void)
{
    if (s_partial_count >= PAGER_UI_PARTIAL_FULL_EVERY) {
        full_refresh();
    } else {
        partial_refresh();
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------

static void draw_msg_line(int y, const msg_t *m, bool show_new)
{
    // Built by hand (not snprintf("%s: %s", who, m->body)) because m->body
    // can be up to 320 bytes and gcc's -Wformat-truncation correctly flags
    // that against any bounded line buffer; only the first line's worth of
    // pixels is ever drawn anyway (draw_text_n clips at max_chars).
    int max_chars = SCREEN_W / 6;
    const char *who = (m->dir == (uint8_t) MSG_DIR_UP) ? "you" : m->from;
    int n = draw_text_n(0, y, who, max_chars);
    n += draw_text_n(n * 6, y, ": ", max_chars - n);
    draw_text_n(n * 6, y, m->body, max_chars - n);
    if (show_new) {
        draw_text_n(SCREEN_W - 4 * 6, y, "NEW", 3);
    }
}

static void render_thread_frame(void)
{
    fb_clear();

    if (msg_history_lost()) {
        draw_text_n(0, 0, "earlier messages lost (restart)", SCREEN_W / 6);
        draw_hline(0, SCREEN_W - 1, 9);
    }

    size_t count = msg_thread_count();
    const msg_t *newest_unread = msg_newest_unread();

    if (count > 0) {
        const msg_t *m0 = msg_thread_at(0);
        bool show_new = (newest_unread != NULL && m0 == newest_unread);
        draw_msg_line(12, m0, show_new);
        draw_hline(0, SCREEN_W - 1, 21);
    } else {
        draw_text_n(0, 12, "no messages yet", SCREEN_W / 6);
    }

    for (size_t i = 1; i < count && i <= 2; i++) {
        draw_msg_line(24 + (int) (i - 1) * 12, msg_thread_at(i), false);
    }
    draw_hline(0, SCREEN_W - 1, 49);

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
    draw_text_n(0, SCREEN_H - 8, status, SCREEN_W / 6);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

static void disp_gpio_init(void)
{
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << PAGER_PIN_DISP_RST) | (1ULL << PAGER_PIN_DISP_DC) |
                        (1ULL << PAGER_PIN_DISP_VCC_EN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    gpio_config_t busy_cfg = {
        .pin_bit_mask = (1ULL << PAGER_PIN_DISP_BUSY),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&busy_cfg);

    gpio_set_level(PAGER_PIN_DISP_VCC_EN, 1); // start powered off
}

static bool disp_spi_init(void)
{
    spi_bus_config_t buscfg = {
        .mosi_io_num = PAGER_PIN_DISP_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = PAGER_PIN_DISP_SCK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = FB_ROW_BYTES,
    };
    esp_err_t err = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_DISABLED);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "spi_bus_initialize failed: %d", (int) err);
        return false;
    }

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 4 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PAGER_PIN_DISP_CS,
        .queue_size = 1,
    };
    err = spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "spi_bus_add_device failed: %d", (int) err);
        return false;
    }
    return true;
}

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
    disp_gpio_init();
    if (!s_spi_ready) {
        s_spi_ready = disp_spi_init();
    }
    i2c_kb_init();

    if (!s_spi_ready) {
        mark_display_dead();
        return false;
    }

    disp_power_on();
    disp_hw_reset();
    if (!disp_run_init_sequence()) {
        ESP_LOGI(TAG, "init sequence BUSY timeout; retrying once");
        disp_hw_reset();
        if (!disp_run_init_sequence()) {
            mark_display_dead();
            return false;
        }
    }

    s_partial_count = PAGER_UI_PARTIAL_FULL_EVERY; // force a full refresh on first render
    ESP_LOGI(TAG, "display init OK");
    return true;
}

void ui_shutdown(void)
{
    if (!s_display_dead && s_spi_ready) {
        disp_send_cmd(0x10);
        disp_send_data1(0x01); // deep sleep mode 1
    }
    disp_power_off();
}

void ui_render_thread(void)
{
    if (s_display_dead) {
        return;
    }
    render_thread_frame();
    refresh_cadence();
}

void ui_render_message_pane(void)
{
    if (s_display_dead) {
        return;
    }
    render_thread_frame();
    partial_refresh(); // forced partial, does not consume/reset the cadence counter's role
}

void ui_show_toast(const char *text)
{
    if (s_display_dead) {
        ESP_LOGI(TAG, "toast (display dead, log only): %s", text);
        return;
    }
    // Overlay just the status band (bottom 8 rows); next real render
    // overwrites it.
    for (int x = 0; x < SCREEN_W; x++) {
        for (int bit = 0; bit < 8; bit++) {
            int y = SCREEN_H - 8 + bit;
            fb_set_pixel(x, y, false);
        }
    }
    draw_text_n(0, SCREEN_H - 8, text, SCREEN_W / 6);
    partial_refresh();
}

void ui_composer_open(const char *reply_to_id)
{
    (void) reply_to_id;
    msg_composer_reset();
    s_composer_open = true;
    s_i2c_fail_count = 0;
    s_composer_last_activity_us = esp_timer_get_time();

    fb_clear();
    draw_text_n(0, 0, "reply:", SCREEN_W / 6);
    draw_hline(0, SCREEN_W - 1, 9);
    draw_text_n(0, SCREEN_H - 8, "enter/hold btn=send  tap btn=cancel", SCREEN_W / 6);
    refresh_cadence();
}

static void render_composer_text(void)
{
    fb_clear();
    draw_text_n(0, 0, "reply:", SCREEN_W / 6);
    draw_hline(0, SCREEN_W - 1, 9);

    const char *text = msg_composer_text();
    int chars_per_line = SCREEN_W / 6;
    int line = 0;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i += (size_t) chars_per_line, line++) {
        int n = (int) (len - i);
        if (n > chars_per_line) {
            n = chars_per_line;
        }
        char buf[64];
        int copy = n < (int) sizeof(buf) - 1 ? n : (int) sizeof(buf) - 1;
        memcpy(buf, text + i, (size_t) copy);
        buf[copy] = '\0';
        draw_text_n(0, 12 + line * 10, buf, chars_per_line);
    }

    char counter[16];
    snprintf(counter, sizeof(counter), "%u/160", (unsigned) msg_composer_len());
    draw_text_n(0, SCREEN_H - 8, counter, SCREEN_W / 6);
}

void ui_composer_close(bool sent)
{
    (void) sent;
    s_composer_open = false;
    s_i2c_fail_count = 0;
    if (s_display_dead) {
        return;
    }
    render_thread_frame();
    full_refresh(); // ui_init() and composer-close both force a full refresh
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
        refresh_cadence();
    } else if (byte == 0x08) {
        msg_composer_backspace();
        key.type = UI_KEYTYPE_BACKSPACE;
        render_composer_text();
        refresh_cadence();
    } else if (byte == 0x0D) {
        key.type = UI_KEYTYPE_ENTER;
    }
    // Everything else (>=0x80, other control bytes) is ignored per §9.4's
    // ASCII-only invariant.
    return key;
}

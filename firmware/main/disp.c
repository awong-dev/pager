/* disp.c — see disp.h. Moved out of ui.c by docs/DEVICE_TASKS.md F6.1: the
 * SSD1680 command sequence, timings and BUSY-timeout/retry logic below are
 * unchanged from the pre-split driver (docs/PROTOCOL.md §6).
 */
#include "disp.h"
#include "gfx.h"
#include "pins.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "disp";

#define PAGER_UI_PARTIAL_FULL_EVERY 20 // firmware/README.md, explicit override of "~10"
#define PAGER_UI_BUSY_TIMEOUT_US (15 * 1000000)

/* Shadow plane: what the panel was last told to show, for partial-refresh
 * diffing. gfx.c owns the "new" framebuffer this is diffed against. */
static uint8_t s_fb_old[GFX_FB_ROWS][GFX_FB_ROW_BYTES];

static spi_device_handle_t s_spi;
static bool s_spi_ready = false;
static bool s_display_dead = false; // logged once, then the device runs headless
static bool s_display_dead_logged = false;
static uint32_t s_partial_count = 0;

/* docs/DEVICE_PLAN.md §5.3: "one mutex in disp.c" — see disp.h's header
 * comment. Created in disp_init(); every public refresh entry point takes
 * it for its whole SPI transaction + shadow-plane update. */
static SemaphoreHandle_t s_mutex;

static void disp_lock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
    }
}

static void disp_unlock(void)
{
    if (s_mutex != NULL) {
        xSemaphoreGive(s_mutex);
    }
}

// ---------------------------------------------------------------------------
// SSD1680 low-level transport.
// ---------------------------------------------------------------------------

// Bug found on the bench (loose IO18 wire): BUSY can read low the whole
// time even though a real 0x20 update is in flight. Measured healthy
// refreshes: partial elapsed=454705 us (~455 ms), full elapsed=3426314 us
// (~3.43 s). Callers with a real update in flight pass one of these grace
// periods (with margin); disp_wait_busy() (no fallback) is for sites where
// BUSY genuinely never asserts on healthy hardware either (SW reset with no
// update queued, post-reinit checks) and must not eat 3.5 s on every call.
#define PAGER_UI_BUSY_FALLBACK_GRACE_US (20 * 1000)         // grace to see BUSY assert at all
#define PAGER_UI_BUSY_FALLBACK_PARTIAL_MS 700                // measured 455ms + margin
#define PAGER_UI_BUSY_FALLBACK_FULL_MS 3500                  // measured 3426ms + margin

static bool s_busy_fallback_logged = false;

// Weak default: no-op. disp.h's own comment explains the layering seam —
// disp.c must not include ui.h, so ui.c/modes.c overrides this with the
// strong definition that polls the CardKB during a long BUSY wait.
__attribute__((weak)) void disp_busy_idle_hook(void) {}

// fallback_ms == 0: no fallback — a real update is not in flight at this
// call site, so if BUSY never reads high we just fall straight through
// (matches pre-fix behaviour exactly). fallback_ms != 0: a real update
// (0x20) was just sent; if BUSY does not assert within the grace period,
// assume the line is disconnected and wait fallback_ms instead of trusting
// a false "already idle" read. Power effect: none in the healthy case
// (unchanged polling); in the fallback case the panel VCC stays on for
// fallback_ms, which is the same order of time a real refresh costs anyway
// — see disp_power_off() callers, unchanged by this function.
static bool disp_wait_busy_fb(uint32_t fallback_ms)
{
    int64_t start = esp_timer_get_time();
    int entry_level = gpio_get_level(PAGER_PIN_DISP_BUSY);
    // TEMPORARY hardware bring-up diagnostic: log the raw BUSY level and how
    // many poll iterations actually happened, so we can tell "genuinely
    // idle already" apart from "BUSY line not really connected" without a
    // multimeter/scope.
    int iters = 0;

    if (fallback_ms != 0) {
        bool asserted = false;
        while (esp_timer_get_time() - start <= PAGER_UI_BUSY_FALLBACK_GRACE_US) {
            if (gpio_get_level(PAGER_PIN_DISP_BUSY) == 1) {
                asserted = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(2)); // NEVER a tight busy-loop
        }
        if (!asserted) {
            if (!s_busy_fallback_logged) {
                ESP_LOGI(TAG, "BUSY line never asserted; using fixed waits (check the IO18 wire)");
                s_busy_fallback_logged = true;
            }
            // 10ms steps rather than one long vTaskDelay(fallback_ms), so
            // disp_busy_idle_hook() gets a chance to run each iteration —
            // see disp.h's own comment (this is the up-to-3.5s full-refresh
            // fallback wait, the one a lost keystroke is most likely to land
            // in).
            uint32_t waited_ms = 0;
            while (waited_ms < fallback_ms) {
                uint32_t step = (fallback_ms - waited_ms < 10) ? (fallback_ms - waited_ms) : 10;
                vTaskDelay(pdMS_TO_TICKS(step)); // NEVER a tight busy-loop
                waited_ms += step;
                disp_busy_idle_hook();
            }
            ESP_LOGD(TAG, "BUSY: entry=%d fixed-wait=%lu ms (fallback, no BUSY assert seen)",
                     entry_level, (unsigned long) fallback_ms);
            return true;
        }
    }

    // BUSY high = busy (common SSD1680 breakout polarity) — UNVERIFIED
    // against this exact panel's datasheet, PENDING_HW.
    while (gpio_get_level(PAGER_PIN_DISP_BUSY) == 1) {
        iters++;
        if (esp_timer_get_time() - start > PAGER_UI_BUSY_TIMEOUT_US) {
            ESP_LOGI(TAG, "BUSY: entry=%d timed out after %d iters (~%lld ms)", entry_level,
                     iters, (esp_timer_get_time() - start) / 1000);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10)); // NEVER a tight busy-loop
        disp_busy_idle_hook();
    }
    ESP_LOGI(TAG, "BUSY: entry=%d exit=%d iters=%d elapsed=%lld us", entry_level,
             gpio_get_level(PAGER_PIN_DISP_BUSY), iters, esp_timer_get_time() - start);
    return true;
}

// No real update in flight at these call sites on healthy hardware either
// (SW reset with nothing queued yet, or a post-reinit sanity check) — never
// apply the fixed-wait fallback here, or every retry path would eat 3.5 s.
static bool disp_wait_busy(void) { return disp_wait_busy_fb(0); }

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

static void full_refresh_locked(void)
{
    if (s_display_dead) {
        return;
    }
    disp_set_ram_window(0, 295);
    disp_send_cmd(0x24);
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        disp_send_data(gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }
    disp_send_cmd(0x26);
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        disp_send_data(gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }
    disp_send_cmd(0x3C);
    disp_send_data1(0x05);
    disp_send_cmd(0x22);
    disp_send_data1(0xF7); // PROTOCOL.md §6: inferred, not datasheet-verified
    disp_send_cmd(0x20);
    // Real update in flight (0x20, full) — use the fixed-wait fallback if
    // BUSY doesn't assert.
    if (!disp_wait_busy_fb(PAGER_UI_BUSY_FALLBACK_FULL_MS)) {
        ESP_LOGI(TAG, "full refresh BUSY timeout; attempting one reset+re-init");
        disp_hw_reset();
        if (!disp_run_init_sequence() || !disp_wait_busy()) {
            mark_display_dead();
            return;
        }
    }
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        memcpy(s_fb_old[r], gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }
    s_partial_count = 0;
}

static void partial_refresh_locked(void)
{
    if (s_display_dead) {
        return;
    }
    int first = -1, last = -1;
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        if (memcmp(gfx_fb_native_row(r), s_fb_old[r], GFX_FB_ROW_BYTES) != 0) {
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
        disp_send_data(gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }
    disp_send_cmd(0x3C);
    disp_send_data1(0x80); // HiZ border for partial, PROTOCOL.md §6
    disp_send_cmd(0x22);
    disp_send_data1(0xFF); // inferred, not datasheet-verified
    disp_send_cmd(0x20);
    // Real update in flight (0x20, partial) — use the fixed-wait fallback if
    // BUSY doesn't assert.
    if (!disp_wait_busy_fb(PAGER_UI_BUSY_FALLBACK_PARTIAL_MS)) {
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
        disp_send_data(gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }
    for (int r = first; r <= last; r++) {
        memcpy(s_fb_old[r], gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }
    s_partial_count++;
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
        .max_transfer_sz = GFX_FB_ROW_BYTES,
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

bool disp_init(void)
{
    if (s_mutex == NULL) {
        s_mutex = xSemaphoreCreateMutex();
    }
    disp_gpio_init();
    if (!s_spi_ready) {
        s_spi_ready = disp_spi_init();
    }

    if (!s_spi_ready) {
        mark_display_dead();
        return false;
    }

    // TEMPORARY hardware bring-up diagnostic.
    ESP_LOGI(TAG, "BUSY raw level before power-on: %d", gpio_get_level(PAGER_PIN_DISP_BUSY));

    // Power effect: VCC on for the duration of reset+init (a few tens of ms).
    disp_power_on();
    ESP_LOGI(TAG, "BUSY raw level after power-on (pre-reset): %d",
             gpio_get_level(PAGER_PIN_DISP_BUSY));
    disp_hw_reset();
    ESP_LOGI(TAG, "BUSY raw level right after hw_reset (pre-command): %d",
             gpio_get_level(PAGER_PIN_DISP_BUSY));
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

void disp_shutdown(void)
{
    disp_lock();
    if (!s_display_dead && s_spi_ready) {
        disp_send_cmd(0x10);
        disp_send_data1(0x01); // deep sleep mode 1
    }
    disp_power_off(); // power effect: panel VCC gated off, ~0 mA (PROTOCOL.md §8.4)
    disp_unlock();
}

bool disp_is_dead(void) { return s_display_dead; }

void disp_full_refresh(void)
{
    disp_lock();
    full_refresh_locked();
    disp_unlock();
}

void disp_partial_refresh(void)
{
    disp_lock();
    partial_refresh_locked();
    disp_unlock();
}

void disp_refresh_cadence(void)
{
    disp_lock();
    if (s_partial_count >= PAGER_UI_PARTIAL_FULL_EVERY) {
        full_refresh_locked();
    } else {
        partial_refresh_locked();
    }
    disp_unlock();
}

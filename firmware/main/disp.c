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

// Each partial refresh widens its changed-row window to a whole multiple of
// this many native rows, aligned to it.
//
// History, corrected 2026-09-22 — the two rationales this comment used to
// give were both wrong, and are recorded here so neither is re-derived:
//   (1) "every keystroke in the same on-screen character band refreshes the
//       exact same physical row range". False. One native row is one screen-x
//       column (gfx.c's gfx_set_pixel(): native_row = 295 - x), and the 12 px
//       font is PROPORTIONAL (glyph_adv() returns 5..8 px, with per-glyph
//       bearings), so consecutive keystrokes land in *adjacent*, not
//       identical, aligned chunks — exactly the adjacent-band case that
//       exposed the garbled-bands bug.
//   (2) "it makes the window match gfx.c's glyph cells, which are drawn on
//       byte-aligned native-row boundaries". False. gfx.c packs bits along y
//       inside each native row (native_byte = y / 8, bit = 7 - y % 8); a
//       native row is 16 bytes covering all 128 y. Byte boundaries therefore
//       lie on the y axis, which a native-row window does not cut at all.
//       Aligning native rows only rounds a screen-x range out to 8 px.
// The garbled bands were never caused by window alignment: the confirmed
// cause was the two-RAM-plane desync fixed in dd694a3 (see
// partial_refresh_locked()'s banner comment).
//
// Kept at 8 anyway, deliberately: it is the configuration verified on
// hardware in dd694a3, and it is nearly free. A single-glyph composer diff is
// 5..8 columns, so aligning costs at most ~11 extra gate lines (an 8- or
// 16-row window instead of 5..8). Shrinking it to 1 is the right move only
// once a bench run confirms unaligned small windows stay clean AND those few
// rows are shown to cost measurable BUSY time (`disptest step <n>` already
// produces an 8-row-band partial, and disp_wait_busy_fb() logs elapsed us).
#define PAGER_UI_PARTIAL_ROW_ALIGN 8

/* Shadow plane: what the panel was last told to show, for partial-refresh
 * diffing. gfx.c owns the "new" framebuffer this is diffed against. */
static uint8_t s_fb_old[GFX_FB_ROWS][GFX_FB_ROW_BYTES];

// Scratch: exactly the bytes handed to the panel (0x24) for the row range of
// the partial refresh currently in flight. partial_refresh_locked() re-syncs
// the SSD1680's "old" RAM plane (0x26) and s_fb_old from THIS buffer, never
// from a fresh gfx_fb_native_row() read taken after the BUSY wait — see that
// function's own comment for the desync this closes off.
static uint8_t s_fb_snap[GFX_FB_ROWS][GFX_FB_ROW_BYTES];

static spi_device_handle_t s_spi;
static bool s_spi_ready = false;
static bool s_display_dead = false; // logged once, then the device runs headless
static bool s_display_dead_logged = false;
static uint32_t s_partial_count = 0;

// Handoff task D2: partial_refresh_locked()'s own BUSY-timeout recovery used
// to set s_partial_count = PAGER_UI_PARTIAL_FULL_EVERY to force the next
// refresh full, but ui.c's render path calls disp_partial_refresh()
// directly (never disp_refresh_cadence()), so that counter bump was silently
// never read. This flag is what disp_partial_refresh() actually honours;
// set here (that recovery) and by disp_init()'s own priming below, cleared
// once consumed. disp_refresh_cadence() is unchanged — it still decides full
// vs. partial from s_partial_count alone, exactly as before.
static bool s_force_full = false;

// Bench A/B for the garbled-bands fix (see partial_refresh_locked()'s own
// comment on the second 0x24 write this flag guards). Default true = fixed
// behaviour; 0 deliberately reproduces the pre-fix bug (confirmed on the
// bench: with again=0, adjacent-band partials come out garbled and never
// settle; with again=1, 13 consecutive adjacent-band partials all came out
// correct and real typing in the composer stayed clean) — kept here, not
// removed once verified, so a future session can re-confirm the fix on the
// bench in about 40 seconds via `disptest again 0` / `disptest seq`. Power
// effect: none by itself — it only decides whether the extra RAM write
// below runs, which is documented at that write.
static bool s_partial_write_again = true;

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

// Weak default: no-op. Same layering seam as disp_busy_idle_hook() above,
// for the same reason (disp.c must not include net.h either) — ui.c
// overrides this with the strong definition that blocks on net.c's
// publish-quiet gate. See full_refresh_locked()/partial_refresh_locked()'s
// own call sites for the field failure this closes.
__attribute__((weak)) void disp_pre_write_gate_hook(void) {}

// fallback_ms == 0: no fallback — a real update is not in flight at this
// call site, so if BUSY never reads high we just fall straight through
// (matches pre-fix behaviour exactly). fallback_ms != 0: a real update
// (0x20) was just sent; if BUSY does not assert within the grace period,
// assume the line is disconnected and wait fallback_ms instead of trusting
// a false "already idle" read. Power effect: none in the healthy case
// (unchanged polling); in the fallback case the panel VCC stays on for
// fallback_ms, which is the same order of time a real refresh costs anyway
// — panel VCC is gated off between refreshes regardless, unchanged by
// this function.
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

// Register half of the SSD1680 init sequence — everything after the SW
// reset + its BUSY wait: MUX, data-entry mode, RAM window, border, display
// update control, temperature source, RAM address counters. Factored out of
// disp_run_init_sequence() so the per-refresh reset below (23 Sep field
// failure fix, see full_refresh_locked()/partial_refresh_locked()) can send
// exactly the same registers without a hardware reset in between.
static void disp_send_init_registers(void)
{
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
}

// Runs the SSD1680 init sequence per docs/PROTOCOL.md §6. Assumes VCC is
// already on and a hardware reset has just completed.
static bool disp_run_init_sequence(void)
{
    disp_send_cmd(0x12); // SW reset
    if (!disp_wait_busy()) {
        return false;
    }
    disp_send_init_registers();
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

// 23 Sep field failure, hardware-verified: mid-way through a full-refresh RAM
// write, the SSD1680 lost its register configuration (the first ~165 native
// rows landed, the rest were dropped) and every refresh after that came out
// garbled, because the controller was now running on power-on register
// defaults — this panel uses 128 of the SSD1680's 176 sources, so the
// default RAM X window skews every row. Only disp_init()'s init sequence
// restored it; a plain reboot fixed the glass, because disp_init() re-runs
// disp_run_init_sequence() and nothing else in this file ever did, again,
// after boot. The reference driver for this exact panel (GxEPD2_290_T94)
// re-runs its whole _InitDisplay() before every refresh (_Init_Full()/
// _Init_Part()); disp_pre_refresh_reset() below is that, called at the top
// of both full_refresh_locked() and partial_refresh_locked(), before any RAM
// window/write.
//
// 0x12 (software reset) resets the SSD1680's registers only, not its RAM —
// datasheet and the reference driver both treat it this way — so the
// two-RAM-plane state partial_refresh_locked() depends on (the panel's own
// "previous image" plane, and s_fb_old which tracks it) survives this call
// untouched.
//
// Deliberately no hardware reset here: disp_hw_reset() (RST pin toggle)
// stays reserved for disp_init() and the two BUSY-timeout recovery branches
// below, which already do a real hardware reset when a software reset alone
// isn't trusted to have worked.
//
// Cost, paid on every refresh: one SW-reset BUSY wait (bench-measured ~10ms:
// "BUSY: entry=1 exit=0 iters=1 elapsed=9648 us") plus the seven short
// register-write commands in disp_send_init_registers().
static bool disp_pre_refresh_reset(const char *who)
{
    if (disp_run_init_sequence()) {
        return true;
    }
    ESP_LOGI(TAG, "%s: pre-refresh re-init BUSY timeout; attempting one reset+re-init", who);
    disp_hw_reset();
    if (!disp_run_init_sequence()) {
        mark_display_dead();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Refresh primitives (PROTOCOL.md §6). Full: border 0x05, update-mode byte
// 0xF7. Partial: only the changed row band, border 0x80 (HiZ), update-mode
// byte 0xFF. PROTOCOL.md used to flag both bytes as inferred/unverified;
// they are now reference-matched: GxEPD2_290_T94.cpp (this panel, waveform
// from OTP) uses 0xF7 full and 0xFC partial, and ours are those values plus
// bits 1+0 (disable analog, disable clock), which this driver wants because
// it powers the panel down between refreshes. Hardware-verified in this
// configuration; do NOT "fix" them to the bare reference values.
//
// Two rules, stated once here rather than repeated at each call site
// (bench-found: task-disp-fix-2.md; the second rule's scope was narrowed
// after hardware testing, see below):
//   1. Every RAM write (0x24 or 0x26) is preceded by its own
//      disp_set_ram_window() call. disp_set_ram_window() sets the address
//      counters (0x4E/0x4F) as well as the window (0x44/0x45); after a RAM
//      write the counters have run to the end of whatever window was set,
//      so a second write with no window/cursor reset in between starts from
//      wherever the first one left off, not from the start of its own
//      window. full_refresh_locked() used to violate this between its 0x24
//      and 0x26 writes; fixed unconditionally below, costs nothing.
//   2. Every PARTIAL update (0x20 with the partial LUT) is followed by
//      writing the exact image just displayed to BOTH RAM planes (0x26 then
//      0x24, matching GxEPD2's writeImageAgain()): the SSD1680 has two
//      physical RAM planes and auto-toggles which one 0x24/0x26 address on
//      every update (Waveshare's own example, verbatim: "once the display
//      is refreshed, the memory area will be auto-toggled... you have to
//      set the frame memory and refresh the display twice"). Skipping this
//      leaves the two planes unequal, and the next differential update
//      reads a plane that never held the displayed image — this is the
//      confirmed cause of the garbled-bands bug; partial_refresh_locked()
//      does this, guarded by s_partial_write_again (see its comment).
//      A full (mode-1) update does NOT read the previous-image plane at
//      all, so it needs no post-update re-sync of its own — an earlier
//      version of this file added one anyway (guarded by a since-removed
//      flag) on the theory that it might explain a bench discrepancy; that
//      theory did not hold up (the discrepancy was a test-harness gap, not
//      a real defect — task-disp-fix-2.md), so full_refresh_locked() below
//      does rule 1 only. This is the configuration verified on hardware:
//      13 consecutive adjacent-band partials after a full refresh, all
//      correct, plus clean real typing in the composer.
// ---------------------------------------------------------------------------

static void full_refresh_locked(void)
{
    if (s_display_dead) {
        return;
    }

    // 00:40 field failure: a full refresh's SPI write started while a
    // pager-originated MQTT publish's LTE uplink was in flight (a reconnect
    // + the `/up` ack publish) and the controller lost its registers again.
    // Block here, before ANY panel command below (including the register
    // re-arm), until net.c's publish-quiet gate says it's clear — see
    // disp_pre_write_gate_hook()'s own comment.
    disp_pre_write_gate_hook();

    // Re-arm the SSD1680's registers before touching RAM — see
    // disp_pre_refresh_reset()'s own banner comment (23 Sep field failure).
    if (!disp_pre_refresh_reset("full refresh")) {
        return;
    }

    // Snapshot before anything below can block on BUSY — same reasoning as
    // partial_refresh_locked()'s own banner comment (never read
    // gfx_fb_native_row() fresh after the wait; a full refresh's BUSY wait
    // is ~1.4s, the same desync exposure the partial path already closed).
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        memcpy(s_fb_snap[r], gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }

    disp_set_ram_window(0, 295);
    disp_send_cmd(0x24);
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        disp_send_data(s_fb_snap[r], GFX_FB_ROW_BYTES);
    }
    disp_set_ram_window(0, 295); // rule 1 above: reset before every RAM write
    disp_send_cmd(0x26);
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        disp_send_data(s_fb_snap[r], GFX_FB_ROW_BYTES);
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

    // No post-update RAM re-sync here — see rule 2 above: a full (mode-1)
    // update never reads the previous-image plane, so there is nothing to
    // desync. (An earlier version of this function did one anyway,
    // task-disp-fix-2.md; removed once hardware testing showed the bench
    // discrepancy that prompted it was a test-harness gap, not a real
    // defect.)
    ESP_LOGI(TAG, "full: %d rows", GFX_FB_ROWS);

    // Commit from the snapshot, not a fresh gfx_fb_native_row() read —
    // this is what was actually sent (see the snapshot comment above), so
    // the shadow-plane commit is truthful even if gfx.c's framebuffer
    // changed while this call was blocked in the BUSY wait above.
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        memcpy(s_fb_old[r], s_fb_snap[r], GFX_FB_ROW_BYTES);
    }
    s_partial_count = 0;
}

// Bug found on the bench: while typing, consecutive partials touching
// adjacent, non-overlapping native-row bands left the panel showing content
// that never matched gfx.c's framebuffer again ("garbled bands that never
// settle"). Root cause: the old-RAM (0x26) and s_fb_old re-sync below used
// gfx_fb_native_row(r) read AFTER the ~0.5s BUSY wait, not the data actually
// handed to the panel in the 0x24 write before it. disp_busy_idle_hook()'s
// current strong definition (ui.c) only queues key events and never draws
// (see its own comment), so it does not itself race this — but nothing in
// disp.c enforced that invariant, and any future or other call path that
// draws into gfx_fb from a different task while this task blocks in the
// wait (disp_lock() only serializes disp.c's own entry points, not gfx.c's
// framebuffer) would silently desync the "old" plane and the shadow plane
// from what the panel actually received: on the next partial, the memcmp
// diff would then be computed against a too-new s_fb_old, so the row range
// that was never actually drawn is never detected as changed again, and the
// SSD1680's differential LUT sees an old-RAM value that was never the truly
// displayed image either. Fix: snapshot the exact bytes sent to 0x24 into
// s_fb_snap before the wait, and use only that snapshot — never a fresh
// framebuffer read — for the 0x26 write and the s_fb_old commit after it.
// Window/pointer (disp_set_ram_window(), shared with full_refresh_locked())
// are reset immediately before each of the two RAM writes.
static void partial_refresh_locked(void)
{
    if (s_display_dead) {
        return;
    }

    // The diff comes FIRST. ui_render() runs on every modes_run() iteration
    // while the UI is awake, so this function is a no-op ~8 times a second
    // for 30 s after every keystroke; the gate and the register re-arm
    // below must only run when there is something to write. (23 Sep,
    // phaseY-keycrash2.log: with the re-arm ahead of the diff, an idle
    // awake pager did a SW reset + two BUSY waits every 120 ms.)
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

    // 00:40 field failure — see full_refresh_locked()'s identical call and
    // its own comment.
    disp_pre_write_gate_hook();

    // Re-arm the SSD1680's registers before touching RAM — see
    // disp_pre_refresh_reset()'s own banner comment (23 Sep field failure).
    // Safe for the two-RAM-plane state this function depends on: 0x12 (SW
    // reset) resets registers only, never RAM.
    if (!disp_pre_refresh_reset("partial refresh")) {
        return;
    }

    // Widen/align to whole PAGER_UI_PARTIAL_ROW_ALIGN-row chunks (Task 3:
    // uniform, cheaper composer refresh — see the #define's comment). Safe:
    // the padding rows are re-sent with their own unchanged, correct
    // content, so this is still a no-op visually for anything outside the
    // real diff.
    int pre_first = first, pre_last = last;
    first -= first % PAGER_UI_PARTIAL_ROW_ALIGN;
    last += (PAGER_UI_PARTIAL_ROW_ALIGN - 1) - (last % PAGER_UI_PARTIAL_ROW_ALIGN);
    if (last >= GFX_FB_ROWS) {
        last = GFX_FB_ROWS - 1;
    }

    // Bench correlation line: lets the bench match "the band the owner saw
    // go wrong" (a screen-x range) against "the rows we actually sent" (a
    // native-row range), and shows the alignment widening (pre_first/
    // pre_last vs. first/last) in the same place. Screen x = 295 -
    // native_row (gfx.c's gfx_set_pixel() mirror), so the higher native row
    // (last) is the lower screen x (x0) and vice versa.
    int x0 = (GFX_FB_ROWS - 1) - last;
    int x1 = (GFX_FB_ROWS - 1) - first;
    ESP_LOGI(TAG, "partial: pre-align rows %d..%d, native rows %d..%d (%d rows) = screen x %d..%d, again=%d",
             pre_first, pre_last, first, last, last - first + 1, x0, x1, (int) s_partial_write_again);

    // Snapshot now, before anything below can block — see this function's
    // banner comment.
    for (int r = first; r <= last; r++) {
        memcpy(s_fb_snap[r], gfx_fb_native_row(r), GFX_FB_ROW_BYTES);
    }

    disp_set_ram_window((uint16_t) first, (uint16_t) last);
    disp_send_cmd(0x24);
    for (int r = first; r <= last; r++) {
        disp_send_data(s_fb_snap[r], GFX_FB_ROW_BYTES);
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
        // s_partial_count covers disp_refresh_cadence() callers;
        // s_force_full covers disp_partial_refresh()'s direct callers, which
        // never read s_partial_count (see s_force_full's own comment).
        s_partial_count = PAGER_UI_PARTIAL_FULL_EVERY;
        s_force_full = true;
        return;
    }

    // Re-sync the "old" RAM plane and the host shadow to EXACTLY what we
    // just sent (s_fb_snap, not a fresh gfx_fb_native_row() read) — window
    // and pointer explicitly reset again first.
    disp_set_ram_window((uint16_t) first, (uint16_t) last);
    disp_send_cmd(0x26);
    for (int r = first; r <= last; r++) {
        disp_send_data(s_fb_snap[r], GFX_FB_ROW_BYTES);
    }
    for (int r = first; r <= last; r++) {
        memcpy(s_fb_old[r], s_fb_snap[r], GFX_FB_ROW_BYTES);
    }

    // Bug found on the bench (garbled/alternating bands per keystroke that
    // never settle, a different band each time). INFERRED mechanism (the FIX
    // is hardware-verified, this explanation of it is not): the SSD1680 has
    // two physical RAM planes, and an update appears to auto-toggle which one
    // 0x24/0x26 address, so after a 0x20 the "previous image" plane is not
    // the one a bare 0x26 write lands in. The known-good
    // reference driver for this exact panel, GxEPD2_290_T94_V2 (also
    // GxEPD2_290_T94.cpp)'s writeImageAgain(), re-writes BOTH planes after
    // every update, not just the "previous image" one (0x26) above:
    //   _writeImage(0x26, bitmap, ...); // set previous
    //   _writeImage(0x24, bitmap, ...); // set current
    // its header comment, verbatim: "for differential update: set current
    // and previous buffers equal (for fast partial update to work
    // correctly)". Waveshare's own example agrees, verbatim: "once the
    // display is refreshed, the memory area will be auto-toggled... you
    // have to set the frame memory and refresh the display twice." Skipping
    // this second write (pre-fix behaviour here) leaves the two planes
    // unequal; the next partial on an adjacent, non-overlapping band then
    // differentially updates against a plane that was never the displayed
    // image, and since s_fb_old was already committed above, the host never
    // re-diffs those rows — the band stays wrong until the next full
    // refresh. Order matters and matches the reference: 0x26 first (above),
    // then 0x24 (here). s_partial_write_again exists ONLY so the `disptest`
    // console command (main.c) can flip it off and A/B both behaviours on
    // one flash; ship builds must never disable it. Power effect: doubles
    // the data phase of every partial's SPI transaction (one more
    // (last-first+1)*GFX_FB_ROW_BYTES-byte write) — negligible next to the
    // panel's own ~0.3-0.8s update time this is guarding the correctness of.
    if (s_partial_write_again) {
        disp_set_ram_window((uint16_t) first, (uint16_t) last);
        disp_send_cmd(0x24);
        for (int r = first; r <= last; r++) {
            disp_send_data(s_fb_snap[r], GFX_FB_ROW_BYTES);
        }
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

    // docs/RCA_SLEEP_URC.md: CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND makes
    // IDF isolate every GPIO (driver off, floating) for the whole of each
    // light sleep. A floating RST is a controller reset waiting to happen
    // (the 23 Sep register-loss failures), a floating VCC_EN gate drops the
    // 3v3_en rail that also feeds the CardKB, and a floating CS/DC turns
    // noise into commands. Keep these four pads driven through sleep, same
    // as net.cpp does for the modem's RTS. The SPI clock/data pads are
    // peripheral-muxed and idle between transfers; not held.
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_RST);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_DC);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_CS);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_VCC_EN);

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
    ESP_LOGD(TAG, "BUSY raw level before power-on: %d", gpio_get_level(PAGER_PIN_DISP_BUSY));

    // Power effect: VCC on for the duration of reset+init (a few tens of ms).
    disp_power_on();
    ESP_LOGD(TAG, "BUSY raw level after power-on (pre-reset): %d",
             gpio_get_level(PAGER_PIN_DISP_BUSY));
    disp_hw_reset();
    ESP_LOGD(TAG, "BUSY raw level right after hw_reset (pre-command): %d",
             gpio_get_level(PAGER_PIN_DISP_BUSY));
    if (!disp_run_init_sequence()) {
        ESP_LOGI(TAG, "init sequence BUSY timeout; retrying once");
        disp_hw_reset();
        if (!disp_run_init_sequence()) {
            mark_display_dead();
            return false;
        }
    }

    s_partial_count = PAGER_UI_PARTIAL_FULL_EVERY; // force a full refresh via disp_refresh_cadence()
    s_force_full = true; // ...and via disp_partial_refresh(), in case the first render call goes there directly
    ESP_LOGI(TAG, "display init OK");
    return true;
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
    if (s_force_full) {
        // Honour a pending forced-full request (disp_init()'s priming, or
        // partial_refresh_locked()'s own BUSY-timeout recovery) — see
        // s_force_full's own comment. Consume it here: this is the only
        // place a caller that bypasses disp_refresh_cadence() can be made to
        // see it.
        s_force_full = false;
        full_refresh_locked();
    } else {
        partial_refresh_locked();
    }
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

// Fault injector for the bench (`disptest swreset`, main.c): sends 0x12 (SW
// reset) alone, waits for BUSY, and does nothing else — deliberately leaves
// the SSD1680 exactly where the 23 Sep field failure left it, registers on
// power-on defaults, with no re-init to follow. Confirms disp_pre_refresh_
// reset() above actually recovers from that state: acceptance sequence is
// `disptest bars` (clean) -> `disptest swreset` -> `disptest bars` again
// (must still be clean, since full_refresh_locked() now re-arms the
// registers itself before writing RAM). Power effect: none beyond the SW
// reset's own BUSY wait (~10ms, bench-measured) — does not touch panel VCC.
void disp_fault_inject_swreset(void)
{
    disp_lock();
    if (!s_spi_ready) {
        disp_unlock();
        return;
    }
    disp_send_cmd(0x12); // SW reset -- deliberately nothing else follows
    disp_wait_busy();
    disp_unlock();
}

// Bench A/B entry points for the garbled-bands fix above (partial_refresh_
// locked()'s second-0x24-write comment). Power effect: none of these three
// touch the panel themselves — they only read/flip state or (dirty_rows)
// diff two in-memory buffers under disp_lock().
void disp_set_partial_write_again(bool on) { s_partial_write_again = on; }

bool disp_partial_write_again(void) { return s_partial_write_again; }

int disp_dirty_rows(void)
{
    disp_lock();
    int count = 0;
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        if (memcmp(gfx_fb_native_row(r), s_fb_old[r], GFX_FB_ROW_BYTES) != 0) {
            count++;
        }
    }
    disp_unlock();
    return count;
}

uint32_t disp_partial_count(void) { return s_partial_count; }

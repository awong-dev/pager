/* disp.c — see disp.h. Moved out of ui.c by docs/DEVICE_TASKS.md F6.1: the
 * SSD1680 command sequence, timings and BUSY-timeout/retry logic below are
 * unchanged from the pre-split driver (docs/PROTOCOL.md §6).
 */
#include "disp.h"
#include "gfx.h"
#include "pins.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "flightrec.h" // round 9: 'D' (BUSY timeout)/'L' (power-loss-to-render latency)
                        // events, so the diagnosis survives a wake where the USB port
                        // never comes back -- no-op stubs outside a debug build

static const char *TAG = "disp";

#define PAGER_UI_PARTIAL_FULL_EVERY 20 // firmware/README.md, explicit override of "~10"

// S12 (docs/SLEEP_URC_DESIGN.md §8.3, docs/SLEEP_URC_TASKS.md S12): this is
// the bound for the "BUSY line genuinely asserted, then never deasserted"
// case (disp_wait_busy_fb()'s polling loop below, reached only after BUSY was
// actually seen high within PAGER_UI_BUSY_FALLBACK_GRACE_US of the call --
// the "BUSY not wired at all" case uses the separate, unchanged
// PAGER_UI_BUSY_FALLBACK_*_MS constants instead and never reaches this
// timeout). Was 15s; phaseAF's `input+ui+render max 31551 ms` is exactly
// 1500 (the pre-write gate's own budget) + 15009 (this timeout, once) +
// ~15042 (the one retry's own wait at this same bound) -- 18s of that is this
// constant, twice. Measured refresh times already in this file (full
// 3426314 us, partial 454705 us, PAGER_UI_BUSY_FALLBACK_FULL_MS/_PARTIAL_MS
// above) justify 6s: >70% margin over the slowest measured refresh, and it
// caps the worst case (gate + timeout + one retry's timeout) at
// 1.5 + 6 + 6 = 13.5s instead of 31.5s. Power: 18s x 40mA / 3600 = ~0.2mAh
// less awake ESP time per occurrence of this specific stall (assumption:
// 40mA awake, docs/SLEEP_URC_DESIGN.md §2, not measured on this board). Does
// not change any panel command, refresh cadence, or mark_display_dead()'s
// own retry logic -- only how long disp_wait_busy_fb() will wait for a BUSY
// line it has already seen asserted before giving up.
#define PAGER_UI_BUSY_TIMEOUT_US (6 * 1000000)

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

// Bench instrumentation: so a bench window can attribute unexplained full
// refreshes (the 3.4s panel event) to a cause instead of just counting them.
// s_cnt_full counts every completed full refresh, however it was reached;
// s_cnt_forced_full is a subset of s_cnt_full, counting only the ones that
// were originally requested as a partial and upgraded (s_force_full, or the
// PAGER_UI_PARTIAL_FULL_EVERY cadence in disp_refresh_cadence());
// s_cnt_partial counts only partials that actually reached the panel (never
// a no-op diff, never one that aborted into a BUSY-timeout recovery).
// Free-running; reset (with the ring below) by disp_reset_refresh_stats(),
// which modes.c's modes_debug_sleeptest_start() calls so each sleeptest
// window's report reflects only that window. Power effect: none of this is
// new panel traffic -- pure bookkeeping alongside refreshes that already
// happen.
static uint32_t s_cnt_full = 0;
static uint32_t s_cnt_partial = 0;
static uint32_t s_cnt_forced_full = 0;

// Ring of the last DISP_REFRESH_RING_LEN refresh completions, oldest first
// once full. kind: 'F' full (as directly requested), 'P' partial, 'U' a
// partial request upgraded to a full. tag: which public entry point started
// the call chain (1=disp_full_refresh, 2=disp_partial_refresh,
// 3=disp_refresh_cadence, 0=other), OR 4 if this particular full refresh
// only completed via its own BUSY-timeout reset+re-init retry (see
// full_refresh_locked()'s "recovered" local) -- that overrides the caller's
// own tag, since "needed a mid-refresh recovery" is the more useful
// attribution for an unexplained full refresh than which function called it.
#define DISP_REFRESH_RING_LEN 8
typedef struct {
    uint32_t ms;
    char kind;
    uint8_t tag;
} disp_refresh_event_t;
static disp_refresh_event_t s_refresh_ring[DISP_REFRESH_RING_LEN];
static uint8_t s_refresh_ring_head = 0;  // next slot to write
static uint8_t s_refresh_ring_count = 0; // valid entries, saturates at DISP_REFRESH_RING_LEN

// Round 9: "wake to render done" latency the owner asked to have logged --
// armed by disp_note_power_loss() (t0 = right after rail_on() returns, as
// close to "the moment of wake" as disp.c itself ever sees), consumed by
// the first refresh completion afterwards, below.
static volatile bool s_power_loss_render_pending = false;
static int64_t s_power_loss_t0_us = 0;

static void disp_refresh_ring_push(char kind, uint8_t tag)
{
    s_refresh_ring[s_refresh_ring_head].ms = (uint32_t) (esp_timer_get_time() / 1000);
    s_refresh_ring[s_refresh_ring_head].kind = kind;
    s_refresh_ring[s_refresh_ring_head].tag = tag;
    s_refresh_ring_head = (uint8_t) ((s_refresh_ring_head + 1) % DISP_REFRESH_RING_LEN);
    if (s_refresh_ring_count < DISP_REFRESH_RING_LEN) {
        s_refresh_ring_count++;
    }
    if (s_power_loss_render_pending) {
        s_power_loss_render_pending = false;
        int64_t elapsed_ms = (esp_timer_get_time() - s_power_loss_t0_us) / 1000;
        ESP_LOGI(TAG, "disp: power-loss-to-render done in %lld ms", elapsed_ms);
        flightrec_event('L', (int32_t) elapsed_ms, 0);
    }
}

// S12 (docs/SLEEP_URC_DESIGN.md §8.3, docs/SLEEP_URC_TASKS.md S12): a real
// BUSY-genuinely-asserted timeout (disp_wait_busy_fb()'s polling loop, never
// the "BUSY not wired" fallback path), so "the panel wedged" is a number in
// the sleeptest report instead of something inferred from a bucket max.
// Free-running, never reset; incremented on disp.c's own task, read by
// modes.c's report.
static uint32_t s_busy_timeout_count = 0;

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

// Round 9: which call site is waiting on BUSY right now, purely for the
// timeout log line and the matching flightrec 'D' event -- set immediately
// before each disp_wait_busy()/disp_wait_busy_fb() call that owner round 9
// task 1 asked to be distinguishable ("logs which BUSY wait timed out, the
// call site"). Not a stack (this driver's own module comment: "one mutex in
// disp.c", every public entry point serialized, so at most one BUSY wait is
// ever outstanding at a time) -- a single static is enough.
static const char *s_busy_tag = "?";
static void disp_set_busy_tag(const char *tag) { s_busy_tag = tag; }

// Round 10: per-call BUSY timeout, PAGER_UI_BUSY_TIMEOUT_US except inside
// disp_wait_busy_max() (the short post-reset and power-loss-restore waits).
static int64_t s_busy_timeout_us = PAGER_UI_BUSY_TIMEOUT_US;

// Round 10: CONFIG_FREERTOS_HZ=100, so pdMS_TO_TICKS(10) and (15) are both 1
// tick, and vTaskDelay(1) blocks anywhere from 0 to 10 ms depending on where
// in the tick the caller is. After a real light-sleep wake the task resumes
// at a random tick phase (railcycle always resumes on a tick boundary, which
// is why it never reproduced), so the "10 ms" RST pulse could be ~0 ms. This
// rounds up and adds one tick so the delay is never shorter than ms.
static void disp_delay_at_least_ms(uint32_t ms)
{
    vTaskDelay((ms + portTICK_PERIOD_MS - 1) / portTICK_PERIOD_MS + 1);
}

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
        if (esp_timer_get_time() - start > s_busy_timeout_us) {
            int64_t elapsed_ms = (esp_timer_get_time() - start) / 1000;
            ESP_LOGI(TAG, "BUSY: %s entry=%d timed out after %d iters (~%lld ms)", s_busy_tag,
                     entry_level, iters, elapsed_ms);
            // Round 9: survives a wake where the USB port never comes back
            // (flightrec.h's own module comment) -- 'D' kind, a=entry_level,
            // b=elapsed_ms; the tag string itself does not fit an int32
            // field, so the dump's a/b plus the surrounding cycle number is
            // what identifies which wake this was, same as every other
            // flightrec event.
            flightrec_event('D', entry_level, (int32_t) elapsed_ms);
            s_busy_timeout_count++; // S12: see disp_busy_timeout_count()'s doc comment
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

// Round 10: same as disp_wait_busy() with a shorter timeout (a timeout still
// logs, flightrec 'D' with b ~= max_us/1000, and counts in
// disp_busy_timeout_count()).
static bool disp_wait_busy_max(int64_t max_us)
{
    int64_t saved = s_busy_timeout_us;
    s_busy_timeout_us = max_us;
    bool ok = disp_wait_busy_fb(0);
    s_busy_timeout_us = saved;
    return ok;
}

// Round 10: normal SW-reset BUSY is ~10 ms (bench: elapsed=9648 us); 200 ms is
// ~20x that, and short enough that one failed attempt plus a retry stays
// under 0.5 s.
#define PAGER_UI_BUSY_RESET_TIMEOUT_US (200 * 1000)

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

// Round 10: owner sequence -- RST low >= 10 ms (really >= 10 ms now, see
// disp_delay_at_least_ms()), then wait for BUSY low before any command (the
// old code sent 0x12 after a nominal, tick-quantized 10 ms without checking
// BUSY). A BUSY that stays high past 200 ms here is left to the caller's own
// SW-reset wait and retry, same as before.
static void disp_hw_reset(void)
{
    gpio_set_level(PAGER_PIN_DISP_RST, 0);
    disp_delay_at_least_ms(10);
    gpio_set_level(PAGER_PIN_DISP_RST, 1);
    disp_delay_at_least_ms(1);
    const char *saved_tag = s_busy_tag;
    disp_set_busy_tag("post-hw-reset");
    (void) disp_wait_busy_max(PAGER_UI_BUSY_RESET_TIMEOUT_US);
    disp_set_busy_tag(saved_tag);
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
    disp_set_busy_tag(who); // round 9: names any BUSY timeout below with its call site
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

// tag: caller attribution for the refresh-stats ring (see s_refresh_ring's
// own comment) — 1=disp_full_refresh, 2=disp_partial_refresh,
// 3=disp_refresh_cadence, 0=other. upgraded: true if this full refresh was
// originally requested as a partial and upgraded (s_force_full or the
// cadence threshold) — counted in s_cnt_forced_full in addition to
// s_cnt_full, and ring-logged as kind 'U' instead of 'F'.
static void full_refresh_locked(uint8_t tag, bool upgraded)
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
    disp_set_busy_tag("full refresh update");
    bool recovered = false;
    if (!disp_wait_busy_fb(PAGER_UI_BUSY_FALLBACK_FULL_MS)) {
        ESP_LOGI(TAG, "full refresh BUSY timeout; attempting one reset+re-init");
        disp_hw_reset();
        if (!disp_run_init_sequence() || !disp_wait_busy()) {
            mark_display_dead();
            return;
        }
        recovered = true;
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
    // Any completed full refresh satisfies a pending forced full, whether it
    // was set by disp_init() (first-render priming, above) or by the
    // BUSY-timeout recovery in partial_refresh_locked() (shadow-plane
    // re-init) — without this, the next partial after boot or after a
    // timeout recovery was upgraded to a second full for no reason.
    s_force_full = false;

    // Bench instrumentation (see s_cnt_full/s_refresh_ring's own comments):
    // effective_tag overrides the caller's tag to 4 when this exact
    // completion needed the reset+re-init retry above, since that is the
    // more useful attribution for an unexplained full refresh.
    uint8_t effective_tag = recovered ? 4 : tag;
    s_cnt_full++;
    if (upgraded) {
        s_cnt_forced_full++;
    }
    disp_refresh_ring_push(upgraded ? 'U' : 'F', effective_tag);
    ESP_LOGI(TAG, "disp: full refresh (tag=%u)", (unsigned) effective_tag);
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
// tag: caller attribution for the refresh-stats ring, forwarded unchanged to
// disp_refresh_ring_push() when (and only when) this call actually issues a
// partial — see s_refresh_ring's own comment for the tag values.
static void partial_refresh_locked(uint8_t tag)
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
    disp_set_busy_tag("partial refresh update");
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

    // Bench instrumentation (see s_cnt_partial/s_refresh_ring's own
    // comments): only reached once a partial has actually been issued to the
    // panel, never on a no-op diff or a BUSY-timeout abort above.
    s_cnt_partial++;
    disp_refresh_ring_push('P', tag);
}

// Round 9 bench diagnostic: one-shot ~200ms tight poll of BUSY right after
// rail-on, logging every transition with esp_timer_get_time() timestamps --
// answers "how long does BUSY take to reach a stable level after rail-on"
// (owner's round 9 measurement request). Armed via disp_debug_arm_busy_
// trace() (main.c's `dispbusytrace` console command), auto-disarms after
// one run so it never costs anything in normal operation.
static volatile bool s_busy_trace_armed = false;

void disp_debug_arm_busy_trace(void) { s_busy_trace_armed = true; }

static void disp_log_busy_settle_once(void)
{
    s_busy_trace_armed = false;
    int64_t t0 = esp_timer_get_time();
    int last = gpio_get_level(PAGER_PIN_DISP_BUSY);
    ESP_LOGI(TAG, "disp: busytrace: BUSY=%d at +0 us (right after rail-on)", last);
    while (esp_timer_get_time() - t0 < 200000) {
        int lvl = gpio_get_level(PAGER_PIN_DISP_BUSY);
        if (lvl != last) {
            ESP_LOGI(TAG, "disp: busytrace: BUSY %d->%d at +%lld us", last, lvl,
                     esp_timer_get_time() - t0);
            last = lvl;
        }
        vTaskDelay(pdMS_TO_TICKS(1)); // NEVER a tight busy-loop; 1ms resolution is plenty here
    }
    ESP_LOGI(TAG, "disp: busytrace: BUSY=%d at +200000 us (end of trace)", last);
}

// UI-first / partial-after-power-loss fix (25 Sep, "10s from tap to
// password:"): disp_note_power_loss() used to force the next refresh to a
// full one (~2-4s) purely because the SSD1680's own SRAM does not survive
// the 3V3 rail's off->on cycle (rail.c) -- the physical glass itself needs
// no power to hold its image (e-paper is bistable), only the controller's
// RAM forgot what was in it. s_fb_old (above) is a plain static array, not
// RTC memory, and this whole power-loss path only ever runs across a LIGHT
// sleep (this build never deep-sleeps) -- ordinary RAM is retained through
// light sleep, so s_fb_old is exactly the frame the glass is still showing,
// untouched by the rail cycle. Silently reloading both SSD1680 RAM planes
// from it (no 0x20 Master Activation below -- RAM writes alone never move
// the glass) puts the controller back in the state it was in right before
// power was cut, so the very next partial_refresh_locked() call diffs the
// real new frame against a plane that is actually correct and only redraws
// the rows that changed, instead of every caller being forced full.
//
// Both planes, not just the "previous image" one (0x26): partial_refresh_
// locked()'s own banner comment already explains why the two planes must
// stay equal outside whatever row band a partial last touched (the auto-
// toggle-on-update behaviour, GxEPD2/Waveshare-documented) -- after a power
// loss BOTH planes are equally garbage, and restoring only 0x26 would leave
// 0x24 out of sync for every row outside the next partial's own diff band,
// which is exactly the desync this file's dd694a3 fix already had to solve
// once for a different cause (garbled, never-settling bands). Writing the
// same content to both closes that off the same way full_refresh_locked()
// already does for an ordinary full refresh.
//
// Returns false (and leaves the caller to fall back to the old force-full
// behaviour) if the pre-refresh re-init itself fails BUSY -- mark_display_
// dead() has already run inside disp_pre_refresh_reset() in that case.
static bool restore_ram_planes_locked(void)
{
    // Round 9 root cause ("6s from tap to password: on a real sleep wake",
    // disp_busy_timeout_count=1 in the round 8 sleeptest report, wrongly
    // written off as the loose-BUSY-wire bench issue): the comment this
    // replaces called a proactive disp_hw_reset() here a "latency
    // optimisation, not a correctness requirement", reasoning that a bare
    // SW reset (0x12) over SPI would work and disp_pre_refresh_reset()'s own
    // BUSY-timeout fallback would catch it otherwise. On the bench that
    // fallback IS what fired -- every real off-sleep wake tested (round 9)
    // hit the 6s PAGER_UI_BUSY_TIMEOUT_US on the FIRST disp_run_init_
    // sequence() attempt inside disp_pre_refresh_reset() ("power-loss
    // restore" tag) and only recovered via that function's own reset+
    // re-init retry -- i.e. exactly the 6s the owner measured, every time.
    // SSD1680 datasheet power-on sequence: VCI up, wait >=10ms, RST low
    // >=10ms, wait BUSY low -- a bare SW reset with the rail freshly
    // restored skips the mandatory RST pulse entirely, and apparently that
    // is load-bearing on this hardware after a real light-sleep power
    // cycle (never reproduced by `railcycle`, which never actually
    // light-sleeps -- ESP-IDF's own sleep GPIO isolation, main suspect,
    // never engages there either). Owner ruling, round 9: this hardware
    // reset is now unconditional here, not a fallback -- the >=10ms-after-
    // VCI-up leg of the same sequence is rail.c's own universal settle
    // (rail_on(), already elapsed by the time this function is ever
    // called, since disp_note_power_loss() only runs after rail_on()
    // returns).
    if (s_busy_trace_armed) {
        disp_log_busy_settle_once(); // round 9 bench diagnostic, see its own comment
    }
    // Round 10 fallback: short BUSY waits and up to 3 hw-reset attempts, so
    // a panel that wedges after rail-on costs ~0.2 s per attempt instead of
    // the 6 s PAGER_UI_BUSY_TIMEOUT_US. Only if all 3 fail does it fall back
    // to the old path (6 s wait, then mark_display_dead()).
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        disp_hw_reset();
        disp_set_busy_tag("power-loss restore");
        disp_send_cmd(0x12); // SW reset
        ok = disp_wait_busy_max(PAGER_UI_BUSY_RESET_TIMEOUT_US);
        if (ok) {
            disp_send_init_registers();
            ok = disp_wait_busy_max(PAGER_UI_BUSY_RESET_TIMEOUT_US);
        }
        if (!ok) {
            ESP_LOGI(TAG, "power-loss restore: attempt %d BUSY timeout, hw reset + retry",
                     attempt + 1);
        }
    }
    if (!ok) {
        disp_hw_reset();
        if (!disp_pre_refresh_reset("power-loss restore")) {
            return false;
        }
    }

    disp_set_ram_window(0, 295);
    disp_send_cmd(0x24);
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        disp_send_data(s_fb_old[r], GFX_FB_ROW_BYTES);
    }
    disp_set_ram_window(0, 295); // rule 1, full_refresh_locked()'s own comment
    disp_send_cmd(0x26);
    for (int r = 0; r < GFX_FB_ROWS; r++) {
        disp_send_data(s_fb_old[r], GFX_FB_ROW_BYTES);
    }
    return true;
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
    // Round 9 root cause, part 2 (the mandatory disp_hw_reset() alone did
    // NOT eliminate the 6s BUSY timeout on real hardware -- bench-measured
    // 2 timeouts in 5 real off-sleep wakes even with it): this pin was the
    // one left out of the four above, so it was NOT excluded from ESP-IDF's
    // sleep GPIO isolation -- an INPUT pad gets isolated (disconnected from
    // its normal input path) for the whole of every light sleep same as any
    // other unexcluded GPIO, per the same CONFIG_ESP_SLEEP_GPIO_RESET_
    // WORKAROUND mechanism this function's own comment above already
    // describes for RST/DC/CS/VCC_EN. disp_wait_busy_fb()'s very first read
    // on wake (entry_level) can then be a stale/isolated level rather than
    // the panel's real BUSY state, which a hardware reset alone cannot fix
    // since the reset itself is gated behind that same unreliable read.
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_DISP_BUSY);

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
    disp_set_busy_tag("boot init");
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

// Rail gate: see disp.h's own doc comment, and restore_ram_planes_locked()'s
// own banner comment just above for the "why a partial is safe here at all"
// argument. Deliberately does NOT touch s_partial_count -- the every-20th-
// partial full-refresh cadence (disp_refresh_cadence()) keeps running
// exactly as it would have if the rail had never dropped; forcing it here
// would just be a second, unrelated reason to go full, which the owner
// asked to keep separate. Falls back to the old force-full behaviour only if
// the restore itself fails (disp_pre_refresh_reset()'s BUSY timeout --
// mark_display_dead() already logged it in that case, or the panel was
// already dead before this call), so a failed restore can never leave the
// shadow plane silently wrong. Taken under the same lock every other public
// entry point here uses. Power effect: one SW-reset BUSY wait (~10ms,
// disp_pre_refresh_reset()'s own measurement) plus one whole-panel RAM write
// over SPI (no Master Activation, so no BUSY wait of its own) -- a fraction
// of a full refresh's ~2-4s, and no glass movement at all.
void disp_note_power_loss(void)
{
    disp_lock();
    if (s_display_dead) {
        disp_unlock();
        return;
    }
    // Round 9: t0 for the "wake to render done" latency the owner asked to
    // be logged/flightrec'd -- this function is called right after
    // rail_on() returns (ui.c's ui_ensure_powered()), so it is as close to
    // "the moment of wake" as disp.c itself ever sees; the matching log
    // line is at disp_refresh_ring_push() below, the one choke point both
    // full_refresh_locked() and partial_refresh_locked() complete through.
    s_power_loss_render_pending = true;
    s_power_loss_t0_us = esp_timer_get_time();
    if (restore_ram_planes_locked()) {
        ESP_LOGI(TAG, "disp: RAM planes restored after rail power loss - next refresh is a partial");
    } else {
        ESP_LOGI(TAG, "disp: RAM restore failed after rail power loss - forcing a full refresh");
        s_partial_count = PAGER_UI_PARTIAL_FULL_EVERY;
        s_force_full = true;
    }
    disp_unlock();
}

void disp_full_refresh(void)
{
    disp_lock();
    full_refresh_locked(1, false); // tag 1: refresh-stats attribution (disp_get_refresh_stats())
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
        full_refresh_locked(2, true); // tag 2, upgraded: refresh-stats attribution
    } else {
        partial_refresh_locked(2); // tag 2: refresh-stats attribution
    }
    disp_unlock();
}

void disp_refresh_cadence(void)
{
    disp_lock();
    if (s_partial_count >= PAGER_UI_PARTIAL_FULL_EVERY) {
        full_refresh_locked(3, true); // tag 3, upgraded: refresh-stats attribution
    } else {
        partial_refresh_locked(3); // tag 3: refresh-stats attribution
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

// S12: see the doc comment on s_busy_timeout_count above.
uint32_t disp_busy_timeout_count(void) { return s_busy_timeout_count; }

// Bench instrumentation: see s_cnt_full/s_cnt_partial/s_cnt_forced_full's own
// comment. Any of the three output pointers may be NULL.
void disp_get_refresh_stats(uint32_t *full, uint32_t *partial, uint32_t *upgraded)
{
    if (full != NULL) {
        *full = s_cnt_full;
    }
    if (partial != NULL) {
        *partial = s_cnt_partial;
    }
    if (upgraded != NULL) {
        *upgraded = s_cnt_forced_full;
    }
}

// Bench instrumentation: formats the refresh ring oldest-first, e.g.
// "F@12345/1 P@12800/2 U@13100/3". Returns the number of characters written
// (excluding the NUL), same convention as snprintf(); buf is always
// NUL-terminated if cap > 0. Empty ring writes an empty string.
int disp_refresh_ring_format(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t n = 0;
    uint8_t start = (s_refresh_ring_count < DISP_REFRESH_RING_LEN) ? 0 : s_refresh_ring_head;
    for (uint8_t i = 0; i < s_refresh_ring_count; i++) {
        uint8_t idx = (uint8_t) ((start + i) % DISP_REFRESH_RING_LEN);
        int w = snprintf(buf + n, cap - n, "%s%c@%u/%u", (i == 0) ? "" : " ", s_refresh_ring[idx].kind,
                          (unsigned) s_refresh_ring[idx].ms, (unsigned) s_refresh_ring[idx].tag);
        if (w < 0) {
            break;
        }
        if ((size_t) w >= cap - n) {
            n = cap - 1; // snprintf already truncated and NUL-terminated at cap
            break;
        }
        n += (size_t) w;
    }
    return (int) n;
}

// Bench instrumentation reset: modes.c's modes_debug_sleeptest_start() calls
// this so each sleeptest window's report reflects only refreshes from that
// window, not everything accumulated since boot. Power effect: none — clears
// counters and the ring only, touches no panel state.
void disp_reset_refresh_stats(void)
{
    s_cnt_full = 0;
    s_cnt_partial = 0;
    s_cnt_forced_full = 0;
    memset(s_refresh_ring, 0, sizeof(s_refresh_ring));
    s_refresh_ring_head = 0;
    s_refresh_ring_count = 0;
}

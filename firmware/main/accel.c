// accel.c — see accel.h for the module comment (bus ownership, why this is
// the path most likely to run first, what accel_poll() does and does not
// touch).
//
// Register map / values below are the standard LIS3DH datasheet layout
// (STMicroelectronics AN/DS, publicly documented, not vendor-specific to
// this board). Every threshold is UNVERIFIED — this hardware has never
// produced a real INT1 edge while writing this — and is the first thing to
// retune once a chip is actually on the bus (look for "LIS3DH found" in the
// boot log, then use the accelerometer's own INT1_SRC/threshold registers
// via a debug build if retuning is needed; no debug command exists for that
// yet, it was not asked for by this task).

#include "accel.h"

#include <stddef.h>

// ---------------------------------------------------------------------------
// Pure function (no ESP-IDF dependency) -- host-tested by
// firmware/host/test_accel_rate.c. See accel.h's own doc comment for the
// contract; the split sms.c/loc.c use.
// ---------------------------------------------------------------------------

bool accel_edge_wanted(int64_t now_us, int64_t last_reported_us, int64_t refractory_us)
{
    if (last_reported_us == 0) {
        return true; // no edge reported yet this power cycle
    }
    int64_t elapsed_us = now_us - last_reported_us;
    if (elapsed_us < 0) {
        return true; // backwards clock: fail open rather than wedge forever
    }
    return elapsed_us >= refractory_us;
}

// See accel.h's doc comment for the evaluation order (a)-(e).
accel_shake_verdict_t accel_shake_step(accel_shake_t *s, const accel_shake_cfg_t *c, int64_t now_us, bool ia2)
{
    // (a) never-wedge rule.
    if (s->cooldown_until_us - now_us > (int64_t) c->cooldown_ms * 1000) {
        s->cooldown_until_us = 0;
    }
    if (s->holdoff_until_us - now_us > (int64_t) c->holdoff_ms * 1000) {
        s->holdoff_until_us = 0;
    }
    if (s->n && now_us < s->last_us) {
        s->n = 0;
        return ACCEL_SHAKE_IDLE;
    }
    // (b) gap broken: the chain is a rejected candidate.
    if (s->n > 0 && now_us - s->last_us > (int64_t) c->gap_ms * 1000) {
        s->out_n = s->n;
        s->out_span_ms = (uint32_t) ((s->last_us - s->first_us) / 1000);
        s->n = 0;
        s->holdoff_until_us = now_us + (int64_t) c->holdoff_ms * 1000;
        return ACCEL_SHAKE_REJECTED;
    }
    // (c) cooldown / holdoff: ia2 ignored.
    if (now_us < s->cooldown_until_us || now_us < s->holdoff_until_us) {
        return ACCEL_SHAKE_IDLE;
    }
    // (d) extend or start the chain.
    if (ia2) {
        if (s->n == 0) {
            s->first_us = now_us;
        }
        s->n++;
        s->last_us = now_us;
        if (s->n >= c->n_min && now_us - s->first_us >= (int64_t) c->span_ms * 1000) {
            s->out_n = s->n;
            s->out_span_ms = (uint32_t) ((now_us - s->first_us) / 1000);
            s->n = 0;
            s->cooldown_until_us = now_us + (int64_t) c->cooldown_ms * 1000;
            return ACCEL_SHAKE_FIRED;
        }
    }
    // (e)
    return s->n > 0 ? ACCEL_SHAKE_PENDING : ACCEL_SHAKE_IDLE;
}

#ifdef ESP_PLATFORM

#include <string.h>

#include "loc.h"
#include "net.h"
#include "pins.h"

#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "accel";

#define LIS3DH_REG_WHO_AM_I 0x0F
#define LIS3DH_WHO_AM_I_VALUE 0x33

#define LIS3DH_REG_CTRL_REG1 0x20
#define LIS3DH_REG_CTRL_REG2 0x21
#define LIS3DH_REG_CTRL_REG3 0x22
#define LIS3DH_REG_CTRL_REG4 0x23
#define LIS3DH_REG_CTRL_REG5 0x24
#define LIS3DH_REG_INT1_CFG 0x30
#define LIS3DH_REG_INT1_SRC 0x31
#define LIS3DH_REG_INT1_THS 0x32
#define LIS3DH_REG_INT1_DURATION 0x33
#define LIS3DH_REG_INT2_CFG 0x34
#define LIS3DH_REG_INT2_SRC 0x35
#define LIS3DH_REG_INT2_THS 0x36
#define LIS3DH_REG_INT2_DURATION 0x37

// CTRL_REG1: ODR=0011 (25 Hz), LPen=1 (low-power mode), Zen=Yen=Xen=1.
#define LIS3DH_CTRL_REG1_25HZ_LOWPOWER 0x3F
// CTRL_REG2: HPIS1=1 and HPIS2=1 (high-pass filter routed to both interrupt
// generators), normal mode (HPM=00), lowest cutoff (FDS=0, HPCF=00).
#define LIS3DH_CTRL_REG2_HPF_INT12 0x03
// CTRL_REG3 bits: route generator 1 / generator 2 to the INT1 pin.
#define LIS3DH_CTRL_REG3_I1_IA1 0x40
#define LIS3DH_CTRL_REG3_I1_IA2 0x20
// CTRL_REG4: FS=01 (+-4 g). A hard shake clips at +-2 g (SHAKE_WAKE_DESIGN D2).
#define LIS3DH_CTRL_REG4_4G 0x10
// CTRL_REG5: LIR_INT1 + LIR_INT2 (latch each generator until its SRC is read),
// so a brief jolt is not missed between two polls.
#define LIS3DH_CTRL_REG5_LATCH_INT12 0x0A
// INT1_CFG / INT2_CFG: OR combination (AOI=0, 6D=0) of XHIE/YHIE/ZHIE (any
// axis high events only; low events are true at rest, SHAKE_WAKE_DESIGN D3).
#define LIS3DH_INT1_CFG_ANY_HIGH 0x2A
#define LIS3DH_INT2_CFG_ANY_HIGH 0x2A
// INT1_THS: 1 LSB = 32 mg at +-4 g. 0x08 = 256 mg, "walking/being carried".
#define LIS3DH_INT1_THS_DEFAULT 0x08
// INT2_THS: 0x24 * 32 mg = 1152 mg, above taps and walking, below a shake.
#define LIS3DH_INT2_THS_DEFAULT 0x24
// DURATION 0 = no hardware debounce (firmware classifies, SHAKE_WAKE_DESIGN D3/D5).
#define LIS3DH_INT1_DURATION_DEFAULT 0x00
#define LIS3DH_INT2_DURATION_DEFAULT 0x00

// INTx_SRC bit 6 (IA): "one or more interrupts have been generated".
#define LIS3DH_INT1_SRC_IA 0x40
#define LIS3DH_INT2_SRC_IA 0x40

static bool s_present = false;

// A1 (docs/DEVICE_NEXT_TASKS.md): refractory-gate state. s_refractory_us
// starts at the ACCEL_REFRACTORY_S default; `acceltest refr <seconds>` (A2)
// is the only way to change it at runtime. s_last_reported_us / s_edges_reported
// are accel_poll()'s bookkeeping for accel_edge_wanted() and the `acceltest`
// "edges reported" line respectively. s_refr_until_us is the end of the
// window during which CTRL_REG3 I1_IA1 is cleared at the sensor.
static int64_t s_refractory_us = (int64_t) ACCEL_REFRACTORY_S * 1000000;
static int64_t s_last_reported_us = 0;
static uint32_t s_edges_reported = 0;
static int64_t s_refr_until_us = 0;

// Shake-to-wake state (SHAKE_WAKE_DESIGN D5/D6) and the cached CTRL_REG3 value.
static accel_shake_t s_shake;
static accel_shake_cfg_t s_shake_cfg = {
    .n_min = ACCEL_SHAKE_N,
    .gap_ms = ACCEL_SHAKE_GAP_MS,
    .span_ms = ACCEL_SHAKE_SPAN_MS,
    .cooldown_ms = ACCEL_SHAKE_COOLDOWN_MS,
    .holdoff_ms = ACCEL_SHAKE_HOLDOFF_MS,
};
static bool s_shake_enabled = true;
static uint32_t s_shake_candidates = 0;
static uint32_t s_shake_rejected = 0;
static uint32_t s_shake_fired = 0;
static uint8_t s_ctrl3 = 0;

// This module's own bus: I2C_NUM_1 on PAGER_PIN_ACCEL_SDA/SCL, separate from
// ui.c's I2C_NUM_0 (CardKB). Installed once, here, at accel_init() -- never
// torn down or re-initialized, since (unlike the CardKB bus) neither the
// LIS3DH nor this bus's pins are on the gated 3V3 rail: the breakout is
// powered straight from the battery via its own regulator, so there is no
// power edge that would ever require a re-init.
//
// Internal pull-ups left ON, not because this bus needs them -- the
// breakout has its own pull-ups to its own regulated rail -- but as a
// harmless backup: the ESP32's internal ~45k pull-up in parallel with the
// breakout's external one only slightly strengthens an already-valid bus
// (same rail voltage), it does not fight it, and it costs nothing since the
// bus is not sleep-gated. Kept consistent with ui.c's i2c_kb_init(), which
// makes the same choice for the same reason on the CardKB bus.
static void accel_bus_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = PAGER_PIN_ACCEL_SDA,
        .scl_io_num = PAGER_PIN_ACCEL_SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100000,
    };
    i2c_param_config(I2C_NUM_1, &conf);
    i2c_driver_install(I2C_NUM_1, conf.mode, 0, 0, 0);
    // Excluded from ESP-IDF's sleep GPIO isolation, the same call rail.c
    // uses for the display bus/RTS/3V3_EN pads: without this, IDF's default
    // light-sleep isolation (CONFIG_ESP_SLEEP_GPIO_RESET_WORKAROUND) forces
    // the pad to input/no-pull for the sleep's duration, which would drop
    // this driver's own internal pull-up backup for that whole window (the
    // breakout's external pull-up would still hold the bus, so this is
    // belt-and-suspenders, not a correctness fix -- unlike the CardKB bus,
    // nothing here is ever unpowered, so there is no phantom-power path to
    // close). Power effect: none -- this bus is never depowered by rail.c
    // and has no rail edge of its own to hold state across.
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_ACCEL_SDA);
    gpio_sleep_sel_dis((gpio_num_t) PAGER_PIN_ACCEL_SCL);
}

static bool reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_NUM_1, PAGER_I2C_ADDR_LIS3DH, buf, sizeof(buf),
                                      pdMS_TO_TICKS(50)) == ESP_OK;
}

static esp_err_t s_last_err = ESP_OK; // 5 Oct 2026: last I2C read result, for acceltest's triage

static bool reg_read(uint8_t reg, uint8_t *out)
{
    s_last_err = i2c_master_write_read_device(I2C_NUM_1, PAGER_I2C_ADDR_LIS3DH, &reg, 1, out, 1,
                                              pdMS_TO_TICKS(50));
    return s_last_err == ESP_OK;
}

esp_err_t accel_debug_last_err(void) { return s_last_err; }

// A2: multi-byte read with the LIS3DH's auto-increment bit (0x80) set on
// the register address, e.g. OUT_X_L..OUT_Z_H in one transaction.
static bool reg_read_multi(uint8_t reg, uint8_t *out, size_t n)
{
    uint8_t addr = reg | 0x80;
    return i2c_master_write_read_device(I2C_NUM_1, PAGER_I2C_ADDR_LIS3DH, &addr, 1, out, n,
                                        pdMS_TO_TICKS(50)) == ESP_OK;
}

// Shared by accel_init() (boot probe) and accel_debug_status() (A2's
// re-probe-without-a-reboot path): writes the fixed configuration and, on
// success, marks the chip present and arms the ext1 wake source. Returns
// false (s_present left false) if any write fails partway through.
static bool configure_and_arm(uint8_t who)
{
    bool ok = true;
    const uint8_t ctrl3 = LIS3DH_CTRL_REG3_I1_IA1 | LIS3DH_CTRL_REG3_I1_IA2;
    ok &= reg_write(LIS3DH_REG_CTRL_REG1, LIS3DH_CTRL_REG1_25HZ_LOWPOWER);
    ok &= reg_write(LIS3DH_REG_CTRL_REG4, LIS3DH_CTRL_REG4_4G);
    ok &= reg_write(LIS3DH_REG_CTRL_REG2, LIS3DH_CTRL_REG2_HPF_INT12);
    ok &= reg_write(LIS3DH_REG_INT1_THS, LIS3DH_INT1_THS_DEFAULT);
    ok &= reg_write(LIS3DH_REG_INT1_DURATION, LIS3DH_INT1_DURATION_DEFAULT);
    ok &= reg_write(LIS3DH_REG_INT1_CFG, LIS3DH_INT1_CFG_ANY_HIGH);
    ok &= reg_write(LIS3DH_REG_INT2_THS, LIS3DH_INT2_THS_DEFAULT);
    ok &= reg_write(LIS3DH_REG_INT2_DURATION, LIS3DH_INT2_DURATION_DEFAULT);
    ok &= reg_write(LIS3DH_REG_INT2_CFG, LIS3DH_INT2_CFG_ANY_HIGH);
    ok &= reg_write(LIS3DH_REG_CTRL_REG5, LIS3DH_CTRL_REG5_LATCH_INT12);
    ok &= reg_write(LIS3DH_REG_CTRL_REG3, ctrl3);
    if (!ok) {
        ESP_LOGI(TAG, "LIS3DH found (WHO_AM_I=0x%02x) but configuration failed partway through - "
                      "motion trigger disabled, running without it",
                 (unsigned) who);
        s_present = false;
        return false;
    }

    // Drop any stale latch left from before the configuration.
    uint8_t stale = 0;
    reg_read(LIS3DH_REG_INT1_SRC, &stale);
    reg_read(LIS3DH_REG_INT2_SRC, &stale);
    s_ctrl3 = ctrl3;

    s_present = true;
    net_enable_accel_wake(); // IO6 becomes a light-sleep wake source, net.cpp's net_sleep()
    ESP_LOGI(TAG, "LIS3DH found (WHO_AM_I=0x%02x), 25Hz LP +-4g, INT1 motion THS=256 mg, "
                  "INT2 shake THS=1152 mg, CTRL_REG3=0x60",
             (unsigned) who);
    return true;
}

bool accel_init(void)
{
    accel_bus_init(); // installs I2C_NUM_1 once; see accel_bus_init()'s own comment
    uint8_t who = 0;
    if (!reg_read(LIS3DH_REG_WHO_AM_I, &who) || who != LIS3DH_WHO_AM_I_VALUE) {
        // Expected outcome on the owner's bench unit (V02_DESIGN.md §5: "it
        // may not be wired yet"). Logged once at INFO, never retried — this
        // is not a paging-path dependency, so there is nothing to recover
        // into and nothing worth a retry loop for.
        ESP_LOGI(TAG,
                 "LIS3DH not found at 0x%02x (WHO_AM_I read 0x%02x, i2c %s) - motion trigger disabled; "
                 "this is the expected/likely case if the accelerometer is not wired yet",
                 (unsigned) PAGER_I2C_ADDR_LIS3DH, (unsigned) who, esp_err_to_name(s_last_err));
        s_present = false;
        return false;
    }
    return configure_and_arm(who);
}

bool accel_poll(void)
{
    if (!s_present) {
        return false;
    }
    uint8_t src = 0;
    if (!reg_read(LIS3DH_REG_INT1_SRC, &src)) {
        return false; // transient I2C failure; try again next cycle, same tolerance ui.c's CardKB read uses
    }
    int64_t now_us = esp_timer_get_time();
    if (src & LIS3DH_INT1_SRC_IA) {
        if (accel_edge_wanted(now_us, s_last_reported_us, s_refractory_us)) {
            s_last_reported_us = now_us;
            s_edges_reported++;
            loc_on_motion_event();
            // A1: the refractory is applied at the sensor (CTRL_REG3 I1_IA1
            // cleared below), not by disarming ext1, so IA2 can always wake.
            if (s_refractory_us > 0) {
                s_refr_until_us = now_us + s_refractory_us;
            }
        }
        // else: latch drained (this read already cleared it) but not
        // reported -- exactly the "bad is ignored" throttle this task adds.
    }

    uint8_t src2 = 0;
    bool ia2 = reg_read(LIS3DH_REG_INT2_SRC, &src2) && s_shake_enabled && (src2 & LIS3DH_INT2_SRC_IA);
    bool was_idle = (s_shake.n == 0);
    bool fired = false;
    accel_shake_verdict_t v = accel_shake_step(&s_shake, &s_shake_cfg, now_us, ia2);
    if (was_idle && s_shake.n == 1) {
        s_shake_candidates++;
    }
    if (v == ACCEL_SHAKE_FIRED) {
        ESP_LOGI(TAG, "intentional shake (n=%u in %u ms)", (unsigned) s_shake.out_n,
                 (unsigned) s_shake.out_span_ms);
        s_shake_fired++;
        fired = true;
    } else if (v == ACCEL_SHAKE_REJECTED) {
        ESP_LOGI(TAG, "shake candidate rejected (n=%u in %u ms), IA2 off pin %u s",
                 (unsigned) s_shake.out_n, (unsigned) s_shake.out_span_ms,
                 (unsigned) (s_shake_cfg.holdoff_ms / 1000));
        s_shake_rejected++;
    }

    // Power effect: one CTRL_REG3 write only when the wanted routing changes
    // (IA1 off for the refractory, IA2 off for the holdoff).
    uint8_t want3 = (now_us >= s_refr_until_us ? LIS3DH_CTRL_REG3_I1_IA1 : 0) |
                    ((s_shake_enabled && now_us >= s_shake.holdoff_until_us) ? LIS3DH_CTRL_REG3_I1_IA2 : 0);
    if (want3 != s_ctrl3 && reg_write(LIS3DH_REG_CTRL_REG3, want3)) {
        s_ctrl3 = want3;
    }
    return fired;
}

bool accel_shake_pending(void)
{
    return s_present && s_shake.n > 0;
}

// ---------------------------------------------------------------------------
// A2: debug-only accessors for main.c's `acceltest` (see accel.h's own doc
// comments for the contract each of these follows).
// ---------------------------------------------------------------------------

// LSB in mg of INT*_THS and (LP mode) of one 8-bit output step, from the
// CTRL_REG4 FS bits 5:4: +-2/4/8/16 g -> 16/32/62/186 mg.
static uint32_t lsb_mg_for_ctrl4(uint8_t ctrl4)
{
    static const uint8_t lsb[4] = { 16, 32, 62, 186 };
    return lsb[(ctrl4 >> 4) & 3];
}

static uint32_t current_lsb_mg(void)
{
    uint8_t c4 = 0;
    reg_read(LIS3DH_REG_CTRL_REG4, &c4);
    return lsb_mg_for_ctrl4(c4);
}

bool accel_debug_status(accel_debug_status_t *out, bool *out_newly_configured)
{
    memset(out, 0, sizeof(*out));
    if (out_newly_configured) {
        *out_newly_configured = false;
    }

    uint8_t who = 0;
    bool answers = reg_read(LIS3DH_REG_WHO_AM_I, &who) && who == LIS3DH_WHO_AM_I_VALUE;
    if (answers && !s_present) {
        // Chip wired since boot (or since the last check): run the same
        // configuration accel_init() would have, so `acceltest` works
        // without a reboot -- this task's own requirement. If this fails
        // partway through, configure_and_arm() already logged why and left
        // s_present false; fall through to the "not present" report below.
        bool newly = configure_and_arm(who);
        if (out_newly_configured) {
            *out_newly_configured = newly;
        }
    }

    out->present = s_present;
    if (!s_present) {
        return false;
    }

    out->who_am_i = who;
    reg_read(LIS3DH_REG_CTRL_REG1, &out->ctrl_reg1);
    reg_read(LIS3DH_REG_CTRL_REG2, &out->ctrl_reg2);
    reg_read(LIS3DH_REG_CTRL_REG3, &out->ctrl_reg3);
    reg_read(LIS3DH_REG_CTRL_REG4, &out->ctrl_reg4);
    reg_read(LIS3DH_REG_CTRL_REG5, &out->ctrl_reg5);
    reg_read(LIS3DH_REG_INT1_CFG, &out->int1_cfg);
    reg_read(LIS3DH_REG_INT1_THS, &out->int1_ths);
    reg_read(LIS3DH_REG_INT1_DURATION, &out->int1_duration);
    reg_read(LIS3DH_REG_INT1_SRC, &out->int1_src);
    reg_read(LIS3DH_REG_INT2_CFG, &out->int2_cfg);
    reg_read(LIS3DH_REG_INT2_THS, &out->int2_ths);
    reg_read(LIS3DH_REG_INT2_DURATION, &out->int2_duration);
    reg_read(LIS3DH_REG_INT2_SRC, &out->int2_src);
    uint32_t lsb = lsb_mg_for_ctrl4(out->ctrl_reg4);
    out->ths_mg = (uint32_t) out->int1_ths * lsb;
    out->ths2_mg = (uint32_t) out->int2_ths * lsb;
    out->shake_enabled = s_shake_enabled;
    out->shake_n = s_shake_cfg.n_min;
    out->shake_gap_ms = s_shake_cfg.gap_ms;
    out->shake_span_ms = s_shake_cfg.span_ms;
    out->shake_cooldown_ms = s_shake_cfg.cooldown_ms;
    out->shake_holdoff_ms = s_shake_cfg.holdoff_ms;
    out->shake_candidates = s_shake_candidates;
    out->shake_rejected = s_shake_rejected;
    out->shake_fired = s_shake_fired;
    out->refractory_us = s_refractory_us;
    out->edges_reported = s_edges_reported;
    out->ext1_wakes = net_get_ext1_wakes();
    return true;
}

bool accel_debug_sample(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg, uint8_t *int1_src,
                        uint8_t *int2_src)
{
    if (!s_present) {
        return false;
    }
    uint8_t buf[6] = { 0 };
    if (!reg_read_multi(0x28 /* OUT_X_L */, buf, sizeof(buf))) {
        return false;
    }
    uint8_t src = 0;
    reg_read(LIS3DH_REG_INT1_SRC, &src); // best-effort; sample still reported if this fails
    uint8_t src2 = 0;
    reg_read(LIS3DH_REG_INT2_SRC, &src2);
    uint32_t lsb = current_lsb_mg();

    // CTRL_REG1's LPen=1 (low-power mode) left-justifies each axis's 8-bit
    // result in the high byte; the low byte reads 0. Reading the 16-bit
    // pair as signed and arithmetic-shifting right 8 sign-extends the
    // 8-bit value; times the FS-dependent LSB converts to mg.
    int16_t raw_x = (int16_t) ((uint16_t) buf[0] | ((uint16_t) buf[1] << 8));
    int16_t raw_y = (int16_t) ((uint16_t) buf[2] | ((uint16_t) buf[3] << 8));
    int16_t raw_z = (int16_t) ((uint16_t) buf[4] | ((uint16_t) buf[5] << 8));
    *x_mg = (int16_t) ((raw_x >> 8) * (int) lsb);
    *y_mg = (int16_t) ((raw_y >> 8) * (int) lsb);
    *z_mg = (int16_t) ((raw_z >> 8) * (int) lsb);
    *int1_src = src;
    *int2_src = src2;
    return true;
}

uint32_t accel_debug_sample_period_ms(void)
{
    uint8_t c1 = 0;
    reg_read(LIS3DH_REG_CTRL_REG1, &c1);
    switch (c1 >> 4) {
    case 1: return 1000;
    case 2: return 100;
    case 3: return 40;
    case 4: return 20;
    case 5: return 10;
    default: return 100;
    }
}

bool accel_debug_set_ths(uint8_t ths, uint8_t *readback)
{
    if (!s_present || !reg_write(LIS3DH_REG_INT1_THS, ths)) {
        return false;
    }
    return reg_read(LIS3DH_REG_INT1_THS, readback);
}

bool accel_debug_set_dur(uint8_t dur, uint8_t *readback)
{
    if (!s_present || !reg_write(LIS3DH_REG_INT1_DURATION, dur)) {
        return false;
    }
    return reg_read(LIS3DH_REG_INT1_DURATION, readback);
}

bool accel_debug_set_ths2(uint8_t ths, uint8_t *readback)
{
    if (!s_present || !reg_write(LIS3DH_REG_INT2_THS, ths)) {
        return false;
    }
    return reg_read(LIS3DH_REG_INT2_THS, readback);
}

bool accel_debug_set_dur2(uint8_t dur, uint8_t *readback)
{
    if (!s_present || !reg_write(LIS3DH_REG_INT2_DURATION, dur)) {
        return false;
    }
    return reg_read(LIS3DH_REG_INT2_DURATION, readback);
}

void accel_debug_set_shake(bool enabled)
{
    s_shake_enabled = enabled;
    s_shake.n = 0;
    s_shake.holdoff_until_us = 0;
}

void accel_debug_set_shake_cfg(uint8_t n, uint16_t gap_ms, uint16_t span_ms, uint32_t holdoff_s)
{
    s_shake_cfg.n_min = n;
    s_shake_cfg.gap_ms = gap_ms;
    s_shake_cfg.span_ms = span_ms;
    s_shake_cfg.holdoff_ms = holdoff_s * 1000;
    s_shake.n = 0;
    s_shake.holdoff_until_us = 0;
}

bool accel_debug_set_cfg(uint8_t ctrl1, uint8_t ctrl4)
{
    if (!s_present || !reg_write(LIS3DH_REG_CTRL_REG1, ctrl1) ||
        !reg_write(LIS3DH_REG_CTRL_REG4, ctrl4)) {
        return false;
    }
    uint8_t r1 = 0, r4 = 0;
    return reg_read(LIS3DH_REG_CTRL_REG1, &r1) && reg_read(LIS3DH_REG_CTRL_REG4, &r4) &&
           r1 == ctrl1 && r4 == ctrl4;
}

void accel_debug_set_refractory_s(uint32_t seconds)
{
    s_refractory_us = (int64_t) seconds * 1000000;
    // A change takes effect on the next accel_poll() call; if a refractory
    // window from the old setting is still pending, leave it be -- it ends
    // on its own original schedule, same tolerance as any other
    // in-flight timer this codebase does not cancel on a config change.
}

#endif /* ESP_PLATFORM */

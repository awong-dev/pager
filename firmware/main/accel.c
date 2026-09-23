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

// CTRL_REG1: ODR=0010 (10 Hz), LPen=1 (low-power mode), Zen=Yen=Xen=1.
#define LIS3DH_CTRL_REG1_10HZ_LOWPOWER 0x2F
// CTRL_REG2: HPIS1=1 (high-pass filter routed to INT1 generator 1), normal
// mode (HPM=00), lowest cutoff (FDS=0, HPCF=00).
#define LIS3DH_CTRL_REG2_HPF_INT1 0x01
// CTRL_REG3: I1_IA1=1 (route interrupt generator 1 to the INT1 pin).
#define LIS3DH_CTRL_REG3_I1_IA1 0x40
// CTRL_REG4: default (+-2g, normal resolution) — the coarsest, most
// sensitive-to-small-motion range, appropriate for "is this pager moving at
// all", not precise measurement.
#define LIS3DH_CTRL_REG4_DEFAULT 0x00
// CTRL_REG5: LIR_INT1=1 (latch INT1 until INT1_SRC is read) — required so a
// brief jolt is not missed between two ~2-5s wake-cycle polls.
#define LIS3DH_CTRL_REG5_LATCH_INT1 0x08
// INT1_CFG: OR combination (AOI=0, 6D=0) of XHIE/YHIE/ZHIE (any axis high).
#define LIS3DH_INT1_CFG_ANY_HIGH 0x2A
// INT1_THS: threshold, 1 LSB = 16 mg at +-2g. 0x10 (16 * 16mg = ~256 mg) is a
// starting guess for "walking/being carried", not a measured value.
#define LIS3DH_INT1_THS_DEFAULT 0x10
// INT1_DURATION: 0 = no minimum duration (react to the first sample past
// threshold) — the 60s "sustained" requirement is loc.c's own classifier's
// job, not this register's.
#define LIS3DH_INT1_DURATION_DEFAULT 0x00

// INT1_SRC bit 6 (IA): "one or more interrupts have been generated".
#define LIS3DH_INT1_SRC_IA 0x40

static bool s_present = false;

// A1 (docs/DEVICE_NEXT_TASKS.md): refractory-gate state. s_refractory_us
// starts at the ACCEL_REFRACTORY_S default; `acceltest refr <seconds>` (A2)
// is the only way to change it at runtime. s_last_reported_us / s_edges_reported
// are accel_poll()'s bookkeeping for accel_edge_wanted() and the `acceltest`
// "edges reported" line respectively. s_wake_disarmed/s_wake_rearm_at_us
// track the ext1-disarm window opened after a wanted edge.
static int64_t s_refractory_us = (int64_t) ACCEL_REFRACTORY_S * 1000000;
static int64_t s_last_reported_us = 0;
static uint32_t s_edges_reported = 0;
static bool s_wake_disarmed = false;
static int64_t s_wake_rearm_at_us = 0;

static bool reg_write(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_write_to_device(I2C_NUM_0, PAGER_I2C_ADDR_LIS3DH, buf, sizeof(buf),
                                      pdMS_TO_TICKS(50)) == ESP_OK;
}

static bool reg_read(uint8_t reg, uint8_t *out)
{
    return i2c_master_write_read_device(I2C_NUM_0, PAGER_I2C_ADDR_LIS3DH, &reg, 1, out, 1,
                                        pdMS_TO_TICKS(50)) == ESP_OK;
}

// A2: multi-byte read with the LIS3DH's auto-increment bit (0x80) set on
// the register address, e.g. OUT_X_L..OUT_Z_H in one transaction.
static bool reg_read_multi(uint8_t reg, uint8_t *out, size_t n)
{
    uint8_t addr = reg | 0x80;
    return i2c_master_write_read_device(I2C_NUM_0, PAGER_I2C_ADDR_LIS3DH, &addr, 1, out, n,
                                        pdMS_TO_TICKS(50)) == ESP_OK;
}

// Shared by accel_init() (boot probe) and accel_debug_status() (A2's
// re-probe-without-a-reboot path): writes the fixed configuration and, on
// success, marks the chip present and arms the ext1 wake source. Returns
// false (s_present left false) if any write fails partway through.
static bool configure_and_arm(uint8_t who)
{
    bool ok = true;
    ok &= reg_write(LIS3DH_REG_CTRL_REG1, LIS3DH_CTRL_REG1_10HZ_LOWPOWER);
    ok &= reg_write(LIS3DH_REG_CTRL_REG4, LIS3DH_CTRL_REG4_DEFAULT);
    ok &= reg_write(LIS3DH_REG_CTRL_REG2, LIS3DH_CTRL_REG2_HPF_INT1);
    ok &= reg_write(LIS3DH_REG_INT1_THS, LIS3DH_INT1_THS_DEFAULT);
    ok &= reg_write(LIS3DH_REG_INT1_DURATION, LIS3DH_INT1_DURATION_DEFAULT);
    ok &= reg_write(LIS3DH_REG_INT1_CFG, LIS3DH_INT1_CFG_ANY_HIGH);
    ok &= reg_write(LIS3DH_REG_CTRL_REG5, LIS3DH_CTRL_REG5_LATCH_INT1);
    ok &= reg_write(LIS3DH_REG_CTRL_REG3, LIS3DH_CTRL_REG3_I1_IA1);
    if (!ok) {
        ESP_LOGI(TAG, "LIS3DH found (WHO_AM_I=0x%02x) but configuration failed partway through - "
                      "motion trigger disabled, running without it",
                 (unsigned) who);
        s_present = false;
        return false;
    }

    s_present = true;
    net_enable_accel_wake(); // IO2 becomes a light-sleep wake source, net.cpp's net_sleep()
    ESP_LOGI(TAG, "LIS3DH found (WHO_AM_I=0x%02x), configured 10Hz low-power + high-pass INT1 "
                  "motion interrupt (thresholds UNVERIFIED, see accel.c)",
             (unsigned) who);
    return true;
}

bool accel_init(void)
{
    uint8_t who = 0;
    if (!reg_read(LIS3DH_REG_WHO_AM_I, &who) || who != LIS3DH_WHO_AM_I_VALUE) {
        // Expected outcome on the owner's bench unit (V02_DESIGN.md §5: "it
        // may not be wired yet"). Logged once at INFO, never retried — this
        // is not a paging-path dependency, so there is nothing to recover
        // into and nothing worth a retry loop for.
        ESP_LOGI(TAG,
                 "LIS3DH not found at 0x%02x (WHO_AM_I read 0x%02x) - motion trigger disabled; "
                 "this is the expected/likely case if the accelerometer is not wired yet",
                 (unsigned) PAGER_I2C_ADDR_LIS3DH, (unsigned) who);
        s_present = false;
        return false;
    }
    return configure_and_arm(who);
}

void accel_poll(void)
{
    if (!s_present) {
        return;
    }
    uint8_t src = 0;
    if (!reg_read(LIS3DH_REG_INT1_SRC, &src)) {
        return; // transient I2C failure; try again next cycle, same tolerance ui.c's CardKB read uses
    }
    int64_t now_us = esp_timer_get_time();
    if (src & LIS3DH_INT1_SRC_IA) {
        if (accel_edge_wanted(now_us, s_last_reported_us, s_refractory_us)) {
            s_last_reported_us = now_us;
            s_edges_reported++;
            loc_on_motion_event();
            // A1: disarm ext1 for the refractory window so the ~10 Hz wake
            // storm this same edge would otherwise cause (LIR_INT1 keeps
            // re-latching above threshold) does not end light sleep again
            // until the classifier could possibly need another edge.
            // Power effect: see net_set_accel_wake()'s own doc comment.
            if (s_refractory_us > 0) {
                net_set_accel_wake(false);
                s_wake_rearm_at_us = now_us + s_refractory_us;
                s_wake_disarmed = true;
            }
        }
        // else: latch drained (this read already cleared it) but not
        // reported -- exactly the "bad is ignored" throttle this task adds.
    }
    if (s_wake_disarmed && now_us >= s_wake_rearm_at_us) {
        net_set_accel_wake(true); // power effect: re-arms the ext1 wake source
        s_wake_disarmed = false;
    }
}

// ---------------------------------------------------------------------------
// A2: debug-only accessors for main.c's `acceltest` (see accel.h's own doc
// comments for the contract each of these follows).
// ---------------------------------------------------------------------------

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
    out->ths_mg = (uint32_t) out->int1_ths * 16;
    out->refractory_us = s_refractory_us;
    out->edges_reported = s_edges_reported;
    out->ext1_wakes = net_get_ext1_wakes();
    return true;
}

bool accel_debug_sample(int16_t *x_mg, int16_t *y_mg, int16_t *z_mg, uint8_t *int1_src)
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

    // CTRL_REG1's LPen=1 (low-power mode) left-justifies each axis's 8-bit
    // result in the high byte; the low byte reads 0. Reading the 16-bit
    // pair as signed and arithmetic-shifting right 8 sign-extends the
    // 8-bit value; *16 converts to mg at the same 16 mg/LSB, +-2g scale
    // INT1_THS uses (accel.c's own INT1_THS comment) -- UNVERIFIED against
    // real hardware, like every other threshold in this file.
    int16_t raw_x = (int16_t) ((uint16_t) buf[0] | ((uint16_t) buf[1] << 8));
    int16_t raw_y = (int16_t) ((uint16_t) buf[2] | ((uint16_t) buf[3] << 8));
    int16_t raw_z = (int16_t) ((uint16_t) buf[4] | ((uint16_t) buf[5] << 8));
    *x_mg = (int16_t) ((raw_x >> 8) * 16);
    *y_mg = (int16_t) ((raw_y >> 8) * 16);
    *z_mg = (int16_t) ((raw_z >> 8) * 16);
    *int1_src = src;
    return true;
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

void accel_debug_set_refractory_s(uint32_t seconds)
{
    s_refractory_us = (int64_t) seconds * 1000000;
    // A change takes effect on the next accel_poll() call; if a disarm
    // window from the old setting is still pending, leave it be -- it will
    // re-arm on its own original schedule, same tolerance as any other
    // in-flight timer this codebase does not cancel on a config change.
}

#endif /* ESP_PLATFORM */

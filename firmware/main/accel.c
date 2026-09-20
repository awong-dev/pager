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

#include "loc.h"
#include "net.h"
#include "pins.h"

#include "driver/i2c.h"
#include "esp_log.h"
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

bool accel_present(void)
{
    return s_present;
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
    if (src & LIS3DH_INT1_SRC_IA) {
        loc_on_motion_event();
    }
}

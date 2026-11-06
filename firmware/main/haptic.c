/* haptic.c -- see haptic.h.
 *
 * Constants: the LRA rated-voltage / overdrive-clamp formulas are the DRV2605L datasheet 8.5.2.1
 * equations as remembered by the author; the datasheet is not in the repo. Auto-calibration trims
 * the result, but check the numbers against the datasheet before trusting `rated_mv`/`clamp_mv`. */
#include "haptic.h"

#include <math.h>
#include <stdio.h>

#include "board.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "pins.h"
#include "rail.h"
#include "ui.h"

static const char *TAG = "haptic";

#define REG_STATUS 0x00
#define REG_MODE 0x01
#define REG_LIBRARY 0x03
#define REG_WAV1 0x04
#define REG_GO 0x0C
#define REG_RATED 0x16
#define REG_CLAMP 0x17
#define REG_CAL_COMP 0x18
#define REG_CAL_BEMF 0x19
#define REG_FEEDBACK 0x1A
#define REG_CTRL1 0x1B
#define REG_CTRL2 0x1C
#define REG_CTRL3 0x1D
#define REG_CTRL4 0x1E
#define REG_LRA_PERIOD 0x22

#define DEFAULT_RATED_MV 1200
#define DEFAULT_CLAMP_MV 1800
#define DEFAULT_F_HZ 235

#define BUS_TIMEOUT_MS 200
#define STANDBY_AFTER_US 600000

static uint32_t s_rated_mv = DEFAULT_RATED_MV, s_clamp_mv = DEFAULT_CLAMP_MV, s_f_hz = DEFAULT_F_HZ;
static bool s_cfg_loaded;
static int64_t s_ready_for_us = -1; /* rail_restored_us() the chip was last configured for */
static bool s_cal_failed;
static bool s_have_cal;
static uint8_t s_cal[3]; /* A_CAL_COMP, A_CAL_BEMF, FEEDBACK low 2 bits (BEMF_GAIN) */
static esp_timer_handle_t s_standby_timer;

static esp_err_t wr(uint8_t reg, uint8_t v)
{
    uint8_t b[2] = {reg, v};
    return i2c_master_write_to_device(I2C_NUM_0, PAGER_I2C_ADDR_DRV2605, b, 2, pdMS_TO_TICKS(50));
}

static esp_err_t rd(uint8_t reg, uint8_t *v)
{
    return i2c_master_write_read_device(I2C_NUM_0, PAGER_I2C_ADDR_DRV2605, &reg, 1, v, 1, pdMS_TO_TICKS(50));
}

static void load_nvs(void)
{
    if (s_cfg_loaded) {
        return;
    }
    s_cfg_loaded = true;
    nvs_handle_t h;
    if (nvs_open("haptic", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint32_t v;
    if (nvs_get_u32(h, "rated_mv", &v) == ESP_OK && v > 0) s_rated_mv = v;
    if (nvs_get_u32(h, "clamp_mv", &v) == ESP_OK && v > 0) s_clamp_mv = v;
    if (nvs_get_u32(h, "f_hz", &v) == ESP_OK && v >= 100 && v <= 400) s_f_hz = v;
    uint8_t blob[4];
    size_t len = sizeof(blob);
    if (nvs_get_blob(h, "cal", blob, &len) == ESP_OK && len == 4 && blob[0] == 1) { /* [version, comp, bemf, gain] */
        s_cal[0] = blob[1];
        s_cal[1] = blob[2];
        s_cal[2] = blob[3];
        s_have_cal = true;
    }
    nvs_close(h);
}

static void save_cal(void)
{
    nvs_handle_t h;
    if (nvs_open("haptic", NVS_READWRITE, &h) == ESP_OK) {
        uint8_t blob[4] = {1, s_cal[0], s_cal[1], s_cal[2]};
        nvs_set_blob(h, "cal", blob, sizeof(blob));
        nvs_commit(h);
        nvs_close(h);
    }
}

static uint8_t clamp8(double x)
{
    return x < 0 ? 0 : (x > 255 ? 255 : (uint8_t) (x + 0.5));
}

/* Writes the LRA configuration registers. */
static esp_err_t write_config(void)
{
    double f = (double) s_f_hz;
    double rated = ((double) s_rated_mv / 1000.0) / (0.02071 * sqrt(1.0 - (4.0 * 300e-6 + 300e-6) * f));
    /* Closed loop (CTRL3 0xA0): OD_CLAMP = Vpeak / 21.96e-3 (no sqrt term; the sqrt form is open loop). */
    double clamp = ((double) s_clamp_mv / 1000.0) / 21.96e-3;
    double drive = (0.5 / f * 1000.0 - 0.5) / 0.1; /* DRIVE_TIME: (half period - 0.5 ms) / 0.1 ms */
    uint8_t drive_code = clamp8(drive);
    if (drive_code > 31) drive_code = 31;
    esp_err_t e = wr(REG_MODE, 0x00); /* out of standby, internal trigger */
    if (e != ESP_OK) return e;
    wr(REG_LIBRARY, 6);               /* LRA library */
    wr(REG_FEEDBACK, 0xB6);           /* LRA, brake 3, loop gain 1, BEMF gain 2 */
    wr(REG_RATED, clamp8(rated));
    wr(REG_CLAMP, clamp8(clamp));
    wr(REG_CTRL1, 0x80 | drive_code); /* startup boost + drive time */
    wr(REG_CTRL2, 0xF5);              /* datasheet default */
    wr(REG_CTRL3, 0xA0);              /* datasheet default, closed loop */
    wr(REG_CTRL4, 0x30);              /* longest auto-cal time */
    return ESP_OK;
}

static bool auto_cal(char *report, unsigned rlen)
{
    wr(REG_MODE, 0x07);
    wr(REG_GO, 1);
    uint8_t go = 1;
    for (int i = 0; i < 80 && go; i++) { /* up to ~1.6 s, polled with a task delay */
        vTaskDelay(pdMS_TO_TICKS(20));
        if (rd(REG_GO, &go) != ESP_OK) break;
    }
    uint8_t st = 0, comp = 0, bemf = 0, fb = 0, per = 0;
    rd(REG_STATUS, &st);
    rd(REG_CAL_COMP, &comp);
    rd(REG_CAL_BEMF, &bemf);
    rd(REG_FEEDBACK, &fb);
    rd(REG_LRA_PERIOD, &per);
    wr(REG_MODE, 0x00);
    bool ok = (go == 0) && !(st & 0x08);
    if (report) {
        snprintf(report, rlen, "cal %s: comp=0x%02x bemf=0x%02x gain=%u status=0x%02x lra_period=0x%02x (~%u Hz)",
                 ok ? "ok" : "FAILED", comp, bemf, fb & 3, st, per, per ? (unsigned) (1000000.0 / (per * 98.46)) : 0u);
    }
    if (ok) {
        s_cal[0] = comp;
        s_cal[1] = bemf;
        s_cal[2] = fb & 3;
        s_have_cal = true;
        s_cal_failed = false;
        save_cal();
    } else {
        s_cal_failed = true;
    }
    return ok;
}

/* First use after each rail_on: the chip lost power, so re-write config and calibration. Caller holds
 * the bus lock and has checked rail_is_on(). */
static bool ensure_ready(void)
{
    int64_t r = rail_restored_us();
    if (s_ready_for_us == r) {
        return true;
    }
    load_nvs();
    if (write_config() != ESP_OK) {
        ESP_LOGI(TAG, "DRV2605 not answering at 0x%02x", PAGER_I2C_ADDR_DRV2605);
        return false;
    }
    if (s_have_cal) {
        uint8_t fb = 0;
        rd(REG_FEEDBACK, &fb);
        wr(REG_CAL_COMP, s_cal[0]);
        wr(REG_CAL_BEMF, s_cal[1]);
        wr(REG_FEEDBACK, (uint8_t) ((fb & ~3u) | (s_cal[2] & 3u)));
    } else {
        /* haptic_play() never auto-calibrates (it blocks ~1.6 s with the bus lock held); only the console does */
        static bool s_warned;
        if (!s_warned) {
            s_warned = true;
            ESP_LOGI(TAG, "haptic: not calibrated (run `haptic calibrate`)");
        }
    }
    s_ready_for_us = r;
    ESP_LOGI(TAG, "configured (rail restore edge %lld)", (long long) r);
    return true;
}

static void standby_cb(void *arg)
{
    (void) arg;
    if (!ui_kb_bus_lock(5)) {
        esp_timer_start_once(s_standby_timer, 100000); /* bus busy: retry in 100 ms, never block the timer task */
        return;
    }
    if (rail_is_on() && s_ready_for_us == rail_restored_us()) {
        wr(REG_MODE, 0x40); /* standby */
    }
    ui_kb_bus_unlock();
}

bool haptic_play(int effect)
{
    if (!board_has_haptic() || !rail_is_on()) {
        return false;
    }
    if (!ui_kb_bus_lock(BUS_TIMEOUT_MS)) {
        return false;
    }
    bool ok = false;
    if (rail_is_on() && ensure_ready()) {
        ok = wr(REG_MODE, 0x00) == ESP_OK;
        if (effect == HAPTIC_NEW_MSG) {
            uint8_t seq[4] = {1, 0x80 | 15, 1, 0}; /* click, 150 ms wait, click, end */
            for (int i = 0; i < 4 && ok; i++) ok = wr(REG_WAV1 + i, seq[i]) == ESP_OK;
        } else if (effect >= 1 && effect <= 123) {
            ok = ok && wr(REG_WAV1, (uint8_t) effect) == ESP_OK && wr(REG_WAV1 + 1, 0) == ESP_OK;
        } else {
            ok = false;
        }
        ok = ok && wr(REG_GO, 1) == ESP_OK;
        if (ok) {
            if (!s_standby_timer) {
                const esp_timer_create_args_t ta = {.callback = standby_cb, .name = "hap_sb"};
                esp_timer_create(&ta, &s_standby_timer);
            }
            esp_timer_stop(s_standby_timer);
            esp_timer_start_once(s_standby_timer, STANDBY_AFTER_US);
        }
    }
    ui_kb_bus_unlock();
    return ok;
}

bool haptic_calibrate(char *report, unsigned report_len)
{
    if (!board_has_haptic() || !rail_is_on() || !ui_kb_bus_lock(BUS_TIMEOUT_MS)) {
        if (report) snprintf(report, report_len, "haptic unavailable (breadboard, rail off or bus busy)");
        return false;
    }
    load_nvs();
    bool ok = false;
    if (write_config() == ESP_OK) {
        ok = auto_cal(report, report_len);
        s_ready_for_us = ok ? rail_restored_us() : -1;
    } else if (report) {
        snprintf(report, report_len, "DRV2605 not answering");
    }
    ui_kb_bus_unlock();
    return ok;
}

bool haptic_set_cfg(uint32_t rated_mv, uint32_t clamp_mv, uint32_t f_hz)
{
    if (rated_mv == 0 || clamp_mv == 0 || f_hz < 100 || f_hz > 400) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open("haptic", NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    nvs_set_u32(h, "rated_mv", rated_mv);
    nvs_set_u32(h, "clamp_mv", clamp_mv);
    nvs_set_u32(h, "f_hz", f_hz);
    nvs_commit(h);
    nvs_close(h);
    s_rated_mv = rated_mv;
    s_clamp_mv = clamp_mv;
    s_f_hz = f_hz;
    s_cfg_loaded = true;
    s_ready_for_us = -1; /* re-write the config at next use */
    return true;
}

void haptic_status(char *out, unsigned len)
{
    load_nvs();
    snprintf(out, len, "rated_mv=%u clamp_mv=%u f_hz=%u cal=%s%s rail=%d", (unsigned) s_rated_mv,
             (unsigned) s_clamp_mv, (unsigned) s_f_hz, s_have_cal ? "saved" : "none",
             s_cal_failed ? " (last attempt failed)" : "", rail_is_on());
}

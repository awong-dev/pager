/* battstat.c — see battstat.h. The core below has no ESP-IDF dependency. */

#include "battstat.h"

#include <stdio.h>
#include <string.h>

#define U32_MAX 0xFFFFFFFFu

static uint32_t sat_add(uint32_t a, uint64_t b)
{
    uint64_t s = (uint64_t) a + b;
    return s > U32_MAX ? U32_MAX : (uint32_t) s;
}

static uint32_t clamp_sub(uint32_t a, uint64_t b) { return b >= a ? 0u : a - (uint32_t) b; }

/* Portable bitwise CRC32 (reflected 0xEDB88320) over everything after `crc`. */
uint32_t bs_core_crc(const battstat_rtc_t *r)
{
    const uint8_t *p = (const uint8_t *) &r->sq;
    size_t n = sizeof(*r) - offsetof(battstat_rtc_t, sq);
    uint32_t c = 0xFFFFFFFFu;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static void recrc(battstat_rtc_t *r) { r->crc = bs_core_crc(r); }

bool bs_core_init(battstat_rtc_t *r)
{
    if (r->magic == BATTSTAT_MAGIC && r->crc == bs_core_crc(r)) {
        return true;
    }
    memset(r, 0, sizeof(*r));
    r->magic = BATTSTAT_MAGIC;
    recrc(r);
    return false;
}

/* Adds the delta of a since-boot source counter; one that went backwards
 * (e.g. disp stats reset by sleeptest) counts as the delta itself. */
static uint32_t src_delta(uint32_t cur, uint32_t *last)
{
    uint32_t d = cur >= *last ? cur - *last : cur;
    *last = cur;
    return d;
}

void bs_core_tick(battstat_rtc_t *r, bs_ram_t *ram, const bs_inputs_t *in)
{
    if (ram->last_tick_us == 0) {
        ram->last_tick_us = in->now_us;
        ram->slept_us = 0;
        ram->cause_max = BS_TIMER;
        ram->rail_ms = in->rail_ms;
        ram->ext1 = in->ext1;
        for (int i = 0; i < 3; i++) {
            ram->rf[i] = in->rf[i];
        }
        ram->link = in->link;
        ram->radio = in->radio;
        return;
    }
    int64_t span_us = in->now_us - ram->last_tick_us;
    uint64_t elapsed_ms = span_us > 0 ? (uint64_t) span_us / 1000u : 0u;
    ram->last_tick_us += (int64_t) (elapsed_ms * 1000u);
    uint64_t slept_ms = ram->slept_us / 1000u;
    uint64_t awake_ms = elapsed_ms > slept_ms ? elapsed_ms - slept_ms : 0u;
    uint8_t c = ram->cause_max;
    if (c >= BS_NCAUSE) {
        c = BS_TIMER;
    }
    r->aw_ms[c] = sat_add(r->aw_ms[c], awake_ms);
    r->dt_ms = sat_add(r->dt_ms, elapsed_ms);
    r->sl_ms = sat_add(r->sl_ms, slept_ms);
    if (in->off) {
        r->md_ms[0] = sat_add(r->md_ms[0], elapsed_ms);
    } else if (in->search) {
        r->md_ms[1] = sat_add(r->md_ms[1], elapsed_ms);
    }
    if (in->gnss) {
        r->md_ms[2] = sat_add(r->md_ms[2], elapsed_ms);
    }
    r->rl_ms = sat_add(r->rl_ms, src_delta(in->rail_ms, &ram->rail_ms));
    r->x1 = sat_add(r->x1, src_delta(in->ext1, &ram->ext1));
    for (int i = 0; i < 3; i++) {
        r->rf[i] = sat_add(r->rf[i], src_delta(in->rf[i], &ram->rf[i]));
    }
    r->cn = sat_add(r->cn, src_delta(in->link, &ram->link));
    r->re = sat_add(r->re, src_delta(in->radio, &ram->radio));
    ram->cause_max = BS_TIMER;
    ram->slept_us = 0;
    recrc(r);
}

void bs_core_sleep(battstat_rtc_t *r, bs_ram_t *ram, int64_t slept_us)
{
    if (slept_us > 0) {
        ram->slept_us += (uint64_t) slept_us;
    }
    r->ns = sat_add(r->ns, 1);
    recrc(r);
}

void bs_core_raise(bs_ram_t *ram, bs_cause_t c)
{
    if ((uint8_t) c > ram->cause_max && c < BS_NCAUSE) {
        ram->cause_max = (uint8_t) c;
    }
}

void bs_core_note_mv(battstat_rtc_t *r, int mv)
{
    if (mv < 2000 || mv > 5000) {
        return;
    }
    if (r->mvn == 0 || mv < (int) r->mvn) {
        r->mvn = (uint16_t) mv;
        recrc(r);
    }
}

void bs_core_snapshot(const battstat_rtc_t *r, bs_snap_t *out)
{
    memset(out, 0, sizeof(*out));
    out->sq = r->sq;
    out->dt = r->dt_ms / 1000u;
    out->sl = r->sl_ms / 1000u;
    for (int i = 0; i < BS_NCAUSE; i++) {
        out->aw[i] = r->aw_ms[i] / 1000u;
    }
    out->ns = r->ns;
    out->x1 = r->x1;
    out->rl = r->rl_ms / 1000u;
    for (int i = 0; i < 3; i++) {
        out->rf[i] = r->rf[i];
        out->md[i] = r->md_ms[i] / 1000u;
    }
    out->mvn = r->mvn;
    out->cn = r->cn;
    out->re = r->re;
}

bool bs_core_encode(cbor_w_t *w, const bs_snap_t *s)
{
    cbor_w_map_key(w, BATTSTAT_KEY, s->mvn != 0 ? 12 : 11);
    cbor_w_uint(w, 0, s->sq);
    cbor_w_uint(w, 1, s->dt);
    cbor_w_uint(w, 2, s->sl);
    cbor_w_array(w, 3, BS_NCAUSE);
    for (int i = 0; i < BS_NCAUSE; i++) {
        cbor_w_uint_item(w, s->aw[i]);
    }
    cbor_w_uint(w, 4, s->ns);
    cbor_w_uint(w, 5, s->x1);
    cbor_w_uint(w, 6, s->rl);
    cbor_w_array(w, 7, 3);
    for (int i = 0; i < 3; i++) {
        cbor_w_uint_item(w, s->rf[i]);
    }
    if (s->mvn != 0) {
        cbor_w_uint(w, 8, s->mvn);
    }
    cbor_w_uint(w, 9, s->cn);
    cbor_w_array(w, 10, 3);
    for (int i = 0; i < 3; i++) {
        cbor_w_uint_item(w, s->md[i]);
    }
    cbor_w_uint(w, 11, s->re);
    return !w->err;
}

void bs_core_commit(battstat_rtc_t *r, const bs_snap_t *s)
{
    r->dt_ms = clamp_sub(r->dt_ms, (uint64_t) s->dt * 1000u);
    r->sl_ms = clamp_sub(r->sl_ms, (uint64_t) s->sl * 1000u);
    for (int i = 0; i < BS_NCAUSE; i++) {
        r->aw_ms[i] = clamp_sub(r->aw_ms[i], (uint64_t) s->aw[i] * 1000u);
    }
    r->rl_ms = clamp_sub(r->rl_ms, (uint64_t) s->rl * 1000u);
    for (int i = 0; i < 3; i++) {
        r->md_ms[i] = clamp_sub(r->md_ms[i], (uint64_t) s->md[i] * 1000u);
        r->rf[i] = clamp_sub(r->rf[i], s->rf[i]);
    }
    r->ns = clamp_sub(r->ns, s->ns);
    r->x1 = clamp_sub(r->x1, s->x1);
    r->cn = clamp_sub(r->cn, s->cn);
    r->re = clamp_sub(r->re, s->re);
    r->sq = sat_add(r->sq, 1);
    r->mvn = 0;
    recrc(r);
}

#ifdef ESP_PLATFORM

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "disp.h"
#include "loc.h"
#include "net.h"
#include "rail.h"

static const char *TAG = "battstat";

/* RTC_NOINIT: survives software, panic and watchdog resets and the airplane
 * reboot; garbage after a power-on, hence the magic + CRC (watchdog.c does the
 * same). */
static RTC_NOINIT_ATTR battstat_rtc_t s_bs;
static bs_ram_t s_ram;

void battstat_init(void)
{
    if (bs_core_init(&s_bs)) {
        ESP_LOGI(TAG, "window kept (sq=%u dt=%u s)", (unsigned) s_bs.sq, (unsigned) (s_bs.dt_ms / 1000u));
    } else {
        ESP_LOGI(TAG, "cold window");
    }
}

// Power effect: none (reads sources, adds counters, one CRC32).
void battstat_tick(uint32_t link_total, bool radio_off)
{
    bs_inputs_t in;
    memset(&in, 0, sizeof(in));
    in.now_us = esp_timer_get_time();
    in.rail_ms = rail_on_ms_total();
    in.ext1 = net_get_ext1_wakes();
    disp_get_refresh_stats(&in.rf[0], &in.rf[1], &in.rf[2]);
    in.link = link_total;
    in.radio = net_get_radio_events();
    in.off = radio_off;
    in.search = !radio_off && net_unregistered_for_s() > 0;
    in.gnss = loc_attempt_in_progress();
    bs_core_tick(&s_bs, &s_ram, &in);
}

// Power effect: none.
void battstat_note_sleep(int64_t slept_us) { bs_core_sleep(&s_bs, &s_ram, slept_us); }

void battstat_raise(bs_cause_t c) { bs_core_raise(&s_ram, c); }

void battstat_note_mv(int mv) { bs_core_note_mv(&s_bs, mv); }

void battstat_snapshot(bs_snap_t *out) { bs_core_snapshot(&s_bs, out); }

bool battstat_encode(cbor_w_t *w, const bs_snap_t *s) { return bs_core_encode(w, s); }

void battstat_commit(const bs_snap_t *s) { bs_core_commit(&s_bs, s); }

void battstat_format(char *buf, size_t cap)
{
    bs_snap_t s;
    bs_core_snapshot(&s_bs, &s);
    uint32_t aw_total = 0;
    for (int i = 0; i < BS_NCAUSE; i++) {
        aw_total += s.aw[i];
    }
    uint32_t per_wake = s.ns ? (uint32_t) (((uint64_t) aw_total * 1000u) / s.ns) : 0u;
    snprintf(buf, cap,
             "battstat sq=%u dt=%u sl=%u aw=[%u %u %u %u %u %u] ns=%u x1=%u rl=%u rf=[%u %u %u] mvn=%u cn=%u "
             "md=[%u %u %u] re=%u | dt=%u sl+aw=%u | per-wake awake ms=%u",
             (unsigned) s.sq, (unsigned) s.dt, (unsigned) s.sl, (unsigned) s.aw[0], (unsigned) s.aw[1],
             (unsigned) s.aw[2], (unsigned) s.aw[3], (unsigned) s.aw[4], (unsigned) s.aw[5], (unsigned) s.ns,
             (unsigned) s.x1, (unsigned) s.rl, (unsigned) s.rf[0], (unsigned) s.rf[1], (unsigned) s.rf[2],
             (unsigned) s.mvn, (unsigned) s.cn, (unsigned) s.md[0], (unsigned) s.md[1], (unsigned) s.md[2],
             (unsigned) s.re, (unsigned) s.dt, (unsigned) (s.sl + aw_total), (unsigned) per_wake);
}

#endif /* ESP_PLATFORM */

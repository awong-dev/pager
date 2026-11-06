/* fuel_fmt.c -- see fuel_fmt.h. Pure C, no ESP-IDF. */
#include "fuel_fmt.h"

#include <stdio.h>

int fuel_mv_from_vcell(uint16_t raw)
{
    return (int) (((uint32_t) raw * 5u + 32u) / 64u); /* raw * 78.125 uV = raw * 5/64 mV */
}

int fuel_pct_from_soc(uint16_t raw)
{
    int p = (int) (((uint32_t) raw + 128u) >> 8);
    return p > 100 ? 100 : p;
}

int fuel_crate_milli_from_raw(int16_t raw) { return (int) raw * 208; }

fuel_state_t fuel_state_from_pins(int vbus, int stat1)
{
    if (!vbus) {
        return FUEL_ON_BATTERY;
    }
    return stat1 ? FUEL_CHARGED : FUEL_CHARGING;
}

const char *fuel_state_name(fuel_state_t s)
{
    switch (s) {
    case FUEL_CHARGING: return "charging";
    case FUEL_CHARGED: return "charged";
    default: return "battery";
    }
}

int fuel_icon_segs(int pct, fuel_state_t s)
{
    if (s == FUEL_CHARGED) {
        return 4;
    }
    if (pct < 0) {
        return -1;
    }
    if (pct > 75) return 4;
    if (pct > 50) return 3;
    if (pct > 25) return 2;
    if (pct > 10) return 1;
    return 0;
}

bool fuel_should_shutdown(int mv, bool plugged, int consecutive_low_count, bool booting)
{
    if (mv <= 0 || plugged) {
        return false;
    }
    if (booting) {
        return mv <= FUEL_BOOT_SHUTDOWN_MV;
    }
    return mv <= FUEL_SHUTDOWN_MV && consecutive_low_count >= FUEL_SHUTDOWN_READS;
}

void fuel_fmt_line(int mv, int pct, fuel_state_t s, char *out, size_t cap)
{
    if (mv <= 0) {
        snprintf(out, cap, "--");
        return;
    }
    int c = (mv + 5) / 10;
    int n = snprintf(out, cap, "%d.%02d V", c / 100, c % 100);
    if (n > 0 && (size_t) n < cap && pct >= 0) {
        n += snprintf(out + n, cap - (size_t) n, " %d%%", pct);
    }
    if (n > 0 && (size_t) n < cap && s != FUEL_ON_BATTERY) {
        snprintf(out + n, cap - (size_t) n, " %s", fuel_state_name(s));
    }
}

/* devinfo_fmt.c -- see devinfo_fmt.h. Pure C, no ESP-IDF. */
#include "devinfo_fmt.h"

#include <stdio.h>
#include <string.h>

void devinfo_fmt_batt(bool known, int mv, char *out, size_t cap)
{
    if (!known || mv <= 0) {
        snprintf(out, cap, "--");
        return;
    }
    int c = (mv + 5) / 10; /* hundredths of a volt */
    snprintf(out, cap, "%d.%02d V%s", c / 100, c % 100, mv >= DEVINFO_USB_MV ? " (USB)" : "");
}

void devinfo_fmt_temp(bool known, int dc, char *out, size_t cap)
{
    if (!known) {
        snprintf(out, cap, "--");
        return;
    }
    int a = dc < 0 ? -dc : dc;
    snprintf(out, cap, "%s%d.%d C", dc < 0 ? "-" : "", a / 10, a % 10);
}

/* Copies the n-th (0-based) double-quoted string of `s` into out; false if there is none. */
static bool quoted(const char *s, int n, char *out, size_t cap)
{
    const char *p = s;
    for (int i = 0;; i++) {
        p = strchr(p, '"');
        if (p == NULL) {
            return false;
        }
        const char *e = strchr(p + 1, '"');
        if (e == NULL) {
            return false;
        }
        if (i == n) {
            size_t len = (size_t) (e - p - 1);
            if (len == 0 || cap == 0) {
                return false;
            }
            if (len > cap - 1) {
                len = cap - 1;
            }
            memcpy(out, p + 1, len);
            out[len] = '\0';
            return true;
        }
        p = e + 1;
    }
}

bool devinfo_parse_cops(const char *line, char *name, size_t cap)
{
    return line != NULL && quoted(line, 0, name, cap);
}

bool devinfo_parse_cnum(const char *line, char *num, size_t cap)
{
    if (line == NULL) {
        return false;
    }
    const char *colon = strchr(line, ':');
    const char *p = colon ? colon + 1 : line;
    while (*p == ' ') {
        p++;
    }
    /* a leading quote means an alpha-tag field comes first and the number is the 2nd string */
    return quoted(line, *p == '"' ? 1 : 0, num, cap);
}

static const char *or_dash(const char *s)
{
    return (s != NULL && s[0] != '\0') ? s : "--";
}

int devinfo_rows(const devinfo_view_t *v, char rows[][DEVINFO_ROW_LEN], int max_rows)
{
    int n = 0;
    char a[24];
#define ROW(...)                                                                                   \
    do {                                                                                           \
        if (n < max_rows) {                                                                        \
            snprintf(rows[n], DEVINFO_ROW_LEN, __VA_ARGS__);                                       \
            n++;                                                                                   \
        }                                                                                          \
    } while (0)

    devinfo_fmt_batt(v->batt_known, v->batt_mv, a, sizeof(a));
    ROW("battery %s", a);
    devinfo_fmt_temp(v->temp_known, v->temp_dc, a, sizeof(a));
    ROW("temperature %s", a);
    ROW("IMEI %s", or_dash(v->imei));
    ROW("IMSI %s", or_dash(v->imsi));
    ROW("ICCID %s", or_dash(v->iccid));
    if (v->msisdn != NULL && v->msisdn[0] != '\0') {
        ROW("number %s", v->msisdn);
    }
    if (v->have_cell && v->mcc != NULL && v->mcc[0] != '\0') {
        ROW("%.20s  %s-%s", or_dash(v->opname), v->mcc, v->mnc != NULL ? v->mnc : "");
    } else {
        ROW("%.20s  --", or_dash(v->opname));
    }
    if (v->have_cell) {
        char b[12];
        if (v->band > 0) {
            snprintf(b, sizeof(b), "%d", v->band);
        } else {
            snprintf(b, sizeof(b), "--");
        }
        ROW("band %s  cell %lu  tac %u", b, v->ci, v->tac);
    } else {
        ROW("band --  cell --  tac --");
    }
    char rp[16] = "--", rq[16] = "--";
    if (v->have_rsrp) {
        snprintf(rp, sizeof(rp), "%d dBm", v->rsrp);
    }
    if (v->have_rsrq) {
        int q = v->rsrq_dtenths < 0 ? -v->rsrq_dtenths : v->rsrq_dtenths;
        snprintf(rq, sizeof(rq), "%s%d.%d dB", v->rsrq_dtenths < 0 ? "-" : "", q / 10, q % 10);
    }
    ROW("rsrp %s  rsrq %s", rp, rq);
    ROW("carrier %.24s  %s", or_dash(v->carrier), !v->bearer_known ? "--" : (v->beam ? "beam" : "direct"));
    ROW("apn %s", (v->apn != NULL && v->apn[0] != '\0') ? v->apn : "(network's choice)");
#undef ROW
    return n;
}

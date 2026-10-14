/* devinfo_fmt.h -- pure row builder + parsers for the "Device info" screen (scr_devinfo.c).
 * No ESP-IDF; host-tested (host/test_devinfo.c) and used by host/render_png.c for the PNG. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DEVINFO_ROW_LEN 48
#define DEVINFO_MAX_ROWS 12
#define DEVINFO_USB_MV 4300 /* at or above this the modem is on a plug, not a cell */

typedef struct {
    bool batt_known;
    int batt_mv;
    bool temp_known;
    int temp_dc; /* tenths of C */
    const char *imei, *imsi, *iccid, *msisdn, *opname; /* "" = unknown (msisdn "" = row omitted) */
    bool have_cell;
    const char *mcc, *mnc;
    unsigned tac;
    unsigned long ci;
    int band; /* 0 = unknown */
    bool have_rsrp;
    int rsrp;
    bool have_rsrq;
    int rsrq_dtenths; /* tenths of dB */
    const char *carrier; /* preset label, "" = none */
    const char *apn;     /* "" = network's choice */
    bool bearer_known;
    bool beam;
} devinfo_view_t;

/* Builds the screen's rows in order. Unknown values print "--". Returns the row count. */
int devinfo_rows(const devinfo_view_t *v, char rows[][DEVINFO_ROW_LEN], int max_rows);

/* "5.20 V", "5.20 V (USB)" at >= DEVINFO_USB_MV, "--" when unknown. */
void devinfo_fmt_batt(bool known, int mv, char *out, size_t cap);
/* "23.5 C", "-0.4 C", "--" when unknown. */
void devinfo_fmt_temp(bool known, int dc, char *out, size_t cap);

/* '+COPS: 0,0,"Verizon",7' -> Verizon. False when there is no quoted name. */
bool devinfo_parse_cops(const char *line, char *name, size_t cap);
/* '+CNUM: ,"+15551234567",145' or '+CNUM: "x","555",129' -> the number. False when empty. */
bool devinfo_parse_cnum(const char *line, char *num, size_t cap);

#ifdef __cplusplus
}
#endif

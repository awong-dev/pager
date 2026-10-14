/* test_devinfo.c -- host test for main/devinfo_fmt.c. No ESP-IDF. */
#include "devinfo_fmt.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            g_failures++;                                                      \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                        \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
        }                                                                      \
    } while (0)

static void batt(bool k, int mv, const char *want)
{
    char b[32];
    devinfo_fmt_batt(k, mv, b, sizeof(b));
    CHECK(strcmp(b, want) == 0, "batt %d -> \"%s\" want \"%s\"", mv, b, want);
}

static void temp(bool k, int dc, const char *want)
{
    char b[32];
    devinfo_fmt_temp(k, dc, b, sizeof(b));
    CHECK(strcmp(b, want) == 0, "temp %d -> \"%s\" want \"%s\"", dc, b, want);
}

int main(void)
{
    batt(true, 5203, "5.20 V (USB)");
    batt(true, 4300, "4.30 V (USB)");
    batt(true, 4299, "4.30 V");
    batt(true, 3925, "3.93 V");
    batt(false, 0, "--");
    batt(true, 0, "--");
    temp(true, 235, "23.5 C");
    temp(true, -4, "-0.4 C");
    temp(true, 0, "0.0 C");
    temp(true, -105, "-10.5 C");
    temp(false, 0, "--");

    char o[40];
    CHECK(devinfo_parse_cops("+COPS: 0,0,\"Verizon\",7", o, sizeof(o)) && !strcmp(o, "Verizon"), "cops");
    CHECK(!devinfo_parse_cops("+COPS: 0", o, sizeof(o)), "cops unregistered");
    CHECK(devinfo_parse_cops("+COPS: 0,0,\"A very long operator name here\",7", o, 8) && !strcmp(o, "A very "), "cops trunc");
    CHECK(devinfo_parse_cnum("+CNUM: ,\"+15551234567\",145", o, sizeof(o)) && !strcmp(o, "+15551234567"), "cnum bare");
    CHECK(devinfo_parse_cnum("+CNUM: \"Mine\",\"5551234\",129", o, sizeof(o)) && !strcmp(o, "5551234"), "cnum named");
    CHECK(!devinfo_parse_cnum("+CNUM: ,\"\",129", o, sizeof(o)), "cnum empty");
    CHECK(!devinfo_parse_cnum("", o, sizeof(o)) && !devinfo_parse_cnum(NULL, o, sizeof(o)), "cnum none");

    static char rows[DEVINFO_MAX_ROWS][DEVINFO_ROW_LEN];
    devinfo_view_t u = {0};
    int n = devinfo_rows(&u, rows, DEVINFO_MAX_ROWS);
    CHECK(n == 10, "unknown view rows %d want 10 (no number row)", n);
    CHECK(!strcmp(rows[0], "battery --") && !strcmp(rows[2], "IMEI --") && !strcmp(rows[6], "band --  cell --  tac --"),
          "unknown rows: %s | %s | %s", rows[0], rows[2], rows[6]);

    devinfo_view_t v = {0};
    v.batt_known = true; v.batt_mv = 5203; v.temp_known = true; v.temp_dc = 235;
    v.imei = "352656100123456"; v.imsi = "310280123456789"; v.iccid = "89148000001234567890";
    v.msisdn = "+15551234567"; v.opname = "Verizon Wireless"; v.have_cell = true; v.mcc = "311"; v.mnc = "480";
    v.tac = 12345; v.ci = 123456789; v.band = 13; v.have_rsrp = true; v.rsrp = -95; v.have_rsrq = true;
    v.rsrq_dtenths = -95; v.carrier = "Soracom"; v.apn = "soracom.io"; v.bearer_known = true; v.beam = true;
    n = devinfo_rows(&v, rows, DEVINFO_MAX_ROWS);
    CHECK(n == 11, "full rows %d want 11", n);
    const char *want[] = {"battery 5.20 V (USB)", "temperature 23.5 C", "IMEI 352656100123456",
        "IMSI 310280123456789", "ICCID 89148000001234567890", "number +15551234567",
        "Verizon Wireless  311-480", "band 13  cell 123456789  tac 12345", "rsrp -95 dBm  rsrq -9.5 dB",
        "carrier Soracom  beam", "apn soracom.io"};
    for (int i = 0; i < 11 && i < n; i++) {
        CHECK(!strcmp(rows[i], want[i]), "row %d \"%s\" want \"%s\"", i, rows[i], want[i]);
        CHECK(strlen(rows[i]) < 40, "row %d too long for 296 px at 12 px (%zu chars)", i, strlen(rows[i]));
    }
    CHECK(devinfo_rows(&v, rows, 3) == 3, "max_rows respected");

    if (g_failures == 0) {
        printf("test_devinfo: all passed\n");
    }
    return g_failures ? 1 : 0;
}

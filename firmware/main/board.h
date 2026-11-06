/* board.h -- which PCB this firmware is running on (docs/CARRIER_V02_FIRMWARE_DESIGN.md §2).
 *
 * Exists on both boards: BOARD_BREADBOARD (Adafruit breakouts, every pager built before
 * Nov 2026) and BOARD_CARRIER_V02 (hardware/carrier). Naming: carrier.c is the CELLULAR
 * carrier (APN); the PCB variant is the "board" everywhere.
 *
 * board_init() must run after nvs_flash_init() and BEFORE anything configures IO12/IO38/IO42
 * (rail_init, disp, ui): the same pads are Friend ENA / SRAM CS / MISO on the breadboard but
 * FL_PWM / CHG_STAT1 / CHG_STAT2 on the carrier. Selection: NVS "board"/"kind" wins if set;
 * otherwise the accelerometer WHO_AM_I (0x33 breadboard, 0x11 carrier) is probed and saved as
 * "last"; if the probe fails "last" is used, else the breadboard default (logged loudly).
 *
 * Power effect: board_init() is one I2C read and at most one NVS write, once per boot; the
 * accessors are free.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BOARD_AUTO = 0, BOARD_BREADBOARD = 1, BOARD_CARRIER_V02 = 2 } board_kind_t;
typedef enum { BOARD_SRC_FIXED, BOARD_SRC_AUTO, BOARD_SRC_LAST, BOARD_SRC_DEFAULT } board_src_t;

void board_init(void); // see above; power effect: one I2C read + at most one NVS write

board_kind_t board_get(void);       // BOARD_BREADBOARD or BOARD_CARRIER_V02 (never AUTO)
const char *board_name(void);       // "breadboard" / "carrier-v0.2"
board_src_t board_source(void);
const char *board_source_name(void); // "fixed" / "auto" / "last" / "default"
bool board_is_carrier(void);
bool board_who_am_i(uint8_t *who);  // WHO_AM_I read at board_init(); false if the probe failed

/* Per-pin helpers the existing code switches on; -1 = not present on this board. */
int board_disp_vcc_en_pin(void);
int board_disp_srcs_pin(void);
int board_disp_miso_pin(void);

bool board_has_haptic(void);
bool board_has_piezo(void);
bool board_has_frontlight(void);
bool board_has_ext_led(void);
bool board_has_fuel_gauge(void);

/* Console `board` support. Persist the requested kind (reboot to apply). */
bool board_set_kind(board_kind_t k);
board_kind_t board_stored_kind(void);
/* Raw levels of VBUS_DET / CHG_STAT1 / CHG_STAT2 (sets them to plain inputs first; carrier only). */
void board_read_charger_pins(int *vbus, int *stat1, int *stat2);

#ifdef __cplusplus
}
#endif

#endif // BOARD_H

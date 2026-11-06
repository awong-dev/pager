/* fuel.h -- MAX17048 fuel gauge + BQ25185 charger pins on the carrier (design §4). Device only;
 * the conversions and the shutdown rule are in fuel_fmt.h. I2C_NUM_1 is shared with the accel:
 * every transaction is under accel_bus_lock(). Not on the breadboard (board_has_fuel_gauge()). */
#ifndef FUEL_H
#define FUEL_H

#include <stdbool.h>
#include <stdint.h>

#include "fuel_fmt.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool ok;         /* false: the I2C read failed, other fields are meaningless */
    int mv, pct;     /* pct 0..100 */
    int crate_milli; /* thousandths of %/h, signed */
} fuel_t;

/* Carrier only (no-op on the breadboard): releases any ext1 pad hold on IO15 left by a Shutdown
 * deep sleep, sets IO15/IO38/IO42 as plain inputs (no pulls), reads VERSION and logs it.
 * Power effect: one I2C read. Call after board_init(). */
void fuel_init(void);
/* One gauge read (VCELL + SOC + CRATE, three I2C transactions, ~1 ms). Power effect: none beyond the bus. */
bool fuel_read(fuel_t *out);
/* Charger state from IO15/IO38 (plain GPIO reads, no bus). */
fuel_state_t fuel_state(void);
/* Raw levels for the console. */
void fuel_pins(int *vbus, int *stat1, int *stat2);
/* MAX17048 quick-start (MODE bit 14); console only. Power effect: none. */
bool fuel_quick_start(void);
/* Raw register read for the console. */
bool fuel_reg_read(uint8_t reg, uint16_t *val);

#ifdef __cplusplus
}
#endif

#endif /* FUEL_H */

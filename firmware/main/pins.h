// Single source of truth for every GPIO the firmware drives (firmware/README.md
// "Hardware" has the same table with the physical Walter header positions).
// Never hardcode a pin elsewhere.
//
// Rewired 29 Sep 2026 (owner): display moved to the Adafruit eInk Breakout
// Friend, accelerometer is the Adafruit LIS3DH breakout, and the pins were
// remapped so the display + button take the whole left Walter header (pins
// 5-14, in the Friend's own header order) and the CardKB + LIS3DH take the
// right header. Walter-internal pins never to touch: IO14/IO21/IO45/IO46/
// IO47/IO48 (modem UART, reset, wake), IO19/IO20 (USB), IO3 (strapping,
// testpoint only).
//
// Rewired again 2 Oct 2026 (owner): the Friend's own header plugs straight
// onto Walter left-header pins 5-13 in the Friend's header order (ENA, BUSY,
// RST, SRCS, D/C, ECS, MOSI, MISO, SCK read top to bottom), so D/C, ECS and
// MOSI shifted down one pin each and MISO is now wired (pin 12, IO42). The
// wake button stays on IO1 (header pin 14).
//
// Rewired again 3 Oct 2026 (owner): power now comes from an Adafruit 6092
// (bq25185 + TLV62569) board with a 3.7 V 2500 mAh LiPo, not the LiFePO4
// 18650 + LFP charger. The board's "4.5V" (SYS) output feeds Walter VIN; its
// always-on "3V" buck feeds the eInk Friend VIN and the LIS3DH breakout VIN
// directly (neither is gated by any Walter GPIO any more). Walter's own
// switched 3V3-OUT (PAGER_PIN_3V3_EN, IO0) now feeds only the CardKB. The
// wake button moved from GND to the board's always-on 3V, so it is
// active-high now, and shares the ext1 wake with the LIS3DH's INT1 (one
// ext0 RTC GPIO is no longer enough; ext1's bitmask covers both).
//
// Rewired again 5 Oct 2026 (owner): right (VIN-side) header final. CardKB
// SDA/SCL moved back to pins 25/24 (IO10/IO9); LIS3DH INT1/SDA/SCL moved to
// pins 17/16/15 (IO6/IO5/IO4). The wake button also moved this same day,
// from IO1 (left header pin 14, now unused) to IO8 (right header pin 23,
// still active-high, still ext1, still shared with the LIS3DH's INT1).
// Pins 22 (IO18) and 19 (IO15) are now unused.

#ifndef PINS_H
#define PINS_H

// Walter 3V3-OUT rail enable (header pin 4, "DFU/3V3_EN"). Off by default on
// power-up/reset; GPIO0 must be driven LOW to turn it on. Rewired 3 Oct 2026
// (owner): this rail now feeds ONLY the CardKB (the LIS3DH and the eInk
// Friend are both powered from the Adafruit 6092 power board's always-on
// "3V" rail, off-Walter entirely). rail.c drives this pin low (on) in
// rail_on() and high (off) in rail_off(), exactly like ENA, so the CardKB
// follows the attentive window instead of staying on for good. GPIO0 is a
// boot strapping pin but is safe to repurpose as a plain GPIO output once
// app_main() is running (strapping is sampled only during reset/boot).
#define PAGER_PIN_3V3_EN 0  // active-low, CardKB supply gate

// Display: Adafruit eInk Breakout Friend (SSD1680 panel), Walter left header
// pins 5-13 top to bottom in the Friend's own header order. The Friend's VIN
// is wired to the Adafruit 6092 power board's always-on "3V" rail (not
// Walter VIN or Walter 3V3-OUT); its onboard MIC5225-3.3 regulator, gated by
// ENA, powers the panel only -- the CardKB is on its own gate now
// (PAGER_PIN_3V3_EN above). SPI goes through the GPIO matrix (none of these
// are the FSPI IO_MUX pins), which is fine at the 4 MHz disp.c uses (matrix
// limit is 40 MHz). SDCS is not wired.
#define PAGER_PIN_DISP_VCC_EN 12  // Friend ENA: regulator enable, ACTIVE-HIGH, pulled up on the Friend
#define PAGER_DISP_VCC_EN_ON 1
#define PAGER_DISP_VCC_EN_OFF 0
#define PAGER_PIN_DISP_BUSY 11
#define PAGER_PIN_DISP_RST 13
#define PAGER_PIN_DISP_SRCS 38  // Friend SRAM chip select: held HIGH (deselected), the SRAM is unused
#define PAGER_PIN_DISP_DC 39  // IO39 is a JTAG pad (MTCK); unused since the USB console owns the JTAG function
#define PAGER_PIN_DISP_CS 40  // Friend ECS
#define PAGER_PIN_DISP_MOSI 41
#define PAGER_PIN_DISP_MISO 42  // Friend SRAM MISO, unused today (SRAM not read); wired through for the Friend's fixed header order
#define PAGER_PIN_DISP_SCK 2

// Button (header pin 23, right header). Rewired 3 Oct 2026 (owner): connects
// the button to the power board's always-on 3V, not GND, so it is
// active-high. Rewired again 5 Oct 2026 (owner): moved from IO1 (header pin
// 14, left header, now unused) to IO8 (header pin 23, right header) as part
// of the same-day right-header rework; still active-high to the board's 3V,
// still an RTC GPIO, still shares the ext1 wake with the LIS3DH's INT1.
#define PAGER_PIN_BUTTON 8  // active-high to the board's 3V, ext1 shared with LIS3DH INT1

// Keyboard (CardKB, I2C addr 0x5F) on I2C_NUM_0, right header pins 25/24
// (moved from pins 16/15 / IO5,IO4 on 3 Oct 2026, then back to 25/24 /
// IO10,IO9 on 5 Oct 2026 -- owner decision, final).
// Powered from Walter's own switched 3V3-OUT (PAGER_PIN_3V3_EN, IO0 above),
// not the Friend's 3V3 output, so ui.c releases these two pads (driven low)
// whenever rail.c drops the rail -- otherwise the ESP32 back-powers the
// CardKB MCU through its I/O clamp diodes.
#define PAGER_PIN_KB_SDA 10
#define PAGER_PIN_KB_SCL 9
#define PAGER_I2C_ADDR_CARDKB 0x5F

// Motion: Adafruit LIS3DH breakout (I2C addr 0x18, SDO/SA0 open), right
// header pins 17/16/15 (moved from pins 23/22/19 / IO8,IO15,IO18 on 5 Oct
// 2026 -- owner decision, final). Its own bus, I2C_NUM_1, separate from the
// CardKB's I2C_NUM_0, and powered from the Adafruit 6092 power board's
// always-on "3V" rail (not Walter 3V3-OUT, not gated by any Walter GPIO) so
// it stays alive through every rail_off() and every light sleep. 3.3 V on
// the breakout's VIN also makes its level shifter transparent: SDA/SCL
// pull-ups sit at the ESP32's own I/O rail, never at a 5 V USB VIN. Sharing
// the CardKB's bus would let these always-on pull-ups feed the unpowered
// CardKB. INT1 is an RTC GPIO (IO6 still is, same as IO8 was): it is an
// ext1 light-sleep wake source (net.cpp), shared with the button, push-pull
// active-high, 3.3 V logic.
#define PAGER_PIN_LIS3DH_INT1 6
#define PAGER_I2C_ADDR_LIS3DH 0x18
#define PAGER_PIN_ACCEL_SDA 5  // own I2C_NUM_1 bus, not the CardKB's
#define PAGER_PIN_ACCEL_SCL 4  // own I2C_NUM_1 bus, not the CardKB's

// LTE_WAKE0 (schematic name): a modem input, unused by firmware and by the
// vendored library today. docs/SLEEP_PAGE_LOSS_BRIEF.md §6 item F: the
// debug-build `wake0`/`sleeptest ... wake0_ms` instrumentation pulses it
// to see whether it affects the modem's UART wake latency. Untouched
// (input, no pull) unless that instrumentation is used.
#define PAGER_PIN_WAKE0 46

// MODEM_RESET (Walter-internal, active-low reset of the Sequans). Documented
// exception to "never touch IO45": only net_airplane_hold_modem() (net.cpp)
// drives it, to keep the modem in reset for the whole boot in airplane mode
// (NVS net/airplane). The vendored walter-modem library drives the same line
// itself in reset(); the two never run in one boot.
#define PAGER_PIN_MODEM_RESET 45

#endif // PINS_H

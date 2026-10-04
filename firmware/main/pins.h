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

#ifndef PINS_H
#define PINS_H

// Walter 3V3-OUT rail enable (header pin 4, "DFU/3V3_EN"). Off by default on
// power-up/reset; GPIO0 must be driven LOW to turn it on. Since the 30 Sep
// rewiring this rail feeds ONLY the LIS3DH breakout, so rail.c drives it low
// at boot and never releases it (the accelerometer has to stay alive to wake
// the pager). The display and CardKB are gated by the eInk Friend's ENA
// instead (PAGER_PIN_DISP_VCC_EN below). GPIO0 is a boot strapping pin but
// is safe to repurpose as a plain GPIO output once app_main() is running
// (strapping is sampled only during reset/boot).
#define PAGER_PIN_3V3_EN 0  // active-low, held low for good

// Display: Adafruit eInk Breakout Friend (SSD1680 panel), Walter left header
// pins 5-13 top to bottom in the Friend's own header order. The Friend's VIN
// is wired to Walter VIN; its onboard MIC5225-3.3 regulator, gated by ENA,
// powers the panel AND (via the Friend's 3V3 output pin) the CardKB, so ENA
// is the one peripheral power gate rail.c toggles. SPI goes through the GPIO
// matrix (none of these are the FSPI IO_MUX pins), which is fine at the
// 4 MHz disp.c uses (matrix limit is 40 MHz). SDCS is not wired.
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

// Button (header pin 14). ext0 light-sleep wake source (RTC GPIO).
#define PAGER_PIN_BUTTON 1  // active-low, RTC GPIO

// Keyboard (CardKB, I2C addr 0x5F) on I2C_NUM_0, right header pins 16/15
// (moved from pins 25/24 / IO10,IO9 on 3 Oct 2026 -- owner decision).
// Powered from the eInk Friend's 3V3 output (gated by ENA together with the
// panel), so ui.c releases these two pads (driven low) whenever rail.c drops
// the rail -- otherwise the ESP32 back-powers the CardKB MCU through its I/O
// clamp diodes.
#define PAGER_PIN_KB_SDA 5
#define PAGER_PIN_KB_SCL 4
#define PAGER_I2C_ADDR_CARDKB 0x5F

// Motion: Adafruit LIS3DH breakout (I2C addr 0x18, SDO/SA0 open), right
// header pins 23/22/19. Its own bus, I2C_NUM_1, separate from the CardKB's
// I2C_NUM_0, and powered from Walter's 3V3-OUT (header pin 26, held on for
// good by PAGER_PIN_3V3_EN) so it stays alive through every rail_off() and
// every light sleep. 3.3 V on the breakout's VIN also makes its level
// shifter transparent: SDA/SCL pull-ups sit at the ESP32's own I/O rail, never
// at a 5 V USB VIN. Sharing the CardKB's bus would let these always-on
// pull-ups feed the unpowered CardKB. INT1 is an RTC GPIO: it is the ext1
// light-sleep wake source (net.cpp), push-pull active-high, 3.3 V logic.
#define PAGER_PIN_LIS3DH_INT1 8
#define PAGER_I2C_ADDR_LIS3DH 0x18
#define PAGER_PIN_ACCEL_SDA 15  // own I2C_NUM_1 bus, not the CardKB's
#define PAGER_PIN_ACCEL_SCL 18  // own I2C_NUM_1 bus, not the CardKB's

// LTE_WAKE0 (schematic name): a modem input, unused by firmware and by the
// vendored library today. docs/SLEEP_PAGE_LOSS_BRIEF.md §6 item F: the
// debug-build `wake0`/`sleeptest ... wake0_ms` instrumentation pulses it
// to see whether it affects the modem's UART wake latency. Untouched
// (input, no pull) unless that instrumentation is used.
#define PAGER_PIN_WAKE0 46

#endif // PINS_H

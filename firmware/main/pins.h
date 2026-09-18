// GPIO numbers are provisional (firmware/README.md); this is the single source of truth
// Never hardcode a pin elsewhere.

#ifndef PINS_H
#define PINS_H

// Board 3V3 peripheral rail enable. Off by default on power-up/reset;
// GPIO0 must be driven LOW to turn it on. Found during hardware bring-up
// (docs/DEVICE_TASKS_LOG.md): the display (and likely other peripherals
// downstream of this rail) get no power at all until this is set, separate
// from PAGER_PIN_DISP_VCC_EN's own local gate below. GPIO0 is a boot
// strapping pin but is safe to repurpose as a plain GPIO output once
// app_main() is running (strapping is sampled only during reset/boot).
#define PAGER_PIN_3V3_EN 0  // active-low

// Display (SSD1680, SPI)
#define PAGER_PIN_DISP_SCK 12
#define PAGER_PIN_DISP_MOSI 11
#define PAGER_PIN_DISP_CS 10
#define PAGER_PIN_DISP_DC 16
#define PAGER_PIN_DISP_RST 17
#define PAGER_PIN_DISP_BUSY 18
#define PAGER_PIN_DISP_VCC_EN 15  // active-low P-MOSFET gate

// Keyboard (CardKB, I2C addr 0x5F)
#define PAGER_PIN_KB_SDA 8
#define PAGER_PIN_KB_SCL 9
#define PAGER_I2C_ADDR_CARDKB 0x5F

// Motion (LIS3DH, I2C addr 0x18)
#define PAGER_PIN_LIS3DH_INT1 2
#define PAGER_I2C_ADDR_LIS3DH 0x18

// Button
#define PAGER_PIN_BUTTON 1  // active-low, RTC GPIO

#endif // PINS_H

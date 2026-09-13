# School Pager Firmware

ESP-IDF 5.x project for the Walter device (ESP32-S3 + Sequans LTE-M modem).

## Dependencies

- ESP-IDF 5.x
- `dptechnics/walter-modem` component
- Display driver (SSD1680) — TBD in Phase 5

## Hardware Acceptance Tests

- Sleep-mode current: PENDING_HW
- Acceptance test (message latency): PENDING_HW

## Build

Set up ESP-IDF, then:

```bash
idf.py build
```

# keepalive-probe firmware

Standalone ESP-IDF 5.2 / ESP32-S3 test for one question: when `AT+SQNSMQTTCONNECT` is given a
keepalive, does the Sequans GM02SP send MQTT PINGREQs by itself while the session is idle under
eDRX with the ESP32 light-sleeping as the pager does? (Prior claim: `firmware/main/xport_lte.cpp`
lines 40-48, "zero PINGREQs in 8+ minutes".) The broker in `../broker/` records packets and is
the judge; this log only tells you when the session died relative to the keepalive.

## What it does

1. Boots, logs its config, keeps the pager's CardKB/e-ink rails off (pins from `firmware/main/pins.h`).
2. Modem bring-up as `net_bringup()`: NO_RF, PDP context, `configEDRX` ("0010"/"0001", granted values
   logged), `AT+SQNIPSCFG=1,100`, `configPSM(DISABLE)`, FULL, wait for attach, read modem clock.
3. TLS profile (VALIDATION_NONE, named non-empty CA slot) -> `mqttConfig(client id, no user/pass)` ->
   `mqttConnect(host, port, keepalive)` -> subscribe `probe/<clientid>/down`.
4. Idles forever: no publishes, no re-subscribes. Light sleep in slices like `net_sleep()` (RTS high,
   `gpio_sleep_sel_dis` on RTS, restore after wake), then 200 ms yield and the pager's per-wake drain
   probe (CRLF + one bare `AT`, wait until answered). That probe is the only idle AT traffic.
5. Logs every MQTT event with `T+<uptime s> <modem UTC>`, incl. `+SQNSMQTTONDISCONNECT` reason rc and
   incoming messages (payload fetched with `mqttReceive`). Heartbeat line every 60 s.
6. On disconnect: logs `SESSION DEAD rc=...` and does not reconnect (unless `PROBE_RECONNECT`).

## Configure

`idf.py menuconfig` -> "Keepalive probe", or put the values in `sdkconfig.defaults` (a stale
`sdkconfig` overrides defaults: delete it and `idf.py reconfigure` after editing the file).

| Option | Default | Meaning |
|---|---|---|
| PROBE_BROKER_HOST / _PORT | CHANGE_ME / 8883 | broker (TLS) |
| PROBE_CLIENT_ID | kaprobe1 | client id; down topic `probe/<id>/down` |
| PROBE_KEEPALIVE_S | 60 | keepalive sent in CONNECT |
| PROBE_EDRX (+ _VALUE/_PTW) | y, 0010/0001 | n = eDRX disabled (control) |
| PROBE_LIGHT_SLEEP | y | n = ESP32 stays awake (control) |
| PROBE_SLEEP_SLICE_MS | 20000 | main loop sleep-mode cadence (`PAGER_WAKE_INTERVAL_SLEEP_MS`) |
| PROBE_POST_WAKE_YIELD_MS | 200 | `PAGER_POST_WAKE_YIELD_MS` |
| PROBE_WAKE_PROBE (+ _WAIT_MS) | y, 15000 | per-wake drain probe; n = silent host |
| PROBE_RECONNECT | n | reconnect after disconnect |
| PROBE_AT_TRACE | n | raw AT TX/RX via the WalterModem log tag |
| PROBE_APN | "" | carrier default, as the pager |
| PROBE_CA_SLOT / PROBE_TLS_PROFILE | 10 / 5 | deliberately not the pager's 12,13 / 2,3 |

## Build and flash

```
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.2_py3.11_env
. ~/src/esp/esp-idf/export.sh
cd scratch/keepalive-probe/fw
idf.py set-target esp32s3 && idf.py build
```

Flash (a sleeping pager does not answer the default reset; use `usb_reset`):

```
python -m esptool --chip esp32s3 -p /dev/cu.usbmodemXXXX --before usb_reset --after hard_reset \
  write_flash --flash_mode dio --flash_size 16MB --flash_freq 80m \
  0x0 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin \
  0x10000 build/keepalive_probe.bin
```

(or `idf.py -p PORT flash` when the board is awake). This overwrites the pager firmware including
OTA layout; reflash the pager image afterwards. Keep one long-lived serial reader: reopening the
USB-JTAG port can reset the chip.

## Reading the log

- The USB-JTAG console is dead during light sleep. Lines appear in the ~200 ms-15 s awake window
  after each wake; expect gaps. A 20 ms drain delay precedes each sleep so the last line gets out.
  The broker capture is authoritative; the log gives context.
- `granted eDRX=... PTW=...` confirms eDRX is really in effect (control run: no line).
- `MQTT CONNECTED rc=0`, `SUBSCRIBED`: session up. Note the T+ of CONNECTED.
- `heartbeat state=subscribed` every 60 s. The first heartbeat showing `DISCONNECTED` plus
  `+SQNSMQTTONDISCONNECT reason rc=N` brackets when the session died. Compare with the broker's
  last-packet time and the keepalive (broker normally drops at 1.5x keepalive).
- `MQTT MESSAGE` lines show downlink delivery. Publish to `probe/<clientid>/down` from the broker
  side; check delay vs. wake slice.
- Modem UTC comes from one `getClock` at start; later stamps are derived from esp_timer, which keeps
  counting through light sleep.

## A/B matrix

| Run | PROBE_EDRX | PROBE_LIGHT_SLEEP | PROBE_WAKE_PROBE | Separates |
|---|---|---|---|---|
| A (pager-like) | y | y | y | the claim under test |
| B | y | y | n | does the host's wake AT traffic itself matter (silent host) |
| C (control) | n | n | - | "modem never pings" vs "doesn't ping while asleep" |
| D | n | y | y | sleep without eDRX |

PINGREQs in C but not A means the modem pings only when awake or non-eDRX. None in C means the modem
never pings on its own (the prior claim). Use the same keepalive (60 s) and run >= 5 keepalive
periods (5+ minutes) per run.

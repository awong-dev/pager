# PINGREQ handoff — does the Sequans send MQTT keepalive on its own?

Written 12 Oct 2026. Tooling lives in `scratch/keepalive-probe/` (untracked, not committed).

## Result so far (11 Oct 2026, 23:56 PDT)

**Yes, at keepalive 60.** Bench pager running the probe firmware: eDRX 0010/0001 granted, the ESP32
light-sleeping with the pager's RTS steps, no application traffic after SUBSCRIBE. Soracom direct
bearer (egress 44.249.137.126), TLS 1.2 `ECDHE-RSA-AES256-GCM-SHA384`, TLS profile 2 / CA slot 12.

| Device → broker (PDT) | Gap |
|---|---|
| CONNECT 23:56:04.09 (`keepalive=60s`), SUBSCRIBE 23:56:04.33 | — |
| PINGREQ 23:57:04.43 | 60.1 s |
| PINGREQ 23:58:07.92 | 63.5 s |
| PINGREQ 23:59:04.33 | 56.4 s |

Each one was answered with a PINGRESP within about 2 ms.

This contradicts `firmware/main/xport_lte.cpp:40-48`: measured 21 Sep 2026 on AT&T/US Mobile, keepalive 480,
modem firmware LR8.2.1.0-61488, zero PINGREQs in 8+ minutes. Until that's explained, **keep**
`PAGER_MQTT_PING_S` (the 300 s re-SUBSCRIBE).

## Still to test

Change one variable at a time. Each run: 20 min idle, or until mosquitto drops the client.

1. **Keepalive 480** (the production value), same SIM and bearer. Expect PINGREQ every ~480 s, or a
   drop at ~720 s. This is the most likely explanation for the old result.
2. **AT&T / US Mobile SIM.** Set the probe's APN to the carrier's (see `firmware/main/carrier.c`
   presets), with user/pass empty.
3. **On a freshly provisioned device.** This also records the modem firmware version (`AT+CGMR`) next to the result.
4. **Optional.** `PROBE_EDRX=n` and `PROBE_LIGHT_SLEEP=n` (control run), and `PROBE_WAKE_PROBE=n` (ESP32 sends
   nothing to the modem while idle).
5. **Downlink during eDRX.** After the keepalive verdict, run `./send-down.sh kaprobe1 hello` on the Pi.
   Serial should show `MQTT MESSAGE` on a later wake.

Beam can't be tested with this setup: Beam terminates at Soracom, so the Pi would never see the
device's own packets.

## How to run

### Broker (Orange Pi 192.168.50.109; public <home-public-ip>; router forwards TCP 8883 to the Pi)

```sh
scp -r scratch/keepalive-probe/broker orangepi@192.168.50.109:~/
ssh orangepi@192.168.50.109
cd ~/broker
./setup.sh                      # once
./gen-certs.sh <home-public-ip>    # once
./run.sh                        # Ctrl-C to stop
./analyze.sh runs/<timestamp>
```

- Watch `runs/<ts>/tlsterm.log`: TLS handshake, CONNECT with keepalive, SUBSCRIBE, then the PINGREQ lines.
- `mosquitto.log` shows `Received PINGREQ from kaprobe1`, or the timeout disconnect.
- Mosquitto lives in `/usr/sbin`; `run.sh` handles that.

### Probe firmware (`scratch/keepalive-probe/fw`)

Settings go in `fw/sdkconfig.defaults`. Then run `rm sdkconfig` and rebuild:

```
CONFIG_PROBE_BROKER_HOST="<home-public-ip>"
CONFIG_PROBE_KEEPALIVE_S=60          # 480 for run 1
CONFIG_PROBE_APN="soracom.io"        # carrier APN for run 2
CONFIG_PROBE_APN_USER="sora"         # "" for non-Soracom
CONFIG_PROBE_APN_PASS="sora"
CONFIG_PROBE_TLS_PROFILE=2           # the pager's profile and slot
CONFIG_PROBE_CA_SLOT=12
CONFIG_PROBE_AT_TRACE=y
```

```sh
cd scratch/keepalive-probe/fw
export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.2_py3.11_env
. ~/src/esp/esp-idf/export.sh
idf.py build
cd build && python -m esptool --chip esp32s3 -p /dev/cu.usbmodem101 \
  --before usb_reset --after hard_reset write_flash @flash_args
```

- Quit `screen` before flashing. It holds the port.
- The console shows bring-up and then goes mostly quiet once light sleep starts. The Pi's logs are the record.

## Gotchas found on the way

- **The modem needs a certificate in the CA slot** the TLS profile names, even with VALIDATION_NONE. With an
  empty slot, `AT+SQNSMQTTCFG=...,2` answers `+CME ERROR: 4` on every retry.
  - The probe now writes the pager's placeholder CA into slot 12, but only if the slot is empty, as `ensure_ca_slot_populated()` does.
  - This modem's slot 12 was probably empty because its pager runs over Beam, which has no TLS. Unconfirmed: which CA line the serial log printed wasn't recorded.
- **Soracom needs PAP** credentials sora/sora on the PDP context (raw `AT+CGAUTH`, as `net.cpp` `apply_pdp_auth()` does).
- **The probe's AT trace logs the MQTT password** (`AT+SQNSMQTTCFG`). That's fine for the `kaprobe1` test login, but never
  paste a real pager's trace.

## Restoring the bench pager afterwards

The probe uses ESP-IDF's single-app partition table. It overwrote:
- the partition table at 0x8000;
- 0xf000, the pager's `otadata`;
- from 0x10000 up, the pager's `assets` partition (fonts).

A plain OTA or app-only flash will not recover it. Do a **full flash from `firmware/`**: bootloader, partition table,
otadata and app, **plus the assets image** (`tools/mkassets.py` output). Then provision it as a new device.
NVS at 0x9000 is the same offset in both tables, so the old identity may have survived. Don't rely on that; reprovision.

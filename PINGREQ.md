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

### AT&T / US Mobile Dark Star, keepalive 60 (14 Oct 2026, 21:52 PDT)

**Yes, also on AT&T.** Same probe and bench pager, APN `ereseller`, no PDP auth, eDRX 0010/0001,
light sleep. Peer 107.122.87.33 (AT&T). CONNECT 21:52:09.30 PDT (`keepalive=60s`), SUBSCRIBE
21:52:09.38, then seven PINGREQs, 21:53:09.63 to 21:59:10.02, gaps 59.3-60.7 s, each answered within 2 ms.
So the carrier is not what silenced pings on 21 Sep. What's left: keepalive 480 vs 60, EMQX vs
mosquitto (and the indirect `recv_pkt` method), full pager firmware vs idle probe, and modem
firmware (not recorded here; the next probe build logs `AT+CGMR`).

### AT&T / US Mobile Dark Star, keepalive 480 (14 Oct 2026, 22:04 PDT)

**No PINGREQ: reproduces 21 Sep.** Same probe, SIM and settings; only the keepalive changed. Peer
107.115.80.33 (AT&T). CONNECT 22:04:18.42 PDT (`keepalive=480s`), SUBSCRIBE 22:04:18.50, then
nothing from the device. The broker closed the connection at 22:16:19.02, exactly 1.5 × 480 s
(720.6 s) after CONNECT. The flow survived the whole 12 min, so the 10-13 min idle kill seen on
21 Sep did not happen before the broker's own timeout.

So the modem pings at keepalive 60 and stays silent at 480, on the same carrier. The broker and
measurement method are ruled out as the cause. Next: bisect the keepalive (e.g. 120, 240, 300)
to find where the pings stop.

### AT&T / US Mobile Dark Star, keepalive 240 (14 Oct 2026, 23:12 PDT)

**Yes.** CONNECT 23:12:23.06 PDT (`keepalive=240s`) from 107.115.81.1, SUBSCRIBE 23:12:23.14, PINGREQ
23:16:24.04 (+241.0 s) and 23:20:23.31 (+239.3 s), each answered within 2 ms. The cutoff is between
240 and 480 s.

### AT&T / US Mobile Dark Star, keepalive 360 (14-15 Oct 2026, 23:55 PDT)

**No PINGREQ.** CONNECT 23:55:55.30 PDT (`keepalive=360s`) from 107.115.81.33, SUBSCRIBE 23:55:55.62,
then nothing. The broker closed the connection at 00:04:56.05, 540.7 s after CONNECT (1.5 × 360 s). At 00:07:56,
three minutes after the drop, a new CONNECT with `keepalive=360s` arrived on a new port. The probe has
`PROBE_RECONNECT=n`, so this is presumably the modem resuming on its own (as on 21 Sep); check the
serial log for an `AT+SQNSMQTTCONNECT` the probe didn't send, and whether a SUBSCRIBE followed.

Cutoff on AT&T: between 240 s (pings) and 360 s (none). Either the modem stops pinging somewhere in
240-360 s, or AT&T's idle timeout for TCP is in that range and the ping never arrives. The same
keepalive on Soracom tells them apart.

**Repeat at 360 (15 Oct 2026, 00:18 PDT): same result, and the flow was already dead.** CONNECT 00:18:56.82,
SUBSCRIBE 00:18:56.90, broker drop 00:27:57.06 (540.2 s after CONNECT). Pi capture
(`runs/20261015-001653/ext-8883.pcap`): the device's last packet was an ACK at 00:18:57.14. At the drop the Pi's
31-byte close_notify went out 6 times over 30 s, then FIN twice, with **no ACK and no RST** from 107.122.87.33.
So either AT&T's NAT had dropped the idle mapping before 9 min (and a 360 s PINGREQ may have been sent and
lost), or downlink to the eDRX-sleeping UE was not delivered (unlikely: 20.48 s cycle vs 30 s of retries).
Check the capture for a PINGREQ arriving from a new NAT port around 00:24:56, and run 360 on Soracom.

### Soracom direct, keepalive 360 (15 Oct 2026, 00:35 PDT)

**Yes.** Same probe and bench pager; only the SIM/APN changed (`soracom.io`, PAP sora/sora). CONNECT
00:34:58.49 PDT (`keepalive=360s`) from 34.217.192.18 (AWS us-west-2, Soracom egress), SUBSCRIBE 00:34:58.81, PINGREQ 00:40:59.22 (+360.7 s), answered within 2 ms. `AT+CGMR` on this modem: `UE8.2.1.0` (the LR8.2.1.0 line of 21 Sep). So the modem does ping at
360 s, and the AT&T "no PINGREQ" results at 360/480 are AT&T's network: an idle-TCP/NAT timeout between
240 s and 360 s that silently drops the flow (no RST, no ACK, no modem URC).

### Soracom direct, keepalive sweep (15 Oct 2026, 01:03-02:48 PDT)

Probe built with `PROBE_KEEPALIVE_SWEEP="600,900,1200,1800"`: one session per value, idle 1.6 × keepalive + 30 s,
then DISCONNECT and the next. Broker and capture on the Pi (`runs/20261015-005704`). Every PINGREQ arrived on the
same connection (same Soracom port) and the pager ACKed the PINGRESP, so the flow was alive in both directions.

| Keepalive | CONNECT (PDT) | PINGREQ (PDT) | After CONNECT |
|---|---|---|---|
| 600 s | 01:03:44.71 | 01:13:45.51 | 600.8 s |
| 900 s | 01:20:28.07 | 01:35:27.46 | 899.4 s |
| 1200 s | 01:45:12.30 | 02:05:12.88 | 1200.6 s |
| 1800 s | 02:18:08.22 | 02:48:09.06 | 1800.8 s |

**Soracom direct keeps an idle flow for at least 30 minutes.** AT&T cuts it between 4 and 6 minutes.

## Decision (15 Oct 2026)

The host re-SUBSCRIBE stays the single liveness mechanism: a modem PINGREQ costs the same radio wake, and when a
flow dies silently the modem raises no URC, so only a host round trip (SUBACK timeout) detects it. Per carrier preset
(`firmware/main/carrier.c`):

- **Soracom** (Beam and direct): re-SUBSCRIBE after **840 s** of uplink silence, keepalive **1200 s** (Beam's ceiling;
  Beam drops at 1.5 × = 1800 s, so one missed re-SUBSCRIBE survives). 840 is well under the ≥1800 s measured above.
- **US Mobile Dark Star, Automatic, Carrier default, custom APN**: re-SUBSCRIBE **240 s** (longest proven on AT&T;
  the old 300 s can be past AT&T's cut), keepalive 480 s.

Still to verify: the real firmware on Soracom **Beam** at 840/1200 (a 1 h+ idle soak with `link` not incrementing
and every re-SUBSCRIBE SUBACKed). The probe can only see the direct bearer.

## Still to test

Change one variable at a time. Each run: 20 min idle, or until mosquitto drops the client.

1. **Keepalive 480** (the production value), same SIM and bearer. Expect PINGREQ every ~480 s, or a
   drop at ~720 s. This is the most likely explanation for the old result.
2. ~~**AT&T / US Mobile SIM.**~~ Done at keepalive 60 (above); repeat at 480. Set the probe's APN to the carrier's (see `firmware/main/carrier.c`
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

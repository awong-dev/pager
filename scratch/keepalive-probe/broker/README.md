# Keepalive probe broker (MQTTS + capture kit)

Question answered: after the modem (Sequans GM02SP, AT+SQNSMQTT, MQTT 3.1.1, TLS)
connects with keepalive K and idles, does it ever send PINGREQ (packet type 12)?

Design: device --TLS:8883--> `tlsterm.py` (byte pipe, logs plaintext packet types)
--plaintext 127.0.0.1:1883--> mosquitto. tcpdump records the encrypted leg
(external interface, 8883) and the plaintext leg (lo, 1883, Wireshark decodes MQTT).
`tlsterm.py` does NOT inject or answer anything.

## Copy and set up (on the Mac)
    scp -r scratch/keepalive-probe/broker <user>@<pi-ip>:~/broker
    ssh <user>@<pi-ip>
    cd ~/broker && ./setup.sh           # apt installs; disables distro mosquitto service
    ./gen-certs.sh <public-ddns-or-ip>  # CN/SAN; device uses VALIDATION_NONE so any works

## Reachability (important)
The device is on cellular, so TCP 8883 on the Pi must be reachable from the public internet:
1. Router: port-forward TCP 8883 -> Pi LAN IP:8883 (also allow it in ufw/iptables if enabled).
2. Put the router's public IP or DDNS name in the firmware's PROBE_BROKER_HOST (port 8883).
   (CGNAT on the home ISP will make this impossible; check the router WAN IP equals your public IP.)
3. Verify from outside the LAN (phone hotspot, not home wifi), with run.sh running:

        openssl s_client -connect <public>:8883 </dev/null

   You should see the certificate; a "TLS OK" line appears in tlsterm.log.

## Run a test
    ./run.sh [external-iface]     # iface defaults to the default route's; Ctrl-C to stop
It prints the absolute paths of the two pcaps, keylog, mosquitto log and tlsterm log
(under `runs/<timestamp>/`). Power the device, let it connect with keepalive K (default 60 s),
leave it idle for several multiples of K, then Ctrl-C.

Downlink test (e.g. during eDRX sleep), in another shell: `./send-down.sh <clientid> [msg]`
publishes to `probe/<clientid>/down` (QoS 1; the device must have subscribed).

## Read results
    ./analyze.sh runs/<timestamp>
Lists every MQTT packet (time, direction, type), the CONNECT keepalive, each PINGREQ/PINGRESP
with delta since the previous client packet, mosquitto's view, and device-side FIN/RST from the
8883 pcap. Decrypt the outer capture (command also printed by analyze.sh):

    tshark -r runs/<ts>/ext-8883.pcap -o tls.keylog_file:runs/<ts>/keylog.txt -d tcp.port==8883,tls -Y mqtt

Also check `tlsterm.log` (every chunk with timestamp/direction/length and packet type; handshake
failures show version/cipher/SNI) and `mosquitto.log`.

## Interpretation
- PINGREQ about every K s (mosquitto: "Received PINGREQ from <id>") -> the modem does keepalive.
- No PINGREQ and mosquitto logs "Client <id> has exceeded timeout, disconnecting" at ~1.5 K
  -> the modem does not send keepalive on an idle session.
- Session ended by device FIN/RST in the 8883 pcap with no timeout -> the modem or network dropped
  it (NAT/carrier idle timeout); compare the time with the last client packet.
- TLS handshake failures in tlsterm.log -> read the version/cipher; the permissive context
  allows TLS 1.2/1.3 with `ALL:@SECLEVEL=0` ciphers.

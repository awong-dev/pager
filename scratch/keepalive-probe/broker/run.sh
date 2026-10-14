#!/usr/bin/env bash
# Starts mosquitto + tcpdump (8883 external, 1883 on lo) + tlsterm. Ctrl-C stops all.
# Usage: ./run.sh [external-iface]   (default: iface of the default route)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
[ -f "$here/certs/server.crt" ] || { echo "run ./gen-certs.sh <host-or-ip> first" >&2; exit 1; }
iface="${1:-$(ip route show default | awk '/default/ {print $5; exit}')}"
[ -n "$iface" ] || { echo "cannot determine interface; pass it as arg" >&2; exit 1; }
run="$here/runs/$(date +%Y%m%d-%H%M%S)"
mkdir -p "$run"
sudo -v
cat > "$run/mosquitto.conf" <<CONF
listener 1883 127.0.0.1
allow_anonymous true
log_type all
log_timestamp true
log_timestamp_format %Y-%m-%dT%H:%M:%S
log_dest file $run/mosquitto.log
persistence false
CONF
: > "$run/mosquitto.log"; : > "$run/tlsterm.log"; : > "$run/keylog.txt"
chmod 666 "$run/mosquitto.log"
pids=()
cleanup() {
  trap - INT TERM EXIT
  echo; echo "stopping..."
  for p in "${pids[@]}"; do sudo kill "$p" 2>/dev/null || true; done
  wait 2>/dev/null || true
  sudo chown -R "$(id -u):$(id -g)" "$run" 2>/dev/null || true
  echo "done. run dir: $run"
}
trap cleanup INT TERM EXIT
# Debian/Armbian install the broker in /usr/sbin, which a normal user's PATH lacks.
mosq=$(command -v mosquitto || ls /usr/sbin/mosquitto 2>/dev/null || true)
[ -n "$mosq" ] || { echo "mosquitto not installed -- run ./setup.sh first" >&2; exit 1; }
"$mosq" -c "$run/mosquitto.conf" & pids+=($!)
sleep 1
sudo tcpdump -i "$iface" -U -n -w "$run/ext-8883.pcap" 'tcp port 8883' 2>"$run/tcpdump-ext.err" & pids+=($!)
sudo tcpdump -i lo -U -n -w "$run/lo-1883.pcap" 'tcp port 1883' 2>"$run/tcpdump-lo.err" & pids+=($!)
python3 -u "$here/tlsterm.py" --cert "$here/certs/server.crt" --key "$here/certs/server.key" \
  --keylog "$run/keylog.txt" >> "$run/tlsterm.log" 2>&1 & pids+=($!)
sleep 1
cat <<INFO
external iface : $iface
ext pcap (8883): $run/ext-8883.pcap
lo pcap  (1883): $run/lo-1883.pcap
keylog         : $run/keylog.txt
mosquitto log  : $run/mosquitto.log
tlsterm log    : $run/tlsterm.log
Ctrl-C to stop. Following tlsterm log:
INFO
tail -f "$run/tlsterm.log" &
pids+=($!)
wait

#!/usr/bin/env bash
# Usage: ./analyze.sh <rundir>
set -euo pipefail
run="${1:?usage: $0 <rundir>}"
lo="$run/lo-1883.pcap"; ext="$run/ext-8883.pcap"
command -v tshark >/dev/null || { echo "tshark missing" >&2; exit 1; }
echo "=== All MQTT packets (loopback) ==="
tshark -r "$lo" -d tcp.port==1883,mqtt -Y mqtt -T fields -e frame.time_epoch -e tcp.srcport -e tcp.dstport -e mqtt.msgtype -e mqtt.kalive -e mqtt.clientid \
  -E separator=/t | awk -F'\t' 'BEGIN{split("CONNECT CONNACK PUBLISH PUBACK PUBREC PUBREL PUBCOMP SUBSCRIBE SUBACK UNSUBSCRIBE UNSUBACK PINGREQ PINGRESP DISCONNECT",N," ")}
 {n=split($4,t,","); for(i=1;i<=n;i++){ dir=($3==1883)?"dev->broker":"broker->dev"; printf "%.3f %-11s %-10s %s\n",$1,dir,N[t[i]],($5!=""?"keepalive="$5:"") }}' | tee "$run/mqtt-packets.txt"
echo
echo "=== Summary ==="
awk '
 { t=$1; dir=$2; ty=$3 }
 ty=="CONNECT" && !c { c=t; print "CONNECT at " c " (" $4 ")" }
 dir=="dev->broker" { if (prev) d=sprintf("%.1fs since previous client packet", t-prev); else d="first";
    if (ty=="PINGREQ") {np++; print "PINGREQ  " t "  " d}
    prev=t; last=t; lasty=ty }
 dir=="broker->dev" && ty=="PINGRESP" { print "PINGRESP " t }
 ty=="DISCONNECT" { print "client DISCONNECT packet at " t }
 END { if(!np) print "No PINGREQ seen from device."; else print np " PINGREQ total";
       if(last) print "last client packet: " lasty " at " last }' "$run/mqtt-packets.txt"
echo
echo "=== Mosquitto log (ping/timeout/disconnect) ==="
grep -Ei 'PINGREQ|PINGRESP|timeout|disconnect|closed|New client' "$run/mosquitto.log" || echo "(none)"
echo
echo "=== Session end per 8883 pcap (device side FIN/RST) ==="
tshark -r "$ext" -Y 'tcp.flags.fin==1 || tcp.flags.reset==1' -T fields -e frame.time_epoch -e ip.src -e tcp.srcport -e tcp.flags.fin -e tcp.flags.reset 2>/dev/null \
  | awk -F'\t' '{printf "%.3f from %s:%s %s\n",$1,$2,$3,($5==1?"RST":"FIN")}' || true
echo
echo "=== tlsterm log (key lines) ==="
grep -E 'TLS OK|FAILED|EOF|ERROR|closed' "$run/tlsterm.log" || true
echo
echo "Decrypt the outer capture:"
echo "  tshark -r $ext -o tls.keylog_file:$run/keylog.txt -d tcp.port==8883,tls -Y mqtt -T fields -e frame.time -e mqtt.msgtype"

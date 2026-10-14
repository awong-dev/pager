#!/usr/bin/env bash
# Usage: ./gen-certs.sh <hostname-or-ip> [more hostnames/IPs...]
set -euo pipefail
[ $# -ge 1 ] || { echo "usage: $0 <host-or-ip> [...]" >&2; exit 1; }
cd "$(dirname "$0")"
mkdir -p certs && cd certs
san=""; i=0
for h in "$@"; do
  i=$((i+1))
  if [[ "$h" =~ ^[0-9.]+$ || "$h" == *:* ]]; then san+="IP.$i = $h"$'\n'; else san+="DNS.$i = $h"$'\n'; fi
done
cat > server.cnf <<CNF
[req]
distinguished_name = dn
prompt = no
req_extensions = v3
[dn]
CN = $1
[v3]
subjectAltName = @alt
basicConstraints = CA:FALSE
keyUsage = digitalSignature, keyEncipherment
extendedKeyUsage = serverAuth
[alt]
$san
CNF
openssl req -x509 -newkey rsa:2048 -nodes -keyout ca.key -out ca.crt -days 825 -subj "/CN=probe-ca" \
  -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=keyCertSign,cRLSign"
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -config server.cnf
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -out server.crt \
  -days 825 -extfile server.cnf -extensions v3
echo "certs in $(pwd): ca.crt server.crt server.key"

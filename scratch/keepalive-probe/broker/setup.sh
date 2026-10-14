#!/usr/bin/env bash
set -euo pipefail
sudo apt-get update
sudo DEBIAN_FRONTEND=noninteractive apt-get install -y mosquitto mosquitto-clients tcpdump tshark python3 openssl
sudo systemctl disable --now mosquitto || true
echo "setup done; next: ./gen-certs.sh <public-host-or-ip> && ./run.sh"

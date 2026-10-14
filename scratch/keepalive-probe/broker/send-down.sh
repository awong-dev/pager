#!/usr/bin/env bash
# Usage: ./send-down.sh <clientid> [msg]
set -euo pipefail
id="${1:?usage: $0 <clientid> [msg]}"
mosquitto_pub -h 127.0.0.1 -p 1883 -t "probe/$id/down" -m "${2:-down-$(date +%s)}" -q 1
echo "published to probe/$id/down"

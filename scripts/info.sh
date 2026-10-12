#!/usr/bin/env bash
# Print a status snapshot of the RG351V over SSH.
set -euo pipefail

HOST="${RG351V_HOST:-root@192.168.1.14}"

ssh -o BatchMode=yes -o ConnectTimeout=8 "$HOST" '
  echo "## power"
  for d in /sys/class/power_supply/*; do
    echo "$(basename "$d"): online=$(cat "$d/online" 2>/dev/null) status=$(cat "$d/status" 2>/dev/null) cap=$(cat "$d/capacity" 2>/dev/null)"
  done
  echo "## os";     grep PRETTY_NAME /etc/os-release
  echo "## kernel"; uname -r
  echo "## mem";    free -m | sed -n 2p
  echo "## disk";   df -h /flash /storage /storage/roms
  echo "## net";    ip -4 -o addr show wlan0
'

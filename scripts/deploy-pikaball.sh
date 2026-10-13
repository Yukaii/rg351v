#!/usr/bin/env bash
# Deploy pikaball_revamped to the RG351V as a port.
#
# Installs the binary + launcher to /storage/roms/ports/pikaball/ and
# refreshes EmulationStation. Safe to re-run (idempotent).
#
# Usage:
#   scripts/deploy-pikaball.sh [--dry-run]
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HOST="${RG351V_HOST:-root@RG351V.local}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BIN="$ROOT/.storage/pikaball/pikaball_revamped"
LAUNCHER="$HERE/pikaball.PikachuVolleyball.sh"
REMOTE_DIR="/storage/roms/ports/pikaball"
SSH=(ssh -4 -o BatchMode=yes -o ConnectTimeout=8)

[[ -f "$BIN" ]] || {
  echo "binary not found: $BIN (run scripts/build-pikaball.sh first)" >&2
  exit 1
}

# Refuse to touch anything that is not the handheld.
remote_name="$("${SSH[@]}" "$HOST" 'hostname')"
[[ "$remote_name" == "RG351V" ]] || {
  echo "refusing: $HOST answers as '$remote_name', not RG351V" >&2
  exit 1
}

if [[ "${1:-}" == "--dry-run" ]]; then
  echo "would install $BIN and ${LAUNCHER##*/} to $HOST:$REMOTE_DIR/"
  exit 0
fi

echo "==> installing to $HOST:$REMOTE_DIR/"
"${SSH[@]}" "$HOST" "mkdir -p $REMOTE_DIR"
scp -4 -o BatchMode=yes -o ConnectTimeout=8 "$BIN" "$HOST:$REMOTE_DIR/pikaball_revamped"
scp -4 -o BatchMode=yes -o ConnectTimeout=8 "$LAUNCHER" "$HOST:$REMOTE_DIR/PikachuVolleyball.sh"
"${SSH[@]}" "$HOST" "chmod +x $REMOTE_DIR/pikaball_revamped $REMOTE_DIR/PikachuVolleyball.sh && ls -la $REMOTE_DIR/"

echo "==> refreshing EmulationStation"
"${SSH[@]}" "$HOST" 'systemctl restart emustation'

echo "done: pick Ports > PikachuVolleyball on the device"

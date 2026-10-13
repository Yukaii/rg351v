#!/usr/bin/env bash
# Deploy a built port to the RG351V.
#
# Installs the binary + launcher to /storage/roms/ports/<REMOTE_SUBDIR>/ and
# refreshes EmulationStation. Safe to re-run (idempotent).
#
# Usage:
#   scripts/deploy-port.sh <name> [--dry-run]
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HOST="${RG351V_HOST:-root@RG351V.local}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SSH=(ssh -4 -o BatchMode=yes -o ConnectTimeout=8)
SCP=(scp -4 -o BatchMode=yes -o ConnectTimeout=8)

NAME="${1:-}"
[[ "$NAME" =~ ^[a-z0-9][a-z0-9_-]*$ && -f "$HERE/ports/$NAME/port.conf" ]] || {
  echo "usage: ${0##*/} <name> [--dry-run]" >&2
  exit 1
}

# shellcheck source=/dev/null
source "$HERE/ports/$NAME/port.conf"

BIN="$ROOT/.storage/$NAME/$BIN_NAME"
LAUNCHER="$HERE/ports/$NAME/launcher.sh"
REMOTE_DIR="/storage/roms/ports/$REMOTE_SUBDIR"

[[ -f "$BIN" ]] || {
  echo "binary not found: $BIN (run scripts/build-port.sh $NAME first)" >&2
  exit 1
}
[[ -f "$LAUNCHER" ]] || {
  echo "launcher not found: $LAUNCHER" >&2
  exit 1
}

# Refuse to touch anything that is not the handheld.
remote_name="$("${SSH[@]}" "$HOST" 'hostname')"
[[ "$remote_name" == "RG351V" ]] || {
  echo "refusing: $HOST answers as '$remote_name', not RG351V" >&2
  exit 1
}

if [[ "${2:-}" == "--dry-run" ]]; then
  echo "would install $BIN and $LAUNCHER to $HOST:$REMOTE_DIR/ (launcher: $LAUNCHER_NAME)"
  exit 0
fi

echo "==> installing to $HOST:$REMOTE_DIR/"
"${SSH[@]}" "$HOST" "mkdir -p $REMOTE_DIR"
"${SCP[@]}" "$BIN" "$HOST:$REMOTE_DIR/$BIN_NAME"
"${SCP[@]}" "$LAUNCHER" "$HOST:$REMOTE_DIR/$LAUNCHER_NAME"
"${SSH[@]}" "$HOST" "chmod +x $REMOTE_DIR/$BIN_NAME $REMOTE_DIR/$LAUNCHER_NAME && ls -la $REMOTE_DIR/"

echo "==> refreshing EmulationStation"
"${SSH[@]}" "$HOST" 'systemctl restart emustation'

echo "done: pick Ports > ${LAUNCHER_NAME%.sh} on the device"

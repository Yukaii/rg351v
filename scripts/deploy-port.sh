#!/usr/bin/env bash
# Deploy a built port to the RG351V.
#
# Installs the binary + launcher under /storage/roms/<MENU>/<REMOTE_SUBDIR>/
# (MENU defaults to "ports"; "homebrew" puts it in the Homebrew menu) and
# refreshes EmulationStation. Safe to re-run (idempotent).
#
# port.conf options beyond the build ones:
#   MENU              ports (default) or homebrew
#   LAUNCHER_IN_MENU  1 = launcher at /storage/roms/<MENU>/<LAUNCHER_NAME>
#                     (what the Homebrew menu lists); default: inside the app
#                     directory
#   EXTRA_DIR         dir under .storage/ holding EXTRA_FILES (default <name>)
#   EXTRA_FILES       extra files installed next to the binary
#   GAME_NAME/GAME_DESC/GAME_AUTHOR   register a gamelist.xml entry
#
# Usage:
#   scripts/deploy-port.sh <name> [--dry-run]
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

NAME="${1:-}"
[[ "$NAME" =~ ^[a-z0-9][a-z0-9_-]*$ && -f "$HERE/ports/$NAME/port.conf" ]] || {
  echo "usage: ${0##*/} <name> [--dry-run]" >&2
  exit 1
}

# shellcheck source=/dev/null
source "$HERE/ports/$NAME/port.conf"

MENU="${MENU:-ports}"
MENU_DIR="/storage/roms/$MENU"
BIN="$ROOT/.storage/$NAME/$BIN_NAME"
LAUNCHER="$HERE/ports/$NAME/launcher.sh"
REMOTE_DIR="$MENU_DIR/$REMOTE_SUBDIR"
EXTRA_SRC="$ROOT/.storage/${EXTRA_DIR:-$NAME}"
if [[ "${LAUNCHER_IN_MENU:-0}" == 1 ]]; then
  LAUNCHER_DEST="$MENU_DIR/$LAUNCHER_NAME"
else
  LAUNCHER_DEST="$REMOTE_DIR/$LAUNCHER_NAME"
fi
GAME_NAME="${GAME_NAME:-}"
HELPER="$HERE/device/gamelist-set.py"

remote() { "${SSH[@]}" "$HOST" "$@"; }

[[ -f "$BIN" ]] || {
  echo "binary not found: $BIN (run scripts/build-port.sh $NAME first)" >&2
  exit 1
}
[[ -f "$LAUNCHER" ]] || {
  echo "launcher not found: $LAUNCHER" >&2
  exit 1
}
for f in ${EXTRA_FILES[@]+"${EXTRA_FILES[@]}"}; do
  [[ -f "$EXTRA_SRC/$f" ]] || {
    echo "extra file not found: $EXTRA_SRC/$f" >&2
    exit 1
  }
done

assert_device

if [[ "${2:-}" == "--dry-run" ]]; then
  echo "would install $BIN_NAME ${EXTRA_FILES[*]:-} to $HOST:$REMOTE_DIR/"
  echo "would install launcher to $HOST:$LAUNCHER_DEST"
  [[ -z "$GAME_NAME" ]] || echo "would register '$GAME_NAME' in $MENU_DIR/gamelist.xml"
  exit 0
fi

echo "==> installing to $HOST:$REMOTE_DIR/"
remote "mkdir -p $REMOTE_DIR"
"${SCP[@]}" "$BIN" "$HOST:$REMOTE_DIR/$BIN_NAME"
for f in ${EXTRA_FILES[@]+"${EXTRA_FILES[@]}"}; do
  "${SCP[@]}" "$EXTRA_SRC/$f" "$HOST:$REMOTE_DIR/$f"
done
"${SCP[@]}" "$LAUNCHER" "$HOST:$LAUNCHER_DEST"
remote "chmod +x $REMOTE_DIR/$BIN_NAME $LAUNCHER_DEST && ls -la $REMOTE_DIR/"

if [[ -n "$GAME_NAME" ]]; then
  # ES rewrites gamelist.xml when it quits, so stop it, edit, then start it.
  echo "==> registering '$GAME_NAME' in $MENU_DIR/gamelist.xml"
  "${SCP[@]}" "$HELPER" "$HOST:/tmp/gamelist-set.py"
  remote "systemctl stop emustation
    [ -f $MENU_DIR/gamelist.xml.pre-$NAME ] || cp -p $MENU_DIR/gamelist.xml $MENU_DIR/gamelist.xml.pre-$NAME 2>/dev/null
    python3 /tmp/gamelist-set.py $MENU_DIR/gamelist.xml ./$LAUNCHER_NAME \
      '$GAME_NAME' '${GAME_DESC:-}' '${GAME_AUTHOR:-}'
    rm -f /tmp/gamelist-set.py
    systemctl start emustation"
else
  echo "==> refreshing EmulationStation"
  remote 'systemctl restart emustation'
fi

echo "done: pick $MENU > ${GAME_NAME:-${LAUNCHER_NAME%.sh}} on the device"

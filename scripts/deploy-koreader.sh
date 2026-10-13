#!/usr/bin/env bash
# Install KOReader (prebuilt linux-arm64 release) on the RG351V.
#
# Stages the release tree from .storage/koreader/root, swaps its bundled
# libSDL3.so.0 for the KMSDRM build (scripts/build-port.sh sdl3-kmsdrm), and
# rsyncs it to /storage/roms/homebrew/koreader/. The launcher goes at the top
# of /storage/roms/homebrew/ (that is what the Homebrew menu lists) and is
# registered in that folder's gamelist.xml. User data lives in
# /storage/roms/homebrew/koreader/data and is excluded from --delete.
#
# Usage:
#   scripts/deploy-koreader.sh [--dry-run]
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

TREE="$ROOT/.storage/koreader/root"
SDL="$ROOT/.storage/sdl3-kmsdrm/libSDL3.so.0"
STAGE="$ROOT/.storage/koreader/stage"
LAUNCHER="$HERE/ports/koreader/launcher.sh"
HELPER="$HERE/device/gamelist-set.py"
BREW="/storage/roms/homebrew"
REMOTE_DIR="$BREW/koreader"

remote() { "${SSH[@]}" "$HOST" "$@"; }

[[ -x "$TREE/lib/koreader/luajit" ]] || {
  echo "KOReader tree not found: $TREE (extract the linux-arm64 release there)" >&2
  exit 1
}
[[ -f "$SDL" ]] || {
  echo "KMSDRM SDL3 not found: $SDL (run scripts/build-port.sh sdl3-kmsdrm)" >&2
  exit 1
}

echo "==> staging with KMSDRM libSDL3"
mkdir -p "$STAGE"
rsync -a --delete "$TREE/" "$STAGE/"
cp -p "$SDL" "$STAGE/lib/koreader/libs/libSDL3.so.0"

assert_device

if [[ "${1:-}" == "--dry-run" ]]; then
  echo "would rsync $STAGE/ to $HOST:$REMOTE_DIR/, install $BREW/KOReader.sh and register it in the gamelist"
  rsync -rLtn --delete --exclude=/data --exclude=/KOReader.sh --stats -e "ssh -4 -o BatchMode=yes" "$STAGE/" "$HOST:$REMOTE_DIR/" | grep -E "Number of (regular )?files|transferred file size"
  exit 0
fi

echo "==> installing to $HOST:$REMOTE_DIR/"
remote "mkdir -p $REMOTE_DIR/data"
# Excluded paths are also protected from --delete: data/ holds the user's
# KOReader settings and reading history, and KOReader.sh is installed below.
# /storage/roms is exFAT: no ownership, permissions or symlinks, so copy
# links as files (-L) and skip owner/group/perms.
rsync -rLt --no-perms --no-owner --no-group --delete \
  --exclude=/data --exclude=/KOReader.sh --stats \
  -e "ssh -4 -o BatchMode=yes" "$STAGE/" "$HOST:$REMOTE_DIR/"
"${SCP[@]}" "$LAUNCHER" "$HOST:$BREW/KOReader.sh"
"${SCP[@]}" "$HELPER" "$HOST:/tmp/gamelist-set.py"
remote "chmod +x $BREW/KOReader.sh && du -sh $REMOTE_DIR | tail -1"

# ES rewrites gamelist.xml when it quits, so stop it, edit, then start it.
echo "==> registering in the Homebrew menu"
remote "systemctl stop emustation
  [ -f $BREW/gamelist.xml.pre-koreader ] || cp -p $BREW/gamelist.xml $BREW/gamelist.xml.pre-koreader
  python3 /tmp/gamelist-set.py $BREW/gamelist.xml ./KOReader.sh KOReader \
    'Document viewer for EPUB, PDF, CBZ and more. Put books in the books folder of the roms share. Pad: A select, B back, L/R page, START menu.' \
    'KOReader contributors'
  rm -f /tmp/gamelist-set.py
  systemctl start emustation"
echo "done: pick Homebrew > KOReader on the device"

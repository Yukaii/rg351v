#!/usr/bin/env bash
# Prepare a prebuilt headless Anki backend; run a disposable on-device probe.
# Usage: scripts/anki.sh prepare | probe
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

LOCAL="$ROOT/.storage/anki-feasibility"
REMOTE=/storage/roms/homebrew/anki-backend-probe

case "${1:-}" in
prepare)
  mkdir -p "$LOCAL"
  # Target Python 3.11 explicitly: pip otherwise resolves for the Mac's Python.
  python3 -m pip install --upgrade --target "$LOCAL/site-packages" \
    --platform manylinux_2_35_aarch64 --platform manylinux_2_34_aarch64 \
    --platform manylinux_2_28_aarch64 --platform manylinux2014_aarch64 \
    --python-version 3.11 --implementation cp --abi cp311 --abi abi3 --abi none \
    --only-binary=:all: --no-compile -r "$ROOT/apps/anki/requirements.txt"
  # AmberELEC's Python 3.11.1 stalls compiling some protobuf source files.
  # Python bytecode is portable across architectures with the same magic.
  uv run --python 3.11 --no-project python -m compileall -q \
    --invalidation-mode checked-hash \
    -s "$LOCAL/site-packages" -p "$REMOTE/site-packages" "$LOCAL/site-packages"
  ;;
probe)
  [[ -f "$LOCAL/site-packages/anki/_rsbridge.so" ]] || {
    echo "run scripts/anki.sh prepare first" >&2
    exit 1
  }
  assert_device
  "${SSH[@]}" "$HOST" "mkdir -p $REMOTE"
  tar -C "$LOCAL" -czf - site-packages |
    "${SSH[@]}" "$HOST" "tar -xzf - -C $REMOTE"
  "${SCP[@]}" "$ROOT/apps/anki/probe.py" "$HOST:$REMOTE/probe.py"
  "${SSH[@]}" "$HOST" \
    "timeout -s KILL 45 env PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$REMOTE/site-packages python3 $REMOTE/probe.py"
  ;;
*)
  echo "usage: ${0##*/} prepare | probe" >&2
  exit 1
  ;;
esac

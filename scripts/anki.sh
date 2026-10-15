#!/usr/bin/env bash
# Anki gamepad reviewer, prebuilt backend, disposable probes, and terminal sync.
# Usage: scripts/anki.sh prepare | deploy | install | ui | demo
#        scripts/anki.sh probe | probe-sync | login | status
#        scripts/anki.sh sync [--full download|upload]
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
deploy | probe | install)
  [[ -f "$LOCAL/site-packages/anki/_rsbridge.so" ]] || {
    echo "run scripts/anki.sh prepare first" >&2
    exit 1
  }
  assert_device
  "${SSH[@]}" "$HOST" "mkdir -p $REMOTE"
  tar -C "$LOCAL" -czf - site-packages |
    "${SSH[@]}" "$HOST" "tar -xzf - -C $REMOTE"
  uv run --python 3.11 --no-project python -m compileall -q \
    --invalidation-mode checked-hash -s "$ROOT/apps/anki" -p "$REMOTE" "$ROOT/apps/anki"
  "${SCP[@]}" "$ROOT/apps/anki/"*.py "$HOST:$REMOTE/"
  tar -C "$ROOT/apps/anki" -czf - __pycache__ |
    "${SSH[@]}" "$HOST" "tar -xzf - -C $REMOTE"
  if [[ "$1" == install ]]; then
    "${SCP[@]}" "$ROOT/.storage/ptterm-data/unifont.bin" "$HOST:$REMOTE/"
    "${SCP[@]}" "$HERE/ports/anki/launcher.sh" "$HOST:/storage/roms/homebrew/Anki.sh"
    "${SCP[@]}" "$HERE/device/gamelist-set.py" "$HOST:/tmp/anki-gamelist.py"
    "${SSH[@]}" "$HOST" "chmod +x /storage/roms/homebrew/Anki.sh
      systemctl stop emustation
      trap 'systemctl start emustation' EXIT
      cp -p /storage/roms/homebrew/gamelist.xml /storage/roms/homebrew/gamelist.xml.bak-anki
      python3 /tmp/anki-gamelist.py /storage/roms/homebrew/gamelist.xml ./Anki.sh Anki \
        'Offline flashcards with AnkiWeb sync. A: reveal/good; X: again; B: hard; Y: easy; L1: undo; R1: audio; SELECT: back.' rg351v"
  fi
  if [[ "$1" == probe ]]; then
    "${SSH[@]}" "$HOST" \
      "timeout -s KILL 45 env PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$REMOTE/site-packages python3 $REMOTE/probe.py"
  fi
  ;;
ui | demo)
  assert_device
  extra=""
  [[ "$1" != demo ]] || extra=" --demo"
  "${SSH[@]}" "$HOST" "systemctl stop emustation
    trap 'systemctl start emustation' EXIT
    bash /storage/roms/homebrew/Anki.sh$extra"
  ;;
probe-sync)
  [[ -x "$ROOT/.storage/anki-host/bin/python" ]] ||
    uv venv --python 3.11 "$ROOT/.storage/anki-host"
  uv pip install --python "$ROOT/.storage/anki-host/bin/python" \
    -r "$ROOT/apps/anki/requirements.txt"
  "$ROOT/.storage/anki-host/bin/python" "$ROOT/apps/anki/sync_probe.py"
  ;;
login | status | sync)
  action="$1"
  shift
  full=""
  if [[ "$action" == sync && $# == 2 && "$1" == --full && "$2" =~ ^(download|upload)$ ]]; then
    full=" --full $2"
  elif [[ $# != 0 ]]; then
    echo "usage: ${0##*/} login | status | sync [--full download|upload]" >&2
    exit 1
  fi
  assert_device
  command="env PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=$REMOTE/site-packages python3 $REMOTE/sync_cli.py $action$full"
  if [[ "$action" == login ]]; then
    "${SSH[@]}" -t "$HOST" "$command"
  else
    "${SSH[@]}" "$HOST" "$command"
  fi
  ;;
*)
  echo "usage: ${0##*/} prepare | deploy | install | ui | demo | probe | probe-sync | login | status | sync [--full download|upload]" >&2
  exit 1
  ;;
esac

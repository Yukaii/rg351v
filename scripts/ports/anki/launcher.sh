#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
mkdir -p /tmp/logs
exec >>/tmp/logs/anki-launcher.log 2>&1
# shellcheck disable=SC1091
. /etc/profile
cleanup() {
  jslisten stop || true
  end_port || true
}
trap cleanup EXIT
init_port anki alsa
cat >/tmp/anki-kill.sh <<'KILL'
p=$(cat /run/rg351v/anki-ui.pid 2>/dev/null)
case "$p" in ''|*[!0-9]*) exit 0 ;; esac
case "$(tr '\000' ' ' </proc/$p/cmdline 2>/dev/null)" in
  */anki-ui.py*) kill -TERM "$p" ;;
esac
KILL
jslisten set '/bin/sh /tmp/anki-kill.sh'
export SDL_VIDEODRIVER=kmsdrm
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPATH=/storage/roms/homebrew/anki-backend-probe/site-packages
cd /storage/roms/homebrew/anki-backend-probe
# EmulationStation appends system/core/controller arguments to Homebrew
# launchers. They belong to the firmware, not the reviewer's argparse CLI.
case "${1:-}" in -P*) set -- ;; esac
python3 /storage/roms/homebrew/anki-backend-probe/anki-ui.py "$@" >/tmp/logs/anki-ui.log 2>&1

#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
mkdir -p /tmp/logs
exec >>/tmp/logs/youtube-launcher.log 2>&1
printf '\nYouTube launch %s\n' "$(date)"
cleanup() {
  local result=$?
  echo "launcher exit=$result"
  end_port || true
}
trap cleanup EXIT
# shellcheck disable=SC1091
. /etc/profile
init_port youtube alsa
export SDL_VIDEODRIVER=kmsdrm
mkdir -p /tmp/logs
cd /storage/roms/homebrew/youtube
if [[ "${1:-}" == --play ]]; then
  [[ $# == 2 ]] || {
    echo 'usage: YouTube.sh --play <file>'
    exit 1
  }
  python3 youtube-player.py "$2" >/tmp/logs/youtube.log 2>&1
else
  python3 youtube-ui.py >/tmp/logs/youtube-ui.log 2>&1
fi

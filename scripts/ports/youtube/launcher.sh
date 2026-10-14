#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
set -euo pipefail
# shellcheck disable=SC1091
. /etc/profile
init_port youtube alsa
trap 'end_port || true' EXIT
export SDL_VIDEODRIVER=kmsdrm
mkdir -p /tmp/logs
cd /storage/roms/ports/youtube
python3 youtube-player.py "${1:-test-360p.mp4}" >/tmp/logs/youtube.log 2>&1

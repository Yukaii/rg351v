#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Launcher for taiko (TJA chart player) on AmberELEC.

# Source predefined functions and variables
# shellcheck disable=SC1091
. /etc/profile

PORT="taiko"
# init_port binary audio(alsa. pulseaudio, default)
init_port ${PORT} alsa

clear >/dev/console

# Hold SELECT+START in the app, or L2+SELECT+START from anywhere, to leave.
jslisten set "/usr/bin/killall ${PORT}"

cd /storage/roms/homebrew/taiko || exit 1

# Pin KMSDRM: SDL3's default driver pick is unreliable without X11/Wayland.
export SDL_VIDEODRIVER=kmsdrm

# Private low-latency dmix (period 512 / buffer 1536 @ 48 kHz, ~21 ms output
# latency, measured clean; see docs/gotchas.md). The system asound.conf is
# loaded after our file, so the PCM gets its own name and SDL is pointed at it.
cat >/tmp/taiko-asound.conf <<'ALSA'
</usr/share/alsa/alsa.conf>
pcm.taiko {
  type plug
  slave.pcm "taikodmix"
}
pcm.taikodmix {
  type dmix
  ipc_key 4361
  slave {
    pcm "hw:0,0"
    period_size 512
    buffer_size 1536
    rate 48000
  }
  bindings {
    0 0
    1 1
  }
}
ALSA
export ALSA_CONFIG_PATH=/tmp/taiko-asound.conf
export SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE=taiko

# Menu with songs from ./songs and the calibration screen. For development,
# TAIKO_SONG (path to a .tja) plus TAIKO_COURSE (0 Easy .. 4 Edit) plays one
# song directly.
if [ -n "${TAIKO_SONG:-}" ]; then
  ./${PORT} --tja "${TAIKO_SONG}" --course "${TAIKO_COURSE:-0}" &>/tmp/logs/taiko.log
else
  ./${PORT} --songs songs &>/tmp/logs/taiko.log
fi
ret_error=$?

jslisten stop
end_port

exit $ret_error

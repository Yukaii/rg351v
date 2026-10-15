#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Launcher for taiko-probe (audio latency probe) on AmberELEC.

# Source predefined functions and variables
# shellcheck disable=SC1091
. /etc/profile

PORT="taiko-probe"
# init_port binary audio(alsa. pulseaudio, default)
init_port ${PORT} alsa

clear >/dev/console

# Hold SELECT+START in the app, or L2+SELECT+START from anywhere, to leave.
jslisten set "/usr/bin/killall ${PORT}"

cd /storage/roms/homebrew/taiko-probe || exit 1

# Pin KMSDRM: SDL3's default driver pick is unreliable without X11/Wayland.
export SDL_VIDEODRIVER=kmsdrm

# Low-latency ALSA for this app only. /storage/.config/asound.conf pins the
# shared dmix to period 1024 / buffer 4096 @ 44.1 kHz (~93 ms), which SDL's
# sample-frames hint cannot change. Private dmix PCMs (own ipc_key each, so
# they never inherit the shared one's parameters) leave the system file alone.
# alsa.conf loads asound.conf after our file, so redefining "default" loses;
# the PCMs get their own names and the app picks one through
# SDL_AUDIO_ALSA_DEFAULT_PLAYBACK_DEVICE. ALSA_CONFIG_PATH replaces alsa.conf,
# which also defines hw:N, hence the include.
# name key period buffer (frames @ 48 kHz); names match VARIANTS in probe.c.
alsa_pcm() {
  cat <<ALSA
pcm.$1 {
  type plug
  slave.pcm "$1dmix"
}
pcm.$1dmix {
  type dmix
  ipc_key $2
  slave {
    pcm "hw:0,0"
    period_size $3
    buffer_size $4
    rate 48000
  }
  bindings {
    0 0
    1 1
  }
}
ALSA
}
{
  echo '</usr/share/alsa/alsa.conf>'
  alsa_pcm taiko256 4351 256 1024
  alsa_pcm taiko256w 4352 256 1536
  alsa_pcm taiko256x 4353 256 2048
  alsa_pcm taiko512t 4354 512 1536
  alsa_pcm taiko512 4355 512 2048
  alsa_pcm taiko1024 4356 1024 4096
} >/tmp/taiko-asound.conf
export ALSA_CONFIG_PATH=/tmp/taiko-asound.conf

./${PORT} &>/tmp/logs/taiko-probe.log
ret_error=$?

jslisten stop
end_port

exit $ret_error

#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Launcher template for an AmberELEC port. Replace CHANGEME values.

# Source predefined functions and variables
# shellcheck disable=SC1091
. /etc/profile

PORT="CHANGEME"
# init_port binary audio(alsa. pulseaudio, default)
init_port ${PORT} alsa

clear >/dev/console

# Hold L2+SELECT+START to kill the port.
jslisten set "/usr/bin/killall ${PORT}"

cd /storage/roms/ports/CHANGEME || exit 1

# Pin KMSDRM: SDL3's default driver pick is unreliable without X11/Wayland.
export SDL_VIDEODRIVER=kmsdrm

./${PORT} &>>/tmp/logs/exec.log
ret_error=$?

jslisten stop
end_port

exit $ret_error

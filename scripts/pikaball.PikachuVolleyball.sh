#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Launcher for Pikachu Volleyball (pikaball-revamped) on AmberELEC.
# Gamepad is supported natively by the game (stick/dpad + face buttons,
# START pauses); no gptokeyb mapping needed.

# Source predefined functions and variables
# shellcheck disable=SC1091
. /etc/profile

PORT="pikaball_revamped"
# init_port binary audio(alsa. pulseaudio, default)
init_port ${PORT} alsa

clear >/dev/console

jslisten set "/usr/bin/killall ${PORT}"

cd /storage/roms/ports/pikaball || exit 1

# Pin the KMSDRM video driver: SDL3's default driver pick is unreliable
# without X11/Wayland, and this is the only driver that shows a picture.
export SDL_VIDEODRIVER=kmsdrm

./${PORT} &>>/tmp/logs/exec.log

jslisten stop

ret_error=$?

end_port

exit $ret_error

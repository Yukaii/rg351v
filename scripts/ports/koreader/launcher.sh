#!/bin/bash

# SPDX-License-Identifier: AGPL-3.0-or-later
# Launcher for KOReader on AmberELEC. Uses the prebuilt linux-arm64 release
# with its bundled SDL3 replaced by one built with the KMSDRM backend (see
# scripts/ports/sdl3-kmsdrm). The gamepad is handled by KOReader itself:
# A=press, B=back, L/R=page back/forward, START/SELECT=menu.

# Source predefined functions and variables
# shellcheck disable=SC1091
. /etc/profile

PORT="luajit"
# init_port binary audio(alsa. pulseaudio, default)
init_port ${PORT} alsa

clear >/dev/console

# Hold L2+SELECT+START to kill the port.
jslisten set "/usr/bin/killall ${PORT}"

APP=/storage/roms/homebrew/koreader
mkdir -p /storage/roms/books "${APP}/data"

# Settings/history live next to the app on the big roms partition; the file
# manager opens in /storage/roms/books (reachable over the SMB roms share).
export KO_HOME="${APP}/data"
export HOME=/storage/roms/books

# KOReader's bundled SDL3 has no KMSDRM; ours does. Pin the driver.
export SDL_VIDEODRIVER=kmsdrm

cd "${APP}/lib/koreader" || exit 1

./reader.lua &>/tmp/logs/koreader.log
ret_error=$?

jslisten stop
end_port

exit $ret_error

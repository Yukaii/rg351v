#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Launcher for ptterm (SSH terminal for ptt.cc) on AmberELEC.

# Source predefined functions and variables
# shellcheck disable=SC1091
. /etc/profile

PORT="ptterm"
# init_port binary audio(alsa. pulseaudio, default)
init_port ${PORT} alsa

clear >/dev/console

# Hold L2+SELECT+START to leave. ptterm keeps the BBS session alive in a
# background holder that is also named ptterm to BusyBox, so `killall` would
# end the session too: signal only the UI process (SIGTERM = detach).
cat >/tmp/ptterm-kill.sh <<'KILL'
p=$(cat /tmp/ptterm-ui.pid 2>/dev/null)
case "$(readlink /proc/$p/exe 2>/dev/null)" in */ptterm) kill -TERM "$p" ;; esac
KILL
jslisten set "/bin/sh /tmp/ptterm-kill.sh"

cd /storage/roms/homebrew/ptterm || exit 1

# Pin KMSDRM: SDL3's default driver pick is unreliable without X11/Wayland.
export SDL_VIDEODRIVER=kmsdrm
# Per-phase timing summary is appended to the log on exit (negligible cost).
export PTTERM_PROF=1

./${PORT} &>/tmp/logs/ptterm.log
ret_error=$?

jslisten stop
end_port

exit $ret_error

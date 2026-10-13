#!/bin/bash

# SPDX-License-Identifier: GPL-2.0-or-later
# Launcher for deck (Spotify player UI) on AmberELEC.

# Source predefined functions and variables
# shellcheck disable=SC1091
. /etc/profile

PORT="deck"
# init_port binary audio(alsa. pulseaudio, default)
init_port ${PORT} alsa

clear >/dev/console

# Hold SELECT+START in the app, or L2+SELECT+START from anywhere, to leave.
# Only the UI is killed: deckd is a separate service and keeps playing.
cat >/tmp/deck-kill.sh <<'KILL'
p=$(cat /tmp/deck-ui.pid 2>/dev/null)
case "$(readlink /proc/$p/exe 2>/dev/null)" in */deck) kill -TERM "$p" ;; esac
KILL
jslisten set "/bin/sh /tmp/deck-kill.sh"

cd /storage/roms/homebrew/deck || exit 1

# Pin KMSDRM: SDL3's default driver pick is unreliable without X11/Wayland.
export SDL_VIDEODRIVER=kmsdrm
# Screen blanks after this many idle seconds (0 = never); music keeps playing.
export DECK_BLANK_SECS=60

./${PORT} &>/tmp/logs/deck.log
ret_error=$?

jslisten stop
end_port

exit $ret_error

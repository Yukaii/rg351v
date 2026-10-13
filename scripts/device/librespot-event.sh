#!/bin/sh
# librespot --onevent hook: keep /run/rg351v/playing while a track plays so
# autosleep.py does not suspend the board mid-playlist.
FLAG=/run/rg351v/playing
case "$PLAYER_EVENT" in
playing | loading)
  mkdir -p /run/rg351v
  : >"$FLAG"
  ;;
paused | stopped | session_disconnected)
  rm -f "$FLAG"
  # Track changes emit stopped/paused for a moment; autosleep.py treats a
  # recent stop as still playing so it does not suspend between songs.
  mkdir -p /run/rg351v
  : >/run/rg351v/stopped
  ;;
esac

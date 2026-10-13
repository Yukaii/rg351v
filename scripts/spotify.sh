#!/usr/bin/env bash
# Spotify Connect daemon (deckd) on the RG351V.
#
# deckd (apps/deckd) is librespot as a library plus a control socket, a
# now-playing state and playback restore after the session drops. It runs as
# a systemd service, shows up as a Connect device named RG351V and plays
# through ALSA. Needs Spotify Premium. The first login is done from a phone
# (Connect devices -> RG351V); the credentials are cached in
# /storage/.cache/librespot, after which it logs in by itself.
#
# While a track plays, deckd keeps /run/rg351v/playing so autosleep.py does
# not suspend the board. Control socket: /run/rg351v/deckd.sock.
#
# Usage:
#   scripts/spotify.sh build       cross-compile deckd (Docker, aarch64)
#   scripts/spotify.sh install     deploy binary + service, enable it
#   scripts/spotify.sh uninstall   stop and remove the service (keeps login)
#   scripts/spotify.sh status      service state, login, now playing, log
#   scripts/spotify.sh ctl <cmd>   send a command (status, play, pause, toggle,
#                                  next, prev, seek <ms>, volume <0-100>,
#                                  shuffle on|off, repeat on|off, load <uri>,
#                                  reconnect)
#   scripts/spotify.sh logout      forget the cached login
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

TARGET="$ROOT/.storage/deckd-target"
BIN="$TARGET/deckd.stripped"
CONF_DIR="/storage/.config/rg351v"
REMOTE_BIN="$CONF_DIR/bin/deckd"
UNIT=rg351v-deckd.service
OLD_UNIT=rg351v-librespot.service
UNIT_DIR="/storage/.config/system.d"
CACHE="/storage/.cache/librespot"
SOCK="/run/rg351v/deckd.sock"

die() {
  echo "$*" >&2
  exit 1
}

remote() { "${SSH[@]}" "$HOST" "$@"; }

# ctl <line>: one request/response on the control socket (python3 is on the
# device; BusyBox nc has no unix socket support).
ctl() {
  local line="$1"
  [[ "$line" != *"'"* && "$line" != *$'\n'* ]] || die "bad characters in command"
  remote "python3 - <<'PY'
import socket
s = socket.socket(socket.AF_UNIX)
s.settimeout(3)
s.connect('$SOCK')
s.sendall(b'$line\n')
buf = b''
while True:
    try:
        d = s.recv(4096)
    except socket.timeout:
        break
    if not d:
        break
    buf += d
    if buf.endswith(b'.\n') or buf.startswith(b'OK') or buf.startswith(b'ERR'):
        break
print(buf.decode('utf-8', 'replace').rstrip())
PY"
}

case "${1:-}" in
build)
  mkdir -p "$TARGET"
  docker volume create rg351v-cargo >/dev/null
  # rust:1-bookworm has glibc 2.36, the same as the device. apps/deckd carries
  # a Cargo.lock seeded from librespot 0.8.0 (fresh resolution picks a vergen
  # that does not build).
  docker run --rm --platform linux/arm64 \
    -v "$ROOT":/work -v rg351v-cargo:/usr/local/cargo/registry \
    -e CARGO_TARGET_DIR=/work/.storage/deckd-target -w /work/apps/deckd \
    rust:1-bookworm bash -c '
      apt-get update -qq && apt-get install -y -qq libasound2-dev pkg-config >/dev/null &&
      cargo build --release --locked &&
      strip -o /work/.storage/deckd-target/deckd.stripped \
        /work/.storage/deckd-target/release/deckd'
  ls -la "$BIN"
  ;;
install)
  [[ -f "$BIN" ]] || die "no binary: run ${0##*/} build first"
  assert_device
  remote "mkdir -p $CONF_DIR/bin $UNIT_DIR/multi-user.target.wants $CACHE"
  # Replace the binary atomically: a running service holds the old one open.
  "${SCP[@]}" "$BIN" "$HOST:$REMOTE_BIN.new"
  "${SCP[@]}" "$HERE/device/$UNIT" "$HOST:$UNIT_DIR/$UNIT"
  remote "chmod 755 $REMOTE_BIN.new && mv $REMOTE_BIN.new $REMOTE_BIN &&
    systemctl stop $OLD_UNIT 2>/dev/null;
    rm -f $UNIT_DIR/multi-user.target.wants/$OLD_UNIT $UNIT_DIR/$OLD_UNIT \
      $CONF_DIR/librespot-event.sh &&
    ln -sf $UNIT_DIR/$UNIT $UNIT_DIR/multi-user.target.wants/$UNIT &&
    systemctl daemon-reload && systemctl restart $UNIT && sleep 3 &&
    echo \"service: \$(systemctl is-active $UNIT)\""
  echo "first time only: Spotify app > Connect devices > RG351V"
  ;;
uninstall)
  assert_device
  remote "systemctl stop $UNIT 2>/dev/null;
    rm -f $UNIT_DIR/multi-user.target.wants/$UNIT $UNIT_DIR/$UNIT /run/rg351v/playing;
    systemctl daemon-reload; echo 'service removed (binary and login kept)'"
  ;;
status)
  assert_device
  remote "echo \"service: \$(systemctl is-active $UNIT 2>&1)\"
    echo \"login:   \$([ -f $CACHE/credentials.json ] && echo cached || echo 'not logged in')\"
    P=\$(pidof deckd); [ -z \"\$P\" ] || grep VmRSS /proc/\$P/status
    echo '--- log'; journalctl -u $UNIT --no-pager -n 10 2>/dev/null | cut -c1-170"
  echo '--- now playing'
  ctl status || true
  ;;
ctl)
  [[ -n "${2:-}" ]] || die "usage: ${0##*/} ctl <command>"
  assert_device
  shift
  ctl "$*"
  ;;
logout)
  assert_device
  remote "systemctl stop $UNIT; rm -f $CACHE/credentials.json; systemctl start $UNIT;
    echo 'login forgotten; pair again from the Spotify app'"
  ;;
*)
  sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
  exit 1
  ;;
esac

#!/usr/bin/env bash
# Spotify Connect speaker (librespot) on the RG351V.
#
# Step 1 of the "deck" player (see apps/deck/README.md): librespot runs as a
# systemd service, shows up as a Connect device named RG351V in any Spotify
# app and plays through ALSA. Needs Spotify Premium. The first login is done
# from a phone (Connect devices -> RG351V); the credentials are cached in
# /storage/.cache/librespot, after which it logs in by itself.
#
# While a track plays, librespot's event hook keeps /run/rg351v/playing so
# autosleep.py does not suspend the board.
#
# Usage:
#   scripts/spotify.sh build       cross-compile librespot (Docker, aarch64)
#   scripts/spotify.sh install     deploy binary + hook + service, enable it
#   scripts/spotify.sh uninstall   stop and remove the service (keeps login)
#   scripts/spotify.sh status      service state, cache, recent log
#   scripts/spotify.sh logout      forget the cached login
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

VERSION=0.8.0
# alsa-backend: audio out; with-libmdns: phone-side discovery (the default
# feature set minus the rodio backend, which would drag in a second audio stack).
FEATURES="alsa-backend,rustls-tls-native-roots,with-libmdns"
OUT="$ROOT/.storage/librespot"
BIN="$OUT/librespot.stripped"
CONF_DIR="/storage/.config/rg351v"
REMOTE_BIN="$CONF_DIR/bin/librespot"
REMOTE_HOOK="$CONF_DIR/librespot-event.sh"
UNIT=rg351v-librespot.service
UNIT_DIR="/storage/.config/system.d"
CACHE="/storage/.cache/librespot"

die() {
  echo "$*" >&2
  exit 1
}

remote() { "${SSH[@]}" "$HOST" "$@"; }

case "${1:-}" in
build)
  mkdir -p "$OUT"
  docker volume create rg351v-cargo >/dev/null
  # rust:1-bookworm has glibc 2.36, the same as the device.
  docker run --rm --platform linux/arm64 \
    -v rg351v-cargo:/usr/local/cargo/registry -v "$OUT":/out \
    -e CARGO_TARGET_DIR=/tmp/t rust:1-bookworm bash -c "
      apt-get update -qq && apt-get install -y -qq libasound2-dev pkg-config >/dev/null &&
      cargo install librespot --version $VERSION --locked --no-default-features \
        --features $FEATURES --root /tmp/r &&
      strip -o /out/librespot.stripped /tmp/r/bin/librespot"
  ls -la "$BIN"
  ;;
install)
  [[ -f "$BIN" ]] || die "no binary: run ${0##*/} build first"
  assert_device
  remote "mkdir -p $CONF_DIR/bin $UNIT_DIR/multi-user.target.wants $CACHE"
  # Replace the binary atomically: a running service holds the old one open.
  "${SCP[@]}" "$BIN" "$HOST:$REMOTE_BIN.new"
  "${SCP[@]}" "$HERE/device/librespot-event.sh" "$HOST:$REMOTE_HOOK"
  "${SCP[@]}" "$HERE/device/$UNIT" "$HOST:$UNIT_DIR/$UNIT"
  remote "chmod 755 $REMOTE_BIN.new $REMOTE_HOOK &&
    mv $REMOTE_BIN.new $REMOTE_BIN &&
    ln -sf $UNIT_DIR/$UNIT $UNIT_DIR/multi-user.target.wants/$UNIT &&
    systemctl daemon-reload && systemctl restart $UNIT && sleep 2 &&
    echo \"service: \$(systemctl is-active $UNIT)\""
  echo "next: Spotify app > Connect devices > RG351V (first time only)"
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
    echo \"playing: \$([ -f /run/rg351v/playing ] && echo yes || echo no)\"
    P=\$(pidof librespot); [ -z \"\$P\" ] || grep VmRSS /proc/\$P/status
    echo '--- log'; journalctl -u $UNIT --no-pager -n 12 2>/dev/null | cut -c1-170"
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

#!/usr/bin/env bash
# Suspend the RG351V automatically after a period without input.
#
# AmberELEC only has a black screensaver, which keeps the SoC, Wi-Fi and
# backlight running (~450 mA). This installs an on-device watcher
# (scripts/device/autosleep.py as a systemd service) that runs
# `systemctl suspend` once no button has been pressed for IDLE_MINUTES.
# Suspend draws <1% battery per hour; wake with the power key.
#
# It holds off while charging, while an SSH session is open and while Wi-Fi
# is moving data (see the header of autosleep.py).
#
# Usage:
#   scripts/autosleep.sh install [minutes]   install/upgrade, default 5
#   scripts/autosleep.sh uninstall
#   scripts/autosleep.sh set <key> <value>   IDLE_MINUTES (0 = off),
#                                            SKIP_WHEN_CHARGING, SKIP_WHEN_REMOTE,
#                                            BUSY_KBPS
#   scripts/autosleep.sh status              config, service state, recent log
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

CONF_DIR="/storage/.config/rg351v"
CONF="$CONF_DIR/autosleep.conf"
REMOTE_PY="$CONF_DIR/autosleep.py"
UNIT=rg351v-autosleep.service
UNIT_DIR="/storage/.config/system.d"

die() {
  echo "$*" >&2
  exit 1
}

remote() { "${SSH[@]}" "$HOST" "$@"; }

set_key() { # <key> <value>
  case "$1" in
  IDLE_MINUTES | SKIP_WHEN_CHARGING | SKIP_WHEN_REMOTE | BUSY_KBPS) ;;
  *) die "unknown key: $1" ;;
  esac
  [[ "$2" =~ ^[0-9]+$ ]] || die "$1 takes a non-negative integer"
  remote "touch $CONF && grep -v '^$1=' $CONF >$CONF.new; echo '$1=$2' >>$CONF.new; mv $CONF.new $CONF"
  echo "ok: $1=$2 (picked up within seconds)"
}

case "${1:-}" in
install)
  minutes="${2:-5}"
  [[ "$minutes" =~ ^[0-9]+$ ]] || die "minutes must be a non-negative integer"
  assert_device
  remote "mkdir -p $CONF_DIR $UNIT_DIR/multi-user.target.wants"
  "${SCP[@]}" "$HERE/device/autosleep.py" "$HOST:$REMOTE_PY"
  "${SCP[@]}" "$HERE/device/$UNIT" "$HOST:$UNIT_DIR/$UNIT"
  # Keep an existing config on upgrade; only a fresh install takes the default.
  remote "[ -f $CONF ] || echo 'IDLE_MINUTES=$minutes' >$CONF"
  [[ -z "${2:-}" ]] || set_key IDLE_MINUTES "$minutes"
  # Enabled like the stock sshd.service (`systemctl enable` would write into
  # the read-only rootfs).
  remote "chmod 755 $REMOTE_PY &&
    ln -sf $UNIT_DIR/$UNIT $UNIT_DIR/multi-user.target.wants/$UNIT &&
    systemctl daemon-reload && systemctl restart $UNIT && sleep 1 &&
    echo \"service: \$(systemctl is-active $UNIT)\""
  ;;
uninstall)
  assert_device
  remote "systemctl stop $UNIT 2>/dev/null;
    rm -f $UNIT_DIR/multi-user.target.wants/$UNIT $UNIT_DIR/$UNIT;
    systemctl daemon-reload; echo 'autosleep removed (config kept)'"
  ;;
set)
  [[ -n "${2:-}" && -n "${3:-}" ]] || die "usage: ${0##*/} set <key> <value>"
  assert_device
  set_key "$2" "$3"
  ;;
status)
  assert_device
  remote "echo \"service: \$(systemctl is-active $UNIT 2>&1)\"
    echo '--- config'; cat $CONF 2>/dev/null || echo '(defaults)'
    echo '--- log'; journalctl -u $UNIT --no-pager -n 15 2>/dev/null | cut -c1-160"
  ;;
*)
  sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
  exit 1
  ;;
esac

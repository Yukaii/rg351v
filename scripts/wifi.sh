#!/usr/bin/env bash
# Multiple Wi-Fi networks on the RG351V.
#
# AmberELEC's ES Wi-Fi menu keeps one network: every save rewrites
# /storage/.cache/connman/wifi.config. This tool installs an on-device hook
# (a systemd .path unit + scripts/device/wifi-remember.sh) that remembers each
# network you save in the ES menu and gives the others to connman through
# known.config. Once installed you do not need this script to add networks:
# just use the ES menu anywhere, even with no connection.
#
# This script covers what the menu cannot: installing the hook, listing and
# forgetting remembered networks, and bulk-adding from the Mac. Mac-side
# networks live in .storage/wifi-networks (git-ignored, mode 600), one per
# line:  ssid<TAB>passphrase<TAB>hidden   (empty passphrase = open network).
#
# Usage:
#   scripts/wifi.sh install            install/upgrade the hook on the device
#   scripts/wifi.sh uninstall          remove the hook (keeps known networks)
#   scripts/wifi.sh known              networks remembered on the device
#   scripts/wifi.sh forget <ssid>      drop one from the device
#   scripts/wifi.sh add <ssid> [--hidden]   store on this Mac (prompts for psk)
#   scripts/wifi.sh remove <ssid>      drop from this Mac's list
#   scripts/wifi.sh list               networks stored on this Mac
#   scripts/wifi.sh push               send this Mac's list to the device
#   scripts/wifi.sh status             hook + connman state on the device
#
# Env:
#   RG351V_HOST      SSH target (default root@RG351V.local)
#   WIFI_NETWORKS    Mac-side list (default .storage/wifi-networks)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

NETWORKS="${WIFI_NETWORKS:-$ROOT/.storage/wifi-networks}"
TAB=$'\t'
CONF_DIR="/storage/.config/rg351v"
REMOTE_SH="$CONF_DIR/wifi-remember.sh"
UNIT_DIR="/storage/.config/system.d"
UNITS=(rg351v-wifi-remember.path rg351v-wifi-remember.service)

die() {
  echo "$*" >&2
  exit 1
}

remote() { "${SSH[@]}" "$HOST" "$@"; }

# GKeyFile (connman's parser) unescapes backslashes and trims whitespace.
escape() { printf '%s' "${1//\\/\\\\}"; }

check_text() { # <what> <value>
  [[ "$2" != *[[:cntrl:]]* ]] || die "$1 must not contain control characters"
  [[ "$2" == "${2#"${2%%[![:space:]]*}"}" && "$2" == "${2%"${2##*[![:space:]]}"}" ]] ||
    die "$1 must not start or end with whitespace"
}

ensure_file() {
  mkdir -p "$(dirname "$NETWORKS")"
  [[ -f "$NETWORKS" ]] || : >"$NETWORKS"
  chmod 600 "$NETWORKS"
}

# split_line <line>: sets L_SSID L_PSK L_HIDDEN (tab IFS would collapse the
# empty passphrase of an open network, so split by parameter expansion).
split_line() {
  local rest
  L_SSID="${1%%"$TAB"*}"
  rest="${1#*"$TAB"}"
  L_PSK="${rest%%"$TAB"*}"
  L_HIDDEN="${rest#*"$TAB"}"
}

# Print the Mac-side list without the entry for <ssid>.
without() {
  local line
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -n "$line" ]] || continue
    split_line "$line"
    [[ "$L_SSID" == "$1" ]] || printf '%s\n' "$line"
  done <"$NETWORKS"
}

replace_networks() { # <file with new contents>
  chmod 600 "$1"
  mv "$1" "$NETWORKS"
}

installed() {
  remote "[ -f $REMOTE_SH ] && [ -L $UNIT_DIR/multi-user.target.wants/${UNITS[0]} ]"
}

cmd="${1:-}"
case "$cmd" in
install)
  assert_device
  echo "==> copying hook to $HOST"
  remote "mkdir -p $CONF_DIR $UNIT_DIR/multi-user.target.wants"
  "${SCP[@]}" "$HERE/device/wifi-remember.sh" "$HOST:$REMOTE_SH"
  for u in "${UNITS[@]}"; do
    "${SCP[@]}" "$HERE/device/$u" "$HOST:$UNIT_DIR/$u"
  done
  # Enabled the same way as the stock sshd.service in that directory
  # (`systemctl enable` would write into the read-only rootfs).
  remote "chmod 755 $REMOTE_SH &&
    ln -sf $UNIT_DIR/${UNITS[0]} $UNIT_DIR/multi-user.target.wants/${UNITS[0]} &&
    systemctl daemon-reload &&
    systemctl start ${UNITS[0]} &&
    echo \"path unit: \$(systemctl is-active ${UNITS[0]})\""
  echo "==> learning the network currently saved in the ES menu"
  remote "sh $REMOTE_SH run && sh $REMOTE_SH list"
  ;;
uninstall)
  assert_device
  remote "systemctl stop ${UNITS[0]} 2>/dev/null;
    rm -f $UNIT_DIR/multi-user.target.wants/${UNITS[0]} $UNIT_DIR/${UNITS[0]} $UNIT_DIR/${UNITS[1]};
    systemctl daemon-reload; echo 'hook removed (store and known.config kept)'"
  ;;
known)
  assert_device
  installed || die "hook not installed (run: ${0##*/} install)"
  remote "sh $REMOTE_SH list"
  ;;
forget)
  [[ -n "${2:-}" ]] || die "usage: ${0##*/} forget <ssid>"
  assert_device
  installed || die "hook not installed (run: ${0##*/} install)"
  [[ "$2" != *"'"* ]] || die "SSIDs containing a single quote are not supported here"
  remote "sh $REMOTE_SH forget '$2'"
  ;;
list)
  ensure_file
  [[ -s "$NETWORKS" ]] || {
    echo "(no networks stored)"
    exit 0
  }
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -n "$line" ]] || continue
    split_line "$line"
    printf '%s  [%s%s]\n' "$L_SSID" "$([[ -n "$L_PSK" ]] && echo psk || echo open)" \
      "$([[ "$L_HIDDEN" == hidden ]] && echo ", hidden")"
  done <"$NETWORKS"
  ;;
add)
  ssid="${2:-}"
  [[ -n "$ssid" ]] || die "usage: ${0##*/} add <ssid> [--hidden]"
  hidden=""
  [[ "${3:-}" == "--hidden" ]] && hidden=hidden
  check_text "SSID" "$ssid"
  [[ ${#ssid} -le 32 ]] || die "SSID is longer than 32 bytes"
  ensure_file
  read -rsp "passphrase for '$ssid' (empty = open network): " psk
  echo
  check_text "passphrase" "$psk"
  [[ -z "$psk" || (${#psk} -ge 8 && ${#psk} -le 63) ]] ||
    die "WPA passphrase must be 8-63 characters"
  tmp="$(mktemp "$NETWORKS.XXXXXX")"
  {
    without "$ssid"
    printf '%s\t%s\t%s\n' "$ssid" "$psk" "$hidden"
  } >"$tmp"
  replace_networks "$tmp"
  echo "stored '$ssid' on this Mac (run: ${0##*/} push)"
  ;;
remove)
  [[ -n "${2:-}" ]] || die "usage: ${0##*/} remove <ssid>"
  ensure_file
  tmp="$(mktemp "$NETWORKS.XXXXXX")"
  without "$2" >"$tmp"
  replace_networks "$tmp"
  echo "removed '$2' from this Mac's list (device keeps it until: ${0##*/} forget)"
  ;;
push)
  ensure_file
  assert_device
  installed || die "hook not installed (run: ${0##*/} install)"
  [[ -s "$NETWORKS" ]] || die "no networks stored on this Mac (use: ${0##*/} add)"
  # Passphrases travel over the SSH channel's stdin, never argv.
  while IFS= read -r line || [[ -n "$line" ]]; do
    [[ -n "$line" ]] || continue
    split_line "$line"
    flags=""
    [[ -n "$L_PSK" ]] || flags="open"
    [[ "$L_HIDDEN" == hidden ]] && flags="${flags:+$flags }hidden"
    printf '%s\t%s\t%s\n' "$(escape "$L_SSID")" "$(escape "$L_PSK")" "$flags"
  done <"$NETWORKS" | remote "sh $REMOTE_SH import"
  remote "sh $REMOTE_SH list"
  ;;
status)
  assert_device
  remote "systemctl is-active ${UNITS[0]}; systemctl is-enabled ${UNITS[0]} 2>&1 | head -1;
    ls -l /storage/.cache/connman/*.config; echo;
    connmanctl services | head -8; echo;
    journalctl -u connman --no-pager | grep -E 'configuration|Failed|failed' | tail -5"
  ;;
*)
  sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
  exit 1
  ;;
esac

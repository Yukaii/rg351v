#!/usr/bin/env bash
# Manage extra Wi-Fi networks on the RG351V.
#
# AmberELEC's `batocera-config wifi enable` overwrites a single file,
# /storage/.cache/connman/wifi.config, so the ES menu only ever remembers one
# SSID. connman reads every *.config in that directory live, so the extras
# live in a second file (extra.config) that the ES menu never touches.
#
# Networks and passphrases are kept on this Mac in .storage/wifi-networks
# (git-ignored, mode 600), one per line:  ssid<TAB>passphrase<TAB>hidden
# An empty passphrase means an open network; "hidden" is optional.
#
# Usage:
#   scripts/wifi.sh list             show networks stored on this Mac
#   scripts/wifi.sh add <ssid> [--hidden]   add/replace (prompts for passphrase)
#   scripts/wifi.sh remove <ssid>
#   scripts/wifi.sh push             write extra.config on the device
#   scripts/wifi.sh status           what connman is doing right now
#
# Env:
#   RG351V_HOST      SSH target (default root@RG351V.local)
#   WIFI_NETWORKS    networks file (default .storage/wifi-networks)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

NETWORKS="${WIFI_NETWORKS:-$ROOT/.storage/wifi-networks}"
REMOTE_DIR="/storage/.cache/connman"
REMOTE_FILE="$REMOTE_DIR/extra.config"
TAB=$'\t'

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

# Print the networks file without the entry for <ssid>.
without() {
  local ssid s rest
  while IFS="$TAB" read -r s rest || [[ -n "$s" ]]; do
    [[ -z "$s" || "$s" == "$1" ]] && continue
    printf '%s\t%s\n' "$s" "$rest"
  done <"$NETWORKS"
}

generate() {
  local ssid psk hidden hex id
  printf '[global]\nName=rg351v-extra\n'
  while IFS="$TAB" read -r ssid psk hidden || [[ -n "$ssid" ]]; do
    [[ -z "$ssid" || "$ssid" == \#* ]] && continue
    hex="$(printf '%s' "$ssid" | xxd -p | tr -d '\n')"
    id="$(printf '%s' "$ssid" | shasum -a 1 | cut -c1-12)"
    printf '\n[service_x%s]\nType=wifi\nName=%s\nSSID=%s\n' \
      "$id" "$(escape "$ssid")" "$hex"
    if [[ -n "$psk" ]]; then
      printf 'Passphrase=%s\n' "$(escape "$psk")"
    else
      printf 'Security=none\n'
    fi
    if [[ "$hidden" == hidden ]]; then
      printf 'Hidden=true\n'
    fi
  done <"$NETWORKS"
}

cmd="${1:-}"
case "$cmd" in
list)
  ensure_file
  [[ -s "$NETWORKS" ]] || {
    echo "(no networks stored)"
    exit 0
  }
  while IFS="$TAB" read -r ssid psk hidden || [[ -n "$ssid" ]]; do
    [[ -n "$ssid" ]] || continue
    printf '%s  [%s%s]\n' "$ssid" "$([[ -n "$psk" ]] && echo psk || echo open)" \
      "$([[ "$hidden" == hidden ]] && echo ", hidden")"
  done <"$NETWORKS"
  ;;
add)
  ssid="${2:-}"
  [[ -n "$ssid" ]] || die "usage: ${0##*/} add <ssid> [--hidden]"
  hidden=""
  [[ "${3:-}" == "--hidden" ]] && hidden=hidden
  check_text "SSID" "$ssid"
  [[ "$ssid" != *"$TAB"* ]] || die "SSID must not contain tabs"
  [[ ${#ssid} -le 32 ]] || die "SSID is longer than 32 bytes"
  ensure_file
  read -rsp "passphrase for '$ssid' (empty = open network): " psk
  echo
  check_text "passphrase" "$psk"
  [[ "$psk" != *"$TAB"* ]] || die "passphrase must not contain tabs"
  [[ -z "$psk" || (${#psk} -ge 8 && ${#psk} -le 63) ]] ||
    die "WPA passphrase must be 8-63 characters"
  tmp="$(mktemp "$NETWORKS.XXXXXX")"
  chmod 600 "$tmp"
  {
    without "$ssid"
    printf '%s\t%s\t%s\n' "$ssid" "$psk" "$hidden"
  } >"$tmp"
  mv "$tmp" "$NETWORKS"
  echo "stored '$ssid' (run: ${0##*/} push)"
  ;;
remove)
  [[ -n "${2:-}" ]] || die "usage: ${0##*/} remove <ssid>"
  ensure_file
  tmp="$(mktemp "$NETWORKS.XXXXXX")"
  chmod 600 "$tmp"
  without "$2" >"$tmp"
  mv "$tmp" "$NETWORKS"
  echo "removed '$2' from this Mac (run: ${0##*/} push)"
  ;;
push)
  ensure_file
  assert_device
  # Back up the ES-managed file once, per the repo's safety rules.
  remote "[ -f $REMOTE_DIR/wifi.config.bak ] || cp -p $REMOTE_DIR/wifi.config $REMOTE_DIR/wifi.config.bak 2>/dev/null; true"
  if [[ -z "$(without "")" ]]; then
    remote "rm -f $REMOTE_FILE"
    echo "no networks stored: removed $REMOTE_FILE"
    exit 0
  fi
  tmp="$(mktemp)"
  trap 'rm -f "$tmp"' EXIT
  generate >"$tmp"
  # Upload under a name connman ignores, then rename so it never reads a
  # half-written file.
  "${SCP[@]}" "$tmp" "$HOST:$REMOTE_FILE.tmp"
  remote "chmod 600 $REMOTE_FILE.tmp && mv $REMOTE_FILE.tmp $REMOTE_FILE"
  sleep 3
  remote "journalctl -u connman --no-pager | grep -E 'configuration' | tail -6"
  echo "pushed $(without "" | grep -c .) network(s) to $HOST:$REMOTE_FILE"
  ;;
status)
  assert_device
  remote "connmanctl services | head -12; echo; ls $REMOTE_DIR/*.config; echo; journalctl -u connman --no-pager | grep -E 'configuration|Failed|failed' | tail -5"
  ;;
*)
  sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
  exit 1
  ;;
esac

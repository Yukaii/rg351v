#!/bin/sh
# Runs ON the RG351V (BusyBox ash), not on the Mac.
#
# AmberELEC's ES Wi-Fi menu keeps exactly one network: every save rewrites
# /storage/.cache/connman/wifi.config. This script remembers each network
# that was ever saved there and hands the others to connman through a second
# file, known.config, which connman loads live. The network currently in
# wifi.config is left out of known.config so connman never sees one SSID
# from two files.
#
# Usage:
#   wifi-remember.sh run             learn the network now in wifi.config
#   wifi-remember.sh import          merge "name<TAB>psk<TAB>flags" lines from stdin
#   wifi-remember.sh forget <name>   drop a remembered network
#   wifi-remember.sh list            remembered networks (no secrets)
#   wifi-remember.sh regen           rebuild known.config from the store
#
# flags: "open" (no passphrase, Security=none), "hidden". Learned networks
# have none. Values are kept in config-file syntax (backslashes escaped), the
# same text connman would read from wifi.config.
#
# Env (for testing):
#   WIFI_DIR     connman state dir  (default /storage/.cache/connman)
#   WIFI_STORE   store file         (default /storage/.config/rg351v/wifi-known)

DIR="${WIFI_DIR:-/storage/.cache/connman}"
STORE="${WIFI_STORE:-/storage/.config/rg351v/wifi-known}"
TAB="$(printf '\t')"

CUR_NAME=
CUR_PSK=

# Sets CUR_NAME/CUR_PSK from the ES-managed service block in wifi.config.
read_current() {
  CUR_NAME=
  CUR_PSK=
  [ -f "$DIR/wifi.config" ] || return 0
  in_block=0
  while IFS= read -r line; do
    case "$line" in
    "[service_amberelec_default]") in_block=1 ;;
    "["*) in_block=0 ;;
    Name=*) [ "$in_block" = 1 ] && CUR_NAME="${line#Name=}" ;;
    Passphrase=*) [ "$in_block" = 1 ] && CUR_PSK="${line#Passphrase=}" ;;
    esac
  done <"$DIR/wifi.config"
  return 0
}

# upsert <name> <psk> <flags>: replace or append in the store (atomic).
upsert() {
  mkdir -p "$(dirname "$STORE")"
  tmp="$STORE.tmp"
  : >"$tmp"
  if [ -f "$STORE" ]; then
    while IFS= read -r l; do
      [ -n "$l" ] || continue
      [ "${l%%"$TAB"*}" = "$1" ] || printf '%s\n' "$l" >>"$tmp"
    done <"$STORE"
  fi
  printf '%s\t%s\t%s\n' "$1" "$2" "$3" >>"$tmp"
  chmod 600 "$tmp"
  mv "$tmp" "$STORE"
}

# Rebuild known.config: every stored network except the one in wifi.config.
regen() {
  read_current
  out="$DIR/known.config.tmp"
  : >"$out"
  count=0
  if [ -f "$STORE" ]; then
    while IFS= read -r l; do
      [ -n "$l" ] || continue
      name="${l%%"$TAB"*}"
      rest="${l#*"$TAB"}"
      psk="${rest%%"$TAB"*}"
      flags="${rest#*"$TAB"}"
      [ "$name" = "$CUR_NAME" ] && continue
      id="$(printf '%s' "$name" | md5sum | cut -c1-12)"
      [ "$count" = 0 ] && printf '[global]\nName=rg351v-known\n' >>"$out"
      {
        printf '\n[service_x%s]\nType=wifi\nName=%s\n' "$id" "$name"
        [ -n "$psk" ] && printf 'Passphrase=%s\n' "$psk"
        case "$flags" in *open*) printf 'Security=none\n' ;; esac
        case "$flags" in *hidden*) printf 'Hidden=true\n' ;; esac
      } >>"$out"
      count=$((count + 1))
    done <"$STORE"
  fi
  if [ "$count" = 0 ]; then
    rm -f "$out" "$DIR/known.config"
  else
    chmod 600 "$out"
    mv "$out" "$DIR/known.config"
  fi
  echo "known.config: $count network(s)"
}

case "${1:-run}" in
run)
  read_current
  case "$CUR_NAME" in
  "")
    echo "no network in wifi.config; nothing to learn"
    ;;
  *"$TAB"*)
    echo "skipping SSID containing a tab" >&2
    ;;
  *)
    upsert "$CUR_NAME" "$CUR_PSK" ""
    echo "remembered: $CUR_NAME"
    ;;
  esac
  regen
  ;;
import)
  while IFS= read -r l; do
    [ -n "$l" ] || continue
    name="${l%%"$TAB"*}"
    rest="${l#*"$TAB"}"
    upsert "$name" "${rest%%"$TAB"*}" "${rest#*"$TAB"}"
  done
  regen
  ;;
forget)
  name="${2:-}"
  [ -n "$name" ] || {
    echo "usage: $0 forget <name>" >&2
    exit 1
  }
  read_current
  if [ "$name" = "$CUR_NAME" ]; then
    echo "'$name' is the network saved in the ES menu; save a different one there first" >&2
    exit 1
  fi
  [ -f "$STORE" ] || exit 0
  tmp="$STORE.tmp"
  : >"$tmp"
  while IFS= read -r l; do
    [ -n "$l" ] || continue
    [ "${l%%"$TAB"*}" = "$name" ] || printf '%s\n' "$l" >>"$tmp"
  done <"$STORE"
  chmod 600 "$tmp"
  mv "$tmp" "$STORE"
  regen
  ;;
list)
  read_current
  [ -f "$STORE" ] || exit 0
  while IFS= read -r l; do
    [ -n "$l" ] || continue
    name="${l%%"$TAB"*}"
    rest="${l#*"$TAB"}"
    psk="${rest%%"$TAB"*}"
    flags="${rest#*"$TAB"}"
    kind=psk
    [ -n "$psk" ] || kind=open
    cur=
    [ "$name" = "$CUR_NAME" ] && cur=" (ES menu)"
    hid=
    case "$flags" in *hidden*) hid=" hidden" ;; esac
    echo "$name  [$kind$hid]$cur"
  done <"$STORE"
  ;;
regen)
  regen
  ;;
*)
  echo "usage: $0 run|import|forget <name>|list|regen" >&2
  exit 1
  ;;
esac

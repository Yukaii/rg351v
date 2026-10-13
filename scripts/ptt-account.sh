#!/usr/bin/env bash
# Manage the PTT login that ptterm's Y button types for you.
#
# The ID and password are written to /storage/.config/ptterm/account on the
# device (ext4, mode 600). /storage is not shared over SMB; do not move the
# file under /storage/roms, which is a public root-writable share. Input is
# read without echo and sent over SSH stdin, never argv or shell history.
#
# Usage:
#   scripts/ptt-account.sh set      prompt for ID and password, save on device
#   scripts/ptt-account.sh status   show whether one is saved (never the password)
#   scripts/ptt-account.sh clear    delete it
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

DIR="/storage/.config/ptterm"
FILE="$DIR/account"

remote() { "${SSH[@]}" "$HOST" "$@"; }

die() {
  echo "$*" >&2
  exit 1
}

case "${1:-}" in
set)
  read -rp "PTT ID: " user
  read -rsp "PTT password: " pass
  echo
  [[ -n "$user" && -n "$pass" ]] || die "ID and password are required"
  [[ "$user$pass" != *[[:cntrl:]]* ]] || die "no control characters allowed"
  [[ ${#user} -le 12 ]] || die "PTT IDs are at most 12 characters"
  [[ ${#pass} -le 8 ]] || echo "note: PTT only uses the first 8 characters of a password" >&2
  assert_device
  printf '%s\n%s\n' "$user" "$pass" |
    remote "umask 077; mkdir -p $DIR && cat > $FILE.tmp && mv $FILE.tmp $FILE && chmod 600 $FILE"
  unset pass
  echo "saved for '$user' (Y on the PTT login screen will use it)"
  ;;
status)
  assert_device
  remote "if [ -f $FILE ]; then
      echo \"saved: ID '\$(head -n1 $FILE)', \$(wc -c < $FILE) bytes, \$(ls -l $FILE | cut -c1-10)\"
    else echo 'none saved (Y falls back to guest)'; fi"
  ;;
clear)
  assert_device
  remote "rm -f $FILE $FILE.tmp && echo cleared"
  ;;
*)
  sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
  exit 1
  ;;
esac

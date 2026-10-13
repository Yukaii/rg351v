#!/usr/bin/env bash
# Snapshot the RG351V's power/CPU/GPU/thermal state over SSH.
#
# Usage:
#   scripts/perf.sh              one snapshot
#   scripts/perf.sh --watch [N]  refresh every N seconds (default 2)
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

snapshot() {
  "${SSH[@]}" "$HOST" 'sh -s -- once' <"$HERE/device/sample.sh"
}

assert_device

case "${1:-}" in
"")
  snapshot
  ;;
--watch)
  secs="${2:-2}"
  while :; do
    out="$(snapshot)"
    printf '\033[H\033[2J%s  %s\n\n%s\n' "$HOST" "$(date +%T)" "$out"
    sleep "$secs"
  done
  ;;
*)
  echo "usage: ${0##*/} [--watch [secs]]" >&2
  exit 1
  ;;
esac

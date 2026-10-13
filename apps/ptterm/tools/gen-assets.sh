#!/usr/bin/env bash
# Download ptterm's font/encoding sources and generate its runtime assets into
# .storage/ptterm-data/ (git-ignored; ~2 MB). Safe to re-run.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(cd "$HERE/../../.." && pwd)/.storage/ptterm-data"
UNIFONT_VER="${UNIFONT_VER:-18.0.01}"

mkdir -p "$OUT"
[[ -f "$OUT/unifont_all.hex.gz" ]] ||
  curl -fsSL -o "$OUT/unifont_all.hex.gz" \
    "https://ftp.gnu.org/gnu/unifont/unifont-$UNIFONT_VER/unifont_all-$UNIFONT_VER.hex.gz"
[[ -f "$OUT/uao250-b2u.txt" ]] ||
  curl -fsSL -o "$OUT/uao250-b2u.txt" \
    "https://moztw.org/docs/big5/table/uao250-b2u.txt"

python3 "$HERE/gen-assets.py" "$OUT/unifont_all.hex.gz" "$OUT/uao250-b2u.txt" \
  "$OUT/unifont.bin" "$OUT/big5uao.bin"

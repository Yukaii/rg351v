#!/usr/bin/env bash
# Cross-build a CMake port for Linux/aarch64 (RG351V) inside Docker.
#
# The game source is mounted read-only; build output stays in a named volume
# and the final binary is copied to .storage/<name>/.
#
# Usage:
#   scripts/build-port.sh <name> [--image-only] [--shell]
#
# <name> is a directory under scripts/ports/ holding port.conf + launcher.sh.
# Start a new port with: cp -r scripts/ports/_template scripts/ports/<name>
#
# Env:
#   PORT_SRC  game source dir (default: SRC_DIR from the port's port.conf)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
IMAGE="rg351v-port-build:latest"

NAME="${1:-}"
[[ "$NAME" =~ ^[a-z0-9][a-z0-9_-]*$ && -f "$HERE/ports/$NAME/port.conf" ]] || {
  echo "usage: ${0##*/} <name> [--image-only|--shell]" >&2
  echo -n "ports:" >&2
  for d in "$HERE"/ports/*/; do
    d="${d%/}"
    [[ "${d##*/}" == _* ]] || echo -n " ${d##*/}" >&2
  done
  echo >&2
  exit 1
}
MODE="${2:-}"

# shellcheck source=/dev/null
source "$HERE/ports/$NAME/port.conf"

SRC="${PORT_SRC:-$ROOT/$SRC_DIR}"
OUT="$ROOT/.storage/$NAME"
BUILD_VOL="$NAME-build"
DEPS_VOL="$NAME-deps"

echo "==> building image $IMAGE"
docker build -f "$HERE/ports/Dockerfile" -t "$IMAGE" "$HERE/ports"

[[ "$MODE" == "--image-only" ]] && exit 0

[[ -f "$SRC/CMakeLists.txt" ]] || {
  echo "game source not found: $SRC (set PORT_SRC)" >&2
  exit 1
}
SRC="$(cd "$SRC" && pwd)"

mkdir -p "$OUT"

if [[ "$MODE" == "--shell" ]]; then
  docker run --rm -it -v "$SRC:/src:ro" -v "$BUILD_VOL:/build" \
    -v "$DEPS_VOL:/deps" -v "$OUT:/out" "$IMAGE" bash
  exit 0
fi

run() {
  docker run --rm -v "$SRC:/src:ro" -v "$BUILD_VOL:/build" \
    -v "$DEPS_VOL:/deps" "$IMAGE" "$@"
}

if [[ -n "${PREPARE_DEPS:-}" ]]; then
  echo "==> preparing dependencies"
  run bash -c "$PREPARE_DEPS"
fi

echo "==> configuring (source: $SRC)"
run cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release "${CMAKE_ARGS[@]}"

echo "==> building"
run cmake --build /build

echo "==> exporting binary to $OUT"
docker run --rm -v "$BUILD_VOL:/build" -v "$OUT:/out" "$IMAGE" \
  bash -c 'for p in "$@"; do
    if [[ -f /build/$p ]]; then
      cp -p "/build/$p" "/out/$0"
      readelf -h "/out/$0" | grep -E "Class|Machine"
      readelf -d "/out/$0" | grep NEEDED
      exit 0
    fi
  done
  echo "binary not found in build tree" >&2; exit 1' "$BIN_NAME" "${BIN_PATHS[@]}"

echo "done: $OUT/$BIN_NAME"

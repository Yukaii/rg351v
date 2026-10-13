#!/usr/bin/env bash
# Build pikaball_revamped for Linux/aarch64 (RG351V) inside a Docker container.
#
# The game source is mounted read-only; all build output stays in the
# container and the final binary is copied to .storage/pikaball/.
#
# Usage:
#   scripts/build-pikaball.sh [--image-only] [--shell]
#
# Env:
#   PIKABALL_SRC  game source dir (default ../pikachu-valleyball-ios)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SRC="${PIKABALL_SRC:-$(cd "$ROOT/../pikachu-valleyball-ios" && pwd)}"
OUT="$ROOT/.storage/pikaball"
IMAGE="pikaball-rg351v:latest"

[[ -f "$SRC/CMakeLists.txt" ]] || {
  echo "game source not found: $SRC (set PIKABALL_SRC)" >&2
  exit 1
}

echo "==> building image $IMAGE"
docker build -f "$HERE/pikaball.Dockerfile" -t "$IMAGE" "$HERE"

if [[ "${1:-}" == "--image-only" ]]; then
  exit 0
fi

mkdir -p "$OUT"

docker_cmd=(cmake -S /src -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release
  # No X11/Wayland on the handheld: console (KMSDRM/evdev) build.
  -DSDL_UNIX_CONSOLE_BUILD=ON
  # SDL_ttf's tarball lacks its external/ submodules; use a pre-seeded checkout.
  -DFETCHCONTENT_SOURCE_DIR_SDL_TTF=/deps/sdl_ttf
  # Static libstdc++/libgcc: the device's system ones may be older.
  -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc")
if [[ "${1:-}" == "--shell" ]]; then
  docker run --rm -it -v "$SRC:/src:ro" -v "$OUT:/out" "$IMAGE" bash
  exit 0
fi

echo "==> pre-seeding SDL_ttf with its external/ submodules"
docker run --rm -v pikaball-deps:/deps "$IMAGE" \
  bash -c 'if [[ -f /deps/sdl_ttf/CMakeLists.txt && -f /deps/sdl_ttf/external/harfbuzz/CMakeLists.txt ]]; then echo "SDL_ttf already seeded"; else rm -rf /deps/sdl_ttf && git clone --depth 1 https://github.com/libsdl-org/SDL_ttf.git /deps/sdl_ttf && (cd /deps/sdl_ttf && ./external/download.sh); fi'

echo "==> configuring (source: $SRC)"
docker run --rm -v "$SRC:/src:ro" -v pikaball-build:/build -v pikaball-deps:/deps "$IMAGE" \
  "${docker_cmd[@]}"

echo "==> building (this takes a while: SDL3 + mixer + ttf from source)"
docker run --rm -v "$SRC:/src:ro" -v pikaball-build:/build -v pikaball-deps:/deps "$IMAGE" \
  cmake --build /build

echo "==> exporting binary to $OUT"
docker run --rm -v pikaball-build:/build -v "$OUT:/out" "$IMAGE" \
  bash -c 'bin=$(ls /build/src/pikaball_revamped /build/pikaball_revamped 2>/dev/null | head -n 1) && cp -p "$bin" /out/ && file /out/pikaball_revamped && readelf -d /out/pikaball_revamped | grep NEEDED'

echo "done: $OUT/pikaball_revamped"

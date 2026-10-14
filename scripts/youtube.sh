#!/usr/bin/env bash
# YouTube playback prototype. Streaming extraction runs on the handheld.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"
LOCAL="$ROOT/.storage/youtube"
REMOTE=/storage/roms/homebrew/youtube
mkdir -p "$LOCAL"

case "${1:-}" in
probe)
  assert_device
  "${SSH[@]}" "$HOST" 'mpv --version; mpv --vo=help; mpv --hwdec=help; python3 --version; df -h /storage /storage/roms'
  ;;
prepare)
  [[ $# == 2 ]] || {
    echo "usage: $0 prepare <YouTube URL>" >&2
    exit 1
  }
  # H.264 + AAC avoids AV1/VP9 decoding on the small CPU. Bound the first
  # milestone to a minute; downloading also removes Wi-Fi from the benchmark.
  yt-dlp --no-playlist --quiet --no-warnings \
    -f 'bestvideo[height<=360][fps<=30][vcodec^=avc1]+bestaudio[ext=m4a]/best[height<=360][fps<=30][vcodec^=avc1]' \
    --download-sections '*0-60' --merge-output-format mp4 \
    --force-overwrites -o "$LOCAL/test-360p.%(ext)s" -- "$2" \
    >"$LOCAL/download.log" 2>&1 || {
    echo "download failed; see $LOCAL/download.log" >&2
    exit 1
  }
  echo "prepared $LOCAL/test-360p.mp4"
  ;;
install)
  [[ -s "$LOCAL/test-360p.mp4" ]] || {
    echo "run prepare first" >&2
    exit 1
  }
  assert_device
  "${SSH[@]}" "$HOST" "if [ -d /storage/roms/ports/youtube ]; then
      if [ ! -d $REMOTE ]; then
        mv /storage/roms/ports/youtube $REMOTE
      else
        for file in search.json saved.json; do
          if [ -f /storage/roms/ports/youtube/\$file ] && [ ! -f $REMOTE/\$file ]; then
            cp -p /storage/roms/ports/youtube/\$file $REMOTE/\$file
          fi
        done
        if [ ! -d $REMOTE/thumbnails ] && [ -d /storage/roms/ports/youtube/thumbnails ]; then
          cp -rp /storage/roms/ports/youtube/thumbnails $REMOTE/
        fi
      fi
    fi"
  "${SSH[@]}" "$HOST" "mkdir -p $REMOTE"
  if [[ ! -s "$LOCAL/yt-dlp" ]]; then
    curl -fL --silent --show-error \
      https://github.com/yt-dlp/yt-dlp/releases/download/2026.08.19/yt-dlp \
      -o "$LOCAL/yt-dlp"
  fi
  [[ "$(shasum -a 256 "$LOCAL/yt-dlp" | awk '{print $1}')" == 1fa6733c37ea6fb51c99ad8fe785e7b7e5f3246c9b980230329d4fb72ed8d4d6 ]] || {
    echo "yt-dlp checksum mismatch" >&2
    exit 1
  }
  "${SCP[@]}" "$LOCAL/test-360p.mp4" "$LOCAL/yt-dlp" "$HERE/device/youtube-player.py" \
    "$ROOT/apps/youtube/youtube-ui.py" "$ROOT/.storage/ptterm-data/unifont.bin" "$HOST:$REMOTE/"
  if [[ ! -s "$LOCAL/qjs" ]]; then
    curl -fL --silent --show-error \
      https://github.com/quickjs-ng/quickjs/releases/download/v0.17.0/qjs-linux-aarch64 \
      -o "$LOCAL/qjs"
  fi
  [[ "$(shasum -a 256 "$LOCAL/qjs" | awk '{print $1}')" == 3372133484edf50a69f3c67903af41206d22a061e930e3cfb63269272ef56d2e ]] || {
    echo "QuickJS checksum mismatch" >&2
    exit 1
  }
  "${SCP[@]}" "$LOCAL/qjs" "$HOST:$REMOTE/"
  "${SCP[@]}" "$HERE/ports/youtube/launcher.sh" "$HOST:/storage/roms/homebrew/YouTube.sh"
  "${SCP[@]}" "$HERE/device/gamelist-set.py" "$HOST:/tmp/youtube-gamelist.py"
  "${SSH[@]}" "$HOST" "chmod +x /storage/roms/homebrew/YouTube.sh $REMOTE/qjs
    systemctl stop emustation
    trap 'systemctl start emustation' EXIT
    cp -p /storage/roms/homebrew/gamelist.xml /storage/roms/homebrew/gamelist.xml.bak-youtube
    python3 /tmp/youtube-gamelist.py /storage/roms/homebrew/gamelist.xml ./YouTube.sh YouTube \
      'Search and watch YouTube videos. X: search, A: play, Y: saved videos, R1: save, B: back/exit.' rg351v
    if [ -f /storage/roms/ports/YouTube.sh ]; then
      mv /storage/roms/ports/YouTube.sh /storage/roms/ports/YouTube.sh.bak-homebrew
    fi"
  echo "installed: Homebrew > YouTube"
  ;;
play)
  assert_device
  # Keep SSH open during measurements, and restore ES even if mpv fails.
  "${SSH[@]}" "$HOST" "systemctl stop emustation
      trap 'systemctl start emustation' EXIT
      bash /storage/roms/homebrew/YouTube.sh --play test-360p.mp4"
  ;;
ui)
  assert_device
  "${SSH[@]}" "$HOST" "systemctl stop emustation
      trap 'systemctl start emustation' EXIT
      bash /storage/roms/homebrew/YouTube.sh"
  ;;
stream)
  [[ $# == 2 ]] || {
    echo "usage: $0 stream <YouTube URL>" >&2
    exit 1
  }
  assert_device
  # Send the URL as data, rather than interpolating untrusted text in ash.
  printf '%s\n' "$2" >"$LOCAL/url.txt"
  "${SCP[@]}" "$LOCAL/url.txt" "$HOST:$REMOTE/url.txt"
  "${SSH[@]}" "$HOST" "python3 $REMOTE/yt-dlp --no-playlist --socket-timeout 15 --dump-single-json \
      --js-runtimes quickjs:$REMOTE/qjs \
      -f 'bestvideo[height<=360][fps<=30][vcodec^=avc1]+bestaudio[ext=m4a]/best[height<=360][fps<=30][vcodec^=avc1]' \
      --batch-file $REMOTE/url.txt > $REMOTE/stream.json"
  "${SSH[@]}" "$HOST" "systemctl stop emustation
      trap 'systemctl start emustation' EXIT
      bash /storage/roms/homebrew/YouTube.sh --play stream.json"
  ;;
log)
  assert_device
  "${SSH[@]}" "$HOST" 'cat /tmp/logs/youtube.log'
  ;;
*)
  echo "usage: $0 {probe|prepare <URL>|install|ui|play|stream <URL>|log}" >&2
  exit 1
  ;;
esac

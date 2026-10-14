#!/usr/bin/env bash
# YouTube playback prototype. Streaming extraction runs on the handheld.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"
LOCAL="$ROOT/.storage/youtube"
REMOTE=/storage/roms/ports/youtube
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
  "${SCP[@]}" "$LOCAL/test-360p.mp4" "$LOCAL/yt-dlp" "$HERE/device/youtube-player.py" "$HOST:$REMOTE/"
  "${SCP[@]}" "$HERE/ports/youtube/launcher.sh" "$HOST:/storage/roms/ports/YouTube.sh"
  "${SSH[@]}" "$HOST" 'chmod +x /storage/roms/ports/YouTube.sh'
  echo "installed: Ports > YouTube (refresh the game list if needed)"
  ;;
play)
  assert_device
  # Keep SSH open during measurements, and restore ES even if mpv fails.
  "${SSH[@]}" "$HOST" "systemctl stop emustation
      trap 'systemctl start emustation' EXIT
      bash /storage/roms/ports/YouTube.sh"
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
      -f 'bestvideo[height<=360][fps<=30][vcodec^=avc1]+bestaudio[ext=m4a]/best[height<=360][fps<=30][vcodec^=avc1]' \
      --batch-file $REMOTE/url.txt > $REMOTE/stream.json"
  "${SSH[@]}" "$HOST" "systemctl stop emustation
      trap 'systemctl start emustation' EXIT
      bash /storage/roms/ports/YouTube.sh stream.json"
  ;;
log)
  assert_device
  "${SSH[@]}" "$HOST" 'cat /tmp/logs/youtube.log'
  ;;
*)
  echo "usage: $0 {probe|prepare <URL>|install|play|stream <URL>|log}" >&2
  exit 1
  ;;
esac

#!/usr/bin/env bash
# Manage TJA songs for the Taiko player on the RG351V.
#
# Usage:
#   scripts/taiko.sh sync <dir> [--dry-run]   copy every chart folder found under <dir>
#   scripts/taiko.sh list                      list songs on the device
#   scripts/taiko.sh remove <folder> [--yes]   delete one song folder from the device
#
# sync only sends what the player can use (.tja plus .ogg/.wav/.mp3 audio) and
# checks each chart's WAVE: file first (exact, other case, other extension,
# Windows paths). Shift-JIS charts are sent as they are; the player converts
# them. Folders are flattened to the chart folder's name so the player's
# two-level scan always finds them.
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

DEST="/storage/roms/homebrew/taiko/songs"

usage() {
  sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-1}"
}

# Prints one "<src>\t<dest-name>\t<charts>\t<ok>\t<note>" line per chart folder.
plan() {
  python3 - "$1" <<'PY'
import os, sys

AUDIO = (".ogg", ".wav", ".mp3")

def text_of(path):
    b = open(path, "rb").read()
    for enc in ("utf-8-sig", "cp932"):
        try:
            return b.decode(enc)
        except UnicodeDecodeError:
            pass
    return b.decode("latin-1")

def wave_of(path):
    for line in text_of(path).splitlines():
        if line.upper().startswith("WAVE:"):
            return line.split(":", 1)[1].strip()
    return ""

def audio_ok(d, wave):
    if not wave:
        return False
    rel = wave.replace("\\", "/")
    if os.path.isfile(os.path.join(d, rel)) and rel.lower().endswith(AUDIO):
        return True
    base = os.path.basename(rel)
    stem = os.path.splitext(base)[0].lower()
    for n in os.listdir(d):
        if n.lower() == base.lower() and n.lower().endswith(AUDIO):
            return True
        if os.path.splitext(n)[0].lower() == stem and n.lower().endswith(AUDIO):
            return True
    return False

root = os.path.abspath(sys.argv[1])
used = {}
for d, _dirs, files in sorted(os.walk(root)):
    charts = [f for f in files if f.lower().endswith(".tja")]
    if not charts:
        continue
    ok = 0
    notes = []
    for c in charts:
        w = wave_of(os.path.join(d, c))
        if audio_ok(d, w):
            ok += 1
        else:
            notes.append(f"{c}: audio '{w}' not found")
    name = os.path.basename(d) or "songs"
    n, i = name, 2
    while n in used and used[n] != d:
        n = f"{name}-{i}"
        i += 1
    used[n] = d
    print("\t".join([d, n, str(len(charts)), str(ok), "; ".join(notes)]))
PY
}

cmd_sync() {
  local src="${1:-}" dry=0
  [[ -d "$src" ]] || usage
  [[ "${2:-}" == "--dry-run" ]] && dry=1
  local rows
  rows="$(plan "$src")"
  [[ -n "$rows" ]] || {
    echo "no .tja charts found under $src" >&2
    exit 1
  }
  if [[ "$dry" -eq 0 ]]; then
    assert_device
    "${SSH[@]}" "$HOST" "mkdir -p '$DEST'"
  fi
  local rsync_args=(-rLt --no-perms --no-owner --no-group --omit-dir-times --prune-empty-dirs
    -e "ssh -4 -o BatchMode=yes -o ConnectTimeout=8"
    --include='*/' --include='*.[tT][jJ][aA]' --include='*.[oO][gG][gG]'
    --include='*.[wW][aA][vV]' --include='*.[mM][pP]3' --exclude='*')
  local sent=0 warned=0 d name n ok note
  while IFS=$'\t' read -r d name n ok note; do
    if [[ "$ok" -lt "$n" ]]; then
      warned=$((warned + 1))
      echo "WARN  $name: $ok/$n charts have audio ($note); unplayable ones are hidden by the player" >&2
    else
      echo "ok    $name ($n chart(s))"
    fi
    [[ "$ok" -gt 0 ]] || continue
    if [[ "$dry" -eq 1 ]]; then
      # local listing only: rsync -n would still need the device to be awake
      (cd "$d" && find . -type f \( -iname '*.tja' -o -iname '*.ogg' -o -iname '*.wav' -o -iname '*.mp3' \) |
        sort | sed "s|^\./|        -> $name/|")
    else
      rsync "${rsync_args[@]}" "$d/" "$HOST:$DEST/$name/"
    fi
    sent=$((sent + 1))
  done <<<"$rows"
  echo "done: $sent folder(s) $([[ "$dry" -eq 1 ]] && echo 'would be ')sent, $warned with warnings"
}

cmd_list() {
  assert_device
  "${SSH[@]}" "$HOST" "cd '$DEST' 2>/dev/null && find . -iname '*.tja' | sort | while IFS= read -r f; do
      printf '%s\t%s\n' \"\$f\" \"\$(awk '/^[Tt][Ii][Tt][Ll][Ee]:/{print; exit}' \"\$f\")\"
    done" |
    python3 -c '
import sys
for raw in sys.stdin.buffer:
    for enc in ("utf-8", "cp932"):
        try:
            line = raw.decode(enc).rstrip("\n"); break
        except UnicodeDecodeError:
            line = raw.decode("latin-1").rstrip("\n")
    path, _, title = line.partition("\t")
    print(f"{path[2:]:<60} {title.split(":",1)[-1].strip().lstrip(chr(0xFEFF))}")
'
}

cmd_remove() {
  local name="${1:-}" yes=0
  [[ "${2:-}" == "--yes" ]] && yes=1
  [[ -n "$name" && "$name" != /* && "$name" != *..* ]] || {
    echo "refusing: give a folder name under $DEST (no '..' or leading '/')" >&2
    exit 1
  }
  assert_device
  "${SSH[@]}" "$HOST" "test -d '$DEST/$name'" || {
    echo "not found: $DEST/$name" >&2
    exit 1
  }
  "${SSH[@]}" "$HOST" "ls -la '$DEST/$name'"
  if [[ "$yes" -eq 0 ]]; then
    read -r -p "delete $DEST/$name ? [y/N] " a
    [[ "$a" == y || "$a" == Y ]] || {
      echo "aborted"
      exit 1
    }
  fi
  "${SSH[@]}" "$HOST" "rm -rf '$DEST/$name'"
  echo "removed $name"
}

case "${1:-}" in
  sync) shift; cmd_sync "$@" ;;
  list) cmd_list ;;
  remove) shift; cmd_remove "$@" ;;
  -h | --help | help) usage 0 ;;
  *) usage ;;
esac

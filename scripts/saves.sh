#!/usr/bin/env bash
# Sync game saves between the RG351V, this Mac, and RomM.
#
#   saves.sh backup [--dry-run]   device save files -> .storage/saves/
#   saves.sh restore [--dry-run]  .storage/saves/ -> device
#   saves.sh pull [--pick] [--dry-run]    RomM cloud saves -> device
#   saves.sh push --rom-id ID FILE [--emulator E] [--slot S] [--overwrite]
#
# The device itself never talks to RomM; this Mac is the bridge.
set -euo pipefail

HOST="${RG351V_HOST:-root@RG351V.local}"
SAVES_DIR="${ROMM_SAVES:-.storage/saves}"
DRY_RUN=0
SSH=(ssh -4 -o BatchMode=yes -o ConnectTimeout=8)
RSYNC_SSH="ssh -4 -o BatchMode=yes -o ConnectTimeout=8"
SCRATCH_DIR=""
trap '[ -n "${SCRATCH_DIR:-}" ] && rm -rf "$SCRATCH_DIR"' EXIT

# Refuse to touch anything that is not the handheld (see docs/gotchas.md).
assert_device() {
  local name
  name="$("${SSH[@]}" "$HOST" 'hostname')"
  [[ "$name" == "RG351V" ]] || {
    echo "refusing: $HOST answers as '$name', not RG351V" >&2
    exit 1
  }
}
usage() {
  cat <<'EOF'
Usage: saves.sh <command> [options]

Commands:
  backup [--dry-run]   pull device save files into .storage/saves/
  restore [--dry-run]  push .storage/saves/ back to the device
  pull [--pick] [--dry-run]
                       download RomM cloud saves and place them next
                       to their ROMs on the device
  push --rom-id ID FILE [--emulator E] [--slot S] [--overwrite]
                       upload a save file to RomM for one ROM

Env:
  RG351V_HOST   SSH target (default root@RG351V.local)
  ROMM_SAVES    local saves dir (default .storage/saves)
EOF
}

rsync_from_device() {
  # $1 = remote path, $2 = local dest, rest = extra rsync filters
  local remote="$1" dest="$2"
  shift 2
  if [[ "$DRY_RUN" -eq 1 ]]; then
    rsync -avh -n -e "$RSYNC_SSH" "$@" "$HOST:$remote" "$dest"
  else
    mkdir -p "$dest"
    rsync -avh -e "$RSYNC_SSH" "$@" "$HOST:$remote" "$dest"
  fi
}

rsync_to_device() {
  # $1 = local src, $2 = remote path
  local src="$1" remote="$2"
  if [[ "$DRY_RUN" -eq 1 ]]; then
    rsync -avh -n --omit-dir-times -e "$RSYNC_SSH" "$src" "$HOST:$remote"
  else
    rsync -avh --omit-dir-times -e "$RSYNC_SSH" "$src" "$HOST:$remote"
  fi
}

cmd_backup() {
  assert_device
  echo "==> backing up device saves to $SAVES_DIR"
  # RetroArch battery saves + core data
  rsync_from_device "/storage/.config/retroarch/savefiles/" "$SAVES_DIR/retroarch-savefiles/"
  # Save states (per-system dirs under /storage/roms/savestates/)
  rsync_from_device "/storage/roms/savestates/" "$SAVES_DIR/savestates/"
  # Sidecar SRAM next to ROMs (*.srm/*.sav), keeping platform subdirs
  rsync_from_device "/storage/roms/" "$SAVES_DIR/roms-sidecar/" \
    --include='*/' --include='*.srm' --include='*.sav' --exclude='*'
  # PPSSPP save data (may not exist yet)
  rsync_from_device "/storage/.config/ppsspp/PSP/SAVEDATA/" "$SAVES_DIR/ppsspp-savedata/" || true
  echo "done"
}

cmd_restore() {
  assert_device
  [[ -d "$SAVES_DIR" ]] || {
    echo "nothing to restore: $SAVES_DIR missing (run backup first)" >&2
    exit 1
  }
  echo "==> restoring $SAVES_DIR to device"
  [[ -d "$SAVES_DIR/retroarch-savefiles" ]] &&
    rsync_to_device "$SAVES_DIR/retroarch-savefiles/" "/storage/.config/retroarch/savefiles/"
  [[ -d "$SAVES_DIR/savestates" ]] &&
    rsync_to_device "$SAVES_DIR/savestates/" "/storage/roms/savestates/"
  [[ -d "$SAVES_DIR/roms-sidecar" ]] &&
    rsync_to_device "$SAVES_DIR/roms-sidecar/" "/storage/roms/"
  [[ -d "$SAVES_DIR/ppsspp-savedata" ]] &&
    rsync_to_device "$SAVES_DIR/ppsspp-savedata/" "/storage/.config/ppsspp/PSP/SAVEDATA/"
  if [[ "$DRY_RUN" -eq 0 ]]; then
    echo "==> refreshing EmulationStation"
    "${SSH[@]}" "$HOST" 'systemctl restart emustation'
  fi
  echo "done"
}

cmd_pull() {
  local pick=0
  while [[ $# -gt 0 ]]; do
    case "$1" in
    --pick)
      pick=1
      shift
      ;;
    -n | --dry-run)
      DRY_RUN=1
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
    esac
  done

  assert_device
  local scratch
  SCRATCH_DIR="$(mktemp -d /tmp/romm-saves.XXXXXX)"
  scratch="$SCRATCH_DIR"

  echo "==> fetching RomM saves index"
  romm-cli saves list --json >"$scratch/saves.json"
  local count
  count="$(jq 'length' "$scratch/saves.json")"
  [[ "$count" -gt 0 ]] || {
    echo "no saves on RomM" >&2
    exit 0
  }

  local rows ids
  rows="$(jq -r '.[] | "\(.id)\t\(.rom_id)\t\(.emulator)\t\(.slot // "-")\t\(.file_name)"' "$scratch/saves.json")"
  if [[ "$pick" -eq 1 ]]; then
    command -v fzf >/dev/null || {
      echo "fzf not found (brew install fzf)" >&2
      exit 1
    }
    rows="$(printf '%s\n' "$rows" | fzf --multi --prompt='save> ' \
      --delimiter=$'\t' --with-nth='2..' || true)"
    [[ -n "$rows" ]] || {
      echo "nothing selected" >&2
      exit 0
    }
  fi
  ids="$(printf '%s\n' "$rows" | cut -f1)"

  local id rom_id file_name rom_json platform base ext stem romdir dest
  while IFS= read -r id; do
    [[ -n "$id" ]] || continue
    rom_id="$(jq -r --argjson id "$id" '.[] | select(.id==$id) | .rom_id // empty' "$scratch/saves.json")"
    file_name="$(jq -r --argjson id "$id" '.[] | select(.id==$id) | .file_name' "$scratch/saves.json")"
    # saves-list omits rom_id, so resolve it via saves-get when missing.
    if [[ -z "${rom_id:-}" ]]; then
      rom_id="$(romm-cli saves get "$id" --json | jq -r '.rom_id // empty')"
    fi
    platform=""
    base=""
    if [[ -n "${rom_id:-}" ]]; then
      rom_json="$(romm-cli roms get "$rom_id" --json)"
      platform="$(printf '%s' "$rom_json" | jq -r '.platform_slug')"
      base="$(printf '%s' "$rom_json" | jq -r '.fs_name_no_ext')"
    fi
    ext="${file_name##*.}"
    stem="${file_name%.*}"

    # Find where that ROM lives on the device (slug dirs may differ, so match by name).
    # Quote-ish chars become ? globs so names like "Yoshi's Island" still match.
    romdir=""
    if [[ -n "${base:-}" ]]; then
      local glob="${base//[\'\"$\`\\]/?}"
      romdir="$("${SSH[@]}" "$HOST" "f=\$(find /storage/roms -maxdepth 2 -name \"$glob.*\" -not -name '*.srm' -not -name '*.sav' 2>/dev/null | head -n 1); [ -n \"\$f\" ] && dirname \"\$f\")" </dev/null)" || true
    fi
    if [[ -z "${romdir:-}" && -n "$platform" ]] &&
      "${SSH[@]}" "$HOST" "test -d /storage/roms/$platform" </dev/null 2>/dev/null; then
      # ROM not on device (yet), but its platform dir exists: stage it there.
      echo "   note: ROM '$base' not on device; staging in platform dir /storage/roms/$platform/"
      romdir="/storage/roms/$platform"
    fi
    if [[ -z "${romdir:-}" || "$romdir" == "." ]]; then
      echo "!! id $id ($file_name): ROM '$base' not on device, downloading to $SAVES_DIR/unplaced/"
      dest="$SAVES_DIR/unplaced"
      [[ "$DRY_RUN" -eq 0 ]] && mkdir -p "$dest"
    else
      dest="device:$romdir"
    fi

    if [[ "$DRY_RUN" -eq 1 ]]; then
      echo "would place $file_name -> $dest"
      continue
    fi

    echo "==> save $id ($file_name) -> $dest"
    if [[ "$dest" == device:* ]]; then
      romdir="${dest#device:}"
      romm-cli saves download "$id" -o "$scratch" --overwrite
      scp -4 -o BatchMode=yes -o ConnectTimeout=8 "$scratch/$file_name" "$HOST:$romdir/$file_name" </dev/null
      if [[ "$stem" != "$base" ]]; then
        echo "   note: emulator SRAM must be named '$base.$ext' to autoload;"
        echo "   rename on device if this is the active save: $romdir/$file_name"
      fi
      rm -f "$scratch/$file_name"
    else
      romm-cli saves download "$id" -o "$dest" --overwrite
    fi
  done <<<"$ids"
  echo "done"
}

cmd_push() {
  local rom_id="" file="" emulator="" slot="" overwrite=()
  while [[ $# -gt 0 ]]; do
    case "$1" in
    --rom-id)
      rom_id="$2"
      shift 2
      ;;
    --emulator)
      emulator="$2"
      shift 2
      ;;
    --slot)
      slot="$2"
      shift 2
      ;;
    --overwrite)
      overwrite=(--overwrite)
      shift
      ;;
    -h | --help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
    *)
      file="$1"
      shift
      ;;
    esac
  done
  [[ -n "$rom_id" && -n "$file" ]] || {
    echo "usage: saves.sh push --rom-id ID FILE [--emulator E] [--slot S] [--overwrite]" >&2
    exit 2
  }
  local args=(--rom-id "$rom_id")
  [[ -n "$emulator" ]] && args+=(--emulator "$emulator")
  [[ -n "$slot" ]] && args+=(--slot "$slot")
  args+=("${overwrite[@]:-}")
  romm-cli saves upload "${args[@]}" "$file"
}

[[ $# -ge 1 ]] || {
  usage >&2
  exit 2
}
cmd="$1"
shift
case "$cmd" in
backup)
  while [[ $# -gt 0 ]]; do
    case "$1" in
    -n | --dry-run)
      DRY_RUN=1
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
    esac
  done
  cmd_backup
  ;;
restore)
  while [[ $# -gt 0 ]]; do
    case "$1" in
    -n | --dry-run)
      DRY_RUN=1
      shift
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
    esac
  done
  cmd_restore
  ;;
pull) cmd_pull "$@" ;;
push) cmd_push "$@" ;;
-h | --help) usage ;;
*)
  echo "unknown command: $cmd" >&2
  usage >&2
  exit 2
  ;;
esac

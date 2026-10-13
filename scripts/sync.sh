#!/usr/bin/env bash
# Push games from a RomM server to the RG351V over SSH.
#
# Two modes:
#   batch: romm-cli download batch -> staging dir -> rsync to /storage/roms/.
#   pick:  fzf over the RomM library -> download chosen IDs -> rsync.
# The device itself never talks to RomM; this Mac is the bridge.
#
# Usage:
#   scripts/sync.sh [--platform SLUG] [--search TERM] [--dry-run]
#   scripts/sync.sh --pick [--platform SLUG] [--search TERM] [--dry-run]
#
# Examples:
#   scripts/sync.sh --platform gbc --dry-run
#   scripts/sync.sh --pick --platform gba
#   RG351V_HOST=root@RG351V.local scripts/sync.sh --pick --search "zelda"
set -euo pipefail

HOST="${RG351V_HOST:-root@RG351V.local}"
STAGING="${ROMM_STAGING:-.storage/romm-staging}"
SCRATCH_DIR=""
trap '[ -n "${SCRATCH_DIR:-}" ] && rm -rf "$SCRATCH_DIR"' EXIT

# Refuse to touch anything that is not the handheld (see docs/gotchas.md).
assert_device() {
  local name
  name="$(ssh -4 -o BatchMode=yes -o ConnectTimeout=8 "$HOST" 'hostname')"
  [[ "$name" == "RG351V" ]] || {
    echo "refusing: $HOST answers as '$name', not RG351V" >&2
    exit 1
  }
}
PLATFORM=""
SEARCH=""
DRY_RUN=0
PICK=0

usage() {
  cat <<'EOF'
Usage: sync.sh [--pick] [--platform SLUG] [--search TERM] [--dry-run]

Options:
  --pick                choose games interactively with fzf (multi-select)
  -p, --platform SLUG   pre-filter to one RomM platform slug (e.g. gbc)
  -s, --search TERM     pre-filter to ROMs matching a search term
  -n, --dry-run         print what would be transferred, change nothing
  -h, --help            show this help

Env:
  RG351V_HOST   SSH target (default root@RG351V.local)
  ROMM_STAGING  local download dir (default .storage/romm-staging)
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --pick)
    PICK=1
    shift
    ;;
  -p | --platform)
    PLATFORM="$2"
    shift 2
    ;;
  -s | --search | --search-term)
    SEARCH="$2"
    shift 2
    ;;
  -n | --dry-run)
    DRY_RUN=1
    shift
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  *)
    echo "unknown argument: $1" >&2
    usage >&2
    exit 2
    ;;
  esac
done

push_to_device() {
  local rsync_args=(-avh --omit-dir-times -e "ssh -4 -o BatchMode=yes -o ConnectTimeout=8")
  [[ "$DRY_RUN" -eq 1 ]] && rsync_args+=(-n)

  echo "==> pushing $STAGING/ to $HOST:/storage/roms/"
  rsync "${rsync_args[@]}" "$STAGING/" "$HOST:/storage/roms/"

  if [[ "$DRY_RUN" -eq 0 ]]; then
    echo "==> refreshing EmulationStation"
    ssh -4 -o BatchMode=yes -o ConnectTimeout=8 "$HOST" 'systemctl restart emustation'
  fi
}

pick_roms() {
  command -v fzf >/dev/null || {
    echo "fzf not found (brew install fzf)" >&2
    exit 1
  }

  local scratch
  SCRATCH_DIR="$(mktemp -d /tmp/romm-sync.XXXXXX)"
  scratch="$SCRATCH_DIR"
  local index="$scratch/index.json"

  echo "==> fetching RomM library index"
  romm-cli --json api call GET /api/roms --query limit=1000 >"$index"
  local total fetched
  total="$(jq -r '.total' "$index")"
  fetched="$(jq -r '.items | length' "$index")"
  if [[ "$fetched" -lt "$total" ]]; then
    echo "warning: index truncated ($fetched of $total), refine with --platform/--search" >&2
  fi

  local rows
  rows="$(jq -r --arg platform "$PLATFORM" --arg search "$SEARCH" '
    .items[]
    | select(($platform == "" or .platform_slug == $platform)
      and ($search == "" or (.name | ascii_downcase | contains($search | ascii_downcase))))
    | "\(.id)\t\(.platform_slug)\t\(.name)"' "$index")"
  [[ -n "$rows" ]] || {
    echo "no ROMs match" >&2
    exit 1
  }

  local selected
  selected="$(printf '%s\n' "$rows" | fzf --multi --prompt='rom> ' \
    --delimiter=$'\t' --with-nth='2..' \
    --preview="jq -r --argjson id '{1}' '.items[] | select(.id==\$id) | .name, (.platform_slug | \"platform: \" + .), ((.regions // []) | join(\", \") | \"regions: \" + .), ((.fs_size_bytes / 1048576 * 10 | round / 10 | tostring) + \" MB\" | \"size: \" + .), \"\", (.summary // \"no description\")' $index" || true)"
  [[ -n "$selected" ]] || {
    echo "nothing selected" >&2
    exit 0
  }

  if [[ "$DRY_RUN" -eq 1 ]]; then
    echo "would sync:"
    printf '%s\n' "$selected"
    return 0
  fi

  local ids id platform dest tmpdir zipfile
  ids="$(printf '%s\n' "$selected" | cut -f1)"
  while IFS= read -r id; do
    [[ -n "$id" ]] || continue
    platform="$(jq -r --argjson id "$id" '.items[] | select(.id==$id) | .platform_slug' "$index")"
    dest="$STAGING/$platform"
    mkdir -p "$dest"
    tmpdir="$(mktemp -d /tmp/romm-dl.XXXXXX)"
    echo "==> downloading id $id to $dest"
    if romm-cli download "$id" -o "$tmpdir"; then
      zipfile="$(find "$tmpdir" -maxdepth 1 -name '*.zip' | head -n 1)"
      if [[ -n "${zipfile:-}" ]]; then
        unzip -o -q "$zipfile" -d "$dest" && rm "$zipfile"
      else
        mv "$tmpdir"/* "$dest"/
      fi
    else
      echo "download failed for id $id" >&2
    fi
    rm -rf "$tmpdir"
  done <<<"$ids"
}

if [[ "$PICK" -eq 1 ]]; then
  assert_device
  pick_roms
  [[ "$DRY_RUN" -eq 1 ]] && exit 0
  push_to_device
  echo "done"
  exit 0
fi

assert_device
download_args=(batch --extract --delete-zip-after-extract -o "$STAGING")
[[ -n "$PLATFORM" ]] && download_args+=(--platform "$PLATFORM")
[[ -n "$SEARCH" ]] && download_args+=(--search-term "$SEARCH")

echo "==> downloading from RomM to $STAGING"
romm-cli download "${download_args[@]}"

push_to_device
echo "done"

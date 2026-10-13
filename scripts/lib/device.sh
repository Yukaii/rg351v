# shellcheck shell=bash disable=SC2034
# Shared helpers for scripts that talk to the handheld. Source, don't run.
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)

HOST="${RG351V_HOST:-root@RG351V.local}"
SSH=(ssh -4 -o BatchMode=yes -o ConnectTimeout=8)
SCP=(scp -4 -o BatchMode=yes -o ConnectTimeout=8)

# Refuse to touch anything that is not the handheld.
assert_device() {
  local name
  name="$("${SSH[@]}" "$HOST" hostname)" || {
    echo "cannot reach $HOST (the device drops Wi-Fi while asleep; wake it)" >&2
    exit 1
  }
  [[ "$name" == "RG351V" ]] || {
    echo "refusing: $HOST answers as '$name', not RG351V" >&2
    exit 1
  }
}

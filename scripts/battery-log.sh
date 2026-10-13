#!/usr/bin/env bash
# Log battery/CPU/thermal samples on the RG351V and summarise runtime.
#
# The sampler runs on the device (so it survives Wi-Fi dropping in sleep) and
# appends a CSV row every INTERVAL seconds under /storage/.cache/rg351v/.
# While the kernel is suspended the sampler is frozen, so gaps between rows
# are standby time; the summary reports awake draw and standby drain apart.
#
# Usage:
#   scripts/battery-log.sh start [secs]   start sampling (default 15 s)
#   scripts/battery-log.sh mark <label>   start a new segment in the log
#   scripts/battery-log.sh status         is it running, how many rows
#   scripts/battery-log.sh stop           stop sampling (keeps the log)
#   scripts/battery-log.sh pull           fetch log to .storage/battery/ + summary
#   scripts/battery-log.sh summary <csv>  summarise a pulled log
#   scripts/battery-log.sh clear          delete the on-device log
#
# Only samples taken while the battery reports "Discharging" count toward
# draw/runtime, so unplug the charger before the run you care about.
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
#   BATTERY_MAH   full capacity for runtime estimates (default 3380, the
#                 fuel gauge's charge_full)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

REMOTE_DIR="/storage/.cache/rg351v"
REMOTE_SH="$REMOTE_DIR/sample.sh"
REMOTE_CSV="$REMOTE_DIR/battery.csv"
REMOTE_PID="$REMOTE_DIR/sampler.pid"
OUT="$ROOT/.storage/battery"

remote() { "${SSH[@]}" "$HOST" "$@"; }

running() {
  remote "[ -f $REMOTE_PID ] && kill -0 \$(cat $REMOTE_PID) 2>/dev/null"
}

summary() {
  python3 - "$1" "${BATTERY_MAH:-3380}" <<'PY'
import statistics
import sys
from datetime import datetime, timezone

path, full_mah = sys.argv[1], float(sys.argv[2])
rows, marks = [], []  # marks: (index of next row, label)
with open(path) as f:
    for line in f:
        line = line.strip()
        if not line or line.startswith("ts,"):
            continue
        if line.startswith("#mark,"):
            _, ts, label = line.split(",", 2)
            marks.append((len(rows), label))
            continue
        p = line.split(",")
        if len(p) != 15:
            continue  # torn last line
        try:
            rows.append(dict(
                ts=int(p[0]), status=p[1], cap=int(p[2]), volt=int(p[3]),
                curr=abs(int(p[4])), soc=int(p[5]), cpu=int(p[7]),
                busy=int(p[11]), bl=int(p[13]),
            ))
        except ValueError:
            continue

if len(rows) < 2:
    sys.exit("not enough samples yet")

dts = [b["ts"] - a["ts"] for a, b in zip(rows, rows[1:])]
gap = max(3 * statistics.median(dts), 60)

# Segment boundaries: row 0 plus each mark.
first = next((l for i, l in marks if i == 0), "start")
bounds = [(0, first)] + [(i, l) for i, l in marks if 0 < i < len(rows)]
bounds.append((len(rows) - 1, None))

def hms(s):
    s = int(s)
    return f"{s // 3600}:{s % 3600 // 60:02d}:{s % 60:02d}"

print(f"{path}: {len(rows)} samples, "
      f"{datetime.fromtimestamp(rows[0]['ts'], timezone.utc):%F %T}Z .. "
      f"{datetime.fromtimestamp(rows[-1]['ts'], timezone.utc):%T}Z, "
      f"gap threshold {gap:.0f}s, pack {full_mah:.0f} mAh\n")

for (lo, label), (hi, _) in zip(bounds, bounds[1:]):
    seg = rows[lo:hi + 1]
    if len(seg) < 2:
        continue
    awake = standby = dis_s = 0
    mah = vsum = 0.0
    cap_drop_standby = 0
    for a, b in zip(seg, seg[1:]):
        dt = b["ts"] - a["ts"]
        both_dis = a["status"] == b["status"] == "Discharging"
        if dt > gap:
            standby += dt
            if both_dis:
                cap_drop_standby += a["cap"] - b["cap"]
            continue
        awake += dt
        if both_dis:
            dis_s += dt
            mah += (a["curr"] + b["curr"]) / 2 / 1000 * dt / 3600
            vsum += (a["volt"] + b["volt"]) / 2 * dt
    wall = seg[-1]["ts"] - seg[0]["ts"]
    print(f"== {label}  {hms(wall)} "
          f"(awake {hms(awake)}, standby {hms(standby)})")
    print(f"   cap {seg[0]['cap']}% -> {seg[-1]['cap']}%  "
          f"status {seg[0]['status']} -> {seg[-1]['status']}  "
          f"soc max {max(r['soc'] for r in seg) / 1000:.0f} C  "
          f"busy avg {statistics.mean(r['busy'] for r in seg):.0f}%  "
          f"backlight avg {statistics.mean(r['bl'] for r in seg):.0f}")
    if dis_s:
        ma = mah / (dis_s / 3600)
        volts = vsum / dis_s / 1e6
        print(f"   awake draw   {ma:6.0f} mA  {ma * volts / 1000:4.2f} W  "
              f"-> full pack ~ {full_mah / ma:4.1f} h")
    else:
        print("   awake draw   n/a (no Discharging samples: unplug the charger)")
    if standby and cap_drop_standby >= 0:
        pct_h = cap_drop_standby / (standby / 3600)
        if pct_h > 0:
            print(f"   standby      {pct_h:5.2f} %/h  -> full pack ~ "
                  f"{100 / pct_h:5.0f} h  "
                  f"({cap_drop_standby}% over {hms(standby)}; 1% steps are coarse)")
        else:
            print(f"   standby      <1% drop over {hms(standby)} "
                  f"(too short to resolve)")
    print()
PY
}

cmd="${1:-}"
case "$cmd" in
start)
  secs="${2:-15}"
  [[ "$secs" =~ ^[0-9]+$ && "$secs" -ge 1 ]] || {
    echo "interval must be a positive integer" >&2
    exit 1
  }
  assert_device
  if running; then
    echo "already running (stop it first)" >&2
    exit 1
  fi
  remote "mkdir -p $REMOTE_DIR"
  "${SCP[@]}" "$HERE/device/sample.sh" "$HOST:$REMOTE_SH"
  remote "nohup sh $REMOTE_SH loop $secs $REMOTE_CSV </dev/null >/dev/null 2>&1 & echo \$! >$REMOTE_PID"
  sleep 1
  running && echo "sampling every ${secs}s -> $HOST:$REMOTE_CSV"
  ;;
mark)
  label="${2:?label}"
  [[ "$label" != *[,\'\"]* ]] || {
    echo "label must not contain commas or quotes" >&2
    exit 1
  }
  assert_device
  remote "echo '#mark,'\$(date +%s)',$label' >>$REMOTE_CSV"
  echo "marked: $label"
  ;;
status)
  assert_device
  if running; then
    echo "running, pid $(remote "cat $REMOTE_PID")"
  else
    echo "not running"
  fi
  remote "[ -f $REMOTE_CSV ] && echo \"\$(wc -l <$REMOTE_CSV) lines, \$(du -k $REMOTE_CSV | cut -f1) KB\" || echo 'no log'"
  ;;
stop)
  assert_device
  remote "[ -f $REMOTE_PID ] && kill \$(cat $REMOTE_PID) 2>/dev/null; rm -f $REMOTE_PID; true"
  echo "stopped (log kept; use pull)"
  ;;
pull)
  assert_device
  mkdir -p "$OUT"
  dest="$OUT/battery-$(date +%Y%m%d-%H%M%S).csv"
  "${SCP[@]}" "$HOST:$REMOTE_CSV" "$dest"
  echo "saved $dest"
  summary "$dest"
  ;;
summary)
  [[ -f "${2:-}" ]] || {
    echo "usage: ${0##*/} summary <csv>" >&2
    exit 1
  }
  summary "$2"
  ;;
clear)
  assert_device
  if running; then
    echo "stop the sampler first" >&2
    exit 1
  fi
  remote "rm -f $REMOTE_CSV"
  echo "cleared"
  ;;
*)
  sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
  exit 1
  ;;
esac

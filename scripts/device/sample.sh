#!/bin/sh
# shellcheck disable=SC2154  # variables are assigned via eval in rd()
# Runs ON the RG351V (BusyBox ash), not on the Mac.
#
# Usage:
#   sample.sh once                   human-readable snapshot (takes ~1 s)
#   sample.sh header                 CSV header line
#   sample.sh loop <secs> <file>     append one CSV row every <secs> to <file>
#
# The loop is frozen while the kernel is suspended, so gaps between rows in
# the CSV are standby time (the RTC keeps the wall clock running).

CPU=/sys/devices/system/cpu/cpu0/cpufreq
BAT=/sys/class/power_supply/battery
SOC=/sys/class/thermal/thermal_zone0/temp
GPUT=/sys/class/thermal/thermal_zone1/temp
GPU=/sys/class/devfreq/ff400000.gpu/cur_freq
DMC=/sys/class/devfreq/dmc/cur_freq
BL=/sys/class/backlight/backlight/brightness
WLAN=/sys/class/net/wlan0/operstate

HEADER="ts,status,cap_pct,volt_uv,curr_ua,soc_mc,gpu_mc,cpu_khz,gov,gpu_hz,dmc_hz,busy_pct,load1,backlight,wlan"
PT=0
PI=0

# rd <file> <var>: read the first line of <file> into <var> ("" if missing).
rd() {
  eval "$2="
  read -r "$2" <"$1" 2>/dev/null || :
}

# Sets BUSY to CPU busy % since the previous call.
cpu_busy() {
  read -r _ u n s i w q sq st _ </proc/stat
  total=$((u + n + s + i + w + q + sq + st))
  idle=$((i + w))
  dt=$((total - PT))
  di=$((idle - PI))
  if [ "$dt" -gt 0 ]; then BUSY=$(((dt - di) * 100 / dt)); else BUSY=0; fi
  PT=$total
  PI=$idle
}

read_all() {
  rd $BAT/status status
  rd $BAT/capacity cap
  rd $BAT/voltage_now volt
  rd $BAT/current_now curr
  rd $SOC soc
  rd $GPUT gput
  rd $CPU/scaling_cur_freq cpuf
  rd $CPU/scaling_governor gov
  rd $GPU gpuf
  rd $DMC dmcf
  rd $BL bl
  rd $WLAN wlan
  read -r load1 _ </proc/loadavg
  cpu_busy
}

row() {
  read_all
  printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$(date +%s)" "$status" "$cap" "$volt" "$curr" "$soc" "$gput" "$cpuf" \
    "$gov" "$gpuf" "$dmcf" "$BUSY" "$load1" "$bl" "$wlan"
}

case "${1:-}" in
once)
  cpu_busy
  sleep 1
  read_all
  printf 'battery   %s%%  %s  %s mV  %s mA (raw %s uA)\n' \
    "$cap" "$status" "$((volt / 1000))" "$((curr / 1000))" "$curr"
  printf 'cpu       %s  %s MHz  busy %s%%  load %s\n' \
    "$gov" "$((cpuf / 1000))" "$BUSY" "$load1"
  printf 'gpu/dmc   %s MHz / %s MHz\n' "$((gpuf / 1000000))" "$((dmcf / 1000000))"
  printf 'temp      soc %s C  gpu %s C\n' "$((soc / 1000))" "$((gput / 1000))"
  printf 'screen    backlight %s  wlan0 %s\n' "$bl" "$wlan"
  ;;
header)
  echo "$HEADER"
  ;;
loop)
  secs="${2:?secs}"
  out="${3:?file}"
  [ -s "$out" ] || echo "$HEADER" >"$out"
  cpu_busy
  while :; do
    sleep "$secs"
    row >>"$out"
  done
  ;;
*)
  echo "usage: $0 once|header|loop <secs> <file>" >&2
  exit 1
  ;;
esac

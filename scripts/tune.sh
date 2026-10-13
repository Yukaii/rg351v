#!/usr/bin/env bash
# Read and change the RG351V's CPU/GPU/DRAM/screen/Wi-Fi power knobs.
#
# Everything here is sysfs and resets on reboot, so a bad value is recovered
# by rebooting. Each write is read back and checked.
#
# Usage:
#   scripts/tune.sh get
#   scripts/tune.sh set <knob> <value>
#   scripts/tune.sh preset default|saver
#
# Knobs:
#   cpu-max <MHz>      max CPU clock (1008 1200 1248 1296)
#   cpu-min <MHz>      min CPU clock (1008 is the hardware floor)
#   cpu-gov <name>     ondemand conservative powersave performance schedutil ...
#   gpu-gov <name>     dmc_ondemand powersave performance userspace ...
#   dmc-gov <name>     same, for the DRAM controller
#   backlight <0-100>  brightness (0 also cuts the panel light)
#   screen on|off [s]  backlight power; off auto-restores after s (default 300)
#   cores <1-4>        number of online CPU cores
#   wifi-ps on|off     Wi-Fi power save (unsupported by this device's driver)
#
# Presets:
#   default  ondemand 1008-1296 MHz, dynamic GPU/DRAM, 4 cores
#   saver    CPU capped at 1008 MHz, GPU/DRAM powersave
#
# Env:
#   RG351V_HOST   SSH target (default root@RG351V.local)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=lib/device.sh
source "$HERE/lib/device.sh"

CPU=/sys/devices/system/cpu/cpu0/cpufreq
GPU=/sys/class/devfreq/ff400000.gpu
DMC=/sys/class/devfreq/dmc
BL=/sys/class/backlight/backlight

die() {
  echo "$*" >&2
  exit 1
}

remote() { "${SSH[@]}" "$HOST" "$@"; }

# write_check <path> <value>: write on the device, then verify by reading back.
write_check() {
  remote "echo $2 >$1 && [ \"\$(cat $1)\" = \"$2\" ]" ||
    die "failed to set $1 to $2 (not an allowed value?)"
  echo "ok: $1 = $2"
}

# mhz <value>: accept MHz or kHz, print kHz.
mhz() {
  [[ "$1" =~ ^[0-9]+$ ]] || die "not a number: $1"
  if [[ "$1" -lt 10000 ]]; then echo $(($1 * 1000)); else echo "$1"; fi
}

name() {
  [[ "$1" =~ ^[a-z_]+$ ]] || die "bad governor name: $1"
  echo "$1"
}

do_get() {
  remote "
    echo \"cpu       \$(cat $CPU/scaling_governor)  \$(cat $CPU/scaling_min_freq)-\$(cat $CPU/scaling_max_freq) kHz (now \$(cat $CPU/scaling_cur_freq))\"
    echo \"          freqs: \$(cat $CPU/scaling_available_frequencies)\"
    echo \"          governors: \$(cat $CPU/scaling_available_governors)\"
    echo \"cores     online \$(cat /sys/devices/system/cpu/online)\"
    echo \"gpu       \$(cat $GPU/governor)  now \$(cat $GPU/cur_freq)  min \$(cat $GPU/min_freq) max \$(cat $GPU/max_freq)\"
    echo \"dmc       \$(cat $DMC/governor)  now \$(cat $DMC/cur_freq)  min \$(cat $DMC/min_freq) max \$(cat $DMC/max_freq)\"
    echo \"screen    brightness \$(cat $BL/brightness)/\$(cat $BL/max_brightness)  bl_power \$(cat $BL/bl_power)\"
    echo \"wifi      \$(iw dev wlan0 get power_save 2>&1)\"
  "
}

do_set() {
  local knob="${1:-}" val="${2:-}"
  [[ -n "$knob" && -n "$val" ]] || die "usage: set <knob> <value>"
  case "$knob" in
  cpu-max) write_check "$CPU/scaling_max_freq" "$(mhz "$val")" ;;
  cpu-min) write_check "$CPU/scaling_min_freq" "$(mhz "$val")" ;;
  cpu-gov) write_check "$CPU/scaling_governor" "$(name "$val")" ;;
  gpu-gov) write_check "$GPU/governor" "$(name "$val")" ;;
  dmc-gov) write_check "$DMC/governor" "$(name "$val")" ;;
  backlight)
    [[ "$val" =~ ^[0-9]+$ && "$val" -le 100 ]] || die "backlight is 0-100"
    write_check "$BL/brightness" "$val"
    ;;
  screen)
    case "$val" in
    on) write_check "$BL/bl_power" 0 ;;
    off)
      local secs="${3:-300}"
      [[ "$secs" =~ ^[0-9]+$ && "$secs" -ge 1 ]] || die "seconds must be a positive integer"
      write_check "$BL/bl_power" 4
      remote "nohup sh -c 'sleep $secs; echo 0 >$BL/bl_power' </dev/null >/dev/null 2>&1 &"
      echo "screen comes back on in ${secs}s (or: tune.sh set screen on)"
      ;;
    *) die "screen takes on|off" ;;
    esac
    ;;
  cores)
    [[ "$val" =~ ^[1-4]$ ]] || die "cores is 1-4"
    local i
    for i in 1 2 3; do
      write_check "/sys/devices/system/cpu/cpu$i/online" "$((i < val ? 1 : 0))"
    done
    ;;
  wifi-ps)
    [[ "$val" == on || "$val" == off ]] || die "wifi-ps takes on|off"
    # The USB MT7601U driver answers "Operation not supported (-95)".
    remote "iw dev wlan0 set power_save $val && iw dev wlan0 get power_save" || {
      echo "wifi-ps: not supported by this Wi-Fi driver (mt7601u)" >&2
      return 1
    }
    ;;
  *) die "unknown knob: $knob" ;;
  esac
}

case "${1:-}" in
get)
  assert_device
  do_get
  ;;
set)
  assert_device
  do_set "${2:-}" "${3:-}" "${4:-}"
  ;;
preset)
  assert_device
  case "${2:-}" in
  default)
    do_set cpu-min 1008
    do_set cpu-max 1296
    do_set cpu-gov ondemand
    do_set gpu-gov dmc_ondemand
    do_set dmc-gov dmc_ondemand
    do_set cores 4
    ;;
  saver)
    do_set cpu-max 1008
    do_set gpu-gov powersave
    do_set dmc-gov powersave
    ;;
  *) die "presets: default saver" ;;
  esac
  ;;
*)
  sed -n '2,/^set -e/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//' >&2
  exit 1
  ;;
esac

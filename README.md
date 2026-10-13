# rg351v

Notes, scripts and tweaks for my Anbernic RG351V running AmberELEC.

## Status (2026-10-13)

- [x] Flashed `AmberELEC-RG351V.aarch64-20230203.img` to SD1 (replaced old 351ELEC install)
- [x] First boot done, on Wi-Fi (`192.168.1.14`, hostname `RG351V`)
- [x] SSH key auth set up (`id_ecdsa`)
- [x] Root password changed from the default (stored outside this repo)
- [x] Charging works with a proper charger (battery 100%)
- [x] Homebrew verified: `hcl`, `opentyrian`, `prince` binaries work; unhid the
  3 entries in `/storage/roms/homebrew/gamelist.xml` (they shipped hidden)
- [x] Game sync via `scripts/sync.sh` (RomM → Mac → rsync to device)
- [x] Save sync via `scripts/saves.sh` (`backup`/`restore` device↔Mac,
  `pull` RomM cloud saves onto the device, `push` a save file to RomM)
- [x] Pikachu Volleyball (pikaball-revamped) built for aarch64 via
  `scripts/build-port.sh pikaball` (Rocky 9 + GCC 13, static SDL3, glibc 2.34)
- [x] Deployed to `/storage/roms/ports/pikaball/` via
  `scripts/deploy-port.sh pikaball` (launcher + native gamepad, no gptokeyb needed)
- [x] Controls/exit documented in the Ports gamelist entry (move: dpad/stick,
  hit: face buttons, START: pause, hold L2+SELECT+START via jslisten to quit)
- [x] Pikachu Volleyball verified playable; clean exit confirmed, ES stays smooth
- [x] Monitoring: `scripts/perf.sh` (snapshot/watch) and
  `scripts/battery-log.sh` (on-device sampler + awake/standby summary)
- [ ] Measure real battery life on discharge (reading + standby)
- [ ] Copy BIOS files / ROMs
- [ ] Back up `/storage` (saves, configs) off-device
- [ ] Decide on update strategy (AmberELEC releases)

## Device

| | |
|---|---|
| Hardware | Anbernic RG351V (RK3326, 4 cores, aarch64) |
| RAM | ~926 MB |
| OS | AmberELEC Community 20230203 |
| Kernel | Linux 4.4.189 |
| Storage | `/flash` 2.0G (boot), `/storage` 1.9G (system/config), `/storage/roms` 10.5G (games, mounted from `mmcblk0p3`) |
| Network | Wi-Fi `wlan0`; SSH (22), SMB (139/445) open |
| Tools on device | python3, git, curl, wget, rsync, nano (no gcc) |

## Access

```sh
ssh root@RG351V.local       # key auth (.14 usually, but DHCP may move it)
smb://RG351V.local          # network share (roms, bios, saves)
```

Suggested `~/.ssh/config` entry:

```
Host rg351v
  HostName RG351V.local
  User root
  IdentityFile ~/.ssh/id_ecdsa
```

## Gotchas learned

- The SD card must be in the **SD1 / left (TF1)** slot; the second slot is for extra game storage.
- `passwd` over a non-interactive SSH hangs (no TTY, busybox). Either use the
  EmulationStation setting (Start → System Settings → Security) or write a hash
  from `cryptpw -m sha512` into `/storage/.cache/shadow` (which `/etc/shadow`
  symlinks to, so it persists).
- Charging is picky about cable/charger. `/sys/class/power_supply/ac/online`
  reads `1` when power is detected, `0` when not.
- macOS flashing: `diskutil unmountDisk` then `dd` to `/dev/rdiskN` right away,
  or it gets remounted and `dd` fails with "Resource busy".
- Stock homebrew entries are `<hidden>true</hidden>` in
  `/storage/roms/homebrew/gamelist.xml`, so EmulationStation hides them even
  though the binaries work. Back up the file, flip to `<hidden>false</hidden>`,
  then `systemctl restart emustation`.
- Non-interactive SSH does not source `/etc/profile`, so `/usr/local/bin`
  (homebrew ports) is missing from `PATH`. Source it first:
  `ssh root@RG351V.local '. /etc/profile; which hcl'`.
- No on-device Wi-Fi sync app exists; sync from the Mac instead:
  `scripts/sync.sh` downloads from RomM (`romm-cli`) and rsyncs to
  `/storage/roms/`. RetroArch saves live in
  `/storage/.config/retroarch/savefiles/` (plus `.srm` next to ROMs, since
  `savefiles_in_content_dir` is true) and savestates in
  `/storage/roms/savestates/<system>/`.
- Prefer `RG351V.local` over the numeric IP: DHCP can move the device.
  Scripts default to `root@RG351V.local`, force IPv4 (`-4`, mDNS also
  resolves a link-local IPv6 address with a conflicting host key), and refuse
  to run when the remote hostname is not `RG351V`.
- This Mac is `ykmadeair.local`. If an SSH session answers with that hostname
  or shows macOS paths (`/etc/cups`, no `/storage`), the command ran locally
  because the `ssh` prefix was missing — not on the device. Always sanity-check
  with `hostname` when output looks wrong.
- Sleep drops Wi-Fi: when the device sleeps, ping and SSH time out. Wake it
  first; no reconfiguration needed.
- After a DHCP change, SSH fails with "Host key verification failed": clear
  the stale entry with `ssh-keygen -R RG351V.local` (and the old numeric IP)
  and reconnect once to store the device key.
- Ports exit via jslisten: hold **L2 + SELECT + START** (buttons 10+7+6) to
  `killall` the running port. A port left running in the background shares the
  screen/CPU with EmulationStation and makes the menu crawl — check with
  `ps | grep -v grep` and kill leftovers.
- SDL3 ports need KMSDRM actually compiled in: the container must have the
  GBM/EGL/udev `-devel` packages or SDL silently builds without it and init
  fails with "No available video device". Pin `SDL_VIDEODRIVER=kmsdrm` in the
  launcher; SDL3's default driver pick is unreliable without X11/Wayland.

## Layout

- `scripts/info.sh` – print a quick status snapshot of the device over SSH
- `scripts/sync.sh` – download games from RomM and push them to `/storage/roms/`
  (`--pick` for an interactive fzf picker, `--platform`/`--search` to filter)
- `scripts/saves.sh` – back up / restore device saves (RetroArch savefiles,
  savestates, sidecar `.srm`/`.sav`, PPSSPP savedata) and pull/push RomM
  cloud saves
- `scripts/build-port.sh <name>` (+ `scripts/ports/Dockerfile`) – cross-build a
  CMake port for aarch64 in Docker; binary lands in `.storage/<name>/`
- `scripts/deploy-port.sh <name>` – install the port to
  `/storage/roms/ports/<REMOTE_SUBDIR>/` and restart EmulationStation
- `scripts/ports/<name>/` – per-port `port.conf` (source dir, cmake args,
  install paths) and `launcher.sh`; start a new port with
  `cp -r scripts/ports/_template scripts/ports/<name>`. Currently: `pikaball`
- `scripts/perf.sh [--watch [N]]` – power/CPU/GPU/thermal snapshot over SSH
- `scripts/battery-log.sh start|mark|status|stop|pull|summary` – on-device CSV
  sampler (`scripts/device/sample.sh`); the summary splits awake draw (mA, W,
  runtime estimate) from standby drain (%/h), per `mark` segment
- `scripts/wifi.sh list|add|remove|push|status` – extra Wi-Fi networks via a
  second connman file (`extra.config`); the ES menu only keeps one SSID
- `scripts/lib/device.sh` – shared host/ssh/hostname-assert helpers
- `.storage/` – git-ignored, for anything secret or machine-specific
- Sleep: the power key runs a real kernel suspend (`mem`, logind
  `HandlePowerKey=suspend`). Wi-Fi is a USB MT7601U dongle with no wake-on-WLAN,
  and AmberELEC's `/usr/lib/systemd/system-sleep/sleep` hook unloads `dwc2`
  (the USB controller) around suspend, so the network is down until resume.
  A Wi-Fi-awake sleep therefore cannot be a kernel suspend.
- Battery sampler caveats: capacity is whole-percent (1% ~ 34 mAh), so
  standby drain needs hours of suspend to resolve. BusyBox here has `nohup`
  but no `setsid`. Never `killall sh` on the device.
- Tuning knobs exposed in sysfs: cpufreq (1008/1200/1248/1296 MHz, governors
  incl. `powersave`/`schedutil`), GPU and DMC devfreq, backlight 0-100, Wi-Fi
  power save (currently off). All reset on reboot.
- Wi-Fi is single-SSID in the ES menu: `batocera-config wifi enable` rewrites
  `/storage/.cache/connman/wifi.config` (one fixed `service_amberelec_default`
  block) on every save. connman reads every `*.config` in that directory live,
  so extra networks go in `extra.config` via `scripts/wifi.sh` (keep
  passphrases in `.storage/wifi-networks`). The ES Wi-Fi screen will not list
  them. Idle floor measured: ~450 mA (~7.5 h) at brightness 24, ES menu.

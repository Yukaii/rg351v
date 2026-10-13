# Gotchas and measurements

What was learned the hard way, grouped by topic. Add new findings under the
matching heading (or a new one), with the date when it is a measurement.

## Access and network

- The SD card must be in the **SD1 / left (TF1)** slot; the second slot is for
  extra game storage.
- `passwd` over non-interactive SSH hangs (no TTY, BusyBox). Use the
  EmulationStation setting (Start → System Settings → Security) or write a hash
  from `cryptpw -m sha512` into `/storage/.cache/shadow` (which `/etc/shadow`
  symlinks to, so it persists).
- Non-interactive SSH does not source `/etc/profile`, so `/usr/local/bin`
  (homebrew ports) is missing from `PATH`:
  `ssh root@RG351V.local '. /etc/profile; which hcl'`.
- Prefer `RG351V.local` over the numeric IP: DHCP can move the device. Scripts
  default to `root@RG351V.local`, force IPv4 (`-4`; mDNS also resolves a
  link-local IPv6 address with a conflicting host key), and refuse to run when
  the remote hostname is not `RG351V`.
- This Mac is `ykmadeair.local`. If an SSH session answers with that hostname or
  shows macOS paths (`/etc/cups`, no `/storage`), the command ran locally
  because the `ssh` prefix was missing. Sanity-check with `hostname`.
- After a DHCP change, SSH fails with "Host key verification failed": clear the
  stale entry with `ssh-keygen -R RG351V.local` (and the old numeric IP) and
  reconnect once.
- Sleep drops Wi-Fi: ping and SSH time out while asleep. Wake it first (power
  key); no reconfiguration needed.
- Never `pkill -f <path>` over SSH: the remote shell's own command line matches
  and the session dies. Use `kill $(pidof name)`. Never `killall sh`.
- SMB: the `roms`, `update` and `logs` shares are `public = yes` with
  `guest account = root`, i.e. anyone on the LAN can read AND write
  `/storage/roms` as root without a password (incl. launcher `.sh` files).
  `/storage` itself is not shared; keep secrets there, never under roms.
- Wi-Fi is single-SSID in the ES menu: `batocera-config wifi enable` rewrites
  `/storage/.cache/connman/wifi.config` (one fixed `service_amberelec_default`
  block) on every save. connman reads every `*.config` in that directory live.
  `scripts/wifi.sh install` adds a systemd `.path` unit
  (`/storage/.config/system.d/rg351v-wifi-remember.*`, enabled via a symlink in
  `multi-user.target.wants` like sshd) that copies each saved network into
  `/storage/.config/rg351v/wifi-known` and gives the others to connman as
  `known.config`. The network in `wifi.config` is excluded so one SSID never
  comes from two files. Forget one with `scripts/wifi.sh forget <ssid>`; the ES
  Wi-Fi screen still lists only the active one.

## Power, sleep and crashes

- Charging is picky about cable/charger. `/sys/class/power_supply/ac/online`
  reads `1` when power is detected, `0` when not.
- Sleep: the power key runs a real kernel suspend (`mem`, logind
  `HandlePowerKey=suspend`). Wi-Fi is a USB MT7601U dongle with no wake-on-WLAN,
  and AmberELEC's `/usr/lib/systemd/system-sleep/sleep` hook unloads `dwc2`
  (the USB controller, which also carries the gamepad) around suspend, so the
  network is down until resume. A Wi-Fi-awake sleep therefore cannot be a
  kernel suspend.
- Auto-suspend: stock AmberELEC has none. ES `ScreenSaverBehavior=black` only
  draws black (SoC, Wi-Fi and backlight stay on, ~450 mA); logind `IdleAction`
  needs session idle hints that ES never sends. `autosleep.py` reads
  `/dev/input/event*` (the gamepad re-enumerates on every resume, so it
  rescans), ignores stick jitter below 10% of the axis range, and holds off
  while charging, with an SSH session open, with Wi-Fi above 20 KB/s, or while
  music plays. While installed, the device suspends ~5 min after the last
  button press, so SSH from the Mac drops until the power key wakes it.
- The RTC (`rk808-rtc`, `/sys/class/rtc/rtc0/wakealarm`) wakes the board from
  suspend, which makes unattended tests possible:
  `AUTOSLEEP_CONF=... autosleep.py --once` plus `echo +100 > wakealarm`.
- Kernel panic on suspend (found 2026-10-13 via pstore): the stock sleep hook
  runs `modprobe -r dwc2` and the Wi-Fi dongle driver oopses while it is torn
  down with frames in flight (`rmmod dwc2 -> usb_disconnect -> mt7601u_disconnect
  -> mt7601u_dma_cleanup -> ieee80211_tx_status -> put_pid`), then `panic=20`
  reboots the board. It looks like a freeze. `rg351v-wifi-sleep.service`
  (installed by `autosleep.sh install`, `Before=sleep.target`) takes wlan0 down
  first; 4 suspend cycles under a ping flood gave no oops, and plugging in
  power no longer freezes (user-confirmed).
- pstore: the previous crash is readable in `/sys/fs/pstore/dmesg-ramoops-*`
  (RAM, lost on power-off). journald is volatile (`/run/log`), so pstore is
  the only record of an earlier boot.
- Powering on with the charger can look like a power-key press to logind
  (`Power key pressed short` ~35 s after boot), which suspends at once.
- Battery sampler: capacity is whole-percent (1% ~ 34 mAh), so standby drain
  needs hours of suspend to resolve. BusyBox has `nohup` but no `setsid`.
- Measured (2026-10-13, 3380 mAh, brightness 24, ES menu): awake idle ~448 mA /
  1.7 W (~7.5 h); power-key suspend <1% drop over 69 min (<~29 mA, >100 h, at
  least 15x better than awake); SoC 49 C awake -> 36 C asleep; Wi-Fi back
  within ~15 s of resume.
- Tuning knobs in sysfs (all reset on reboot): cpufreq (1008/1200/1248/1296 MHz,
  governors incl. `powersave`/`schedutil`), GPU and DMC devfreq, backlight
  0-100, Wi-Fi power save (off; the mt7601u driver does not support it, `-95`).
- CPU tuning (4 python jobs, same work): capping `cpu-max` at 1008 MHz is ~25%
  slower but ~25% less energy above idle (noisy: idle swings 420-480 mA under
  ES). Idle is unaffected because idle cores sit in WFI. 1008 MHz is the lowest
  OPP. Backlight off saves only ~21 mA at brightness 24.

## Storage, EmulationStation and ports

- macOS flashing: `diskutil unmountDisk` then `dd` to `/dev/rdiskN` right away,
  or it gets remounted and `dd` fails with "Resource busy".
- Stock homebrew entries are `<hidden>true</hidden>` in
  `/storage/roms/homebrew/gamelist.xml`, so ES hides them though the binaries
  work. Back up the file, flip to `false`, then `systemctl restart emustation`.
- Homebrew menu = `/storage/roms/homebrew`: a top-level `<Name>.sh` launcher, a
  sibling app folder, and a `gamelist.xml` entry (stop `emustation` before
  editing the gamelist: ES rewrites it on quit). Ports = `/storage/roms/ports`.
- `/storage/roms` is exFAT: no symlinks, ownership or permission bits. rsync
  there needs `-rLt --no-perms --no-owner --no-group`, and `--delete` must
  exclude user data dirs.
- No on-device Wi-Fi sync app exists; sync from the Mac with `scripts/sync.sh`.
  RetroArch saves live in `/storage/.config/retroarch/savefiles/` (plus `.srm`
  next to ROMs, since `savefiles_in_content_dir` is true), savestates in
  `/storage/roms/savestates/<system>/`.
- Ports exit via jslisten: hold **L2 + SELECT + START** (buttons 10+7+6) to
  `killall` the running port. A port left running shares screen/CPU with ES and
  makes the menu crawl; check with `ps | grep -v grep`. `jslisten` runs its
  command through `system()`, so shell syntax works in `jslisten set`.
- SDL3 ports need KMSDRM compiled in: the container must have the GBM/EGL/udev
  `-devel` packages or SDL silently builds without it and init fails with "No
  available video device". Pin `SDL_VIDEODRIVER=kmsdrm` in the launcher.
- Prebuilt SDL3 apps (KOReader) bundle an SDL3 without KMSDRM; swap in
  `scripts/build-port.sh sdl3-kmsdrm` at the same release. glibc 2.36 and
  libstdc++ GLIBCXX 3.4.30 are the device's ceilings.
- Ghostty is not an option: it needs GTK4 and OpenGL, and the device has no
  X11/Wayland or desktop GL (Mali-G31, GLES only). Graphics go through
  SDL/KMSDRM.

## Spotify (deckd and deck)

- Audio is plain ALSA (no PulseAudio): the default device is a `dmix` shared
  with ES. Suspend stops playback (Wi-Fi drops), so `autosleep.py` skips while
  `/run/rg351v/playing` exists and deckd runs.
- Build: a fresh `cargo` resolve of librespot 0.8.0 picks a vergen that fails
  to build, so `apps/deckd/Cargo.lock` is seeded from the librespot-0.8.0 crate
  and builds use `--locked`. Default features pull native-tls (OpenSSL 3,
  absent on the device): use `default-features = false, features =
  ["rustls-tls-native-roots"]`. Discovery needs `with-libmdns`, or the daemon
  exits with "Credentials are required if discovery and oauth login are
  disabled". The Spirc task is not `Send`, so it cannot be `tokio::spawn`ed.
- A weak Wi-Fi link (-65..-85 dBm) can drop the dealer websocket; librespot
  reconnects in ~1 s as an idle device and the phone loses control, which is
  what deckd's restore addresses.
- A freshly started or rebuilt session ignores `load` ("will be ignored while
  Not Active") until `activate()` is called first.
- The dealer never echoes our own device's state back, so
  `hm://connect-state/v1/cluster` listeners stay silent. `GET
  /connect-state/v1/cluster` on spclient returns the live state (context_uri,
  next_tracks, provider `queue` for user-queued tracks); used for the queue
  view and to keep `context` fresh.
- After a reboot the server still lists the device as the active player and
  can resume it; deckd pauses playback that starts by itself in the first 25 s.
- Collection API: `/collection/v2/paging` answers 405 to GET and 400 to any
  POST that carries a Content-Type (even application/x-protobuf); the same
  body with no Content-Type returns the page. The Web API/keymaster token route
  returns 403 "Invalid client", so internal endpoints are used.
- deck UI clocks: `Model.pos_at`/`title_at` are absolute `mono_ms()`, so
  `draw_screen()` must get the same absolute time. Passing time since app start
  made the elapsed term underflow and pinned the progress bar to the end on the
  device (the host preview now uses a large absolute base so this shows).
- zsh: `$U:collection` expands `:c` as a history modifier, use `${U}`; a loop
  variable named `path` clobbers `PATH`.

## ptterm (PTT BBS terminal)

- PTT (`ssh bbsu@ptt.cc`) streams UTF-8 text with raw Big5-UAO art in the same
  connection, and `bbs@ptt.cc` is pure Big5. BBS layout counts every non-ASCII
  character as 2 cells (even box/block glyphs Unifont draws 8 px wide), so
  generic terminal emulators misalign it; ptterm decodes UTF-8 first with
  Big5-UAO fallback and fixes width at 2 cells.
- Env knobs: `PTTERM_ENCODING=utf8|big5`, `PTTERM_ACCOUNT=<file>`,
  `PTTERM_AUTOLOGIN=1`, `PTTERM_DUMP=<file>` (screen text on exit),
  `PTTERM_PALETTE=vga`, `PTTERM_PROF=1` (per-phase timing on exit; output in
  /tmp/logs/ptterm.log). SDL's `offscreen` driver uses a slow software
  renderer, so headless CPU numbers overstate the real KMSDRM cost.
- Cost: ~1% of one core, battery draw equals the ~440 mA idle floor.
  Foreground palette is lifted (>= 5:1 contrast on black) while backgrounds
  stay dark, because PTT puts white text on blue bars.
- Layout: 24 rows x 19 px (Unifont's 16 glyph rows with rows 0, 7 and 12
  repeated; chosen by measuring isolated ink per row over all 60k glyphs) plus
  a 24 px hint line. The on-screen keyboard overlays the half of the screen
  opposite the cursor. UI text uses native 16 px glyph rows.
- Sessions persist across leaving the app: the first launch forks a background
  holder (`ptsess` in `ps`, listening on `/tmp/ptterm-<target>.sock`) that owns
  `ssh` and mirrors the 80x24 grid; the UI is a client. Leaving (SELECT+START,
  or the L2+SELECT+START combo) only detaches. The holder exits when ssh ends,
  after `PTTERM_DETACH_MINUTES` (default 30) unattended, or on reboot (/tmp).
  `PTTERM_PERSIST=0` runs the old direct mode.
- Do not `killall ptterm`: BusyBox matches `/proc/PID/exe`, so the holder
  matches too. The launcher's exit combo signals only the UI via
  `/tmp/ptterm-ui.pid`.
- Security: while detached the holder stays logged in as your PTT account for
  up to the detach timeout; anyone who launches PTT reattaches to it.

## PocketJS (evaluated, not pursued)

Cost (2026-10-13, `hero` demo, 960x544 raster, headless): boot ~250 ms, 19.6 MB
RSS, 1.9 MB binary; normal UI frame ~1.9 ms flat out / 2.8 ms at 30 fps paced
(~8% of one core, damage-driven); a full-screen repaint every frame is ~19 ms of
raster (~50 fps ceiling, 61% of a core at 30 fps). The guest is under 1 ms per
frame, so cost is raster pixels, not JS. No Linux handheld host upstream; only
the PocketBook (fb blit) pattern applies. Decided not to pursue: the CPU is fast
enough for it to be a non-event, and display output was never wired up.
`apps/pocketjs-host/` needs a pocketjs checkout at `.storage/pocketjs` (measured
at `fcd49be`) with its bundle built via `bun pocket compile --target pocketbook
--manifest apps/hero/pocket.json`; build in `rust:1-bookworm`, run with
`POCKET_JS`/`POCKET_PAK`, `BENCH_FPS`, `BENCH_FULL=1`.

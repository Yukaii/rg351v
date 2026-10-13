# Repository Guidelines

Notes, scripts and tweaks for an Anbernic RG351V handheld running AmberELEC (CoreELEC lineage, Linux 4.4 aarch64). The device is hacked over SSH from a macOS host; this repo holds what lives on the Mac side, plus the apps cross-built for the device.

## Device Quick Facts
- Host: `root@RG351V.local` (hostname `RG351V`, Wi-Fi `wlan0`, DHCP so the
  numeric IP moves), key auth with `~/.ssh/id_ecdsa`.
- RK3326, 4 cores, ~926 MB RAM, glibc 2.36. On-device tools: python3, git, curl, wget, rsync, nano. No gcc; cross-compile on the Mac (Docker).
- Partitions: `/flash` (boot, 2G), `/storage` (system/config, 1.9G, keep small), `/storage/roms` (games, ~10G, exFAT).
- Config persists under `/storage`; the rootfs is read-only and resets on reboot. `/etc/shadow` is a symlink to `/storage/.cache/shadow`.
- The device auto-suspends after 5 idle minutes (autosleep) and then drops off the network; if SSH times out, ask the user to wake it with the power key.
- SMB share at `smb://RG351V.local`; SSH on 22.

## Documentation Map
- `README.md`: overview, device facts, access, quick start. Keep it short.
- `TODO.md`: open work and the done list. Update it when something starts or finishes.
- `docs/gotchas.md`: lessons, root causes and measurements, grouped by topic. Add new findings under the right heading, with the date for measurements.
- `apps/deck/README.md`: deck/deckd design, protocol, controls, roadmap.
- This file: conventions only; do not put status or findings here.

## Project Structure
- `scripts/`: Mac-side helpers that talk to the device over SSH; `scripts/lib/device.sh` has the shared host/ssh/hostname-assert helpers.
  - `info.sh`, `perf.sh`, `battery-log.sh`, `tune.sh`: inspect and tune.
  - `sync.sh`, `saves.sh`, `wifi.sh`: games (RomM), saves, remembered Wi-Fi networks.
  - `build-port.sh <name>` + `deploy-port.sh <name>`: CMake cross-build in Docker, install under `/storage/roms/ports/`; per-port config in `scripts/ports/<name>/` (`port.conf`, `launcher.sh`; start from `scripts/ports/_template`).
  - `autosleep.sh`, `spotify.sh`, `ptt-account.sh`, `deploy-koreader.sh`: install/manage on-device services and apps.
  - `device/`: files that run on the device (`autosleep.py`, systemd units, samplers).
- `apps/deckd/`: Rust Spotify Connect daemon (librespot 0.8.0 as a library, control socket `/run/rg351v/deckd.sock`). Built by `scripts/spotify.sh build`.
- `apps/deck/`: C/SDL3 Spotify UI (talks to deckd). Host preview with `-DDECK_NO_SDL`.
- `apps/ptterm/`: C/SDL3 BBS terminal.
- `apps/pocketjs-host/`: headless benchmark host (evaluated, not a port).
- `.storage/`: git-ignored; secrets, passwords, backups, build output, machine-specific files.

## Coding Style
- Shell: `bash`, `set -euo pipefail`, 2-space indent, `shellcheck` clean, format with `shfmt -i 2` when available.
- Scripts take the target from `RG351V_HOST` (default `root@RG351V.local`) rather than hard-coding it elsewhere.
- Scripts that write to the device must assert the remote hostname is `RG351V` and force IPv4 (`ssh -4`) before touching anything.
- Remote commands run under BusyBox `ash`: no `nproc`, limited `ps`/`awk` flags. Test remote snippets on the device.
- Rust and C follow the surrounding code; Rust builds use `--locked` (see `docs/gotchas.md`, Spotify).
- Keep changes minimal and portable.

## Device Pitfalls
- Never `pkill -f <path>` or `killall sh` over SSH; use `kill $(pidof name)`. Never `killall ptterm`.
- Run device-side long tests in a way that survives autosleep (it suspends when idle and SSH is closed); an open SSH session or charging blocks it.
- After an unexplained reboot or freeze, read `/sys/fs/pstore/dmesg-ramoops-*` first; journald is volatile.
- zsh on the host: write `${VAR}` before a colon, and do not use `path` as a loop variable.

## Safety Rules
- Never commit passwords, tokens, or the contents of `rg351passwd`; use `.storage/`.
- Flashing the SD card is destructive: confirm the disk identifier with `diskutil list external` and get explicit user approval before any `dd`.
- Before editing system files on the device (`/storage/.cache/shadow`, `/storage/.config/...`), back them up on the device first (`cp -p file file.bak`).
- Avoid anything that could leave the device unbootable or locked out (changing SSH auth, boot config under `/flash`) without keeping a working session open and a recovery path.
- `passwd` hangs over non-interactive SSH; use `cryptpw -m sha512 -P 0` and write the hash into `/storage/.cache/shadow`, or the EmulationStation Security setting.

## Testing
- `bash scripts/info.sh` should print power, OS, kernel, memory, disk and network sections.
- Run `shellcheck scripts/*.sh` after editing scripts.
- Verify device-side changes on the device, and by rebooting and checking the change persisted.
- UI changes: render the host preview first (deck: `DECK_DUMP`, `DECK_VIEW`), then confirm on the screen.

## Commit Guidelines
- Conventional Commits (`feat(scripts): ...`, `docs: ...`, `chore: ...`).
- Commit when a feature is verified, with the docs updated in the same change (`TODO.md`, `docs/gotchas.md`, app README).

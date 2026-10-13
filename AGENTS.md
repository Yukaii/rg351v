# Repository Guidelines

Notes, scripts and tweaks for an Anbernic RG351V handheld running AmberELEC (CoreELEC lineage, Linux 4.4 aarch64). The device is hacked over SSH from a macOS host; this repo holds what lives on the Mac side.

## Device Quick Facts
- Host: `root@RG351V.local` (hostname `RG351V`, Wi-Fi `wlan0`, DHCP so the
  numeric IP moves; `192.168.1.14` is stale), key auth with `~/.ssh/id_ecdsa`.
- RK3326, 4 cores, ~926 MB RAM. On-device tools: python3, git, curl, wget, rsync, nano. No gcc; cross-compile on the Mac.
- Partitions: `/flash` (boot, 2G), `/storage` (system/config, 1.9G, keep small), `/storage/roms` (games, ~10G).
- Config persists under `/storage`; the rootfs is read-only and resets on reboot. `/etc/shadow` is a symlink to `/storage/.cache/shadow`.
- SMB share at `smb://RG351V.local`; SSH on 22.

## Project Structure
- `README.md`: status checklist, device facts, access, gotchas. Keep the status section current.
- `scripts/`: helper scripts that run on the Mac and talk to the device over SSH.
- `.storage/`: git-ignored; secrets, passwords, backups, machine-specific files.

## Coding Style
- Shell: `bash`, `set -euo pipefail`, 2-space indent, `shellcheck` clean, format with `shfmt -i 2` when available.
- Scripts take the target from `RG351V_HOST` (default `root@RG351V.local`) rather than hard-coding it elsewhere.
- Scripts that write to the device must assert the remote hostname is `RG351V` and force IPv4 (`ssh -4`) before touching anything.
- Remote commands run under BusyBox `ash`: no `nproc`, limited `ps`/`awk` flags. Test remote snippets on the device.
- Keep changes minimal and portable.

## Safety Rules
- Never commit passwords, tokens, or the contents of `rg351passwd`; use `.storage/`.
- Flashing the SD card is destructive: confirm the disk identifier with `diskutil list external` and get explicit user approval before any `dd`.
- Before editing system files on the device (`/storage/.cache/shadow`, `/storage/.config/...`), back them up on the device first (`cp -p file file.bak`).
- Avoid anything that could leave the device unbootable or locked out (changing SSH auth, boot config under `/flash`) without keeping a working session open and a recovery path.
- `passwd` hangs over non-interactive SSH; use `cryptpw -m sha512 -P 0` and write the hash into `/storage/.cache/shadow`, or the EmulationStation Security setting.

## Testing
- `bash scripts/info.sh` should print power, OS, kernel, memory, disk and network sections.
- Run `shellcheck scripts/*.sh` after editing scripts.
- Verify device-side changes by rebooting and checking the change persisted.

## Commit Guidelines
- Conventional Commits (`feat(scripts): ...`, `docs: ...`, `chore: ...`).
- Update the README status checklist and gotchas when something new is learned or done.

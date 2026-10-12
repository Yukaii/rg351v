# rg351v

Notes, scripts and tweaks for my Anbernic RG351V running AmberELEC.

## Status (2026-10-12)

- [x] Flashed `AmberELEC-RG351V.aarch64-20230203.img` to SD1 (replaced old 351ELEC install)
- [x] First boot done, on Wi-Fi (`192.168.1.14`, hostname `RG351V`)
- [x] SSH key auth set up (`id_ecdsa`)
- [x] Root password changed from the default (stored outside this repo)
- [x] Charging works with a proper charger (battery 100%)
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
ssh root@192.168.1.14          # key auth
smb://192.168.1.14             # network share (roms, bios, saves)
```

Suggested `~/.ssh/config` entry:

```
Host rg351v
  HostName 192.168.1.14
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

## Layout

- `scripts/info.sh` – print a quick status snapshot of the device over SSH
- `.storage/` – git-ignored, for anything secret or machine-specific

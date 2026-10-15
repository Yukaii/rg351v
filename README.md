# rg351v

Notes, scripts and tweaks for my Anbernic RG351V handheld running AmberELEC
(RK3326, aarch64, Linux 4.4). Everything is driven from a Mac over SSH; this
repo is the Mac side plus the apps cross-built for the device.

## What is here

| | |
|---|---|
| **deck** | Spotify player with a retro UI: now playing, liked songs, albums, playlists, search, queue, album art (`apps/deck`, daemon `apps/deckd`) |
| **Auto-suspend** | suspends after 5 idle minutes unless charging, SSH, busy Wi-Fi, music or video |
| **ptterm** | tiny SDL3 BBS terminal for PTT with an on-screen keyboard |
| **YouTube** | Homebrew browser with search, thumbnails, saved videos and 360p playback ([usage](apps/youtube/README.md)) |
| **Anki** | Gamepad flashcard reviewer with offline reviews and manual AnkiWeb collection/media sync ([usage](apps/anki/README.md)) |
| **Ports / homebrew** | Pikachu Volleyball, KOReader built or repackaged for KMSDRM |
| **Sync** | games from RomM, save backup/restore, Wi-Fi network memory |
| **Monitoring** | perf snapshot, battery sampler, CPU/GPU/backlight tuning |

Open work: [TODO.md](TODO.md). What went wrong and what was measured:
[docs/gotchas.md](docs/gotchas.md). Repo conventions and layout for agents:
[AGENTS.md](AGENTS.md).

## Device

| | |
|---|---|
| Hardware | Anbernic RG351V (RK3326, 4 cores, aarch64) |
| RAM | ~926 MB |
| OS | AmberELEC Community 20230203 |
| Kernel | Linux 4.4.189 |
| Storage | `/flash` 2.0G (boot), `/storage` 1.9G (system/config), `/storage/roms` 10.5G (games, `mmcblk0p3`) |
| Network | Wi-Fi `wlan0` (USB MT7601U); SSH (22), SMB (139/445) |
| Tools on device | python3, git, curl, wget, rsync, nano (no gcc) |

## Access

```sh
ssh root@RG351V.local       # key auth; DHCP may move the IP
smb://RG351V.local          # network share (roms, bios, saves)
```

Optional `~/.ssh/config` entry:

```
Host rg351v
  HostName RG351V.local
  User root
  IdentityFile ~/.ssh/id_ecdsa
```

## Quick start

```sh
bash scripts/info.sh                                  # power, OS, memory, disk, network

# Spotify player (Premium; first login from a phone: Connect devices -> RG351V)
scripts/spotify.sh build && scripts/spotify.sh install
scripts/build-port.sh deck && scripts/deploy-port.sh deck   # Homebrew > Spotify

scripts/autosleep.sh install 5                        # idle auto-suspend
scripts/sync.sh --pick                                # games from RomM
scripts/saves.sh backup                               # saves to the Mac
```

Scripts need Docker for cross-builds (aarch64 containers) and read the target
from `RG351V_HOST` (default `root@RG351V.local`). The device sleeps after 5 idle
minutes and drops off the network; wake it with the power key.

## Status

Device is flashed, networked and in daily use; the Spotify player, auto-suspend
and the suspend-crash fix are done and verified on hardware. See
[TODO.md](TODO.md) for what is open. Per-app docs: [apps/deck](apps/deck/README.md).

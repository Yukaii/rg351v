# RG351V homebrew

Music, rhythm games, flashcards, video and a BBS terminal for the
**Anbernic RG351V**, with interfaces built around its physical controls.
Also includes tools to sync games and saves, remember Wi-Fi networks and
put the handheld to sleep when idle.

Developed and used on **AmberELEC Community 20230203** (RK3326, aarch64,
Linux 4.4.189). The apps run on the handheld; build and management scripts
run on a Mac and connect over SSH. This is a personal homebrew collection;
each app has its own setup steps.

## Apps

| App | What you can do |
|---|---|
| [deck](apps/deck/README.md) | Play Spotify with a retro UI, album art, library browsing, search and a queue. Requires Spotify Premium; online playback only. |
| [Taiko](apps/taiko/README.md) | Play your own TJA charts with gamepad drumming, calibration, combo effects and a soul gauge. Some chart and scoring features are still in progress. |
| [Anki](apps/anki/README.md) | Review flashcards offline with physical buttons, then manually sync with AnkiWeb. Supports basic/cloze cards, images and local audio; rendering is limited. |
| [YouTube](apps/youtube/README.md) | Search, save and watch videos on the handheld, with tested 360p playback. Preparing a video can take tens of seconds. |
| [ptterm](apps/ptterm/src/main.c) | Browse PTT through a Big5-UAO terminal with CJK text and an on-screen keyboard. |
| Other ports | Read books with KOReader or play Pikachu Volleyball using the included build/deployment helpers. External port sources or downloads are required. |

Follow the linked app guides for installation, controls and current limits.
See [TODO.md](TODO.md) for verified work and remaining features.

## Get started

You need an RG351V running AmberELEC, Wi-Fi, and a Mac with working SSH key
access to the handheld. The helpers use Bash, Python 3, SSH and rsync;
native builds also need Docker with Linux/aarch64 containers. Individual
apps and sync tools have additional dependencies documented in their guides
or script headers.

From the repository directory, check your connection and device:

```sh
ssh root@RG351V.local
# Run exit to return to the Mac, then:
bash scripts/info.sh
```

The default target is `root@RG351V.local`. To use another address, set
`RG351V_HOST` before running a helper:

```sh
export RG351V_HOST=root@your-device-address
```

Write helpers verify that the remote hostname is `RG351V`. If the handheld
has gone to sleep, wake it with the power key before connecting. Files can
also be transferred through `smb://RG351V.local` in Finder.

## Device helpers

Run these from the Mac after setting up SSH access:

```sh
bash scripts/saves.sh backup          # back up game saves to the Mac
bash scripts/sync.sh --pick           # choose games from your configured RomM server
bash scripts/wifi.sh install          # remember networks saved in the device's Wi-Fi menu
bash scripts/autosleep.sh install 5   # suspend after five idle minutes
bash scripts/perf.sh                  # inspect CPU, GPU, temperature and power
```

Auto-suspend stays awake while charging, during an open SSH session, with
busy Wi-Fi, or while supported music/video playback is active. Backups,
credentials and build output belong in the ignored `.storage/` directory.

## Development and troubleshooting

Native C/SDL apps use `scripts/build-port.sh <name>` and
`scripts/deploy-port.sh <name>`. Spotify's Rust daemon uses
`scripts/spotify.sh build` and `scripts/spotify.sh install`. Follow each
app's guide for required assets and setup before deploying.

- [Troubleshooting and measurements](docs/gotchas.md): sleep, audio, video,
  networking and hardware-specific lessons.
- [Roadmap](TODO.md): open work and completed milestones.
- [Repository conventions](AGENTS.md): project layout, build conventions
  and device safety rules.

## License

Copyright (c) 2026 Yukai Huang. Original code and documentation are licensed
under [GPL-2.0-or-later](LICENSE), except where another license is stated:

- `apps/deckd/` and `apps/pocketjs-host/` use MIT; each has its own `LICENSE`.
- `scripts/ports/koreader/launcher.sh` uses
  [AGPL-3.0-or-later](LICENSES/AGPL-3.0-or-later.txt).
- Vendored libraries retain the license and copyright notices in their files.

Downloaded dependencies, fonts, firmware and external ports retain their
upstream licenses. This repository's license does not relicense those assets.
Personal credentials, ROMs, BIOS files and song libraries are not included.

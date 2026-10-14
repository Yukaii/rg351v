# TODO

Open work and what is already done. Facts learned along the way go in
`docs/gotchas.md`, not here.

## Open

- [ ] Copy BIOS files / ROMs
- [ ] Back up `/storage` (saves, configs) off-device
- [ ] Decide on update strategy (AmberELEC releases)
- [ ] Confirm the plug-in-power freeze is gone over a few days (check
  `/sys/fs/pstore` after any odd reboot)

### deck (Spotify player)

- [ ] Followed artists and saved shows (same `/collection/v2/paging` call with
  other set names)
- [ ] Add-to-queue from lists
- [ ] 16-level album art option
- [ ] Local mp3 player (separate project, offline)

### YouTube player

- [x] Playback prototype: cached and direct YouTube 360p H.264, SDL output,
  on-device extraction, gamepad controls and measurements
- [x] QuickJS JavaScript challenge support and playback-aware autosleep
- [x] Homebrew browser: search keyboard, thumbnails, saved videos and return
  to results after playback
- [ ] Broader streaming coverage, 480p and longer battery measurements

### Anki client

- [x] Find an official prebuilt aarch64 headless backend candidate (Anki 26.9.3)
- [x] Verify backend import and collection/scheduler operations on the device
- [ ] Verify AnkiWeb sync round-trip with a test deck, including offline reviews
- [ ] Build a gamepad review UI with a limited card renderer

## Done

- [x] Flashed `AmberELEC-RG351V.aarch64-20230203.img` to SD1 (replaced 351ELEC)
- [x] First boot, Wi-Fi, SSH key auth, root password changed (stored outside the repo)
- [x] Charging works with a proper charger
- [x] Homebrew verified (`hcl`, `opentyrian`, `prince`); unhid the 3 entries
- [x] Game sync (`scripts/sync.sh`) and save sync (`scripts/saves.sh`)
- [x] Fix RomM picker downloads nesting platform folders and failing on the second game
- [x] Fix game sync attempting to preserve Unix ownership on the exFAT ROM partition
- [x] Pikachu Volleyball built, deployed and verified (`scripts/build-port.sh pikaball`)
- [x] Monitoring: `scripts/perf.sh`, `scripts/battery-log.sh`
- [x] Wi-Fi remember hook (`scripts/wifi.sh install`), verified across a reboot
- [x] Battery life measured (see `docs/gotchas.md`, Power)
- [x] KOReader, ptterm (PTT BBS terminal)
- [x] PocketJS cost measured; decided not to pursue
- [x] Auto-suspend after idle (`scripts/autosleep.sh`), installed at 5 min
- [x] Spotify Connect daemon `deckd` + `deck` UI: now playing, liked songs,
  albums, playlists, search, queue, album art, fast reconnect after suspend
- [x] Suspend kernel panic fixed (`rg351v-wifi-sleep.service`); no autoplay at boot

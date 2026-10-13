# deck: Spotify player for the RG351V

A retro/cyberdeck-style Spotify player for the handheld. Online only (Spotify
Premium); local-file playback is a separate, later project.

## Status

- [x] Step 1: `librespot` runs as a systemd service and plays through ALSA as
      a Spotify Connect device named `RG351V` (`scripts/spotify.sh`).
- [x] Playing flag keeps `autosleep.py` from suspending mid-playlist.
- [x] First login from a phone (Connect devices -> RG351V); credentials
      cached, later starts log in by themselves.
- [x] Measured (2026-10-13, 160 kbps): 5.8% of one core at 1008 MHz, 17 MB
      RSS; autosleep held off for 392 s of idle playback; survived a forced
      suspend (same PID, ALSA recovered, Connect session re-established ~6 s
      after resume, playback continued); a second ALSA stream mixes in via
      `dmix` without warnings.
- [ ] Mixing with a real game/emulator (RetroArch may use its own ALSA setup).
- [x] Step 2 (v0): `deckd` (apps/deckd) replaces the librespot binary:
      Connect device + ALSA player, control socket, now-playing state,
      playing flag for autosleep, 8 MB RSS. Built with rustls (the device has
      no OpenSSL 3) and a Cargo.lock seeded from librespot 0.8.0.
- [x] Restore after a dropped session (verified with `ctl reconnect`: back
      as the active device within ~4 s, position kept to the second). The
      player keeps playing a buffered track through an outage, but the new
      Connect session is idle, so deckd re-loads the current track at the
      current position with autoplay on. The queue/context is not restored.
      Cutting Wi-Fi for 15 s or 90 s did not always drop the session
      (TCP and the websocket often ride it out), so `reconnect` is the
      deterministic test.
- [ ] Context-level restore, playlists/search/library browsing (metadata).
- [x] Step 3 (v0): `deck` (apps/deck), the SDL3 now-playing UI in the
      Homebrew menu as "Spotify": title/artist/album with marquee, progress,
      time, shuffle/repeat, volume, procedural cover, animated spectrum,
      battery and Wi-Fi in the header, backlight blanking while idle
      (`DECK_BLANK_SECS`, default 60), CJK titles via Unifont. Host-side
      preview: `cc -DDECK_NO_SDL apps/deck/src/main.c -lm` then
      `DECK_ASSETS=.storage/ptterm-data DECK_DUMP=out.ppm ./a.out`.
- [ ] On-device check of deck v0 (input, blanking, exit combo, CPU).
- [x] Library browsing (v0): deckd `playlists`, `tracks <ctx> <offset>`,
      `search <offset> <query>`, `play <ctx|-> <track>`; deck has a library
      menu, scrolling lists with lazy paging and D-pad key-repeat, and an OSK
      for search (START from now-playing opens the library).
- [ ] On-device check of browsing; real album art; albums/artists/queue.

## Constraints that shaped the design

- ES blocks while an app runs and the exit combo kills it, so anything that
  must keep playing lives in a systemd service, not in the UI process.
- No PulseAudio: the default ALSA device is a `dmix` (44.1 kHz) shared with ES
  and emulators, so a second player mixes in without extra setup.
- Suspend drops Wi-Fi (no wake-on-WLAN), so playback stops when the board
  sleeps; `autosleep.py` skips suspending while `/run/rg351v/playing` exists
  and librespot is running.
- 640x480 at 8x16 cells is exactly 80x30, the same grid ptterm renders with
  the Unifont assets, so the UI is a character-cell display (CJK titles
  included) at about 1% of a core.

## Architecture

```
Spotify app / deck UI
        |  Connect (Spirc)
   deckd  (Rust, systemd)  = librespot as a library: session, Connect control,
        |                    metadata/search, Web API token provider, ALSA sink
   unix socket, line protocol (no JSON parser needed on the C side)
        |
   deck   (C, SDL3 KMSDRM)  = ptterm's renderer, fonts and on-screen keyboard
```

- Browsing uses librespot's internal endpoints, not the Web API: the
  keymaster token route (`session.token_provider()`) now answers 403 "Invalid
  client" for librespot's client id (2026-10-13). Playlists come from
  `spclient().get_rootlist()` (names are decorated into the response),
  track names from `Track::get`, liked songs from the context
  `spotify:user:<name>:collection` and search from `spotify:search:<q>`
  (tracks only) via `get_context()`.
- `deck` is started from the Homebrew menu with the same jslisten exit-combo
  plumbing as ptterm and may be killed freely; `deckd` keeps the music going.

## UI

```
+ DECK//SPOT ------------------- WIFI ###-  BAT 78%  VOL ###-- +
| > LIBRARY            | NOW PLAYING                           |
|   PLAYLISTS     23   |  [album art, 4-colour ordered dither] |
|   LIKED SONGS  412   |  TRACK TITLE (Unifont, CJK ok)        |
|   ALBUMS        67   |  ARTIST - ALBUM                       |
|   ARTISTS       31   |  > 01:42 =======*---- 03:58           |
|   SEARCH...          |  next: TRACK 2                        |
+ A:Play B:Back Y:Search X:Queue L1/R1:Prev/Next START:Pause    +
```

- Near-black background, one phosphor accent (amber default; green or cyan
  selectable), block-character borders, optional scanlines (off by default).
- v0 controls: A play/pause, L1/R1 prev/next, D-pad left/right seek +-10 s,
  D-pad up/down or R2/L2 volume +-5, X shuffle, Y repeat, SELECT+START leave.
  Planned with browsing: D-pad moves lists, A plays/enters, B back, Y search
  (OSK from ptterm), X queue.
- Backlight blanks after a UI idle timeout and wakes on any button; music
  keeps playing.
- Offline is out of scope: Spotify downloads are DRM-bound to its own
  clients and are not reproduced here.

## Operations

```sh
scripts/spotify.sh build      # deckd for aarch64 (Docker, rust:1-bookworm)
scripts/spotify.sh install    # binary + systemd unit (replaces librespot's)
scripts/spotify.sh status     # service, login cache, log, now playing
scripts/spotify.sh ctl toggle # play/pause/next/prev/seek/volume/load ...
scripts/spotify.sh logout     # forget the cached login
```

Binary and unit live under `/storage/.config/rg351v/` and
`/storage/.config/system.d`; the login cache is `/storage/.cache/librespot`.

## Control socket protocol

`/run/rg351v/deckd.sock`, one request per line:

- `playlists`, `tracks <liked|playlist uri> [offset]`, `search <offset>
  <query>` -> `page\t<offset>\t<total>`, optional `context\t<uri>`, then
  `item\t<playlist|track>\t<uri>\t<title>\t<subtitle>` lines, then `.`
  (24 items per page); `play <context uri|-> <track uri>` plays a track
  inside a context (or alone with autoplay for `-`); `web <path>` is a debug
  probe of the Web API (currently fails with 403)
- `status` -> `key=value` lines (state, connected, user, uri, title, artist,
  album, cover, duration_ms, position_ms, volume, shuffle, repeat) ending with
  a line containing `.`
- `play | pause | toggle | next | prev`, `seek <ms>`, `volume <0-100>`,
  `shuffle on|off`, `repeat on|off`, `load <spotify uri>`, `reconnect`
  (drops the session on purpose) -> `OK` / `ERR msg`
- `subscribe` -> `OK`, then `event <name>` lines (track, playing, paused,
  stopped, loading, volume, shuffle, repeat, session); re-read `status` on
  each event.

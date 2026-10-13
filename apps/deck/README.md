# deck: Spotify player for the RG351V

A retro/cyberdeck-style Spotify player for the handheld. Online only (Spotify
Premium); local-file playback is a separate, later project.

## Status

- [x] Step 1: `librespot` runs as a systemd service and plays through ALSA as
      a Spotify Connect device named `RG351V` (`scripts/spotify.sh`).
- [x] Playing flag keeps `autosleep.py` from suspending mid-playlist.
- [ ] First login from a phone (Connect devices -> RG351V), then measure CPU,
      mixing with ES/games and behaviour across suspend and Wi-Fi drops.
- [ ] Step 2: `deckd`, the control daemon.
- [ ] Step 3: `deck`, the SDL3 UI.

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

- Web API tokens come from librespot's token provider for the logged-in
  session (the way ncspot and spotify-player work), so no developer app,
  user cap or redirect URL is needed on a keyboardless device.
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
- D-pad moves, A plays/enters, B back, Y search (OSK from ptterm), X queue,
  L1/R1 skip, L2/R2 volume, Start pause, Select menu.
- Backlight blanks after a UI idle timeout and wakes on any button; music
  keeps playing.
- Offline is out of scope: Spotify downloads are DRM-bound to its own
  clients and are not reproduced here.

## Step 1 operations

```sh
scripts/spotify.sh build      # librespot 0.8.0 for aarch64 (Docker)
scripts/spotify.sh install    # binary + event hook + systemd unit
scripts/spotify.sh status     # service, login cache, playing flag, log
scripts/spotify.sh logout     # forget the cached login
```

Binary, hook and login live under `/storage/.config/rg351v/` and
`/storage/.cache/librespot`.

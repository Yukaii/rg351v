# taiko

Native TJA (Taiko no Tatsujin chart) player for the RG351V, built on the same
C/SDL3/KMSDRM pipeline as `apps/deck`. Charts and audio are user supplied and
never committed.

## Status

- `src/probe.c` (`taiko-probe`): phase 0 audio/input latency probe. Results and
  the chosen ALSA settings are in `docs/gotchas.md` (Taiko / game audio
  latency): private dmix PCM, period 512 / buffer 1536 @ 48 kHz, ~21 ms output
  latency.
- `src/tja.c`, `src/tja.h`: TJA parser + timeline compiler (no SDL). Notes
  carry absolute audio-clock times, BPM, scroll and gogo state.
- Next: playable single song (see `TODO.md`).

## Build and deploy

    scripts/build-port.sh taiko-probe
    scripts/deploy-port.sh taiko-probe     # Homebrew > Taiko Probe

Probe controls: A click, B metronome, D-pad up/down switch PCM variant, X reset
stats, SELECT+START leave. Output goes to `/tmp/logs/taiko-probe.log`.

## Host tools

    cc -O2 -Wall -Wextra -Isrc -o /tmp/tja_dump tools/tja_dump.c src/tja.c
    /tmp/tja_dump song.tja [--notes <course-id>]
    python3 tools/check_timeline.py [songs-dir]   # needs .storage/taiko-songs

## Design notes

- The audio clock is the master; judge hits with the gamepad event timestamp,
  not the frame time.
- Launchers generate a private ALSA config (see
  `scripts/ports/taiko-probe/launcher.sh`) instead of editing the system one.

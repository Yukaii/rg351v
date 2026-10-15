# taiko

Native TJA (Taiko no Tatsujin chart) player for the RG351V, built on the same
C/SDL3/KMSDRM pipeline as `apps/deck`. Charts and audio are user supplied and
never committed.

## Layout

- `src/tja.c`, `src/tja.h`: TJA parser and timeline compiler (no SDL). Notes
  carry absolute audio-clock times, BPM, scroll and gogo state.
- `src/game.c`, `src/game.h`: judgement engine (hits, rolls, balloons, score);
  no SDL, tested on the host.
- `src/taiko.c`: the app: menu, calibration, play. Audio callback is the clock;
  an evdev thread timestamps hits; SDL draws.
- `src/audio_codecs.c`: audio loading (ogg via `stb_vorbis.c`, wav via
  `dr_wav.h`, mp3 via `dr_mp3.h`; all public domain) and `WAVE:` resolution.
- Text uses GNU Unifont from `unifont.bin` (shared with deck/ptterm, shipped by
  the port via `EXTRA_FILES`); without it the app falls back to ASCII only.
  All artwork is drawn from primitives, no game assets.
- `tools/tja_dump.c`, `tools/check_timeline.py`, `tools/game_test.c`: host
  tools and tests.

## Build, deploy, songs

    scripts/build-port.sh taiko
    scripts/deploy-port.sh taiko        # Homebrew > Taiko

Songs are not part of the port:

    scripts/taiko.sh sync <dir> [--dry-run]   # every folder with a .tja under <dir>
    scripts/taiko.sh list
    scripts/taiko.sh remove <folder>

They end up in `/storage/roms/homebrew/taiko/songs/<name>/` (the SMB share
works too: drop a chart folder in, the menu rescans on start). Sample charts:
see `docs/gotchas.md`, TJA charts.

Old charts are supported as they are: Shift-JIS (CP932) text is converted by
the player (`src/cp932_table.h`, from `tools/gen_cp932.py`), audio may be
`.ogg`, `.wav` or `.mp3`, and a `WAVE:` name that does not match the file
exactly (case, other extension, `\` paths) is resolved to the closest file.
Charts whose audio cannot be found are hidden and the reason is in
`/tmp/logs/taiko.log`.

## Controls

Menu: D-pad up/down song, left/right course, A play, SELECT+START quit.
In game: A/B/D-pad don, X/Y/L1/R1 ka, L2/R2 judgement offset -/+5 ms,
SELECT+START back. Calibration: tap in time with the clicks (judgement
offset), L2/R2 aligns the flash with the click (visual offset); START applies.

## Timing model

- The audio clock is the master; hits carry the kernel event timestamp, so
  frames never delay judgement or the hit sound.
- Two trims, kept apart on purpose: the judgement offset (`g_user_off`) only
  shifts hit times, to cancel how early/late you habitually tap; the visual
  offset (`g_vis_off`) only shifts where notes are drawn, to cancel display
  delay. Notes on screen always follow the sound.
- Output latency is set by a private ALSA dmix (period 512 / buffer 1536 @
  48 kHz, ~21 ms) created in `scripts/ports/taiko/launcher.sh`; the system
  `asound.conf` is left alone.

## Host tools

    cc -O2 -Wall -Wextra -Isrc -o /tmp/tja_dump tools/tja_dump.c src/tja.c
    /tmp/tja_dump song.tja [--notes <course-id>]
    cc -O2 -Wall -Wextra -Isrc -o /tmp/game_test tools/game_test.c src/game.c src/tja.c -lm
    /tmp/game_test song.tja <course-id>
    python3 tools/check_timeline.py [songs-dir]   # needs .storage/taiko-songs

Debug options of the app: `--tja f --course N [--autoplay]`,
`--dump out.bmp --at T` (one frame, headless), `--snap out.bmp --snap-after S`
and `--screen calibrate`, `TAIKO_FAKE_TAP_MS`, `TAIKO_LATENCY_MS`,
`TAIKO_VISUAL_MS`. Headless runs: Docker with `SDL_VIDEODRIVER=offscreen
SDL_AUDIODRIVER=dummy`.

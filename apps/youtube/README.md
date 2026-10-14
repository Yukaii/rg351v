# YouTube player

Open **Homebrew > YouTube** to search, browse thumbnails, save videos and play
360p/30 fps H.264 with AAC audio. Extraction and playback run on the handheld;
the Mac is only needed for installation and development. The browser uses
stock SDL2 through Python ctypes, the same Unifont asset as deck/ptterm, and
the existing mpv/Python playback wrapper.

Files live under `/storage/roms/homebrew/youtube`, with a top-level
`YouTube.sh` launcher registered in Homebrew. Installation preserves the
earlier Ports search/saved data and disables its old launcher.

Browser controls:

- D-pad: navigate; left/right move a page (three videos).
- A: play the selected video; B: exit the browser.
- X/Start: search keyboard; Y: switch between results and saved videos.
- R1: save/remove the selected video; Select: exit.
- Keyboard: A types, X deletes, Y adds space, L1 clears, Start submits,
  B returns without searching. The keyboard supports ASCII letters/digits;
  titles can display Unicode through Unifont.

During playback, A/Start pauses, B/Select returns to the browser and the
D-pad seeks/adjusts volume. Results, selection and the query survive playback.
Search results, saved videos and thumbnails persist across launches.
Search returns up to 12 videos. Missing artwork uses a placeholder.

Loading shows elapsed time and supports B to cancel the extraction process
group (including QuickJS). Preparing a new video can take tens of seconds
on the RK3326. Replaying the same video reuses a manifest up to ten minutes
old only if all stream URLs have at least a minute left before expiry.

```sh
scripts/youtube.sh probe
scripts/youtube.sh prepare 'https://www.youtube.com/watch?v=aqz-KE-bpKQ'
scripts/youtube.sh install
scripts/youtube.sh ui
scripts/youtube.sh play
scripts/youtube.sh stream 'https://www.youtube.com/watch?v=aqz-KE-bpKQ'
scripts/youtube.sh log
```

`prepare` uses Mac-side yt-dlp and ffmpeg to download the first minute without
transcoding. `stream` runs the bundled, checksum-verified yt-dlp 2026.08.19
on the handheld, then plays its video/audio URLs directly. SSH stays open
during these measurements; EmulationStation returns on exit.
The Mac provides orchestration, not a media proxy.

Normal menu launches ignore EmulationStation's `-P...`/`-G...` arguments.
The launcher's private `--play <file>` mode is used by the Mac-side playback
and stream benchmark commands.

The installer bundles checksum-verified QuickJS-NG 0.17.0 (2.4 MB). yt-dlp
uses it to execute YouTube's JavaScript stream-URL challenges, together with
the solver scripts already in the yt-dlp zip. It runs during extraction,
not continuously during video playback. No web browser is installed.

Upgrade the autosleep watcher with `scripts/autosleep.sh install`. The player
refreshes `/run/rg351v/video-playing.json` once per second while unpaused.
The watcher checks heartbeat age (<15 seconds), mpv PID and process start
time, so a crashed wrapper or reused PID cannot leave sleep blocked indefinitely.
Pausing removes the marker; other normal blockers such as charging and busy
Wi-Fi still apply. The wrapper rescans the controller after USB re-enumeration.

Run the marker safety checks with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/device/tests -v
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s apps/youtube/tests -v
```

Search/play/return and keyboard controls were confirmed on the screen.
After reboot, cached results and font assets persisted, and opening/closing
the native display twice with results/keyboard rendering passed.

The OSH evdev button map is 304..315, while the installed SDL controller DB
assigns face buttons to b0..b3, shoulders to b4/b5 and Start/Select to b6/b7.
The browser and player use that measured mapping; it differs from assuming
standard Linux BTN_START/BTN_SELECT codes. Controller events are rescanned
after USB re-enumeration.

Before launching mpv, the browser closes its SDL display and input handles.
When playback ends, it creates a fresh display/controller and restores the
results. This avoids competing for DRM and replaying queued playback button
events in the browser. SIGTERM cancels extraction or the active player group.

The wrapper reads only the OSH controller, leaving the power key and system
volume buttons alone. SDL's KMSDRM renderer handles video output. mpv's built-in
gamepad reader is disabled because it initializes SDL before the video output
and prevents `--vo=sdl` from opening. A process lock prevents concurrent players.

CLI playback's `/tmp/logs/youtube.log` records position, pause, dropped frames,
audio/video drift, CPU use (100% = one core), battery current and temperature
every five seconds. It resets each launch and does not survive reboot.
Browser and GUI-playback messages are in `/tmp/logs/youtube-ui.log` and the launch
boundary is in `/tmp/logs/youtube-launcher.log`; extraction stderr is in the
app's `extract.log`. `YOUTUBE_DEBUG_INPUT=1` logs input/state transitions.
Stream manifests contain expiring URLs; they stay outside git.

Host layout preview (PPM needs only Python; PNG additionally needs Pillow):

```sh
python3 apps/youtube/youtube-ui.py --font .storage/ptterm-data/unifont.bin \
  --root .storage/youtube --preview results --dump .storage/youtube/results.ppm
```

Other preview states: `keyboard`, `empty`, `loading`, `error`. Rendering uses
the same framebuffer/text code as the live UI. `YOUTUBE_QUERY` starts an
initial search when testing on the device.

Limits and next steps:

- Playback coverage is still narrow. QuickJS supports URL challenges, but
  cannot make unavailable videos, login requirements or missing client formats
  disappear. More videos and longer streaming sessions need verification.
- Measure 480p, longer playback and battery use across more demanding scenes.

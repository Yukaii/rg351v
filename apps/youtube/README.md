# YouTube playback prototype

First milestone: 360p/30 fps H.264 with AAC audio, using AmberELEC's existing
mpv and a small Python controller/measurement wrapper. No custom browsing UI
yet. Files live under `/storage/roms/ports/youtube`, with a `YouTube.sh`
launcher in Ports. The menu entry plays a cached one-minute sample.

```sh
scripts/youtube.sh probe
scripts/youtube.sh prepare 'https://www.youtube.com/watch?v=aqz-KE-bpKQ'
scripts/youtube.sh install
scripts/youtube.sh play
scripts/youtube.sh stream 'https://www.youtube.com/watch?v=aqz-KE-bpKQ'
scripts/youtube.sh log
```

`prepare` uses Mac-side yt-dlp and ffmpeg to download the first minute without
transcoding. `stream` runs the bundled, checksum-verified yt-dlp 2026.08.19
on the handheld, then plays its video/audio URLs directly. SSH stays open
during these measurements; EmulationStation returns on exit.
The Mac provides orchestration, not a media proxy.

The installer bundles checksum-verified QuickJS-NG 0.17.0 (2.4 MB). yt-dlp
uses it to execute YouTube's JavaScript stream-URL challenges, together with
the solver scripts already in the yt-dlp zip. It runs during extraction,
not continuously during video playback. No browser is installed.

Upgrade the autosleep watcher with `scripts/autosleep.sh install`. The player
refreshes `/run/rg351v/video-playing.json` once per second while unpaused.
The watcher checks heartbeat age (<15 seconds), mpv PID and process start
time, so a crashed wrapper or reused PID cannot leave sleep blocked indefinitely.
Pausing removes the marker; other normal blockers such as charging and busy
Wi-Fi still apply. The wrapper rescans the controller after USB re-enumeration.

Run the marker safety checks with:

```sh
PYTHONDONTWRITEBYTECODE=1 python3 -m unittest discover -s scripts/device/tests -v
```

Controls: A/Start pauses; B/Select exits; D-pad left/right seeks 10 seconds;
D-pad up/down adjusts volume. Button names follow the Linux input mapping;
pause, seek and exit were confirmed on the RG351V.
Installation survived a reboot; playback and IPC pause/seek/resume were
verified again afterward.

The wrapper reads only the OSH controller, leaving the power key and system
volume buttons alone. SDL's KMSDRM renderer handles video output. mpv's built-in
gamepad reader is disabled because it initializes SDL before the video output
and prevents `--vo=sdl` from opening. A process lock prevents concurrent players.

`/tmp/logs/youtube.log` records playback position, pause, dropped frames,
audio/video drift, CPU use (100% = one core), battery current and temperature
every five seconds. It resets each launch and does not survive reboot.
Stream manifests contain expiring URLs; they stay outside git.

Limits and next steps:

- Playback coverage is still narrow. QuickJS supports URL challenges, but
  cannot make unavailable videos, login requirements or missing client formats
  disappear. More videos and longer streaming sessions need verification.
- Add a standalone URL/search interface, thumbnails and saved videos.
- Measure 480p, longer playback and battery use across more demanding scenes.

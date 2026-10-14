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
during playback to prevent autosleep; EmulationStation returns on exit.
The Mac provides orchestration, not a media proxy.

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

- Only one public video has been exercised. Full YouTube support requires
  a supported JavaScript runtime; extraction currently warns that formats
  may be missing. Add QuickJS and verify more videos before calling it complete.
- Add a standalone URL/search interface, thumbnails and saved videos.
- Add playback-aware autosleep handling before launching long videos without SSH.
- Measure 480p, longer playback and battery use across more demanding scenes.

# Anki client (backend prototype)

The official prebuilt headless Anki backend runs on the RG351V. The planned
client uses upstream collection storage, scheduling and AnkiWeb sync, with a
custom gamepad UI and a limited card renderer. There is no review UI yet.

## Reproduce the offline check

On the Mac, with Python/pip and `uv` available:

```sh
bash scripts/anki.sh prepare
bash scripts/anki.sh probe
```

`prepare` installs pinned Linux aarch64/Python 3.11 wheels into the ignored
`.storage/anki-feasibility/` directory and precompiles Python 3.11 bytecode.
`uv` may download its Python 3.11 interpreter. The backend needs no cross-build,
Qt, browser engine or device-side pip. Precompilation avoids a reproducible
source-compilation stall in AmberELEC's Python 3.11.1.

`probe` asserts the device hostname and copies the bundle into
`/storage/roms/homebrew/anki-backend-probe/`. It creates and removes a temporary
collection there; it does not log in or access any existing Anki collection.
The runtime stays installed for subsequent development. The probe is bounded
to 45 seconds and checks:

- Basic note/card creation and upstream question/answer HTML generation.
- The v3 scheduler's four rating choices and a Good review.
- Review history, scheduling changes, undo/redo and persistence after reopening.
- Local media writing and reading on the exFAT ROM partition.

Verified on device and again after reboot (2026-10-14). Imports took ~3 seconds
warm and ~12 seconds just after boot; collection checks took ~2 seconds. The
installed bundle occupies ~76 MB on exFAT. These checks do not verify screen
rendering, FSRS-specific behavior or AnkiWeb connectivity/sync.

## Next milestones

1. Sync a disposable test collection through AnkiWeb and verify offline reviews
   appear in desktop Anki, including media and initial full-download handling.
2. Build the deck picker and question/answer UI, with four physical rating buttons.
3. Add undo, scrolling, media playback and suspend/resume handling.

Keep credentials and real collections outside git. The upstream backend is
AGPL-3.0-or-later; retain its license/source obligations when distributing.

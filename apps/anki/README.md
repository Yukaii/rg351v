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
rendering or FSRS-specific behavior.

## Sync and login

```sh
bash scripts/anki.sh deploy
bash scripts/anki.sh login
bash scripts/anki.sh status
bash scripts/anki.sh sync --full download  # initial download from AnkiWeb
bash scripts/anki.sh sync                  # subsequent incremental sync
```

`login` opens a terminal over SSH and prompts for your Anki email and password.
It stores only the returned sync token, in
`/storage/.config/rg351v/anki/auth.bin` (mode 600, on ext4), backing up an
existing token before replacing it. Passwords are not stored. Enter credentials
in your terminal, not in chat or command arguments.

The collection and media live in `/storage/roms/homebrew/anki-data/`, with
automatic pre-sync collection backups in its `backups/` directory. These are
collection backups, not copies of all media. Normal sync waits for media to
finish; a media failure is reported even if collection sync succeeded.
The terminal prints collection/media progress to stderr (the final result is
JSON on stdout). Initial media downloads can take several minutes; the wrapper
times out after 180 seconds without progress, rather than limiting total runtime.
Retrying a failed media sync resumes using the files already downloaded.

A required full sync stops until you choose `--full download` or `--full upload`.
Download replaces the handheld collection; upload replaces the remote collection.
The wrapper also rejects a direction the backend says is unavailable. The first
download is appropriate when your existing collection is already on AnkiWeb.

## Disposable sync integration test

```sh
bash scripts/anki.sh probe-sync
```

Requires the deployed backend. This installs a matching Mac headless backend in
`.storage/anki-host/`, starts the official sync server bound to Mac loopback,
and gives the handheld access through a temporary SSH reverse tunnel. It uses
random test credentials and separate temporary collections; it never contacts
AnkiWeb or uses your account. Server, tunnel and test collections are cleaned
up on completion. The open tunnel also prevents device autosleep during testing.

Verified on hardware (2026-10-14): initial login/full card and image download;
review and note edit while the server is stopped; concurrent Mac card addition;
bidirectional incremental review/scheduling/note/media sync; repeat sync without
duplicate reviews; full-sync direction guards; pre-sync backups; and terminal
status/sync with private token storage. The deployed CLI also loaded after reboot.
Public AnkiWeb HTTPS/API connectivity also passed a separate test using a
deliberately invalid token (expected AUTH rejection).

## AnkiWeb verification

Verified with the connected account (2026-10-15): full collection/media download,
then an isolated test card and PNG uploaded from the handheld and downloaded
by a separate Mac headless client. A locally recorded Good review subsequently
arrived on the Mac through AnkiWeb, including scheduling and review history.
The test note, deck, tag and active media file were removed afterward.

Anki retains review logs when cards are deleted. The real-account test therefore
left one synthetic review record; existing review records were not modified.
Use the disposable local-server test for future automated review tests, so those
records stay out of a real account. Do not force a full upload just to erase a
test review from all clients.

## Next milestones

1. Build the deck picker and question/answer UI, with four physical rating buttons.
2. Add undo, scrolling, media playback and suspend/resume handling.

Keep credentials and real collections outside git. The upstream backend is
AGPL-3.0-or-later; retain its license/source obligations when distributing.

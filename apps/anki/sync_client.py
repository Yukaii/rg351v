"""Headless upstream sync, shared by the CLI and the disposable integration test."""

import time
from pathlib import Path

from anki.collection import Collection
from anki.sync import SyncAuth, SyncOutput


class FullSyncRequired(RuntimeError):
    pass


def sync_collection(col: Collection, auth: SyncAuth, *, full=None, backup_dir=None,
                    on_progress=None):
    """Sync collection and media, requiring an explicit full-sync direction.

    `full` resolves a full-sync request; it never forces one over a normal sync.
    The caller persists auth after this returns, including endpoint redirects.
    """
    if full not in (None, "download", "upload"):
        raise ValueError("full must be download or upload")
    if on_progress:
        on_progress("Syncing collection...")
    if backup_dir is not None:
        Path(backup_dir).mkdir(parents=True, exist_ok=True)
        col.create_backup(backup_folder=str(backup_dir), force=True,
                          wait_for_completion=True)
    auth.io_timeout_secs = 30
    out = col.sync_collection(auth, sync_media=False)
    if out.new_endpoint:
        auth.endpoint = out.new_endpoint
    required = out.required
    if required in (SyncOutput.FULL_SYNC, SyncOutput.FULL_DOWNLOAD, SyncOutput.FULL_UPLOAD):
        name = SyncOutput.ChangesRequired.Name(required)
        if full is None:
            raise FullSyncRequired(f"{name}: rerun with --full download or --full upload")
        if required == SyncOutput.FULL_DOWNLOAD and full != "download":
            raise FullSyncRequired("The local collection is empty; only download is available")
        if required == SyncOutput.FULL_UPLOAD and full != "upload":
            raise FullSyncRequired("The remote collection is empty; only upload is available")
        col.close_for_full_sync()
        if on_progress:
            on_progress(f"Full {full}...")
        try:
            col.full_upload_or_download(auth=auth, server_usn=None, upload=full == "upload")
        finally:
            col.reopen(after_full_sync=True)
    elif required != SyncOutput.NO_CHANGES:
        raise RuntimeError(f"Unexpected sync result: {required}")

    # Explicitly wait for media completion; collection success alone isn't enough.
    col.sync_media(auth)
    deadline = time.monotonic() + 180
    last_progress = None
    last_report = 0
    if on_progress:
        on_progress("Syncing media...")
    while True:
        status = col.media_sync_status()
        if not status.active:
            break
        now = time.monotonic()
        progress = status.progress.SerializeToString()
        if progress != last_progress:
            # Large initial libraries can take much longer than three minutes.
            # Bound time without progress, rather than total transfer duration.
            deadline = now + 180
            last_progress = progress
            if on_progress and now - last_report >= 5:
                details = " | ".join(value for value in (
                    status.progress.checked, status.progress.added, status.progress.removed
                ) if value)
                if details:
                    on_progress("Media: " + details)
                last_report = now
        if now > deadline:
            col.abort_media_sync()
            raise TimeoutError("Collection synced, but media made no progress for 180 seconds; retry sync")
        time.sleep(0.1)
    return {"collection": "complete", "media": "complete",
            "full": full if required != SyncOutput.NO_CHANGES else None}

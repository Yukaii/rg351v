"""Headless upstream sync, shared by the CLI and the disposable integration test."""

import time
from pathlib import Path

from anki.collection import Collection
from anki.sync import SyncAuth, SyncOutput


class FullSyncRequired(RuntimeError):
    pass


def sync_collection(col: Collection, auth: SyncAuth, *, full=None, backup_dir=None):
    """Sync collection and media, requiring an explicit full-sync direction.

    `full` resolves a full-sync request; it never forces one over a normal sync.
    The caller persists auth after this returns, including endpoint redirects.
    """
    if full not in (None, "download", "upload"):
        raise ValueError("full must be download or upload")
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
        try:
            col.full_upload_or_download(auth=auth, server_usn=None, upload=full == "upload")
        finally:
            col.reopen(after_full_sync=True)
    elif required != SyncOutput.NO_CHANGES:
        raise RuntimeError(f"Unexpected sync result: {required}")

    # Explicitly wait for media completion; collection success alone isn't enough.
    col.sync_media(auth)
    deadline = time.monotonic() + 180
    while col.media_sync_status().active:
        if time.monotonic() > deadline:
            col.abort_media_sync()
            raise TimeoutError("Collection synced, but media sync timed out; retry sync")
        time.sleep(0.1)
    return {"collection": "complete", "media": "complete",
            "full": full if required != SyncOutput.NO_CHANGES else None}

"""Regression checks for initial media libraries that take over three minutes."""

import unittest
from unittest.mock import Mock, patch

from anki.sync import SyncAuth, SyncOutput
from anki.sync_pb2 import MediaSyncProgress, MediaSyncStatusResponse

from sync_client import sync_collection


class MediaTimeoutTests(unittest.TestCase):
    def run_media_sync(self, progress):
        col = Mock()
        col.sync_collection.return_value = SyncOutput(required=SyncOutput.NO_CHANGES)
        col.media_sync_status.side_effect = progress
        clock = [0]

        def sleep(_):
            clock[0] += 100

        with patch("sync_client.time.monotonic", side_effect=lambda: clock[0]), \
                patch("sync_client.time.sleep", side_effect=sleep):
            result = sync_collection(col, SyncAuth(hkey="test"))
        return col, result, clock[0]

    def test_progress_allows_transfer_longer_than_three_minutes(self):
        statuses = [MediaSyncStatusResponse(active=True, progress=MediaSyncProgress(checked=str(i)))
                    for i in range(4)]
        statuses.append(MediaSyncStatusResponse(active=False))
        col, result, elapsed = self.run_media_sync(statuses)
        self.assertGreater(elapsed, 180)
        self.assertEqual(result["media"], "complete")
        col.abort_media_sync.assert_not_called()

    def test_stalled_transfer_is_aborted(self):
        col = Mock()
        col.sync_collection.return_value = SyncOutput(required=SyncOutput.NO_CHANGES)
        col.media_sync_status.return_value = MediaSyncStatusResponse(
            active=True, progress=MediaSyncProgress(checked="0"))
        clock = [0]

        def sleep(_):
            clock[0] += 100

        with patch("sync_client.time.monotonic", side_effect=lambda: clock[0]), \
                patch("sync_client.time.sleep", side_effect=sleep):
            with self.assertRaisesRegex(TimeoutError, "no progress"):
                sync_collection(col, SyncAuth(hkey="test"))
        col.abort_media_sync.assert_called_once()


if __name__ == "__main__":
    unittest.main()

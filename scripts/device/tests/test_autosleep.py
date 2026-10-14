"""Regression checks for video sleep inhibition, including crash/PID reuse."""
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    'autosleep', Path(__file__).resolve().parents[1] / 'autosleep.py')
autosleep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(autosleep)

# Linux /proc stat: field 22 is the process start time in clock ticks.
STAT = '42 (mpv) S 1 42 42 0 -1 4194304 0 0 0 0 5 5 0 0 20 0 1 0 12345 1024 12'


class VideoSleepTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.flag = Path(self.directory.name) / 'video.json'
        self.state = {'pid': 42, 'start_ticks': '12345', 'updated': 95}
        for target, value in (
            ('VIDEO_PLAYING_FLAG', str(self.flag)),
            ('read_text', lambda name: 'mpv' if name.endswith('/comm') else STAT),
        ):
            mock = patch.object(autosleep, target, value)
            mock.start()
            self.addCleanup(mock.stop)
        clock = patch.object(autosleep.time, 'monotonic', return_value=100)
        clock.start()
        self.addCleanup(clock.stop)

    def check_state(self, expected):
        self.flag.write_text(json.dumps(self.state))
        self.assertEqual(autosleep.video_playing(), expected)

    def test_fresh_live_player(self):
        self.check_state(True)

    def test_stale_player_heartbeat(self):
        self.state['updated'] = 85
        self.check_state(False)

    def test_future_heartbeat(self):
        self.state['updated'] = 101
        self.check_state(False)

    def test_reused_pid(self):
        self.state['start_ticks'] = '12344'
        self.check_state(False)

    def test_dead_or_different_process(self):
        with patch.object(autosleep, 'read_text', return_value=''):
            self.check_state(False)
        with patch.object(autosleep, 'read_text', return_value='python3'):
            self.check_state(False)

    def test_invalid_pid(self):
        self.state['pid'] = '../42'
        self.check_state(False)

    def test_missing_and_malformed_marker(self):
        self.assertFalse(autosleep.video_playing())
        for content in ('{', '{}', 'null', '[]'):
            self.flag.write_text(content)
            self.assertFalse(autosleep.video_playing())


if __name__ == '__main__':
    unittest.main()

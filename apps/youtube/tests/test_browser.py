"""Browser state, saved-video persistence and cancellable extraction checks."""
import importlib.util
import os
import json
from pathlib import Path
import queue
import tempfile
import time
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('youtube_ui', Path(__file__).resolve().parents[1] / 'youtube-ui.py')
ui = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ui)

ROW = {'id': 'aqz-KE-bpKQ', 'title': 'Big Buck Bunny', 'channel': 'Blender', 'duration': 634}


class BrowserTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.font = self.root / 'font.bin'
        self.font.write_bytes(b'UF16\0\0\0\0' + bytes(65536 * 4))
        self.browser = ui.Browser(self.root, self.font)

    def test_keyboard_edit_cancel_and_submit(self):
        b = self.browser
        b.query = 'original'
        b.action('x')
        b.action('clear')
        b.action('a')  # a
        b.action('right')
        b.action('a')  # b
        b.action('y')
        b.action('x')
        self.assertEqual(b.draft, 'ab')
        b.action('b')
        self.assertEqual(b.query, 'original')
        b.action('x')
        b.action('clear')
        with patch.object(b, 'start_job') as start:
            b.action('start')
            start.assert_not_called()
            b.action('a')
            b.action('start')
            start.assert_called_once_with('search', 'b')
        self.assertFalse(b.keyboard)

    def test_save_remove_and_reload(self):
        b = self.browser
        b.results = [ROW.copy()]
        b.action('save')
        self.assertEqual(ui.Browser(self.root, self.font).saved, [ROW])
        b.action('y')
        b.action('save')
        self.assertEqual(b.rows, [])
        self.assertEqual(json.loads((self.root / 'saved.json').read_text()), [])

    def test_selection_bounds_and_empty_library(self):
        b = self.browser
        b.results = [ROW.copy() for _ in range(12)]
        b.action('up')
        self.assertEqual(b.selected, 0)
        for _ in range(10):
            b.action('right')
        self.assertEqual(b.selected, 11)
        b.action('y')
        b.action('a')
        self.assertFalse(b.busy)

    def test_invalid_cache_and_video_rows(self):
        (self.root / 'search.json').write_text('{"entries":null}')
        (self.root / 'saved.json').write_text('null')
        self.assertEqual(ui.Browser(self.root, self.font).rows, [])
        self.assertEqual(ui.video_rows({'entries': [None, {'id': '../bad'}, ROW]}), [ROW])

    def test_stale_job_cannot_overwrite_new_results(self):
        b = self.browser
        old, current = object(), object()
        b.job, b.busy = current, True
        b.messages.put((old, 'results', [ROW]))
        b.update()
        self.assertEqual(b.results, [])
        self.assertTrue(b.busy)

    def test_failed_job_remains_retryable(self):
        b = self.browser
        b.job, b.busy = object(), True
        b.messages.put((b.job, 'error', 'Check Wi-Fi'))
        b.update()
        self.assertFalse(b.busy)
        self.assertEqual(b.status, 'Check Wi-Fi')

    def test_cancel_running_extractor(self):
        (self.root / 'yt-dlp').write_text('import time\ntime.sleep(60)\n')
        messages = queue.Queue()
        job = ui.Job(self.root, 'search', 'safe query', messages)
        self.addCleanup(job.cancel)
        deadline = time.monotonic() + 3
        while job.process is None and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertIsNotNone(job.process)
        job.cancel()
        job.thread.join(timeout=3)
        self.assertFalse(job.thread.is_alive())
        self.assertEqual(messages.get(timeout=1)[1], 'cancelled')
        self.assertIsNotNone(job.process.poll())

    def test_cached_stream_requires_matching_id_and_unexpired_urls(self):
        file = self.root / 'stream.json'
        valid = {'id': ROW['id'], 'requested_formats': [
            {'url': 'https://example.com/video?expire=' + str(int(time.time()) + 3600)},
            {'url': 'https://example.com/audio?expire=' + str(int(time.time()) + 3600)},
        ]}
        file.write_text(json.dumps(valid))
        self.assertEqual(ui.cached_stream(self.root, ROW['id']), file)
        self.assertIsNone(ui.cached_stream(self.root, 'jNQXAC9IVRw'))
        valid['requested_formats'][1]['url'] = 'https://example.com/audio?expire=1'
        file.write_text(json.dumps(valid))
        self.assertIsNone(ui.cached_stream(self.root, ROW['id']))
        valid['requested_formats'][1]['url'] = valid['requested_formats'][0]['url']
        file.write_text(json.dumps(valid))
        os.utime(file, (time.time() - 700, time.time() - 700))
        self.assertIsNone(ui.cached_stream(self.root, ROW['id']))

    def test_actual_osh_button_order(self):
        # Captured JSIOCGBTNMAP is 304..315; SDL DB maps a/b/x/y to b0..b3,
        # shoulders to b4/b5 and start/back to b6/b7.
        controller = ui.Controller()
        controller.inputs, controller.next_scan = {'/dev/input/event3': 42}, float('inf')
        events = b''.join(controller.event.pack(0, 0, 1, code, 1) for code in range(304, 312))
        with patch.object(ui.os, 'read', return_value=events):
            self.assertEqual(controller.events(), ['a', 'b', 'x', 'y', 'clear', 'save', 'start', 'exit'])


if __name__ == '__main__':
    unittest.main()

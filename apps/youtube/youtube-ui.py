#!/usr/bin/env python3
"""Console YouTube browser using stock SDL2, Unifont and the playback wrapper.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import argparse
import ctypes as C
import glob
import json
import os
from pathlib import Path
import queue
import re
import signal
import struct
import subprocess
import sys
import threading
import time
import urllib.request
import urllib.parse

W, H = 640, 480
BG, PANEL, SELECTED = 0xFF10161C, 0xFF1B252E, 0xFF293C49
WHITE, MUTED, RED, TEAL = 0xFFF0F3F5, 0xFF98ACB9, 0xFFFF605B, 0xFF65D5C7
VIDEO_ID = re.compile(r'^[A-Za-z0-9_-]{11}$')
FORMAT = 'bestvideo[height<=360][fps<=30][vcodec^=avc1]+bestaudio[ext=m4a]/best[height<=360][fps<=30][vcodec^=avc1]'
KEYS = 'abcdefghijklmnopqrstuvwxyz0123456789'


def read_json(file, fallback):
    try:
        return json.loads(file.read_text())
    except (OSError, ValueError):
        return fallback


def write_json(file, data):
    temporary = file.with_suffix('.tmp')
    temporary.write_text(json.dumps(data, ensure_ascii=False))
    temporary.replace(file)


def video_rows(data):
    rows = []
    for entry in data.get('entries', []):
        if not entry or not VIDEO_ID.fullmatch(str(entry.get('id', ''))):
            continue
        rows.append({
            'id': entry['id'], 'title': entry.get('title') or 'Untitled video',
            'channel': entry.get('channel') or entry.get('uploader') or '',
            'duration': entry.get('duration'),
        })
    return rows


def cached_stream(root, video_id):
    file = root / 'stream.json'
    data = read_json(file, {})
    if not isinstance(data, dict) or data.get('id') != video_id:
        return None
    try:
        if not 0 <= time.time() - file.stat().st_mtime < 600:
            return None
        formats = data.get('requested_formats') or [data]
        for track in formats:
            url = urllib.parse.urlparse(track['url'])
            expires = int(urllib.parse.parse_qs(url.query)['expire'][0])
            if url.scheme != 'https' or expires < time.time() + 60:
                return None
    except (OSError, TypeError, ValueError, KeyError, IndexError):
        return None
    return file


class Canvas:
    def __init__(self, font):
        raw = Path(font).read_bytes()
        if raw[:4] != b'UF16' or len(raw) < 8 + 65536 * 4:
            raise ValueError('invalid Unifont asset')
        self.index = struct.unpack_from('<65536I', raw, 8)
        self.glyphs = raw[8 + 65536 * 4:]
        self.pixels = bytearray(W * H * 4)

    def fill(self, x, y, w, h, color):
        x0, y0, x1, y1 = max(x, 0), max(y, 0), min(x + w, W), min(y + h, H)
        if x1 <= x0 or y1 <= y0:
            return
        row = struct.pack('<I', color) * (x1 - x0)
        for cy in range(y0, y1):
            start = (cy * W + x0) * 4
            self.pixels[start:start + len(row)] = row

    def width(self, char):
        cp = ord(char)
        return 16 if cp < 65536 and self.index[cp] & 0x80000000 else 8

    def text(self, x, y, text, color=WHITE, scale=1, max_width=W):
        end = min(W, x + max_width)
        for char in str(text):
            cp = ord(char) if ord(char) < 65536 else 0xFFFD
            ent = self.index[cp]
            width = 16 if ent & 0x80000000 else 8
            if x + width * scale > end:
                break
            offset = (ent & 0x7FFFFFFF) - 1
            if ent:
                for row in range(16):
                    bits = self.glyphs[offset + row] if width == 8 else int.from_bytes(
                        self.glyphs[offset + row * 2:offset + row * 2 + 2], 'big')
                    for col in range(width):
                        if bits & (1 << (width - col - 1)):
                            self.fill(x + col * scale, y + row * scale, scale, scale, color)
            x += width * scale

    def lines(self, text, width, limit=2):
        lines, current, size = [], '', 0
        for char in str(text).replace('\n', ' '):
            cw = self.width(char)
            if size + cw > width:
                lines.append(current)
                current, size = '', 0
                if len(lines) == limit:
                    lines[-1] = lines[-1][:-3] + '...'
                    return lines
            current += char
            size += cw
        if current:
            lines.append(current)
        return lines

    def ppm(self, file):
        rgb = bytearray(W * H * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = self.pixels[2::4], self.pixels[1::4], self.pixels[0::4]
        Path(file).write_bytes(f'P6\n{W} {H}\n255\n'.encode() + rgb)


class Rect(C.Structure):
    _fields_ = [('x', C.c_int), ('y', C.c_int), ('w', C.c_int), ('h', C.c_int)]


class Display:
    def __init__(self):
        self.sdl = C.CDLL('libSDL2.so')
        self.img = C.CDLL('libSDL2_image.so')
        ptr, integer = C.c_void_p, C.c_int
        signatures = {
            'SDL_Init': ([C.c_uint32], integer), 'SDL_Quit': ([], None),
            'SDL_GetError': ([], C.c_char_p),
            'SDL_CreateWindow': ([C.c_char_p, integer, integer, integer, integer, C.c_uint32], ptr),
            'SDL_CreateRenderer': ([ptr, integer, C.c_uint32], ptr),
            'SDL_CreateTexture': ([ptr, C.c_uint32, integer, integer, integer], ptr),
            'SDL_CreateTextureFromSurface': ([ptr, ptr], ptr),
            'SDL_UpdateTexture': ([ptr, ptr, ptr, integer], integer),
            'SDL_RenderCopy': ([ptr, ptr, ptr, ptr], integer),
            'SDL_RenderClear': ([ptr], integer), 'SDL_RenderPresent': ([ptr], None),
            'SDL_PollEvent': ([ptr], integer), 'SDL_FreeSurface': ([ptr], None),
            'SDL_DestroyTexture': ([ptr], None), 'SDL_DestroyRenderer': ([ptr], None),
            'SDL_DestroyWindow': ([ptr], None),
        }
        for name, (args, result) in signatures.items():
            fn = getattr(self.sdl, name)
            fn.argtypes, fn.restype = args, result
        self.img.IMG_Load.argtypes, self.img.IMG_Load.restype = [C.c_char_p], ptr
        self.window = self.renderer = self.texture = None
        self.thumbnails = {}
        try:
            if self.sdl.SDL_Init(0x20) != 0:
                raise RuntimeError(self.sdl.SDL_GetError().decode())
            self.window = self.sdl.SDL_CreateWindow(b'YouTube', 0, 0, W, H, 1)
            if self.window:
                self.renderer = self.sdl.SDL_CreateRenderer(self.window, -1, 2)
            if not self.renderer:
                raise RuntimeError(self.sdl.SDL_GetError().decode())
            self.texture = self.sdl.SDL_CreateTexture(self.renderer, 0x16362004, 0, W, H)
            if not self.texture:
                raise RuntimeError(self.sdl.SDL_GetError().decode())
        except Exception:
            self.close()
            raise

    def draw(self, canvas, images):
        for key in list(self.thumbnails):
            if not Path(key).exists():
                self.sdl.SDL_DestroyTexture(self.thumbnails.pop(key))
        pixels = (C.c_ubyte * len(canvas.pixels)).from_buffer(canvas.pixels)
        if self.sdl.SDL_UpdateTexture(self.texture, None, pixels, W * 4) != 0:
            raise RuntimeError(self.sdl.SDL_GetError().decode())
        self.sdl.SDL_RenderClear(self.renderer)
        self.sdl.SDL_RenderCopy(self.renderer, self.texture, None, None)
        for file, rect in images:
            key = str(file)
            if key not in self.thumbnails and file.exists():
                surface = self.img.IMG_Load(os.fsencode(file))
                if surface:
                    self.thumbnails[key] = self.sdl.SDL_CreateTextureFromSurface(self.renderer, surface)
                    self.sdl.SDL_FreeSurface(surface)
            texture = self.thumbnails.get(key)
            if texture:
                self.sdl.SDL_RenderCopy(self.renderer, texture, None, C.byref(Rect(*rect)))
        self.sdl.SDL_RenderPresent(self.renderer)

    def quit_requested(self):
        event = C.create_string_buffer(56)
        while self.sdl.SDL_PollEvent(event):
            kind = struct.unpack_from('I', event)[0]
            if kind == 0x100 or (kind == 0x300 and struct.unpack_from('i', event, 20)[0] == 27):
                return True
        return False

    def close(self):
        for texture in self.thumbnails.values():
            if texture:
                self.sdl.SDL_DestroyTexture(texture)
        self.thumbnails.clear()
        if self.texture:
            self.sdl.SDL_DestroyTexture(self.texture)
        if self.renderer:
            self.sdl.SDL_DestroyRenderer(self.renderer)
        if self.window:
            self.sdl.SDL_DestroyWindow(self.window)
        self.texture = self.renderer = self.window = None
        self.sdl.SDL_Quit()


class Controller:
    def __init__(self):
        self.inputs, self.held = {}, {}
        self.next_scan = 0
        self.event = struct.Struct('@llHHi')

    def close(self):
        for fd in self.inputs.values():
            os.close(fd)
        self.inputs.clear()
        self.held.clear()

    def events(self):
        now = time.monotonic()
        if now >= self.next_scan:
            for node, fd in list(self.inputs.items()):
                if not Path(node).exists():
                    os.close(fd)
                    del self.inputs[node]
            for node in glob.glob('/dev/input/event*'):
                if node in self.inputs:
                    continue
                try:
                    name = Path('/sys/class/input', Path(node).name, 'device/name').read_text()
                    if 'OSH PB Controller' in name:
                        self.inputs[node] = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
                except OSError:
                    pass
            self.next_scan = now + 2
        actions = []
        for node, fd in list(self.inputs.items()):
            try:
                data = os.read(fd, self.event.size * 64)
            except BlockingIOError:
                continue
            except OSError:
                data = b''
            if not data:
                os.close(fd)
                del self.inputs[node]
                self.held.clear()
                continue
            for offset in range(0, len(data), self.event.size):
                _, _, kind, code, value = self.event.unpack_from(data, offset)
                if kind == 1 and value == 1:
                    # OSH driver button order matches the device's SDL mapping:
                    # a:b0,b:b1,x:b2,y:b3,L1:b4,R1:b5,start:b6,back:b7.
                    action = {304: 'a', 305: 'b', 306: 'x', 307: 'y', 309: 'save',
                              308: 'clear', 311: 'exit', 310: 'start'}.get(code)
                    if action:
                        actions.append(action)
                elif kind == 3 and code in (16, 17):
                    if value:
                        action = ('left' if value < 0 else 'right') if code == 16 else (
                            'up' if value < 0 else 'down')
                        actions.append(action)
                        self.held[code] = (action, now + .35)
                    else:
                        self.held.pop(code, None)
        for code, (action, deadline) in list(self.held.items()):
            if now >= deadline:
                actions.append(action)
                self.held[code] = (action, now + .12)
        return actions


class Job:
    def __init__(self, root, kind, value, messages):
        self.root, self.kind, self.value, self.messages = root, kind, value, messages
        self.cancelled, self.process = threading.Event(), None
        self.started = time.monotonic()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def cancel(self):
        self.cancelled.set()
        if self.process and self.process.poll() is None:
            try:
                os.killpg(self.process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def run(self):
        try:
            if self.kind == 'resolve':
                cached = cached_stream(self.root, self.value)
                if cached:
                    self.messages.put((self, 'resolved', cached))
                    return
            command = [sys.executable, str(self.root / 'yt-dlp'), '--no-playlist',
                       '--no-warnings', '--socket-timeout', '15', '--retries', '1',
                       '--js-runtimes', 'quickjs:' + str(self.root / 'qjs'), '--dump-single-json']
            if self.kind == 'search':
                command += ['--flat-playlist', '--', 'ytsearch12:' + self.value]
            else:
                if not VIDEO_ID.fullmatch(self.value):
                    raise ValueError('Invalid video ID')
                command += ['-f', FORMAT, '--', 'https://www.youtube.com/watch?v=' + self.value]
            self.process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                            start_new_session=True)
            if self.cancelled.is_set():
                self.cancel()
            try:
                output, errors = self.process.communicate(timeout=120)
            except subprocess.TimeoutExpired:
                self.cancel()
                self.process.communicate()
                raise RuntimeError('Connection timed out. Try again.')
            if self.cancelled.is_set():
                self.messages.put((self, 'cancelled', None))
                return
            (self.root / 'extract.log').write_bytes(errors)
            if self.process.returncode:
                lines = errors.decode(errors='replace').splitlines()
                error = next((line.split(': ', 1)[-1] for line in lines if line.startswith('ERROR:')),
                             'Could not load videos. Check Wi-Fi and try again.')
                raise RuntimeError(error)
            data = json.loads(output)
            if self.kind == 'resolve':
                manifest = self.root / 'stream.json'
                write_json(manifest, data)
                self.messages.put((self, 'resolved', manifest))
                return
            rows = video_rows(data)
            write_json(self.root / 'search.json', {'query': self.value, 'entries': rows})
            self.messages.put((self, 'results', rows))
            cache = self.root / 'thumbnails'
            cache.mkdir(exist_ok=True)
            keep = {row['id'] + '.jpg' for row in rows}
            saved = read_json(self.root / 'saved.json', [])
            if isinstance(saved, list):
                keep.update(row['id'] + '.jpg' for row in saved if isinstance(row, dict)
                            and VIDEO_ID.fullmatch(str(row.get('id', ''))))
            for file in cache.glob('*.jpg'):
                if file.name not in keep:
                    file.unlink(missing_ok=True)
            for row in rows:
                if self.cancelled.is_set():
                    break
                file = cache / (row['id'] + '.jpg')
                if file.exists():
                    continue
                try:
                    with urllib.request.urlopen('https://i.ytimg.com/vi/' + row['id'] + '/mqdefault.jpg',
                                                timeout=6) as response:
                        image = response.read(512 * 1024 + 1)
                    if len(image) <= 512 * 1024 and not self.cancelled.is_set():
                        file.write_bytes(image)
                        self.messages.put((self, 'image', None))
                except OSError:
                    pass  # A missing thumbnail must never prevent playing.
        except Exception as error:
            self.messages.put((self, 'error', str(error)))


class Browser:
    def __init__(self, root, font):
        self.root, self.canvas = root, Canvas(font)
        cached = read_json(root / 'search.json', {})
        if not isinstance(cached, dict):
            cached = {}
        self.query = cached.get('query', '')
        entries = cached.get('entries') if isinstance(cached.get('entries'), list) else []
        self.results = [row for row in entries if isinstance(row, dict)
                        and VIDEO_ID.fullmatch(str(row.get('id', '')))]
        saved = read_json(root / 'saved.json', [])
        if not isinstance(saved, list):
            saved = []
        self.saved = [row for row in saved if isinstance(row, dict) and VIDEO_ID.fullmatch(str(row.get('id', '')))]
        self.library, self.keyboard, self.busy, self.running = False, False, False, True
        self.selected, self.key, self.draft = 0, 0, self.query
        self.status, self.job = '', None
        self.messages = queue.Queue()
        self.display, self.controller, self.player = None, None, None
        self.dirty = True
        self.last_busy_second = -1

    @property
    def rows(self):
        return self.saved if self.library else self.results

    def start_job(self, kind, value):
        if self.job:
            self.job.cancel()
        self.busy, self.status = True, 'Finding videos...' if kind == 'search' else 'Opening video...'
        self.job = Job(self.root, kind, value, self.messages)
        self.dirty = True

    def stop(self):
        self.running = False
        if self.job:
            self.job.cancel()
        if self.player and self.player.poll() is None:
            try:
                os.killpg(self.player.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass

    def action(self, action):
        if os.environ.get('YOUTUBE_DEBUG_INPUT'):
            print(f'input {action}: keyboard={self.keyboard} busy={self.busy} rows={len(self.rows)}', flush=True)
        self.dirty = True
        if action == 'exit':
            self.running = False
            return
        if self.busy:
            if action == 'b':
                self.job.cancel()
                self.status = 'Cancelling...'
            return
        if self.keyboard:
            moves = {'up': -6, 'down': 6, 'left': -1, 'right': 1}
            if action in moves:
                self.key = (self.key + moves[action]) % len(KEYS)
            elif action == 'a' and len(self.draft) < 100:
                self.draft += KEYS[self.key]
            elif action == 'x':
                self.draft = self.draft[:-1]
            elif action == 'clear':
                self.draft = ''
            elif action == 'y' and len(self.draft) < 100:
                self.draft += ' '
            elif action == 'b':
                self.keyboard = False
            elif action == 'start' and self.draft.strip():
                self.keyboard, self.library = False, False
                self.query = self.draft.strip()
                self.start_job('search', self.query)
            return
        if action in ('x', 'start'):
            self.keyboard, self.draft = True, self.query
        elif action == 'b':
            self.running = False
        elif action == 'y':
            self.library = not self.library
            self.selected, self.status = 0, ''
        elif action in ('up', 'down', 'left', 'right') and self.rows:
            delta = {'up': -1, 'down': 1, 'left': -3, 'right': 3}[action]
            self.selected = max(0, min(len(self.rows) - 1, self.selected + delta))
        elif action == 'a' and self.rows:
            self.start_job('resolve', self.rows[self.selected]['id'])
        elif action == 'save' and self.rows:
            row = self.rows[self.selected]
            if any(saved['id'] == row['id'] for saved in self.saved):
                self.saved = [saved for saved in self.saved if saved['id'] != row['id']]
                self.status = 'Removed from saved videos'
            else:
                self.saved.append(row.copy())
                self.status = 'Saved video'
            write_json(self.root / 'saved.json', self.saved)
            self.selected = max(0, min(self.selected, len(self.rows) - 1))

    def update(self):
        while True:
            try:
                job, kind, data = self.messages.get_nowait()
            except queue.Empty:
                break
            if job is not self.job:
                continue
            self.dirty = True
            if getattr(job, 'cancelled', None) and job.cancelled.is_set() and kind != 'cancelled':
                self.busy, self.status = False, 'Cancelled'
                continue
            if kind == 'image':
                continue
            self.busy = False
            if kind == 'results':
                print(f'search completed: {len(data)} results', flush=True)
                self.results, self.selected = data, 0
                self.status = '' if data else 'No videos found. Try another search.'
            elif kind == 'resolved':
                self.controller.close()
                self.display.close()
                self.display = None
                try:
                    print('starting playback', flush=True)
                    self.player = subprocess.Popen(
                        [sys.executable, str(self.root / 'youtube-player.py'), str(data)], start_new_session=True)
                    result = self.player.wait()
                    print(f'playback exited: {result}', flush=True)
                    self.status = '' if result == 0 else 'Playback failed. Try another video.'
                finally:
                    self.player = None
                    if self.running:
                        self.display = Display()
                        self.controller = Controller()
            elif kind == 'error':
                self.status = data
            elif kind == 'cancelled':
                self.status = 'Cancelled'

    def render(self):
        c, images = self.canvas, []
        c.fill(0, 0, W, H, BG)
        c.fill(16, 18, 38, 28, RED)
        for line in range(12):
            c.fill(30, 26 + line, 1 + min(line, 11 - line), 1, WHITE)
        c.text(68, 17, 'YouTube', WHITE, 2)
        c.text(488, 26, 'SAVED' if self.library else 'SEARCH', TEAL)
        c.fill(16, 64, 608, 42, PANEL)
        value = (self.draft + '_') if self.keyboard else self.query
        c.text(28, 77, (value or 'Search videos...')[-70:], WHITE if value else MUTED, max_width=568)
        if self.keyboard:
            c.text(20, 124, 'Type a search, then press START', MUTED)
            for index, char in enumerate(KEYS):
                x, y = 83 + index % 6 * 80, 158 + index // 6 * 43
                c.fill(x, y, 72, 36, TEAL if index == self.key else PANEL)
                c.text(x + 28, y + 2, char.upper(), BG if index == self.key else WHITE, 2)
            c.text(20, 446, 'A Type  X Delete  Y Space  L1 Clear  START Search  B Back', MUTED)
            return images
        c.text(18, 119, 'Saved videos' if self.library else ('Results' if self.rows else 'Browse YouTube'), TEAL)
        if self.rows:
            c.text(534, 119, f'{self.selected + 1}/{len(self.rows)}', MUTED)
        first = self.selected // 3 * 3
        for position, row in enumerate(self.rows[first:first + 3]):
            y, selected = 147 + position * 94, first + position == self.selected
            c.fill(12, y, 616, 88, SELECTED if selected else PANEL)
            if selected:
                c.fill(12, y, 4, 88, RED)
            c.fill(26, y + 8, 128, 72, BG)
            c.text(78, y + 34, '>', MUTED)
            images.append((self.root / 'thumbnails' / (row['id'] + '.jpg'), (26, y + 8, 128, 72)))
            for line, text in enumerate(c.lines(row['title'], 442, 2)):
                c.text(169, y + 8 + line * 19, text)
            c.text(169, y + 50, row.get('channel', ''), MUTED, max_width=350)
            duration = row.get('duration')
            duration = f'{int(duration) // 60}:{int(duration) % 60:02}' if duration else '--:--'
            c.text(169, y + 69, duration, MUTED)
            if any(saved['id'] == row['id'] for saved in self.saved):
                c.text(562, y + 69, 'SAVED', TEAL)
        if not self.rows:
            c.text(94, 216, 'Find something to watch', WHITE, 2)
            c.text(122, 264, 'X opens the gamepad keyboard', MUTED)
            c.text(122, 292, 'R1 saves a selected video', MUTED)
        if self.busy or self.status:
            c.fill(12, 354, 616, 82, SELECTED)
            for line, text in enumerate(c.lines(self.status, 568, 2)):
                c.text(28, 367 + 21 * line, text, TEAL if self.busy else WHITE)
            if self.busy:
                elapsed = int(time.monotonic() - self.job.started) if self.job else 0
                c.text(28, 413, f'{elapsed}s elapsed    B Cancel', MUTED)
        c.text(16, 450, 'A Play   X Search   Y Saved   R1 Save   B Exit', MUTED)
        return images

    def run(self):
        self.controller = Controller()
        try:
            self.display = Display()
            print('browser started', flush=True)
            if os.environ.get('YOUTUBE_QUERY'):
                self.query = os.environ['YOUTUBE_QUERY'][:100]
                self.start_job('search', self.query)
            while self.running:
                if self.display.quit_requested():
                    break
                for action in self.controller.events():
                    self.action(action)
                self.update()
                if not self.running:
                    break
                if self.busy and self.job:
                    second = int(time.monotonic() - self.job.started)
                    if second != self.last_busy_second:
                        self.dirty, self.last_busy_second = True, second
                if self.dirty:
                    self.display.draw(self.canvas, self.render())
                    self.dirty = False
                time.sleep(.03)
        finally:
            self.stop()
            self.controller.close()
            if self.display:
                self.display.close()
            print('browser stopped', flush=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--font', default='unifont.bin')
    parser.add_argument('--root', default='.')
    parser.add_argument('--preview', choices=['results', 'keyboard', 'empty', 'loading', 'error'])
    parser.add_argument('--dump', default='youtube.ppm')
    args = parser.parse_args()
    root = Path(args.root).resolve()
    browser = Browser(root, args.font)
    if args.preview:
        browser.query = 'big buck bunny'
        if args.preview != 'empty':
            browser.results = [
                {'id': 'aqz-KE-bpKQ', 'title': 'Big Buck Bunny - Official Blender Foundation Short Film',
                 'channel': 'Blender', 'duration': 634},
                {'id': 'YE7VzlLtp-4', 'title': 'Big Buck Bunny: Behind the scenes',
                 'channel': 'Blender Studio', 'duration': 312},
                {'id': 'jNQXAC9IVRw', 'title': 'A short film to watch on the go',
                 'channel': 'Videos', 'duration': 18},
            ]
        browser.saved = browser.results[:1]
        browser.keyboard, browser.draft = args.preview == 'keyboard', 'big buck bunny'
        browser.busy = args.preview == 'loading'
        browser.status = 'Opening video...' if browser.busy else (
            'Could not load videos. Check Wi-Fi and try again.' if args.preview == 'error' else '')
        images = browser.render()
        if Path(args.dump).suffix == '.png':
            from PIL import Image  # Host preview only; never needed on-device.
            rgb = bytearray(W * H * 3)
            pixels = browser.canvas.pixels
            rgb[0::3], rgb[1::3], rgb[2::3] = pixels[2::4], pixels[1::4], pixels[0::4]
            frame = Image.frombytes('RGB', (W, H), bytes(rgb))
            for file, (x, y, w, h) in images:
                if file.exists():
                    with Image.open(file) as source:
                        frame.paste(source.convert('RGB').resize((w, h)), (x, y))
            frame.save(args.dump)
        else:
            browser.canvas.ppm(args.dump)
    else:
        signal.signal(signal.SIGTERM, lambda *_: browser.stop())
        browser.run()


if __name__ == '__main__':
    main()

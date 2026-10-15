"""SDL2 framebuffer, Unifont and RG351V input, adapted from youtube-ui.py.

SPDX-License-Identifier: GPL-2.0-or-later
"""
import ctypes as C
import glob
import os
from pathlib import Path
import struct
import time
import zlib
import unicodedata

W, H = 640, 480
WHITE = 0xFFF0F3F5

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
        if unicodedata.category(char) == 'Cf':
            return 0
        cp = ord(char)
        return 16 if cp < 65536 and self.index[cp] & 0x80000000 else 8

    def text(self, x, y, text, color=WHITE, scale=1, max_width=W):
        end = min(W, x + max_width)
        for char in str(text):
            if unicodedata.category(char) == 'Cf':
                continue
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

    def png(self, file):
        rgb = bytearray(W * H * 3)
        rgb[0::3], rgb[1::3], rgb[2::3] = self.pixels[2::4], self.pixels[1::4], self.pixels[0::4]
        def chunk(kind, data):
            return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
        rows = b''.join(b'\0' + rgb[y * W * 3:(y + 1) * W * 3] for y in range(H))
        Path(file).write_bytes(b'\x89PNG\r\n\x1a\n' +
                              chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) +
                              chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


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
            'SDL_RenderSetClipRect': ([ptr, ptr], integer),
            'SDL_RenderReadPixels': ([ptr, ptr, C.c_uint32, ptr, integer], integer),
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
        self.capture = False
        try:
            if self.sdl.SDL_Init(0x20) != 0:
                raise RuntimeError(self.sdl.SDL_GetError().decode())
            self.window = self.sdl.SDL_CreateWindow(b'Anki', 0, 0, W, H, 1)
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

    def draw(self, canvas, images, clip=None):
        for key in list(self.thumbnails):
            if not Path(key).exists():
                self.sdl.SDL_DestroyTexture(self.thumbnails.pop(key))
        pixels = (C.c_ubyte * len(canvas.pixels)).from_buffer(canvas.pixels)
        if self.sdl.SDL_UpdateTexture(self.texture, None, pixels, W * 4) != 0:
            raise RuntimeError(self.sdl.SDL_GetError().decode())
        self.sdl.SDL_RenderClear(self.renderer)
        self.sdl.SDL_RenderCopy(self.renderer, self.texture, None, None)
        if clip:
            self.sdl.SDL_RenderSetClipRect(self.renderer, C.byref(Rect(*clip)))
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
        self.sdl.SDL_RenderSetClipRect(self.renderer, None)
        # Keep a bounded texture cache when reviewing many image cards.
        for key in list(self.thumbnails)[:-8]:
            self.sdl.SDL_DestroyTexture(self.thumbnails.pop(key))
        if self.capture:
            self.sdl.SDL_RenderReadPixels(self.renderer, None, 0x16362004, pixels, W * 4)
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
                    action = {304: 'a', 305: 'b', 306: 'x', 307: 'y', 309: 'audio',
                              308: 'undo', 311: 'back', 310: 'start'}.get(code)
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

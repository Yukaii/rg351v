#!/usr/bin/env python3
"""Short playback benchmark with evdev controls and mpv IPC; no dependencies."""
import glob
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import sys
import time


def main():
    media = str(Path(sys.argv[1]).resolve())
    extra = []
    if media.endswith('.json'):
        manifest = json.loads(Path(media).read_text())
        formats = manifest.get('requested_formats') or [manifest]
        video = next(f for f in formats if f.get('vcodec') != 'none')
        media = video['url']
        for track in formats:
            if track.get('vcodec') == 'none':
                extra.append('--audio-file=' + track['url'])
        extra.append('--force-media-title=' + manifest.get('title', 'YouTube'))
    runtime = Path('/run/rg351v')
    runtime.mkdir(exist_ok=True)
    # One player at a time; the lock is released automatically after a crash.
    import fcntl
    lock = (runtime / 'youtube.lock').open('w')
    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    ipc = str(runtime / 'youtube.sock')
    playing = runtime / 'video-playing.json'
    playing.unlink(missing_ok=True)
    Path(ipc).unlink(missing_ok=True)
    inputs = {}

    def rescan_inputs():
        for node, fd in list(inputs.items()):
            if not Path(node).exists():
                os.close(fd)
                del inputs[node]
        for node in glob.glob('/dev/input/event*'):
            if node in inputs:
                continue
            try:
                name = Path('/sys/class/input', Path(node).name, 'device/name')
                if 'OSH PB Controller' in name.read_text():
                    inputs[node] = os.open(node, os.O_RDONLY | os.O_NONBLOCK)
            except OSError:
                pass  # Controller is re-enumerating after suspend.

    rescan_inputs()
    player = subprocess.Popen([
        'mpv', '--no-config', '--vo=sdl', '--hwdec=no', '--ao=alsa',
        '--audio-device=alsa/default', '--input-gamepad=no', '--fullscreen',
        '--input-default-bindings=no', '--input-ipc-server=' + ipc,
        '--quiet', '--msg-level=all=warn', *extra, media,
    ], env={**os.environ, 'SDL_VIDEODRIVER': 'kmsdrm'})
    signal.signal(signal.SIGTERM, lambda *_: player.terminate())
    sock = socket.socket(socket.AF_UNIX)
    sock.settimeout(2)
    try:
        for _ in range(100):
            if player.poll() is not None:
                raise RuntimeError('mpv exited before opening IPC')
            try:
                sock.connect(ipc)
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.05)
        else:
            raise RuntimeError('mpv IPC timeout')
        reader = sock.makefile('rb')
        request = 0

        def command(*args):
            nonlocal request
            request += 1
            sock.sendall((json.dumps({'command': args, 'request_id': request}) + '\n').encode())
            while True:
                line = reader.readline()
                if not line:
                    raise EOFError('mpv IPC closed')
                reply = json.loads(line)
                if reply.get('request_id') == request:
                    return reply.get('data')

        # Linux input_event uses native long timestamps on this aarch64 build.
        event = struct.Struct('@llHHi')
        next_sample = time.monotonic() + 5
        cpu_time = time.monotonic()
        cpu_ticks = 0
        next_heartbeat = 0
        next_scan = 0
        start_ticks = Path('/proc', str(player.pid), 'stat').read_text().split(') ')[1].split()[19]
        print('A/Start: pause; B/Select: exit; D-pad: seek/volume', flush=True)
        command('show-text', 'A/Start: pause | B/Select: exit | D-pad: seek/volume', 5000)
        while player.poll() is None:
            if time.monotonic() >= next_scan:
                rescan_inputs()
                next_scan = time.monotonic() + 2
            ready, _, _ = select.select(list(inputs.values()), [], [], 0.1)
            for fd in ready:
                try:
                    data = os.read(fd, event.size * 64)
                except OSError:
                    data = b''
                if not data:
                    node = next(node for node, current in inputs.items() if current == fd)
                    os.close(fd)
                    del inputs[node]
                    continue
                for offset in range(0, len(data), event.size):
                    _, _, kind, code, value = event.unpack_from(data, offset)
                    if kind == 1 and value == 1:
                        if code in (304, 310):
                            command('cycle', 'pause')
                        elif code in (305, 311):
                            command('quit')
                            return player.wait()
                    elif kind == 3 and value != 0:
                        if code == 16:
                            command('seek', 10 * value, 'relative')
                        elif code == 17:
                            command('add', 'volume', -5 * value)
            if time.monotonic() >= next_heartbeat:
                if command('get_property', 'pause'):
                    playing.unlink(missing_ok=True)
                else:
                    heartbeat = {'pid': player.pid, 'start_ticks': start_ticks,
                                 'updated': time.monotonic()}
                    temporary = playing.with_suffix('.tmp')
                    temporary.write_text(json.dumps(heartbeat))
                    temporary.replace(playing)
                next_heartbeat = time.monotonic() + 1
            if time.monotonic() >= next_sample:
                stats = {key: command('get_property', key) for key in (
                    'time-pos', 'pause', 'frame-drop-count',
                    'decoder-frame-drop-count', 'avsync')}
                fields = Path('/proc', str(player.pid), 'stat').read_text().split(') ')[1].split()
                ticks = int(fields[11]) + int(fields[12])
                now = time.monotonic()
                stats['cpu_percent_one_core'] = round(
                    100 * (ticks - cpu_ticks) / os.sysconf('SC_CLK_TCK') / (now - cpu_time), 1)
                cpu_ticks, cpu_time = ticks, now
                for key, file in (
                    ('current_uA', '/sys/class/power_supply/battery/current_now'),
                    ('temperature_mC', '/sys/class/thermal/thermal_zone0/temp'),
                ):
                    stats[key] = int(Path(file).read_text())
                print(json.dumps(stats), flush=True)
                next_sample = time.monotonic() + 5
        return player.returncode
    except (EOFError, BrokenPipeError, ConnectionResetError):
        return player.wait()
    finally:
        if player.poll() is None:
            player.terminate()
            try:
                player.wait(timeout=5)
            except subprocess.TimeoutExpired:
                player.kill()
                player.wait()
        sock.close()
        for fd in inputs.values():
            os.close(fd)
        Path(ipc).unlink(missing_ok=True)
        playing.unlink(missing_ok=True)


if __name__ == '__main__':
    sys.exit(main())

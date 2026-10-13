#!/usr/bin/env python3
"""Suspend the RG351V after a period without input.

AmberELEC has no auto-suspend: ES only draws a black screensaver, which keeps
the SoC, Wi-Fi and backlight running (~450 mA). This daemon watches the input
devices and runs `systemctl suspend` (same path as the power key, so the
stock sleep hook runs) once nothing has been pressed for IDLE_MINUTES.

It does not suspend while:
  * the device is charging (SKIP_WHEN_CHARGING=1),
  * an SSH session is open (SKIP_WHEN_REMOTE=1),
  * Wi-Fi is moving data faster than BUSY_KBPS (a copy or download),
  * music is playing: deckd keeps /run/rg351v/playing while a
    track plays (a stop within the last 45 s still counts, to bridge track
    changes), and only while deckd (or librespot) is running.

Config (re-read every loop, so `autosleep.sh set` needs no restart):
  /storage/.config/rg351v/autosleep.conf   KEY=value lines
    IDLE_MINUTES=5        0 disables suspending
    SKIP_WHEN_CHARGING=1
    SKIP_WHEN_REMOTE=1
    BUSY_KBPS=20

Runs under systemd (rg351v-autosleep.service); output goes to the journal.
`--once` exits after the first suspend/resume cycle (for testing); the
AUTOSLEEP_CONF environment variable overrides the config path.
"""
import errno
import fcntl
import glob
import os
import select
import struct
import subprocess
import sys
import time

CONF = os.environ.get("AUTOSLEEP_CONF", "/storage/.config/rg351v/autosleep.conf")
DEFAULTS = {
    "IDLE_MINUTES": 5,
    "SKIP_WHEN_CHARGING": 1,
    "SKIP_WHEN_REMOTE": 1,
    "BUSY_KBPS": 20,
}
TICK = 5.0  # seconds between idle checks
RATE_WINDOW = 60.0  # seconds of Wi-Fi counters kept for the busy check

EV_KEY, EV_ABS = 1, 3
EVENT = struct.Struct("llHHi")  # struct input_event on 64-bit Linux
SKIP_NAMES = ("Headphones",)  # jack insertion is not a button press


def load_conf():
    conf = dict(DEFAULTS)
    try:
        with open(CONF) as f:
            for line in f:
                line = line.split("#", 1)[0].strip()
                key, sep, val = line.partition("=")
                if sep and key.strip() in conf:
                    try:
                        conf[key.strip()] = int(val.strip())
                    except ValueError:
                        pass
    except OSError:
        pass
    return conf


def read_text(path, default=""):
    try:
        with open(path) as f:
            return f.read().strip()
    except OSError:
        return default


def abs_threshold(fd, code):
    """A tenth of the axis range: stick jitter is not activity."""
    req = (2 << 30) | (24 << 16) | (ord("E") << 8) | (0x40 + code)
    try:
        buf = fcntl.ioctl(fd, req, b"\0" * 24)
        _, lo, hi, _, _, _ = struct.unpack("6i", buf)
        return max(2, (hi - lo) // 10)
    except OSError:
        return 16


class Watcher:
    def __init__(self):
        self.fds = {}  # fd -> path
        self.abs_last = {}  # (fd, code) -> last value
        self.abs_thr = {}  # (fd, code) -> threshold

    def rescan(self):
        paths = set(glob.glob("/dev/input/event*"))
        for fd, path in list(self.fds.items()):
            if path not in paths:
                self.drop(fd)
        have = set(self.fds.values())
        for path in paths - have:
            name = read_text("/sys/class/input/%s/device/name" % os.path.basename(path))
            if any(s in name for s in SKIP_NAMES):
                continue
            try:
                fd = os.open(path, os.O_RDONLY | os.O_NONBLOCK)
            except OSError:
                continue
            self.fds[fd] = path
            log("watching %s (%s)" % (path, name))

    def drop(self, fd):
        path = self.fds.pop(fd, None)
        try:
            os.close(fd)
        except OSError:
            pass
        for k in [k for k in self.abs_last if k[0] == fd]:
            del self.abs_last[k]
            self.abs_thr.pop(k, None)
        if path:
            log("lost %s" % path)

    def wait(self, timeout):
        """Block up to timeout; return True if any real input arrived."""
        if not self.fds:
            time.sleep(timeout)
            return False
        active = False
        ready, _, _ = select.select(list(self.fds), [], [], timeout)
        for fd in ready:
            try:
                data = os.read(fd, EVENT.size * 64)
            except OSError as e:
                if e.errno not in (errno.EAGAIN, errno.EINTR):
                    self.drop(fd)
                continue
            if not data:
                self.drop(fd)
                continue
            for off in range(0, len(data) - EVENT.size + 1, EVENT.size):
                _, _, etype, code, value = EVENT.unpack_from(data, off)
                if etype == EV_KEY:
                    active = True
                elif etype == EV_ABS:
                    key = (fd, code)
                    if key not in self.abs_thr:
                        self.abs_thr[key] = abs_threshold(fd, code)
                        self.abs_last[key] = value
                    elif abs(value - self.abs_last[key]) >= self.abs_thr[key]:
                        self.abs_last[key] = value
                        active = True
        return active


def charging():
    for name in ("ac", "usb"):
        if read_text("/sys/class/power_supply/%s/online" % name) == "1":
            return True
    return read_text("/sys/class/power_supply/battery/status") in ("Charging", "Full")


def ssh_open():
    """True if any TCP connection to local port 22 is established."""
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            with open(table) as f:
                next(f)
                for line in f:
                    parts = line.split()
                    if parts[3] == "01" and parts[1].endswith(":0016"):
                        return True
        except (OSError, StopIteration):
            pass
    return False


PLAYING_FLAG = "/run/rg351v/playing"
STOPPED_FLAG = "/run/rg351v/stopped"
STOP_GRACE = 45.0  # seconds a stop still counts as playing (track changes)


def music_playing():
    if not os.path.exists(PLAYING_FLAG):
        try:
            recent = time.time() - os.stat(STOPPED_FLAG).st_mtime < STOP_GRACE
        except OSError:
            recent = False
        if not recent:
            return False
    for comm in glob.glob("/proc/[0-9]*/comm"):
        if read_text(comm) in ("deckd", "librespot"):
            return True
    return False


def wifi_bytes():
    total = 0
    for name in ("rx_bytes", "tx_bytes"):
        try:
            total += int(read_text("/sys/class/net/wlan0/statistics/" + name, "0"))
        except ValueError:
            pass
    return total


def boot_minus_mono():
    """Grows by the time spent suspended."""
    return time.clock_gettime(time.CLOCK_BOOTTIME) - time.monotonic()


def log(msg):
    print(msg, flush=True)


def main():
    w = Watcher()
    w.rescan()
    last_input = time.monotonic()
    slept_base = boot_minus_mono()
    samples = []  # (monotonic, wifi bytes)
    last_scan = 0.0
    log("autosleep started, config %s" % load_conf())
    while True:
        now = time.monotonic()
        if now - last_scan >= 10:
            w.rescan()
            last_scan = now
        if w.wait(TICK):
            last_input = time.monotonic()

        # Anything that suspended us (power key included) counts as activity:
        # the user just woke the device.
        base = boot_minus_mono()
        if base - slept_base > 2.0:
            log("resumed after %.0f s asleep" % (base - slept_base))
            last_input = time.monotonic()
            samples.clear()
        slept_base = base

        now = time.monotonic()
        samples.append((now, wifi_bytes()))
        while samples and now - samples[0][0] > RATE_WINDOW:
            samples.pop(0)

        conf = load_conf()
        limit = conf["IDLE_MINUTES"] * 60
        if limit <= 0 or now - last_input < limit:
            continue
        reason = None
        if conf["SKIP_WHEN_CHARGING"] and charging():
            reason = "charging"
        elif conf["SKIP_WHEN_REMOTE"] and ssh_open():
            reason = "ssh session"
        elif music_playing():
            reason = "music playing"
        elif len(samples) >= 2:
            dt = samples[-1][0] - samples[0][0]
            kbps = (samples[-1][1] - samples[0][1]) / 1024.0 / max(dt, 1.0)
            if kbps > conf["BUSY_KBPS"]:
                reason = "network busy (%.0f KB/s)" % kbps
        if reason:
            continue
        log("idle %.0f s, suspending" % (now - last_input))
        subprocess.call(["systemctl", "suspend"])
        # systemctl returns once the system has resumed.
        if "--once" in sys.argv:
            log("resumed, exiting (--once)")
            return
        last_input = time.monotonic()
        slept_base = boot_minus_mono()
        samples.clear()


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)

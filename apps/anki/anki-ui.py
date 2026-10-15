#!/usr/bin/env python3
"""Anki for the RG351V: an offline gamepad reviewer with upstream scheduling."""
import argparse
import json
import os
from pathlib import Path
import queue
import signal
import subprocess
import tempfile
import time

from card_document import layout, media_path
from console import Canvas, Controller, Display, W, H
from review_backend import Worker

BG, PANEL, PAPER = 0xFF101B23, 0xFF1C2B35, 0xFFF2EEE5
WHITE, MUTED, INK = 0xFFF4F2EA, 0xFF9CADB7, 0xFF263640
TEAL, BLUE, RED, GOLD = 0xFF76E0CB, 0xFF82B7F2, 0xFFFF9C91, 0xFFF2C56C
TOP, BOTTOM = 106, 382


class Reviewer:
    def __init__(self, args):
        self.args, self.canvas = args, Canvas(args.font)
        self.state = {"view": "loading"}
        self.worker = self.display = self.controller = self.audio = None
        self.busy, self.dirty, self.running, self.fatal = True, True, True, False
        self.selected, self.scroll = 0, 0
        self.items, self.content_height = [], 0
        self.message, self.sync_label = "Opening collection...", "LOCAL"
        self.revealed_at, self.last_refresh = 0, time.monotonic()
        self.capture_count = 0
        self.last_script = 0
        self.script_index = 0
        self.last_sleep = self.sleep_offset()

    @staticmethod
    def sleep_offset():
        try:
            return time.clock_gettime(time.CLOCK_BOOTTIME) - time.monotonic()
        except AttributeError:
            return 0

    def send(self, command, *args):
        if self.busy:
            return
        self.busy, self.dirty = True, True
        self.message = "Syncing... B cancels" if command == "sync" else ""
        self.worker.send(command, *args)

    def stop_audio(self):
        if self.audio and self.audio.poll() is None:
            self.audio.terminate()
            try:
                self.audio.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.audio.kill()
                self.audio.wait()
        self.audio = None

    def play_audio(self):
        self.stop_audio()
        files = [media_path(self.state.get("media", "."), name)
                 for name in self.state.get("audio", [])]
        files = [str(file) for file in files if file]
        if files and not self.args.no_audio:
            try:
                self.audio = subprocess.Popen(
                    ["mpv", "--no-video", "--no-config", "--ao=alsa", "--really-quiet", "--", *files],
                    stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except OSError:
                self.message, self.dirty = "Audio player unavailable", True

    def apply(self, state):
        previous = (self.state.get("card_id"), self.state.get("view"))
        self.state, self.busy, self.dirty = state, False, True
        self.message = ""
        view = state["view"]
        if view == "decks":
            self.selected = min(self.selected, max(0, len(state.get("decks", [])) - 1))
        if view in ("question", "answer"):
            self.items, self.content_height, anchor = layout(
                state["html"], self.canvas, state["media"])
            self.scroll = min(anchor if view == "answer" else 0,
                              max(0, self.content_height - (BOTTOM - TOP)))
            if view == "answer":
                self.revealed_at = time.monotonic()
            if previous != (state.get("card_id"), view):
                self.play_audio()
        else:
            self.stop_audio()
        print("view=" + view, flush=True)

    def action(self, action):
        self.dirty = True
        if self.busy:
            if action in ("b", "back") and self.worker:
                self.worker.cancel()
                self.message = "Cancelling..."
            return
        if self.fatal:
            if action in ("b", "back", "a"):
                self.running = False
            return
        view = self.state["view"]
        if action == "back" or (action == "b" and view != "answer"):
            if view == "decks":
                self.running = False
            else:
                self.stop_audio()
                self.send("decks")
        elif action == "undo" and self.state.get("can_undo"):
            self.stop_audio()
            self.send("undo")
        elif view == "decks":
            rows = self.state.get("decks", [])
            if rows and action in ("up", "down", "left", "right"):
                step = {"up": -1, "down": 1, "left": -5, "right": 5}[action]
                self.selected = max(0, min(len(rows) - 1, self.selected + step))
            elif action == "a" and rows:
                self.send("choose", rows[self.selected]["id"])
            elif action == "start":
                self.sync_label = "SYNCING"
                self.send("sync")
        elif view in ("question", "answer"):
            if action in ("up", "down", "left", "right"):
                step = {"up": -38, "down": 38, "left": -228, "right": 228}[action]
                self.scroll = max(0, min(self.content_height - (BOTTOM - TOP), self.scroll + step))
            elif action == "audio":
                self.play_audio()
            elif view == "question" and action == "a":
                self.send("flip", self.state["card_id"])
            elif view == "answer" and action in ("x", "b", "a", "y"):
                # Require a separate deliberate press after revealing the answer.
                if time.monotonic() - self.revealed_at >= .35:
                    self.stop_audio()
                    self.send("rate", self.state["card_id"], {"x": 1, "b": 2, "a": 3, "y": 4}[action])

    def poll(self):
        while True:
            try:
                kind, value = self.worker.replies.get_nowait()
            except queue.Empty:
                break
            if kind == "state":
                if self.sync_label == "SYNCING":
                    self.sync_label = "SYNC " + time.strftime("%H:%M")
                self.apply(value)
            elif kind == "progress":
                self.message, self.dirty = value, True
            else:
                self.busy, self.dirty = False, True
                self.message = value
                if self.sync_label == "SYNCING":
                    self.sync_label = "LOCAL"
                if kind == "fatal":
                    self.fatal = True
                    self.state = {"view": "error"}
        now = time.monotonic()
        if not self.busy and self.state["view"] == "done" and now - self.last_refresh > 15:
            self.last_refresh = now
            self.send("next_card")
        offset = self.sleep_offset()
        if offset - self.last_sleep > 2 and not self.busy:
            self.last_sleep = offset
            if self.state["view"] in ("question", "answer", "done"):
                self.send("next_card")
            else:
                self.send("decks")
        if self.args.control and not self.busy and now - self.last_script > .5:
            file = Path(self.args.control)
            if file.exists():
                actions = json.loads(file.read_text())
                if self.script_index < len(actions):
                    action = actions[self.script_index]
                    self.script_index += 1
                    self.last_script = now
                    if action == "quit":
                        self.running = False
                    else:
                        self.action(action)

    def fit(self, value, width, scale=1):
        result, size = "", 0
        for char in str(value):
            advance = self.canvas.width(char) * scale
            if size + advance > width:
                return result[:-2] + ".."
            result, size = result + char, size + advance
        return result

    def render(self):
        c, images = self.canvas, []
        c.fill(0, 0, W, H, BG)
        c.fill(20, 20, 24, 29, TEAL)
        c.fill(24, 24, 16, 21, BG)
        c.fill(27, 27, 10, 2, TEAL)
        c.text(58, 20, "ANKI", WHITE, 2)
        c.text(148, 29, "HANDHELD", MUTED)
        label = "DEMO / LOCAL" if self.args.demo else self.sync_label
        c.text(620 - len(label) * 8, 29, label, TEAL)
        c.fill(20, 63, 600, 1, PANEL)
        view = self.state["view"]
        if view == "decks":
            rows = self.state.get("decks", [])
            c.text(20, 80, "YOUR DECKS", MUTED)
            total = f"{self.state.get('total', 0):,} cards"
            c.text(620 - len(total) * 8, 80, total, MUTED)
            first = self.selected // 5 * 5
            for index, row in enumerate(rows[first:first + 5], first):
                y, selected = 108 + (index - first) * 60, index == self.selected
                c.fill(16, y, 608, 54, PAPER if selected else PANEL)
                if selected:
                    c.fill(16, y, 4, 54, TEAL)
                left = 30 + min(row.get("level", 0), 3) * 14
                c.text(left, y + 7, self.fit(row["name"], 390 - (left - 30)), INK if selected else WHITE)
                c.text(left, y + 31, f"{row['total']:,} cards", 0xFF64737A if selected else MUTED)
                for x, key, color in ((445, "new", BLUE), (505, "learn", RED), (565, "review", TEAL)):
                    c.text(x, y + 18, str(row[key]), INK if selected else color)
            if not rows:
                c.text(58, 200, "No cards on this device yet.", WHITE, 2)
                c.text(58, 246, "START syncs your AnkiWeb collection.", MUTED)
            c.text(20, 423, "D-pad Choose   A Study   START Sync", MUTED)
            c.text(20, 450, "Blue New   Red Learn   Green Review     SELECT Exit", MUTED)
        elif view in ("question", "answer"):
            c.text(20, 80, self.fit(self.state.get("deck", ""), 480), MUTED)
            c.text(548, 80, "ANSWER" if view == "answer" else "FRONT", TEAL)
            c.fill(16, 101, 608, 291, PAPER)
            for kind, x, y, value, detail in self.items:
                y = TOP + y - self.scroll
                if kind == "image":
                    if y + value[1] > TOP and y < BOTTOM:
                        images.append((detail, (36 + x, y, *value)))
                elif TOP <= y and y + (32 if kind == "glyph" else 16) <= BOTTOM:
                    if kind == "glyph":
                        c.text(36 + x, y, value, 0xFF147A70 if detail else INK, 2)
                        if detail:
                            c.text(37 + x, y, value, 0xFF147A70, 2)
                    elif kind == "rule":
                        c.fill(36, y, value, 1, 0xFFB4BEB9)
                    else:
                        c.text(36 + x, y, value, 0xFF64737A)
            if self.content_height > BOTTOM - TOP:
                track = BOTTOM - TOP
                thumb = max(18, int(track * track / self.content_height))
                pos = int((track - thumb) * self.scroll / max(1, self.content_height - track))
                c.fill(612, TOP, 3, track, 0xFFD9DCD4)
                c.fill(612, TOP + pos, 3, thumb, 0xFF658179)
            if view == "question":
                c.fill(16, 406, 608, 35, TEAL)
                c.text(228, 415, "A  SHOW ANSWER", BG)
            else:
                for index, (key, title, color) in enumerate((
                    ("X", "AGAIN", RED), ("B", "HARD", GOLD),
                    ("A", "GOOD", TEAL), ("Y", "EASY", BLUE))):
                    x = 16 + index * 154
                    c.fill(x, 402, 146, 43, PANEL)
                    c.text(x + 10, 407, key + " " + title, color)
                    intervals = self.state.get("intervals", ["", "", "", ""])
                    c.text(x + 10, 426, self.fit(intervals[index], 126), MUTED)
            c.text(20, 455, "D-pad Scroll   L1 Undo   R1 Audio   SELECT Decks", MUTED)
        elif view == "done":
            c.text(20, 80, self.fit(self.state.get("deck", ""), 590), MUTED)
            c.text(86, 190, "Nothing due right now.", WHITE, 2)
            c.text(86, 240, "Learning cards return when they are due.", MUTED)
            c.text(86, 270, "L1 undoes your last review.", TEAL)
            c.text(20, 450, "B / SELECT Back to decks", MUTED)
        elif view == "loading":
            c.text(116, 205, "Opening your collection", WHITE, 2)
            c.text(116, 253, "Reviews stay on this device until you sync.", MUTED)
        else:
            c.text(40, 154, "Could not open Anki", RED, 2)
            c.text(40, 193, "Your collection could not be opened.", MUTED)
            c.text(40, 450, "B / SELECT Exit", MUTED)
        if self.message and (view != "loading" or not self.busy):
            c.fill(16, 326, 608, 68, PANEL)
            for i, line in enumerate(c.lines(self.message, 576, 3)):
                c.text(30, 334 + 18 * i, line, TEAL if self.busy else RED)
            # Do not draw an image over an error/progress message.
            images = []
        return images

    def run(self):
        self.controller, self.display = Controller(), Display()
        self.display.capture = bool(self.args.capture)
        self.worker = Worker(self.args.data_dir, self.args.auth_file, self.args.demo)
        self.worker.start()
        started = time.monotonic()
        try:
            while self.running:
                if self.display.quit_requested():
                    break
                for action in self.controller.events():
                    self.action(action)
                self.poll()
                if self.args.run_seconds and time.monotonic() - started > self.args.run_seconds:
                    break
                if self.dirty:
                    images = self.render()
                    self.display.draw(self.canvas, images, (36, TOP, 568, BOTTOM - TOP))
                    if self.args.capture:
                        directory = Path(self.args.capture)
                        directory.mkdir(parents=True, exist_ok=True)
                        self.canvas.png(directory / f"{self.capture_count:03}-{self.state['view']}.png")
                        self.capture_count += 1
                    self.dirty = False
                time.sleep(.03)
        finally:
            self.stop_audio()
            self.worker.stop()
            self.worker.join(timeout=35)
            self.controller.close()
            self.display.close()


def preview(app, view, filename):
    app.busy = False
    app.message = ""
    app.state = {"view": "decks", "total": 248,
                 "decks": [{"id": i, "name": name, "level": level, "new": n,
                            "learn": l, "review": r, "total": total}
                           for i, (name, level, n, l, r, total) in enumerate((
                               ("Japanese", 0, 12, 3, 24, 182),
                               ("Everyday words / 日常語", 1, 8, 2, 18, 124),
                               ("Reading practice", 1, 4, 1, 6, 58),
                               ("Things worth remembering", 0, 4, 0, 9, 66)))]}
    if view in ("question", "answer"):
        app.state = {"view": view, "deck": "Japanese / Everyday words", "card_id": 1,
                     "media": ".", "audio": [], "intervals": ["<1m", "6m", "10m", "4d"],
                     "html": "<b>木漏れ日</b><br>こもれび" + (
                         '<hr id="answer">Sunlight filtering through the trees.<br><br>'
                         '木: tree　漏れる: to leak　日: sunlight' if view == "answer" else "")}
        app.items, app.content_height, _ = layout(app.state["html"], app.canvas, ".")
    elif view == "done":
        app.state = {"view": "done", "deck": "Japanese / Everyday words"}
    elif view == "error":
        app.state = {"view": "error"}
        app.message = "Anki is already open. Close its UI or terminal sync first."
    elif view == "sync":
        app.busy, app.sync_label = True, "SYNCING"
        app.message = "Syncing media... B cancels"
    else:
        app.message = ""
    app.render()
    app.canvas.png(filename)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--font", default="unifont.bin")
    parser.add_argument("--data-dir", default="/storage/roms/homebrew/anki-data")
    parser.add_argument("--auth-file", default="/storage/.config/rg351v/anki/auth.bin")
    parser.add_argument("--demo", action="store_true")
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--control", help="demo-only JSON action list for hardware checks")
    parser.add_argument("--capture", help="directory for rendered SDL frame captures")
    parser.add_argument("--run-seconds", type=int, default=0)
    parser.add_argument("--preview", choices=("decks", "question", "answer", "done", "sync", "error"))
    parser.add_argument("--dump", default="anki-preview.png")
    args = parser.parse_args()
    if args.control and not args.demo:
        parser.error("scripted actions are only allowed with --demo")
    if args.demo:
        args.data_dir = str(Path(args.data_dir).with_name("anki-demo"))
    app = Reviewer(args)
    if args.preview:
        preview(app, args.preview, args.dump)
        return
    signal.signal(signal.SIGTERM, lambda *_: setattr(app, "running", False))
    pidfile = Path("/run/rg351v/anki-ui.pid")
    pidfile.parent.mkdir(parents=True, exist_ok=True)
    pidfile.write_text(str(os.getpid()))
    try:
        app.run()
    finally:
        pidfile.unlink(missing_ok=True)


if __name__ == "__main__":
    main()

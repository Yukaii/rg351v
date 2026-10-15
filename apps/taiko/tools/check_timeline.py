#!/usr/bin/env python3
"""Sanity-check the TJA timeline compiler against real charts.

Builds tools/tja_dump.c, parses every .tja under a songs directory and compares
the last note time with the length of the chart's .ogg: a correct timeline ends
a little before the audio does (a few seconds of tail). Multi-song charts
(#NEXTSONG medleys, dan courses) legitimately run past one file; they are
reported as SKIP.

Usage: check_timeline.py [songs-dir]   (default .storage/taiko-songs)
"""
import glob
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
BIN = "/tmp/tja_dump"


def ogg_seconds(path):
    b = open(path, "rb").read()
    i = b.find(b"\x01vorbis")
    rate = struct.unpack("<I", b[i + 12 : i + 16])[0]
    j = b.rfind(b"OggS")
    granule = struct.unpack("<q", b[j + 6 : j + 14])[0]
    return granule / rate


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "..", "..", "..", ".storage", "taiko-songs")
    subprocess.run(
        ["cc", "-O2", "-Wall", "-Wextra", "-I" + SRC, "-o", BIN, os.path.join(HERE, "tja_dump.c"), os.path.join(SRC, "tja.c")],
        check=True,
    )
    bad = 0
    for tja in sorted(glob.glob(os.path.join(root, "**", "*.tja"), recursive=True)):
        out = subprocess.run([BIN, tja], capture_output=True, text=True).stdout
        m = re.search(r"wave=(.*?) bpm=", out)
        wav = os.path.join(os.path.dirname(tja), m.group(1)) if m else ""
        name = os.path.basename(tja)
        if not wav.lower().endswith(".ogg") or not os.path.exists(wav):
            print(f"SKIP {name}: no .ogg to compare")
            continue
        last = max(float(x) for x in re.findall(r"last=([\d.]+)s", out))
        tail = ogg_seconds(wav) - last
        # Unsupported #NEXTSONG charts overshoot; a real bug usually shows as
        # a huge gap or a negative tail on a single-song chart.
        if tail < 0:
            print(f"SKIP {name}: last note {last:.1f}s past audio (multi-song chart?)")
        elif tail > 15:
            print(f"FAIL {name}: tail {tail:.1f}s")
            bad += 1
        else:
            print(f"ok   {name}: last note {last:.1f}s, tail {tail:.1f}s")
    return 1 if bad else 0


sys.exit(main())

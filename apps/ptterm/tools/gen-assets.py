#!/usr/bin/env python3
"""Generate ptterm's runtime assets from public sources.

  unifont.bin   GNU Unifont BMP bitmaps, indexed by code point
  big5uao.bin   Big5-UAO (PTT's encoding) -> Unicode table

Inputs (download once, see gen-assets.sh):
  unifont_all-<ver>.hex.gz   https://ftp.gnu.org/gnu/unifont/
  uao250-b2u.txt             https://moztw.org/docs/big5/table/uao250-b2u.txt

unifont.bin layout (little endian):
  "UF16"  u32 nglyphs
  u32 index[65536]   0 = missing, else (offset into blob) | 0x80000000 if the
                     glyph is 16 px wide (otherwise 8 px); offsets are biased
                     by +1 so that offset 0 stays distinguishable
  blob               16 bytes per 8x16 glyph, 32 bytes per 16x16 glyph

big5uao.bin: uint16 LE table[126][157], row = lead - 0x81,
  column = trail - 0x40 for 0x40..0x7E, 63 + (trail - 0xA1) for 0xA1..0xFE.
  0 = unmapped.
"""
import gzip
import struct
import sys


def gen_unifont(hex_gz, out):
    index = [0] * 65536
    blob = bytearray()
    n = 0
    with gzip.open(hex_gz, "rt") as f:
        for line in f:
            cp_s, _, bits = line.strip().partition(":")
            cp = int(cp_s, 16)
            if cp > 0xFFFF:
                continue
            data = bytes.fromhex(bits)
            if len(data) == 16:
                wide = 0
            elif len(data) == 32:
                wide = 0x80000000
            else:
                continue
            index[cp] = (len(blob) + 1) | wide
            blob += data
            n += 1
    with open(out, "wb") as o:
        o.write(b"UF16" + struct.pack("<I", n))
        o.write(struct.pack("<65536I", *index))
        o.write(blob)
    print(f"{out}: {n} glyphs, {len(blob)} bytes of bitmap data")


def gen_big5(txt, out):
    table = [0] * (126 * 157)
    n = 0
    with open(txt) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            b, u = line.split()[:2]
            code, uni = int(b, 16), int(u, 16)
            lead, trail = code >> 8, code & 0xFF
            if not 0x81 <= lead <= 0xFE:
                continue
            if 0x40 <= trail <= 0x7E:
                col = trail - 0x40
            elif 0xA1 <= trail <= 0xFE:
                col = 63 + trail - 0xA1
            else:
                continue
            if uni > 0xFFFF:
                continue  # PTT has a handful of non-BMP; Unifont BMP-only
            table[(lead - 0x81) * 157 + col] = uni
            n += 1
    with open(out, "wb") as o:
        o.write(struct.pack("<%dH" % len(table), *table))
    print(f"{out}: {n} Big5-UAO mappings")


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit("usage: gen-assets.py <unifont.hex.gz> <uao250-b2u.txt> "
                 "<out unifont.bin> <out big5uao.bin>")
    gen_unifont(sys.argv[1], sys.argv[3])
    gen_big5(sys.argv[2], sys.argv[4])

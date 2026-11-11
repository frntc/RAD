#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
hondani.py - the HONDANI logo, for both screens.

games/quake/hondani.art is the C64 hires original (the Doom port's ilogo6.art):
two bytes of load address, 8000 bytes of bitmap, 1000 of screen. Every
cell's high nibble colours its set bits and the low nibble its clear ones.
The logo is red (2) on black, plus two brown (9) pixels that are red too.

Both screens show the same crop -- cells 2..38 by 6..17 -- at the same
place, so the light sweep runs across both in step:

  * C64: cell rows 6..17 at full width -- one contiguous span of bitmap and
    one of screen -- RLE-packed by `main` into a binary the game includes
    and unpacks straight into VIC bank 3. The sweep recolours every
    non-zero screen nibble, so it needs no copy of the original.
  * HDMI: a 296x96 picture in the level file (quake_pics.py) whose logo
    pixels are palette index LOGO_BASE + the C64 cell column (2..38) and
    whose background is index 0. The game sweeps it by loading 40 palette
    entries a frame -- the same ramp, column for column.

Usage: tools/hondani.py OUT.bin     (RLE bitmap span, then RLE screen span)
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ART = os.path.join(HERE, '..', 'games', 'quake', 'hondani.art')

CX0, CX1 = 2, 38            # cell columns, inclusive
CY0, CY1 = 6, 17            # cell rows, inclusive
NCX, NCY = CX1 - CX0 + 1, CY1 - CY0 + 1
W, H = NCX * 8, NCY * 8     # 296 x 96
X, Y = CX0 * 8, CY0 * 8     # where both screens draw it: 16, 48
LOGO_BASE = 192             # palette index of C64 column 0


def _art(path=ART):
    d = open(path, 'rb').read()
    if len(d) < 9002 or (d[0] | d[1] << 8) != 0x2000:
        raise ValueError('%s is not a $2000 hires .art' % path)
    return d[2:8002], d[8002:9002]


def _red(nib):
    return 2 if nib in (2, 9) else nib


def hdmi_pixels(path=ART):
    """(w, h, pixels): index LOGO_BASE+column where lit, 0 elsewhere."""
    bm, sc = _art(path)
    px = bytearray(W * H)
    for cy in range(CY0, CY1 + 1):
        for cx in range(CX0, CX1 + 1):
            s = sc[cy * 40 + cx]
            for r in range(8):
                b = bm[(cy * 40 + cx) * 8 + r]
                for bit in range(8):
                    col = s >> 4 if b & (0x80 >> bit) else s & 15
                    if _red(col):
                        px[((cy - CY0) * 8 + r) * W + (cx - CX0) * 8 + bit] = \
                            LOGO_BASE + cx
    return W, H, bytes(px)


def c64_spans(path=ART):
    """Cell rows CY0..CY1 at full width: the bitmap span (NCY*320 bytes,
    contiguous at $E000 + CY0*320) and the screen span (NCY*40 bytes,
    contiguous at screen + CY0*40), brown folded into red."""
    bm, sc = _art(path)
    bits = bytes(bm[CY0 * 320:(CY1 + 1) * 320])
    scr = bytes(_red(s >> 4) << 4 | _red(s & 15)
                for s in sc[CY0 * 40:(CY1 + 1) * 40])
    return bits, scr


RLE_ESC = 0xE5


def rle(data):
    """ESC n v = n copies of v (n 1..255); ESC 0 ends the stream; any other
    byte is itself. A run of 4 or more, or any ESC, becomes a triple."""
    out, i = bytearray(), 0
    while i < len(data):
        j = i
        while j < len(data) and data[j] == data[i] and j - i < 255:
            j += 1
        if j - i >= 4 or data[i] == RLE_ESC:
            out += bytes([RLE_ESC, j - i, data[i]])
        else:
            out += data[i:j]
        i = j
    return bytes(out) + bytes([RLE_ESC, 0])


def unrle(data):
    out, i = bytearray(), 0
    while True:
        if data[i] != RLE_ESC:
            out.append(data[i])
            i += 1
            continue
        if data[i + 1] == 0:
            return bytes(out)
        out += bytes([data[i + 2]]) * data[i + 1]
        i += 3


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    bits, scr = c64_spans()
    a, b = rle(bits), rle(scr)
    assert unrle(a) == bits and unrle(b) == scr
    open(sys.argv[1], 'wb').write(a + b)
    print('wrote %s: bitmap %d -> %d, screen %d -> %d bytes (RLE)' %
          (sys.argv[1], len(bits), len(a), len(scr), len(b)))

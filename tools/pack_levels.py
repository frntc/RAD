#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""pack_levels.py: every .g64lev in one REU image, the level pack.

    tools/pack_levels.py OUT.reu E1M1=build/e1m1.g64lev E1M2=build/e1m2.g64lev
    tools/pack_levels.py --list OUT.reu

Select OUT.reu as the REU image in the RAD menu (it sizes the REU from the
file), then launch the game PRG. The firmware takes LOAD_LEVEL slot N from
the pack's Nth level, in argument order, straight out of REU memory. The
pack is the only place the firmware takes levels and pictures from: a game
ships as its PRG plus this image.

Format (little-endian, gpu64_level.h GPU64_PACK_*):
  0 'G64P'  4 u16 version 1  6 u16 count  8 u32 image bytes
  12 u32 FNV-1a of the directory
  16 count x {u32 offset, u32 bytes, u32 FNV-1a of the level, char name[4]}
Levels start 4 KB aligned. The image is padded to 128 KB, RAD's smallest REU.
"""
import struct
import sys

MAGIC = b'G64P'
VERSION = 1
MAX_SLOTS = 8
ALIGN = 4096
MIN_IMAGE = 128 * 1024


def fnv(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xffffffff
    return h


def pack(levels):
    """levels: [(name, bytes)] -> image bytes."""
    if not 0 < len(levels) <= MAX_SLOTS:
        raise ValueError('1..%d levels' % MAX_SLOTS)
    ofs = ALIGN
    dirb = b''
    body = bytearray(ALIGN)
    for name, data in levels:
        dirb += struct.pack('<III4s', ofs, len(data), fnv(data),
                            name.encode('ascii')[:4].ljust(4, b'\0'))
        body += data
        ofs += len(data)
        pad = -ofs % ALIGN
        body += bytes(pad)
        ofs += pad
    if len(body) < MIN_IMAGE:
        body += bytes(MIN_IMAGE - len(body))
    hdr = MAGIC + struct.pack('<HHII', VERSION, len(levels), len(body), fnv(dirb))
    body[0:len(hdr) + len(dirb)] = hdr + dirb
    return bytes(body)


def unpack(img):
    """image bytes -> [(name, bytes)]; raises ValueError on any damage."""
    if img[:4] != MAGIC:
        raise ValueError('not a level pack')
    ver, n, size, dsum = struct.unpack_from('<HHII', img, 4)
    dirb = img[16:16 + 16 * n]
    if ver != VERSION or not 0 < n <= MAX_SLOTS or size > len(img) \
            or fnv(dirb) != dsum:
        raise ValueError('bad pack header')
    out = []
    for i in range(n):
        ofs, ln, s, name = struct.unpack_from('<III4s', dirb, 16 * i)
        data = img[ofs:ofs + ln]
        if ofs % 64 or ofs + ln > size or fnv(data) != s:
            raise ValueError('level %d damaged' % i)
        out.append((name.rstrip(b'\0').decode('ascii'), data))
    return out


def main(argv):
    if len(argv) == 2 and argv[0] == '--list':
        with open(argv[1], 'rb') as f:
            img = f.read()
        for i, (name, data) in enumerate(unpack(img)):
            print('slot %d  %-4s  %8d bytes' % (i, name, len(data)))
        print('%d bytes, pack OK' % len(img))
        return 0
    if len(argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    levels = []
    for a in argv[1:]:
        name, path = a.split('=', 1)
        with open(path, 'rb') as f:
            levels.append((name, f.read()))
    img = pack(levels)
    unpack(img)
    with open(argv[0], 'wb') as f:
        f.write(img)
    print('%s: %d levels, %d bytes (%.1f MB REU needed)' %
          (argv[0], len(levels), len(img), len(img) / 1048576))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))

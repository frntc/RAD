#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
worldticktest -- milestone 20 stage B's PC gate for WORLD_TICK ($6C).

    make -C tools/hostsim libclipmove.so
    python3 tools/worldticktest.py [build/e1m1.g64lev]

Two halves, split the way CLIP_MOVE's model is split:

  * the queries are the firmware's own gpu64_levelTraceEnts() and
    gpu64_levelMoveEnts(), reached through tools/hostsim/libclipmove.so --
    so the hull-0 checks below are verdicts on the Pi's C, run on E1M1;
  * the protocol -- what refuses the whole block, what refuses one record,
    what is written back and where -- is tools/prgsim's model of it, which
    stage C's 6502 code runs against. opWorldTick() in
    gpu64_3d_class1.cpp is written to the same contract and does not build
    on a PC; a disagreement between the two is found at the bench.

Every C64 buffer is poisoned first, so "wrote nothing" is a check and not an
assumption. Exit 0 if every check passes.
"""

import os
import struct
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'prgsim'))

import gpu64model                                           # noqa: E402
from gpu64model import ERR_OK, ERR_BAD_ARGS                 # noqa: E402
from gpu64class1 import (NODE_OBJECT, NODE_SPRITE, NODE_LIGHT,  # noqa: E402
                         NODE_CAMERA)

MAGIC_IN, MAGIC_OUT = 0xD8, 0x8D
CONT_EMPTY, CONT_SOLID = 0, 1
TR_CLEAR, TR_STARTSOLID, TR_ALLSOLID = 1, 2, 4
MESH_BASE, NODE_BASE = 256, 0x1000
BLOCK = 0x4000
POISON = 0xA7

fails = []


def check(ok, what):
    print('  %s  %s' % ('ok  ' if ok else 'FAIL', what))
    if not ok:
        fails.append(what)


class C64:
    def __init__(self):
        self.ram = bytearray(65536)

    def rd(self, a):
        return self.ram[a & 0xFFFF]

    def wr(self, a, v):
        self.ram[a & 0xFFFF] = v & 0xFF


def block(actors=(), traces=(), moves=(), movers=(), frame=0x5A,
          magic=MAGIC_IN, bad_sum=False):
    """actors: (node, res, flags, yaw, pitch, roll, x, y, z)
    traces: (start3, end3, hull)   moves: (start3, delta3, hull, mode)
    movers: (model, ofs3)          positions all s32 16.16"""
    body = bytearray()
    for n, r, f, y, p, ro, x, yy, z in actors:
        body += struct.pack('<HHBBBB3i', n, r, f, y, p, ro, x, yy, z)
    for s, e, h in traces:
        body += struct.pack('<3i3iBBBB', *s, *e, h, 0, 0, 0)
    for s, d, h, m in moves:
        body += struct.pack('<3i3iBBBB', *s, *d, h, m, 0, 0)
    for m, o in movers:
        body += struct.pack('<BBBB3i', m, 0, 0, 0, *o)
    hdr = bytearray(16)
    hdr[0:6] = bytes((magic, len(actors), len(traces), len(moves),
                      len(movers), frame))
    s = (sum(hdr[0:6]) + sum(hdr[8:]) + sum(body)) & 0xFFFF
    if bad_sum:
        s ^= 1
    hdr[6:8] = struct.pack('<H', s)
    return bytes(hdr + body)


def out_len(nt, nm):
    return 8 + (nt + nm) * 16 + 4


def run(gpu, c64, blk, nt, nm, length=None):
    """Poison, write the block, dispatch. Returns (err, out bytes)."""
    total = len(blk) + out_len(nt, nm)
    for i in range(total + 64):
        c64.ram[BLOCK + i] = POISON
    c64.ram[BLOCK:BLOCK + len(blk)] = blk
    gpu.result = 0xEE
    err = gpu.c1_world_tick(0, BLOCK, total if length is None else length)
    return err, bytes(c64.ram[BLOCK + len(blk):BLOCK + total])


def untouched(c64, blk, nt, nm):
    o = BLOCK + len(blk)
    return all(b == POISON for b in c64.ram[o:o + out_len(nt, nm) + 64])


def answers(out, n):
    a = []
    for i in range(n):
        r = out[8 + i * 16:8 + i * 16 + 16]
        x, y, z = struct.unpack('<3i', r[0:12])
        a.append(((x, y, z), struct.unpack('<H', r[12:14])[0], r[14], r[15],
                  r[12:16]))
    return a


def trailer_ok(out, frame):
    t = len(out) - 4
    return (struct.unpack('<H', out[t:t + 2])[0] == sum(out[:t]) & 0xFFFF
            and out[t + 2] == MAGIC_OUT and out[t + 3] == frame
            and out[0] == MAGIC_OUT and out[1] == frame)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'e1m1.g64lev')
    c64 = C64()
    gpu = gpu64model.Gpu64Model(c64.rd, c64.wr)
    gpu.c1_level_data = open(path, 'rb').read()
    lev_scale = None

    # --- before a level: actors work, queries refuse -----------------------
    print('no level loaded')
    blk = block(traces=[((0, 0, 0), (0, 0, 65536), 0)])
    err, _ = run(gpu, c64, blk, 1, 0)
    check(err == ERR_BAD_ARGS and untouched(c64, blk, 1, 0),
          'a query before LOAD_LEVEL is BAD_ARGS and writes nothing')

    # --- load E1M1 the way the demo does ------------------------------------
    import gpu64level
    lev = gpu64level.Level(gpu.c1_level_data)
    lev_scale = lev.scale
    assert gpu.c1_load_level(MESH_BASE, NODE_BASE, 0) == ERR_OK
    while gpu.c1_level_step() == ERR_OK and gpu.c1_load['phase'] != 'done':
        pass
    check(gpu.c1_load['phase'] == 'done', 'E1M1 loads (%d meshes, %d nodes)'
          % (lev.nmesh, lev.nnode))
    qu = 65536 // lev_scale            # one Quake unit, 16.16 world units

    px, py, pz, _ = lev.player_start()
    eye = (px, py, pz)

    # --- traces: the hull-0 answers on the real map --------------------------
    print('traces (firmware C via libclipmove.so)')
    down = (px, py - 128 * qu, pz)
    up_short = (px, py + 4 * qu, pz)
    t = [(eye, down, 0), (eye, up_short, 0), (eye, eye, 0)]
    blk = block(traces=t, frame=0x31)
    err, out = run(gpu, c64, blk, 3, 0)
    check(err == ERR_OK and trailer_ok(out, 0x31), 'answer block, trailer, frame echo')
    a = answers(out, 3)
    end, frac, fl, hit, _ = a[0]
    floor_qu = (py - end[1]) / qu
    check(not (fl & TR_CLEAR) and hit == CONT_SOLID and frac < 65535,
          'eye straight down hits SOLID (frac %d, hit %d)' % (frac, hit))
    check(30 <= floor_qu <= 60,
          'floor is %.1f qu below the eye (player eye ~46 qu up)' % floor_qu)
    end, frac, fl, hit, _ = a[1]
    check(fl == TR_CLEAR and frac == 65535 and hit == CONT_EMPTY
          and end == up_short, '4 qu up from the eye is CLEAR, end exact')
    check(a[2][2] == TR_CLEAR, 'zero-length trace at the eye is CLEAR')

    # A trace that starts inside the floor: startsolid, never CLEAR.
    inside = (px, end[1] - 0, pz)
    below = (px, py - 60 * qu, pz)
    blk = block(traces=[(below, (px, py - 70 * qu, pz), 0)])
    err, out = run(gpu, c64, blk, 1, 0)
    fl = answers(out, 1)[0][2]
    check(err == ERR_OK and not (fl & TR_CLEAR) and (fl & TR_STARTSOLID),
          'a trace starting under the floor is STARTSOLID (flags %02x)' % fl)

    # --- doors: a mover blocks a shot the world alone lets through -----------
    print('movers')
    doors = [e for e in (lev.ent(i) for i in range(lev.nent))
             if e['classname'] == 'func_door' and e['model'] > 0]
    tested = 0
    for d in doors:
        h = lev.hull(d['model'])
        mn, mx = h['mins'], h['maxs']
        c = [(mn[k] + mx[k]) // 2 for k in range(3)]
        ext = [mx[k] - mn[k] for k in range(3)]
        thin = 0 if ext[0] < ext[2] else 2
        if ext[thin] > 24 * qu:
            continue                      # not a thin slab; skip
        s = list(c)
        e = list(c)
        s[thin] -= ext[thin] // 2 + 8 * qu
        e[thin] += ext[thin] // 2 + 8 * qu
        s, e = tuple(s), tuple(e)
        blk = block(traces=[(s, e, 0)])
        err, out = run(gpu, c64, blk, 1, 0)
        free = answers(out, 1)[0]
        blk = block(traces=[(s, e, 0)], movers=[(d['model'], (0, 0, 0))])
        err2, out = run(gpu, c64, blk, 1, 0)
        shut = answers(out, 1)[0]
        up = 4096 * qu
        blk = block(traces=[(s, e, 0)], movers=[(d['model'], (0, up, 0))])
        err3, out = run(gpu, c64, blk, 1, 0)
        opened = answers(out, 1)[0]
        if free[2] != TR_CLEAR:
            continue                      # the frame is in the way; not a test
        tested += 1
        check(err2 == ERR_OK and shut[2] == 0 and shut[3] == CONT_SOLID
              and shut[1] < 65535,
              'door model %d closed blocks the shot (frac %d, hit %d)'
              % (d['model'], shut[1], shut[3]))
        check(err3 == ERR_OK and opened[2] == TR_CLEAR,
              'door model %d raised out of the way lets it through'
              % d['model'])
        if tested == 3:
            break
    check(tested > 0, '%d doors tested' % tested)

    # --- monsters: every spawn point is free in hull 1 -----------------------
    print('monster spawns')
    mons = [e for e in (lev.ent(i) for i in range(lev.nent))
            if e['classname'].startswith('monster_')]
    stuck = []
    for m in mons:
        p = (m['x'], m['y'], m['z'])
        blk = block(traces=[(p, p, 1)])
        err, out = run(gpu, c64, blk, 1, 0)
        if err != ERR_OK or answers(out, 1)[0][2] != TR_CLEAR:
            stuck.append('%s@%d,%d,%d' % (m['classname'], m['x'] // qu,
                                          m['y'] // qu, m['z'] // qu))
    check(len(mons) > 0 and not stuck,
          '%d monster spawns clear in hull 1%s' % (len(mons),
                                                   (': ' + ' '.join(stuck)) if stuck else ''))

    # --- moves: byte-identical to CLIP_MOVE's answer --------------------------
    print('moves')
    fwd = (0, 0, 40 * qu)
    blk = block(moves=[(eye, fwd, 1, 7)], frame=0x77)
    err, out = run(gpu, c64, blk, 0, 1)
    wt = answers(out, 1)[0][4]
    wt_end = answers(out, 1)[0][0]
    cm = bytearray(56)
    cm[0:24] = struct.pack('<6i', *eye, *fwd)
    cm[24], cm[25], cm[26], cm[27], cm[29] = 1, 7, 0, 0xC5, 0
    x = 0
    for b in cm[:28]:
        x ^= b
    cm[28] = x
    c64.ram[BLOCK:BLOCK + 56] = cm
    check(gpu.c1_clip_move(0, BLOCK, 56) == ERR_OK, 'CLIP_MOVE reference')
    cmo = bytes(c64.ram[BLOCK + 32:BLOCK + 48])
    check(err == ERR_OK and trailer_ok(out, 0x77)
          and struct.pack('<3i', *wt_end) + wt == cmo,
          'WORLD_TICK move answer == CLIP_MOVE out bytes 32-47')

    # --- actors ---------------------------------------------------------------
    print('actors')
    sc = gpu.c1_scene
    n = sc.slot(0x0100)
    n.reset(0x0100, NODE_OBJECT)
    n.mesh_id = MESH_BASE
    s = sc.slot(0x0101)
    s.reset(0x0101, NODE_SPRITE)
    s.tex_id = 0
    li = sc.slot(0x0102)
    li.reset(0x0102, NODE_LIGHT)
    cam = sc.slot(0x0103)
    cam.reset(0x0103, NODE_CAMERA)

    acts = [
        (0x0100, MESH_BASE + 1, 3, 0x40, 0x10, 0x00, 1 << 16, 2 << 16, -3 << 16),
        (0x0101, 1, 1, 0, 0, 0, 5 << 16, 0, 0),
        (0x0102, 0xFFFF, 2, 0, 0, 0, 0, 0, 7 << 16),
        (0x0103, 0xFFFF, 3, 0x80, 0, 0, 9 << 16, 0, 0),
        (0x0100, 1, 1, 0, 0, 0, 0, 0, 0),           # texture on an OBJECT
        (0x0102, 3, 1, 0, 0, 0, 0, 0, 0),           # resource on a LIGHT
        (0x0101, 0xFFFF, 0x05, 0, 0, 0, 0, 0, 0),   # undefined flag bit
        (0x0999, 0xFFFF, 1, 0, 0, 0, 0, 0, 0),      # no such node
        (0x0101, 0x7777, 1, 0, 0, 0, 0, 0, 0),      # no such texture
    ]
    blk = block(actors=acts, frame=0x09)
    err, out = run(gpu, c64, blk, 0, 0)
    refused = struct.unpack('<I', out[4:8])[0]
    check(err == ERR_OK and trailer_ok(out, 0x09), 'actor-only block answers')
    check(refused == 0b111110000 and out[2] == 4 and gpu.result == 5,
          'refusal mask %03x, applied %d, RESULT %d' % (refused, out[2], gpu.result))
    check(n.mesh_id == MESH_BASE + 1 and n.visible and n.view_space
          and n.pos == [1 << 16, 2 << 16, -3 << 16]
          and (n.yaw, n.pitch, n.roll) == (0x4000, 0x1000, 0),
          'OBJECT: mesh, visible, view-space, position, angles<<8')
    check(s.tex_id == 1 and s.visible and not s.view_space and s.pos[0] == 5 << 16,
          'SPRITE: texture, world-space')
    check(not li.visible and li.view_space and li.pos[2] == 7 << 16,
          'LIGHT: hidden, view-space, moved')
    check(not cam.view_space and cam.yaw == 0x8000,
          'CAMERA: view-space bit ignored')

    # --- whole-block refusals write nothing and change nothing ---------------
    print('refusals')
    before = (n.pos[:], n.mesh_id)
    moved = [(0x0100, 0xFFFF, 1, 0, 0, 0, 99 << 16, 0, 0)]
    cases = [
        ('bad magic', block(actors=moved, traces=[(eye, eye, 0)], magic=0xD9), 1, 0, None),
        ('bad checksum', block(actors=moved, traces=[(eye, eye, 0)], bad_sum=True), 1, 0, None),
        ('trace hull 3', block(actors=moved, traces=[(eye, eye, 3)]), 1, 0, None),
        ('move hull 0', block(actors=moved, moves=[(eye, fwd, 0, 0)]), 0, 1, None),
        ('mover model past the table', block(actors=moved, traces=[(eye, eye, 0)],
                                             movers=[(lev.nhull, (0, 0, 0))]), 1, 0, None),
        ('33 actors', None, 0, 0, None),
        ('length one short', block(actors=moved, traces=[(eye, eye, 0)]), 1, 0, -1),
    ]
    for name, blk, nt, nm, short in cases:
        if blk is None:
            blk = bytearray(block())
            blk[1] = 33
        length = None
        if short is not None:
            length = len(blk) + out_len(nt, nm) + short
        err, _ = run(gpu, c64, bytes(blk), nt, nm, length)
        check(err == ERR_BAD_ARGS and untouched(c64, blk, nt, nm)
              and (n.pos[:], n.mesh_id) == before and gpu.result == 0xEE,
              '%s: BAD_ARGS, no answer, no pose, no RESULT' % name)

    print()
    if fails:
        print('worldticktest: %d FAILED' % len(fails))
        sys.exit(1)
    print('worldticktest: PASS')


if __name__ == '__main__':
    main()

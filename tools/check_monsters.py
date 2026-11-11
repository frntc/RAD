#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_monsters.py -- every monster type a level's bank carries, awake.

A level carries only the monster frames its map uses: one gpu64_bank entity
per type (tools/quake_assets.py, build_bank), scanned before any monster,
and the C64 animates a monster from those mesh indices alone
(games/quake/gpu64_game_monsters.inc). check_ai.py proves the grunt and the
dog on E1M1; this proves the rest, one type at a time, on whichever level
it is given:

  for each banked type, up to three of its monsters present at normal
  skill: the eye is warped 6 units (192 QU) from one, on each side in turn
  until one works -- the converter knows nothing of the room around a
  monster, and some wait in closets -- and by frame 220
    - the monster woke (row 23 WK) and left its spawn point;
    - it hurt the player (row 21 HP < 100);
    - its node's mesh is a frame of its OWN bank, never another type's;
    - no move of its was refused by the bound (row 23 RF).

A type banked for another skill only (E1M3's shamblers are hard) is noted
and passed over; --skill=2 tests it.

Usage: tools/check_monsters.py --prg=... --level=build/e1m2.g64lev [--skill=N]
"""

import math
import struct
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'prgsim'))
import gpu64level                                       # noqa: E402

ONE = 65536
NAMES = {51: 'grunt', 52: 'dog', 53: 'ogre', 54: 'knight', 55: 'zombie',
         56: 'scrag', 57: 'fiend', 58: 'shambler'}
CLASS = {51: 'monster_army', 52: 'monster_dog', 53: 'monster_ogre',
         54: 'monster_knight', 55: 'monster_zombie', 56: 'monster_wizard',
         57: 'monster_demon1', 58: 'monster_shambler'}
NOT_IN = (256, 512, 1024, 1024)     # Quake's "not in easy/medium/hard"
AWAY = 6.0
EYE_UP = 0.1875                     # as check_ai.py's dog warp
TRIES = 3                           # monsters of a type, at most
SIDES = ((0, -1), (0, 1), (-1, 0), (1, 0))


def run(prg, level, frame, keys, skill):
    cmd = [sys.executable, os.path.join(HERE, 'prgsim', 'runsim.py'), prg,
           '--demo', '--frame-ms=32', '--stop-after=%d' % frame,
           '--level=' + level, '--dump-scene',
           '--skill=%d' % skill] + keys
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit('check_monsters.py: the simulated run failed')
    nodes, rows = {}, {}
    for line in r.stdout.splitlines():
        m = re.match(r'\s*node\s+(\d+)\s+type\s+\d+\s+vis\s+(\d+)\s+'
                     r'pos\s+(-?\d+)\s+(-?\d+)\s+(-?\d+).*\smesh\s+(\d+)',
                     line)
        if m:
            nodes[int(m.group(1))] = {
                'vis': int(m.group(2)),
                'pos': tuple(int(m.group(i)) for i in (3, 4, 5)),
                'mesh': int(m.group(6))}
            continue
        m = re.match(r'\s*(\d+)\|\s(.*)$', line)
        if m:
            rows[int(m.group(1))] = m.group(2)
    stats = {}
    for r_ in (21, 22, 23):
        for name, val in re.findall(r'([A-Z]+)(\d+)', rows.get(r_, '')):
            stats.setdefault(name, int(val))
    return nodes, stats, rows


def bank_ranges(lev, base):
    """kind -> set of the mesh ids its bank names, MESH_BASE added."""
    out = {}
    for i in range(lev.nent):
        e = lev.ent(i)
        if e['classname'] != 'gpu64_bank':
            break
        raw = struct.pack('<3i3i', e['x'], e['y'], e['z'], *e['ofs'])
        ids = set()
        for s in range(8):
            first, n = raw[2 * s], raw[2 * s + 1]
            ids.update(base + first + k for k in range(n))
        out[e['p0']] = ids
    return out


class Verdict:
    def __init__(self):
        self.bad = 0

    def check(self, ok, what):
        print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
        if not ok:
            self.bad += 1


def main():
    prg = level = None
    skill = 1
    for a in sys.argv[1:]:
        if a.startswith('--prg='):
            prg = a[6:]
        elif a.startswith('--level='):
            level = a[8:]
        elif a.startswith('--skill='):
            skill = int(a[8:])
        else:
            sys.exit('check_monsters.py: unknown argument %s' % a)
    if not (prg and level):
        sys.exit('check_monsters.py: --prg and --level are required')

    demos = os.path.join(REPO, 'games', 'quake')
    base = None
    for line in open(os.path.join(demos, 'gpu64_demo_game.a')):
        m = re.match(r'MESH_BASE\s*=\s*(\d+)', line)
        if m:
            base = int(m.group(1))
    lev = gpu64level.Level(open(level, 'rb').read())
    banks = bank_ranges(lev, base)
    v = Verdict()

    nodes0, _, _ = run(prg, level, 10, ['--notarget'], skill)
    for kind, ids in banks.items():
        name = NAMES[kind]
        mons = []
        for i in range(lev.nent):
            e = lev.ent(i)
            if e['classname'] == CLASS[kind] and \
                    not e['spawnflags'] & NOT_IN[skill]:
                pos = (e['x'], e['y'], e['z'])
                nn = [n for n, d in nodes0.items() if d['pos'] == pos]
                if len(nn) == 1:
                    mons.append((pos, nn[0]))
        if not mons:
            # Banked for another skill: E1M3's shamblers are hard only.
            print('      %s: none at skill %d' % (name, skill))
            continue
        print('      %s: %d placed' % (name, len(mons)))
        v.check(all(nodes0[n]['mesh'] in ids for _, n in mons),
                '%s: asleep on frames of its own bank' % name)
        best = None
        for pos, node in mons[:TRIES]:
            x, y, z = (p / ONE for p in pos)
            for dx, dz in SIDES:
                ex, ez = x + dx * AWAY, z + dz * AWAY
                yaw = round(math.atan2(-dx, -dz) * 128 / math.pi) & 255
                nodes, st, rows = run(prg, level, 220, [
                    '--warp=5:%.4f,%.4f,%.4f,%d' % (ex, y + EYE_UP, ez, yaw)],
                    skill)
                d = nodes.get(node, {})
                good = (st.get('WK', 0) >= 1 and st.get('HP', 100) < 100 and
                        d.get('pos') != pos)
                best = (d, st, rows, (dx, dz), pos)
                if good:
                    break
            if good:
                break
        d, st, rows, side, pos = best
        p = d.get('pos', (0, 0, 0))
        print('      %s from side %s: at %.2f %.2f %.2f, mesh %s'
              % (name, side, p[0] / ONE, p[1] / ONE, p[2] / ONE,
                 d.get('mesh')))
        v.check(st.get('WK', 0) >= 1 and d.get('pos') != pos,
                '%s: woke and moved (row 23: %s)' % (name, rows.get(23)))
        v.check(st.get('HP', 100) < 100,
                '%s: hurt the player (row 21: %s)' % (name, rows.get(21)))
        v.check(d.get('mesh') in ids,
                '%s: still on a frame of its own bank' % name)
        v.check(st.get('RF', 1) == 0, '%s: no move refused' % name)

    print('check_monsters: %s' % ('PASS' if not v.bad
                                   else '%d FAILED' % v.bad))
    return 1 if v.bad else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_ai.py - milestone 20 stage E: do the monsters fight back?

check_combat.py runs with --notarget, so everything it proves is the
player's half. This is the other half, without it: a monster that sees the
player wakes, turns, walks at them through CLIP_MOVE answers, and attacks
-- and a restart puts it back where it started, asleep. Every link ends in
a scene node or a number on row 21 (HP) or row 23 (AWK WK SEE BLK RF HT).

Three scripted runs:

  grunt    the door route's first two legs toward grunt 245, by the button,
           and then standing still. It must wake, leave its spawn point, and
           hurt the player -- a volley is the only thing that can.
  dog      runsim --warp puts the player 7.5 units south of dog 247 in the
           room below, facing it. The dog must close in (its z falls toward
           the player's), stay on the floor (y no lower than its spawn),
           and hurt the player; the grunt beside it wakes too.
  restart  the grunt run again until it is awake and off its spawn point,
           runsim --hurt five times to be sure of dying, joystick 2's
           button to restart. The level start is out of every monster's
           sight, so what the frame after shows is the restart's own work:
           the grunt back on its spawn point and every monster asleep, row
           23 AWK 000.

RF (moves refused by the bound) must be zero in every run: in a simulation
there is no bus to corrupt an answer, so a refusal is a bug.

Usage: tools/check_ai.py --prg=... --level=build/e1m1.g64lev
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'prgsim'))
import gpu64level                                       # noqa: E402

ONE = 65536

TO_GRUNT = ['--key=LSHIFT:20-207', '--key=W:20-43', '--key=E:44-55',
            '--key=W:154-168', '--key=Q:169-172', '--key=W:173-187',
            '--key=Q:188-207', '--key=A:211-242']
# The eye 7.5 units south of dog 247, on its floor (22 Quake units up).
DOG_WARP = ['--warp=5:2.75,-5.5625,40,0']
RESTART = TO_GRUNT + ['--hurt=300,310,320,330,340', '--joy=FIRE:380-390']


def run(prg, level, frame, keys):
    cmd = [sys.executable, os.path.join(HERE, 'prgsim', 'runsim.py'), prg,
           '--demo', '--frame-ms=32', '--stop-after=%d' % frame,
           '--level=' + level,
           '--dump-scene'] + keys
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit('check_ai.py: the simulated run failed')
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


class Verdict:
    def __init__(self):
        self.bad = 0

    def check(self, ok, what):
        print('%s  %s' % ('ok  ' if ok else 'FAIL', what))
        if not ok:
            self.bad += 1


def main():
    prg = level = None
    for a in sys.argv[1:]:
        if a.startswith('--prg='):
            prg = a[6:]
        elif a.startswith('--level='):
            level = a[8:]
        else:
            sys.exit('check_ai.py: unknown argument %s' % a)
    if not (prg and level):
        sys.exit('check_ai.py: --prg and --level are required')

    lev = gpu64level.Level(open(level, 'rb').read())

    def spawn(i, cls):
        e = lev.map_ent(i)
        if e['classname'] != cls:
            sys.exit('check_ai.py: entity %d is %s, not %s'
                     % (i, e['classname'], cls))
        return (e['x'], e['y'], e['z'])

    grunt = spawn(245, 'monster_army')
    dog = spawn(247, 'monster_dog')
    v = Verdict()

    # --- grunt ------------------------------------------------------------
    # Node numbers come from the first run, where nothing has moved yet.
    nodes, st, rows = run(prg, level, 10, ['--notarget'])
    gn = [n for n, d in nodes.items() if d['pos'] == grunt]
    dn = [n for n, d in nodes.items() if d['pos'] == dog]
    v.check(len(gn) == 1 and len(dn) == 1,
            'one node on grunt 245 and one on dog 247')
    if v.bad:
        return 1
    gn, dn = gn[0], dn[0]

    nodes, st, rows = run(prg, level, 400, TO_GRUNT)
    g = nodes[gn]['pos']
    v.check(st.get('WK', 0) >= 1 and st.get('AWK', 0) >= 1,
            'the grunt woke (row 23: %s)' % rows.get(23))
    v.check(g != grunt and abs(g[1] - grunt[1]) < ONE // 2,
            'it left its spawn point and kept to its floor'
            ' (%.2f %.2f %.2f)' % tuple(p / ONE for p in g))
    v.check(st.get('HT', 0) >= 1 and st.get('HP', 100) < 100,
            'it hurt the player (row 21: %s)' % rows.get(21))
    v.check(st.get('RF', 1) == 0, 'no move refused by the bound')

    # --- dog --------------------------------------------------------------
    nodes, st, rows = run(prg, level, 120, DOG_WARP)
    d = nodes[dn]['pos']
    v.check(d[2] < dog[2] - 3 * ONE and d[1] >= dog[1],
            'the dog ran at the player, on the floor'
            ' (%.2f %.2f %.2f from z %.2f)'
            % (d[0] / ONE, d[1] / ONE, d[2] / ONE, dog[2] / ONE))
    v.check(st.get('WK', 0) >= 2,
            'the dog and the grunt beside it woke (row 23: %s)'
            % rows.get(23))
    v.check(st.get('HP', 100) < 100 and st.get('HT', 0) >= 1,
            'the player was hurt (row 21: %s)' % rows.get(21))
    v.check(st.get('RF', 1) == 0, 'no move refused by the bound')

    # --- restart ----------------------------------------------------------
    nodes, st, rows = run(prg, level, 400, RESTART)
    v.check(st.get('DIE') == 1 and st.get('HP') == 100,
            'died once and restarted at full health (rows 21-22: %s / %s)'
            % (rows.get(21), rows.get(22)))
    v.check(nodes[gn]['pos'] == grunt,
            'the grunt is back on its spawn point')
    v.check(st.get('AWK', 1) == 0,
            'and asleep (row 23: %s)' % rows.get(23))

    print('check_ai: %s' % ('PASS' if not v.bad else '%d FAILED' % v.bad))
    return 1 if v.bad else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_combat.py - milestone 20 stage D: did the C64 fight?

A picture of a grunt proves nothing about a shotgun. What stage D claims is
a chain -- the fire key, the cone search, a trace riding the frame's
WORLD_TICK, pellets resolved against the answer, damage, a pain animation,
a death animation that HOLDS, a backpack that drops and can be picked up,
and a player who can die and start again -- and every link of it ends in
either a scene node or a number the demo prints on rows 21 and 22. This
runs the game in prgsim and checks both.

Two scripted runs, each driven from the start of the level:

  kill     the door route's first two legs, a quarter turn to face the
           grunt standing by the button (entity 245), two shotgun blasts
           and a third at the corpse, then a walk over the backpack. The
           three shots are fired with B, C= and the mouse's button, one
           each, so every fire input is exercised.
           Pain after the first (24 < 30 hp), dead after the second, the
           third finds nothing, and the backpack is worth exactly 5.
  respawn  runsim --hurt five times: 100 hp in 20s, so dead on the
           fifth. Dead means HP 0, one death, and the gun node hidden.
           Then joystick 2's button restarts -- camera back on the start,
           full health, the gun back -- and the press that restarted must
           NOT also fire. A later B fires normally.

Both run with --notarget: the monsters never wake on their own, so the
grunt is where its entity says and the only damage is --hurt's.
tools/check_ai.py is the run where they fight back.

Usage: tools/check_combat.py --prg=... --level=build/e1m1.g64lev
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'prgsim'))
import gpu64level                                       # noqa: E402

ONE = 65536.0

# The door route's first two legs (tools/examples.sh) end at 8, 1.4, 18 facing
# +z, eight units east of the grunt. A turns 2/256 a frame: 32 frames is a
# quarter turn, to face -x.
TO_GRUNT = ['--key=LSHIFT:20-207', '--key=W:20-43', '--key=E:44-55',
            '--key=W:154-168', '--key=Q:169-172', '--key=W:173-187',
            '--key=Q:188-207', '--key=A:211-242']
KILL = TO_GRUNT + ['--key=B:250-253', '--key=CBM:270-273',
                   '--mouse=0,0,FIRE:290-293',
                   '--key=LSHIFT:300-316', '--key=W:300-316']
RESPAWN = ['--key=B:25-27', '--hurt=50,60,70,80,90',
           '--joy=FIRE:130-140', '--key=B:160-163']


def consts(path):
    out = {}
    for line in open(path):
        m = re.match(r'(\w+)\s*=\s*(\d+)\s*(;.*)?$', line.strip())
        if m:
            out[m.group(1)] = int(m.group(2))
    return out


def run(prg, level, frame, keys):
    cmd = [sys.executable, os.path.join(HERE, 'prgsim', 'runsim.py'), prg,
           '--demo', '--frame-ms=32', '--stop-after=%d' % frame,
           '--level=' + level,
           '--dump-scene', '--notarget'] + keys
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit('check_combat.py: the simulated run failed')
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
    # Rows 21 and 22 are NAME followed by digits: HP100 AR000 ... K001/023.
    stats = {}
    for r_ in (21, 22):
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
            sys.exit('check_combat.py: unknown argument %s' % a)
    if not (prg and level):
        sys.exit('check_combat.py: --prg and --level are required')

    demos = os.path.join(REPO, 'games', 'quake')
    c = consts(os.path.join(demos, 'gpu64_quake_actors.inc'))
    base = consts(os.path.join(demos, 'gpu64_demo_game.a'))['MESH_BASE']
    lev = gpu64level.Level(open(level, 'rb').read())
    g = lev.map_ent(245)
    if g['classname'] != 'monster_army':
        sys.exit('check_combat.py: entity 245 is %s, not the grunt'
                 % g['classname'])
    gpos = (g['x'], g['y'], g['z'])
    # The level's monster bank: DEATH, then DEATHB (Quake's deathc).
    death = (base + c['LB_M_SOLDIER_DEATH'],
             base + c['LB_M_SOLDIER_DEATHB'] + c['LB_N_SOLDIER_DEATHB'] - 1)
    pack = base + c['M_BACKPACK']
    v = Verdict()

    # --- kill -------------------------------------------------------------
    nodes, st, rows = run(prg, level, 340, KILL)
    grunt = [n for n, d in nodes.items() if d['pos'] == gpos]
    v.check(len(grunt) == 1, 'one node on grunt 245 at %.2f %.2f %.2f'
            % tuple(p / ONE for p in gpos))
    if grunt:
        m = nodes[grunt[0]]['mesh']
        v.check(death[0] <= m <= death[1],
                'grunt mesh %d is a death frame (%d..%d)' % (m, *death))
    v.check(st.get('K') == 1, 'one kill (row 21: %s)' % rows.get(21))
    v.check(st.get('SHOT') == 3 and st.get('HIT') == 2,
            'three shots, two hits: pain, then death, then a corpse'
            ' (row 22: %s)' % rows.get(22))
    packs = [d for d in nodes.values() if d['mesh'] == pack]
    v.check(len(packs) == 1 and packs[0]['vis'] == 0,
            'the backpack dropped and was taken (%d node(s), vis %s)'
            % (len(packs), [d['vis'] for d in packs]))
    v.check(st.get('PICK') == 1 and st.get('SH') == 22 + 5,
            'the backpack is worth 5 shells: 25 - 3 + 5 = 27 (SH%03d)'
            % st.get('SH', -1))

    # --- respawn ----------------------------------------------------------
    nodes, st, rows = run(prg, level, 110, RESPAWN)
    v.check(st.get('HP') == 0 and st.get('DIE') == 1,
            'five 20-point hurts kill a 100 hp player (%s / %s)'
            % (rows.get(21), rows.get(22)))
    v.check(nodes.get(2, {}).get('vis') == 0, 'the dead player has no gun')

    nodes, st, rows = run(prg, level, 175, RESPAWN)
    start = lev.player_start()[:3]
    cam = nodes.get(1, {}).get('pos')
    v.check(cam == start, 'the camera is back on the start (%s, start %s)'
            % (cam and tuple(round(p / ONE, 2) for p in cam),
               tuple(round(p / ONE, 2) for p in start)))
    v.check(st.get('HP') == 100 and st.get('DIE') == 1,
            'restarted at full health (%s)' % rows.get(21))
    v.check(nodes.get(2, {}).get('vis') == 1, 'the gun is back')
    v.check(st.get('SHOT') == 2 and st.get('SH') == 24,
            'the restart press did not fire; the next one did'
            ' (SHOT%03d SH%03d, want 2 and 24)'
            % (st.get('SHOT', -1), st.get('SH', -1)))
    return 1 if v.bad else 0


if __name__ == '__main__':
    sys.exit(main())

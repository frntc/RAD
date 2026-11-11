#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_game.py - did a door actually MOVE?

tools/examples.sh normally asserts nothing: it establishes that a demo
assembles, runs and produces a picture, and leaves the verdict to whoever
looks at the PPM. gpu64_demo_game is the one demo where that is not enough.
Its whole claim is that a C64 can run the game logic -- read the level's
entities, match a button to its door, animate the door, and keep the
collision hull in step with what is drawn -- and every part of that claim is
invisible in a picture of a corridor. A door frozen shut and a door that
opens look identical in any single frame a reader happens to photograph.

So this checks the one thing the pictures cannot: that at a frame where the
player is standing in a doorway, the scene node the door is drawn with has
MOVED away from the entity's origin, along the entity's own travel vector
and no further than it. That is the whole animation path end to end -- the
scan, the touch test, the state machine, the fixed-point multiply and
SET_POSITION -- reduced to one number a script can compare.

It is deliberately not a check that the door reached any particular
fraction: DOOR_STEP is a gameplay constant and this must not break when
somebody makes the doors slower.

Usage (see the game) case in tools/examples.sh):
  tools/check_game.py --prg=... --level=... --frame=110 --ent=310 --key=W:20-51 ...
  tools/check_game.py ... --frame=400 --ent=29 --shut   # and it closed again
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'prgsim'))
import gpu64level                                       # noqa: E402

ONE = 65536.0


def main():
    prg = level = None
    frame = 110
    ent = None
    shut = False
    keys = []
    for a in sys.argv[1:]:
        if a.startswith('--prg='):
            prg = a[6:]
        elif a.startswith('--level='):
            level = a[8:]
        elif a.startswith('--frame='):
            frame = int(a[8:])
        elif a.startswith('--ent='):
            ent = int(a[6:])
        elif a == '--shut':
            # The other half of the state machine: at this frame the door
            # must be back where it started. Without it a door that opens
            # and never closes passes every check here.
            shut = True
        elif a.startswith('--key=') or a in ('--demo', '--notarget'):
            keys.append(a)
        elif a.startswith('--frame-ms='):
            pass                    # always 32 here, see run()
        else:
            sys.exit('check_game.py: unknown argument %s' % a)
    if not (prg and level and ent is not None):
        sys.exit('check_game.py: --prg, --level and --ent are required')

    lev = gpu64level.Level(open(level, 'rb').read())
    e = lev.map_ent(ent)
    if not e['model']:
        sys.exit('check_game.py: entity %d has no brush model' % ent)
    if not any(e['ofs']):
        sys.exit('check_game.py: entity %d does not move' % ent)

    # Which scene nodes draw it, and where they sit when it is shut. The
    # node ids are the load's own base plus the level's node index; the base
    # is what LOAD_LEVEL reports and what the demo never has to know,
    # because LEVEL_ENT hands back absolute ids.
    idx = [i for i in range(lev.nnode) if lev.node_model(i) == e['model']]
    if not idx:
        sys.exit('check_game.py: entity %d has no nodes to draw' % ent)

    cmd = [sys.executable, os.path.join(HERE, 'prgsim', 'runsim.py'), prg,
           '--demo', '--frame-ms=32', '--stop-after=%d' % frame,
           '--level=' + level,
           '--dump-scene'] + [k for k in keys if k != '--demo']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit('check_game.py: the simulated run failed')

    nodes = {}
    for line in r.stdout.splitlines():
        m = re.match(r'\s*node\s+(\d+)\s+type\s+\d+\s+vis\s+\d+\s+'
                     r'pos\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)', line)
        if m:
            nodes[int(m.group(1))] = tuple(int(m.group(i)) for i in (2, 3, 4))

    # The node whose closed position matches this entity's origin. Matching
    # on the position rather than trusting a node-id arithmetic keeps this
    # honest if the loader's numbering ever changes.
    base = (e['x'], e['y'], e['z'])
    travel = tuple(e['ofs'])
    cand = [n for n, p in nodes.items()
            if all(abs(p[k] - (base[k] + travel[k])) < (1 << 16) or
                   abs(p[k] - base[k]) < (1 << 16) or
                   min(base[k], base[k] + travel[k]) - (1 << 12) <= p[k] <=
                   max(base[k], base[k] + travel[k]) + (1 << 12)
                   for k in range(3))]
    if not cand:
        print('FAIL  no scene node sits on entity %d\'s travel line' % ent)
        print('      base   %.2f %.2f %.2f' % tuple(v / ONE for v in base))
        print('      travel %.2f %.2f %.2f' % tuple(v / ONE for v in travel))
        return 1

    # A node is only evidence if it has left the closed position without
    # overshooting the open one, on every axis the door travels along.
    for n in sorted(cand):
        p = nodes[n]
        moved = 0.0
        over = False
        for k in range(3):
            if not travel[k]:
                if abs(p[k] - base[k]) > (1 << 12):
                    over = True
                continue
            f = (p[k] - base[k]) / float(travel[k])
            if f < 0.0 or f > 1.0001:
                over = True
            moved = max(moved, f)
        if shut:
            if moved <= 0.0 and not over:
                print('ok    ent %d node %d is shut again at frame %d'
                      % (ent, n, frame))
                return 0
            continue
        if over or moved <= 0.0:
            continue
        print('ok    ent %d node %d has moved %d%% of its travel at frame %d'
              % (ent, n, round(moved * 100), frame))
        print('      %.2f %.2f %.2f  (shut %.2f %.2f %.2f, open %.2f %.2f %.2f)'
              % (tuple(v / ONE for v in p) + tuple(v / ONE for v in base) +
                 tuple((base[k] + travel[k]) / ONE for k in range(3))))
        return 0

    if shut:
        print('FAIL  entity %d has not closed again by frame %d' % (ent, frame))
    else:
        print('FAIL  entity %d is still shut at frame %d -- nothing opened it'
              % (ent, frame))
    for n in sorted(cand):
        print('      node %d at %.2f %.2f %.2f, shut is %.2f %.2f %.2f'
              % ((n,) + tuple(v / ONE for v in nodes[n]) +
                 tuple(v / ONE for v in base)))
    return 1


if __name__ == '__main__':
    sys.exit(main())

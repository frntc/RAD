#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_env.py - milestone 20: water, slime and the radiation suit.

CLIP_MOVE reports Quake's waterlevel (block bytes 50/51) and the contents at
the player's feet from hull 0 (byte 45); gpu64_game_env.inc turns them into
swimming, drowning and liquid damage. Every link ends in a number on row 21
(HP), row 22 (PICK) or row 24 (WL CT SW AIR SUIT EH), or in the camera on
row 4.

Five scripted runs, each from runsim --warp:

  drown    eye-deep in the pool, still. No hit before twelve seconds of air
           (frame 360), hits after (frame 420): Quake's 4 then 6.
  surface  the same, SPACE held: the swimmer rises until the eyes are out,
           and the air comes back at once.
  slime    eye-deep in the slime pit: 4 x 3 = 12 per second, the first on
           contact.
  suit     picked up on the ledge above the slime, then strafe and jump in:
           in the slime, eyes under, and neither slime nor drowning hurts.
  swimout  from the pool straight at the east ledge (secret 45's trigger):
           the swim's step-up lands the player dry on the ledge.

Usage: tools/check_env.py --prg=... --level=build/e1m1.g64lev
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

POOL = '--warp=5:23,-9.5,30,0'
SLIME = '--warp=5:38.5,-15,56,0'
SUIT = ['--warp=5:22,-11,64.5,0', '--key=S:10-20', '--key=E:30-90',
        '--key=SPACE:60-64']
SWIMOUT = ['--warp=5:23,-9.5,29,64', '--key=W:10-80']


def run(prg, level, frame, keys):
    cmd = [sys.executable, os.path.join(HERE, 'prgsim', 'runsim.py'), prg,
           '--demo', '--frame-ms=32', '--stop-after=%d' % frame,
           '--level=' + level, '--notarget'] + keys
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit('check_env.py: the simulated run failed')
    rows = {}
    for line in r.stdout.splitlines():
        m = re.match(r'\s*(\d+)\|\s(.*)$', line)
        if m:
            rows[int(m.group(1))] = m.group(2)
    st = {}
    for r_ in (21, 22, 24):
        for name, val in re.findall(r'([A-Z]+)(\d+)', rows.get(r_, '')):
            st.setdefault(name, int(val))
    m = re.search(r'CAM X([0-9A-F]{4}) Y([0-9A-F]{4}) Z([0-9A-F]{4})',
                  rows.get(4, ''))
    if m:
        st['cam'] = tuple(int(g, 16) - (0x10000 if int(g, 16) & 0x8000
                                        else 0) for g in m.groups())
    return st, rows


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
            sys.exit('check_env.py: unknown argument %s' % a)
    if not (prg and level):
        sys.exit('check_env.py: --prg and --level are required')
    v = Verdict()

    # --- drown ------------------------------------------------------------
    st, rows = run(prg, level, 360, [POOL])
    v.check(st.get('WL') == 3 and st.get('CT') == 2 and st.get('SW') == 1,
            'eye-deep in water, swimming (row 24: %s)' % rows.get(24))
    v.check(st.get('EH') == 0 and st.get('HP') == 100,
            'no hurt inside twelve seconds of air (row 24: %s)'
            % rows.get(24))
    st, rows = run(prg, level, 420, [POOL])
    v.check(st.get('EH') == 2 and st.get('HP') == 90,
            'then drowning, 4 and 6 (rows 21/24: %s / %s)'
            % (rows.get(21), rows.get(24)))

    # --- surface ----------------------------------------------------------
    st, rows = run(prg, level, 100, [POOL, '--key=SPACE:40-200'])
    v.check(st.get('WL', 3) <= 2 and st.get('AIR', 1) == 0,
            'SPACE brought the eyes out and the air back (row 24: %s)'
            % rows.get(24))

    # --- slime ------------------------------------------------------------
    st, rows = run(prg, level, 50, [SLIME])
    v.check(st.get('WL') == 3 and st.get('CT') == 3,
            'eye-deep in slime (row 24: %s)' % rows.get(24))
    v.check(st.get('EH') == 2 and st.get('HP') == 76,
            'slime bit at once and again a second later, 12 each'
            ' (rows 21/24: %s / %s)' % (rows.get(21), rows.get(24)))

    # --- suit -------------------------------------------------------------
    st, rows = run(prg, level, 250, SUIT)
    v.check(st.get('PICK') == 1 and st.get('SUIT', 0) > 0,
            'picked the suit up and it is running (rows 22/24: %s / %s)'
            % (rows.get(22), rows.get(24)))
    v.check(st.get('WL') == 3 and st.get('CT') == 3,
            'in the slime, eyes under (row 24: %s)' % rows.get(24))
    v.check(st.get('EH') == 0 and st.get('HP') == 100 and
            st.get('AIR', 1) == 0,
            'and nothing hurt, not even the air (rows 21/24: %s / %s)'
            % (rows.get(21), rows.get(24)))

    # --- swimout ----------------------------------------------------------
    st, rows = run(prg, level, 80, SWIMOUT)
    cam = st.get('cam', (0, 0, 0))
    v.check(st.get('WL') == 0 and cam[0] >= 29,
            'swam east and climbed out dry onto the ledge (rows 4/24: %s / %s)'
            % (rows.get(4), rows.get(24)))

    print('check_env: %s' % ('PASS' if not v.bad else '%d FAILED' % v.bad))
    return 1 if v.bad else 0


if __name__ == '__main__':
    sys.exit(main())

#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_exit.py - milestone 20 stage F: the counter, the exit, the flash.

gpu64_game_exit.inc adds four things a picture of a corridor cannot show:
a trigger_counter that opens its door only on the third button, the
trigger_changelevel that ends E1M1 in an intermission, the stats that
intermission prints, and the muzzle-flash point light. Each is reduced to
a number or a line of text here.

Five scripted runs, each from runsim --warp (and --hop, which moves the eye
without restarting the level):

  one      button 213 alone: the counter says "ONLY 2 MORE TO GO..." on
           C64 row 3, and door 215 is still shut.
  three    buttons 213, 212 and 211 by --hop: "ONLY 1 MORE", then
           "SEQUENCE COMPLETED!", and door 215 has moved along its travel
           and is still open 200 frames later: it is wait -1, and every
           other door would have shut again by then.
  exit     standing in the changelevel brush: the intermission's rows
           (E1M1 COMPLETED, TIME, SECRETS, KILLS) reach the C64 screen,
           then RUN/STOP leaves it the way the game's quit does.
  leave    FIRE on the armed stats screen: the level restarts and play
           goes on (a --hop then presses button 213 to prove it).
  flash    fire held from frame 30: light node 90 is lit by the shot's
           first frames and dark again after.

Usage: tools/check_exit.py --prg=... --level=build/e1m1.g64lev
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'prgsim'))
import gpu64level                                       # noqa: E402

B213 = '25,-4,63.5,0'
B212 = '38.5,-8,77,0'
B211 = '39,-6,64,0'
EXIT = '41,-6.38,17,0'
DOOR = 215
NODE_BASE = 100
FLASH_ID = 90


def run(prg, level, frame, extra):
    cmd = [sys.executable, os.path.join(HERE, 'prgsim', 'runsim.py'), prg,
           '--demo', '--frame-ms=32', '--stop-after=%d' % frame,
           '--level=' + level, '--notarget', '--dump-scene',
           '--max', '200000000'] + extra
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit('check_exit.py: the simulated run failed')
    log, nodes = [], {}
    for line in r.stdout.splitlines():
        m = re.match(r'frame\s+(\d+)\s+ink\s+\d+\s+(.*)$', line)
        if m:
            log.append((int(m.group(1)), m.group(2)))
            continue
        m = re.match(r'\s*node\s+(\d+)\s.*pos\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)'
                     r'.*light\s+(\d+)\s+(\d+)', line)
        if m:
            nodes[int(m.group(1))] = ([int(m.group(i)) for i in (2, 3, 4)],
                                      int(m.group(5)), int(m.group(6)))
    return log, nodes


def seen(log, text):
    return [f for f, s in log if text in s]


def door_moved(lev, nodes):
    """The fraction of its travel door 215's node has covered, or None."""
    e = lev.map_ent(DOOR)
    idx = [i for i in range(lev.nnode) if lev.node_model(i) == e['model']]
    if len(idx) != 1 or NODE_BASE + idx[0] not in nodes:
        return None
    p = nodes[NODE_BASE + idx[0]][0]
    return (p[0] - e['x']) / float(e['ofs'][0])


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
            sys.exit('check_exit.py: unknown argument %s' % a)
    if not (prg and level):
        sys.exit('check_exit.py: --prg and --level are required')
    lev = gpu64level.Level(open(level, 'rb').read())
    e = lev.map_ent(DOOR)
    if lev.map_ent(214)['classname'] != 'trigger_counter' or \
            e['targetname'] != lev.map_ent(214)['target'] or \
            not e['ofs'][0] or any(e['ofs'][1:]):
        sys.exit('check_exit.py: entities 214/215 are not the counter and '
                 'its x-travelling door any more')
    v = Verdict()

    # --- one ---------------------------------------------------------------
    log, nodes = run(prg, level, 100, ['--warp=5:' + B213, '--frame-log',
                                       '--log-rows=3-3'])
    v.check(seen(log, 'ONLY 2 MORE TO GO'),
            'one button: "ONLY 2 MORE TO GO" (frames %s)'
            % seen(log, 'ONLY 2')[:1])
    f = door_moved(lev, nodes)
    v.check(f is not None and abs(f) < 0.01,
            'one button: door %d still shut (moved %s)' % (DOOR, f))

    # --- three -------------------------------------------------------------
    log, nodes = run(prg, level, 300, ['--warp=5:' + B213,
                                       '--hop=50:' + B212,
                                       '--hop=100:' + B211, '--frame-log',
                                       '--log-rows=3-3'])
    a, b, c = (seen(log, t) for t in ('ONLY 2 MORE', 'ONLY 1 MORE',
                                      'SEQUENCE COMPLETED'))
    v.check(a and b and c and a[0] < b[0] < c[0],
            'three buttons: 2 more, 1 more, completed (frames %s %s %s)'
            % (a[:1], b[:1], c[:1]))
    f = door_moved(lev, nodes)
    v.check(f is not None and f > 0.5,
            'three buttons: door %d open, and still open ~200 frames on'
            ' because it is wait -1 (%s of its travel)' % (DOOR, f))

    # --- exit --------------------------------------------------------------
    log, nodes = run(prg, level, 40, ['--warp=5:' + EXIT, '--frame-log',
                                      '--log-rows=6-14'])
    rows = [s for f_, s in log if 'COMPLETED' in s]
    v.check(bool(rows), 'exit: the intermission came up (%d flips)'
            % len(rows))
    last = rows[-1] if rows else ''
    v.check(re.search(r'TIME\s+\d+:\d\d', last) and
            re.search(r'SECRETS\s+0*0/0*6\b', last) and
            re.search(r'KILLS\s+0*0/0*[1-9]\d*', last),
            'exit: TIME mm:ss, SECRETS 0/6, KILLS 0/n (%s)' % last)

    # --- leave -------------------------------------------------------------
    # FIRE once the prompt is armed (20 ticks, one flip each). The restart
    # puts the eye back on the warp, inside the exit brush, so a --hop
    # armed during the stats (the intermission never takes it) moves it to
    # button 213 on the first game frame: the counter's message is the
    # proof that play resumed with the level's state reset.
    log, nodes = run(prg, level, 14000, ['--warp=5:' + EXIT,
                                         '--joy=FIRE:31-32',
                                         '--hop=20:' + B213, '--frame-log',
                                         '--log-rows=3-6'])
    after = [f_ for f_, s in log if f_ > 31 and 'ONLY 2 MORE' in s]
    again = [f_ for f_, s in log if f_ > 33 and 'COMPLETED' in s]
    v.check(after and not again,
            'leave: FIRE left the stats and play resumed (frames %s, '
            're-entered %s)' % (after[:1], again[:1]))

    # --- flash -------------------------------------------------------------
    # The end-of-run scene dump comes after the game's own quit, so this
    # reads the light per flip instead (runsim --log-light).
    log, nodes = run(prg, level, 60, ['--joy=FIRE:30-31', '--frame-log',
                                      '--log-rows=22-22',
                                      '--log-light=%d' % FLASH_ID])
    lit = {}
    for f_, s in log:
        m = re.search(r'\[L%d (\d+)/(\d+)\]' % FLASH_ID, s)
        lit[f_] = int(m.group(1)) if m else None
    on = [f_ for f_ in sorted(lit) if lit[f_]]
    v.check(on and 30 <= on[0] <= 36 and len(on) <= 4,
            'flash: node %d lit for a moment after the shot (frames %s)'
            % (FLASH_ID, on))
    v.check(all(lit.get(f_) == 0 for f_ in range(45, 55)),
            'flash: and dark again after it')

    print('check_exit: %s' % ('PASS' if not v.bad else '%d FAILED' % v.bad))
    return 1 if v.bad else 0


if __name__ == '__main__':
    sys.exit(main())

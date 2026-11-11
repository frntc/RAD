#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_nighthawk.py - the autopilot checkride: one circuit, one landing.

tools/examples.sh runs gpu64_demo_nighthawk for 1500 frames and leaves the
verdict to whoever looks at the picture. That proves the scene draws, but
the claim the demo actually makes is that a C64 flies the aeroplane: the
flight model, the terrain height under the wheels, the touchdown rules and
the autopilot that strings them together. A picture of a jet over a field
looks the same whether or not any of that works.

So this flies the whole circuit with the autopilot (F3) -- the takeoff,
four waypoints over the land and the sea, a 3-degree final from the south
and the rollout -- and then reads the cockpit screen:

  LAND 001 CRASH 000   one landing, and nothing broke a touchdown rule
                       (CRASH 000/n names the rule if one did: see flTouch)
  FIRSTBAD 000         set-up never saw an ERRCODE
  FRAMES == OK         every frame committed

The circuit takes about 4800 frames. The run stops at 5300: past the
landing, and before the autopilot's second lap could land again.

Then the strike (key 2 selects mission 2): the same autopilot flies to the
radar site, opens the bay, drops the GBU and comes home. Row 13 must read
"M2 HIT" -- the bomb's own ballistics hit the site, nothing was scripted --
and the landing rules above apply again. It lands at about frame 4500 and
stops at 5000.

Then the two night missions. Key 3, the bridge: "M3 HIT", landed by about
6240, stopped at 6300. Key 4, the factory, where two MiGs fly a combat air
patrol over the target: the autopilot has to shoot at least one down with
its AIM-9s ("K 1" or more on row 13) and survive their IR missiles with
flares, which it can only do if the missiles, the flares and the MiGs all
work; "M4 HIT" and the landing as before. It is the long one, most of ten
minutes, so all four rides run at once.

Usage:
  tools/check_nighthawk.py --prg=games/nighthawk/gpu64_demo_nighthawk.prg
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.join(HERE, 'prgsim', 'runsim.py')
FACTORY_FRAMES = 9780      # LANDED at 9632, airborne again at 9836
RIDES = [
    # name, frames, keys, extra check (row, regex)
    ('checkride', 5300, ['--key=F3:30-35'], None),
    ('strike', 5000, ['--key=2:10-15', '--key=F3:30-35'], (13, r'M2 HIT')),
    # the night bridge: low under the radars, never seen
    ('bridge', 6300, ['--key=3:10-15', '--key=F3:30-35'], (13, r'M3 HIT')),
    # the factory: the CAP MiGs have to come down to the autopilot's
    # AIM-9s, and their IR shots to its flares, or it never lands
    ('factory', FACTORY_FRAMES, ['--key=4:10-15', '--key=F3:30-35'],
     (13, r'M4 HIT .* K [1-9]')),
]


def start(prg, name, frames, keys, extra):
    return subprocess.Popen(
        [sys.executable, SIM, prg, '--demo', '--stop-after=%d' % frames]
        + keys + ['--max', '120000000000'],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)


def ride(proc, name, frames, keys, extra):
    text = proc.communicate()[0]
    if proc.returncode != 0:
        print(text)
        return ['%s: the simulator did not return cleanly' % name]

    rows = {}
    for line in text.splitlines():
        m = re.match(r'\s*(\d+)\| ?(.*)$', line)
        if m:
            rows[int(m.group(1))] = m.group(2)

    bad = []
    m = re.search(r'LAND (\d+) CRASH (\d+)/(\d)', rows.get(8, ''))
    if not m:
        bad.append('no LAND/CRASH row: %r' % rows.get(8))
    else:
        land, crash, why = int(m.group(1)), int(m.group(2)), m.group(3)
        if land != 1:
            bad.append('%d landings, expected 1' % land)
        if crash != 0:
            bad.append('%d crashes (the last broke rule %s)' % (crash, why))
    m = re.search(r'FIRSTBAD (\d+)/', rows.get(17, ''))
    if not m or int(m.group(1)) != 0:
        bad.append('set-up failed: %r' % rows.get(17))
    m = re.search(r'FRAMES ([0-9A-F]{4}) OK ([0-9A-F]{4})', rows.get(19, ''))
    if not m or m.group(1) != m.group(2):
        bad.append('frames lost: %r' % rows.get(19))
    if extra and not re.search(extra[1], rows.get(extra[0], '')):
        bad.append('row %d lacks %r: %r' % (extra[0], extra[1],
                                            rows.get(extra[0])))

    if bad:
        print(text)
        return ['%s: %s' % (name, b) for b in bad]
    print('PASS  %s: %s' % (name, rows[8][16:].strip()))
    if extra:
        print('      %s' % rows[extra[0]].strip())
    return []


def main():
    prg = None
    for a in sys.argv[1:]:
        if a.startswith('--prg='):
            prg = a[6:]
        else:
            sys.exit('unknown argument: ' + a)
    if not prg:
        sys.exit('usage: check_nighthawk.py --prg=PRG')

    # one simulator per ride, all at once: the factory alone is ten
    # minutes, and the rides share nothing
    procs = [start(prg, *r) for r in RIDES]
    bad = []
    for p, r in zip(procs, RIDES):
        bad += ride(p, *r)
    for b in bad:
        print('FAIL  ' + b)
    if bad:
        sys.exit(1)


if __name__ == '__main__':
    main()

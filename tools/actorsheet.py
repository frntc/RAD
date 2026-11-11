#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
Renders every animation in games/quake/gpu64_quake_actors.inc through
tools/hostsim/levelsim -- the firmware's own renderer -- one contact sheet per
animation, into tools/hostsim/out/actors/. Milestone 20's stage A gate: a
wrong axis mapping, winding, frame range or skin shows up here, on a PC,
before any firmware or 6502 code exists to place the model.

    make -C tools/hostsim levelsim
    python3 tools/actorsheet.py [build/e1m1.g64lev]

Monsters are drawn from three-quarter front (yaw 150 against a camera
looking down +z), so the face and the handedness are both visible; view
models from the side, so the muzzle flash of a firing frame sits at the
muzzle and the idle frames hide it behind the eye.
"""

import os, re, subprocess, sys, tempfile
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
INC = os.path.join(ROOT, 'games', 'quake', 'gpu64_quake_actors.inc')
SIM = os.path.join(ROOT, 'tools', 'hostsim', 'levelsim')
OUT = os.path.join(ROOT, 'tools', 'hostsim', 'out', 'actors')

# key prefix -> (camera pos, actor pos, yaw, crop box on the 320x200 frame)
VIEWS = {
    'SOLDIER': ('0,0,0', '0,0,2.6', 150, (60, 0, 260, 200)),
    'DOG':     ('0,0,0', '0,0,2.6', 120, (60, 20, 260, 180)),
    'V_AXE':   ('0,-0.5,0', '-0.6,0,2.2', 90, (40, 0, 300, 200)),
    'V_':      ('0,-0.3,0', '0,0,1.3', 90, (40, 20, 280, 140)),
}


def load_inc():
    sym = {}
    for ln in open(INC):
        ln = ln.split(';')[0].strip()
        if '=' in ln:
            k, v = (t.strip() for t in ln.split('=', 1))
            sym[k] = int(v[1:], 16) if v.startswith('$') else int(v)
    return sym


def render(level, mesh, cam, pos, yaw, tmp):
    subprocess.run([SIM, level, tmp, '--no-level', '--bg=15', '--quiet',
                    '--pos=' + cam, '--actor=%d@%s,%d' % (mesh, pos, yaw),
                    '--frames=1'], check=True, stdout=subprocess.DEVNULL)
    return Image.open(os.path.join(tmp, 'level0000.ppm')).convert('RGB')


def main():
    level = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'e1m1.g64lev')
    sym = load_inc()
    os.makedirs(OUT, exist_ok=True)
    anims = []
    for k, n in sym.items():
        if not k.startswith('N_'):
            continue
        name = k[2:]
        first = sym.get('M_' + name)
        view = next((v for p, v in VIEWS.items() if name.startswith(p)), None)
        # A whole-model count (N_SOLDIER) duplicates its animations; skip it
        # where the model has named animations.
        if first is None or view is None:
            continue
        if any(o.startswith('N_' + name + '_') for o in sym):
            continue
        anims.append((name, first, n, view))
    blank = []
    with tempfile.TemporaryDirectory() as tmp:
        for name, first, n, (cam, pos, yaw, box) in anims:
            tiles = [render(level, first + i, cam, pos, yaw, tmp).crop(box)
                     for i in range(n)]
            for i, t in enumerate(tiles):
                if len(set(t.getdata())) < 2:
                    blank.append('%s frame %d' % (name, i))
            w, h = tiles[0].size
            cols = min(n, 6)
            rows = (n + cols - 1) // cols
            sheet = Image.new('RGB', (cols * w, rows * h), (40, 40, 40))
            for i, t in enumerate(tiles):
                sheet.paste(t, ((i % cols) * w, (i // cols) * h))
            path = os.path.join(OUT, name.lower() + '.png')
            sheet.save(path)
            print('%-18s mesh %3d + %2d  -> %s' % (name, first, n, os.path.relpath(path, ROOT)))
    if blank:
        print('BLANK: ' + ', '.join(blank))
        sys.exit(1)
    print('actorsheet: %d animations, no blank frames' % len(anims))


if __name__ == '__main__':
    main()

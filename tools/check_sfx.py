#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""check_sfx.py - milestone 20: does the game make the right noises?

Nobody can listen to a prgsim run, but the SID only ever hears register
writes, and runsim.py --sid-log records every one with the frame and the
IRQ tick it landed in. sfxPlay has a fingerprint: a test-bit write ($08)
to a voice's control register, then that effect's AD, SR and pulse width;
the next tick writes the first segment's control byte and frequency. This
reads gpu64_game_sfx.inc's own SOUNDS table, decodes the log back into
effect names, and checks that the scripted events made theirs:

  kill     check_combat.py's kill run: three shotgun blasts, the grunt's
           pain, its death, and the backpack (an ITEM); the doors the
           route opens, and the button it walks onto at the end.
           (--notarget: nothing wakes, so no SIGHT here.)
  respawn  four 20-point hurts are PAIN, the fifth is DIE, not PAIN too.
  dog      check_ai.py's dog run, monsters awake: SIGHT, then a BITE
           and the player's PAIN.
  swim     check_env.py's swim-out: warped into the pool and out over its
           edge, a SPLASH each way -- and the pool holds a secret trigger,
           so one SECRET.
  jump     SPACE at the start: one JUMP.

Never scripted: AXE, SSHOT, NAIL, HEALTH, WEAPON, POWER. They share the
code paths above (wpSfx, pickSfx); tools/sfx/sfxtest.prg plays them all.

It also checks that the effects really play out: every voice that starts
must drop its gate again within 120 ticks (the longest run is 64), which
catches an IRQ that stopped chaining or a run with no end row. The log
counts IRQs, and the game takes MOUSE_SUBS of them a tick (the mouse is
sampled between ticks), so that is read from the source and divided out.

Usage: tools/check_sfx.py --prg=... --level=build/e1m1.g64lev
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from check_combat import KILL, RESPAWN, Verdict        # noqa: E402
from check_ai import DOG_WARP                           # noqa: E402
from check_env import SWIMOUT                           # noqa: E402

SFX_INC = os.path.join(REPO, 'games', 'quake', 'gpu64_game_sfx.inc')
GAME_A = os.path.join(REPO, 'games', 'quake', 'gpu64_demo_game.a')


def irqs_per_tick():
    m = re.search(r'^MOUSE_SUBS\s*=\s*(\d+)', open(GAME_A,
                  encoding='latin-1').read(), re.M)
    return int(m.group(1)) if m else 1


def sounds():
    """{(AD, SR, PWhi, ctrl0, fhi0): name} from the include's own tables."""
    src = open(SFX_INC).read()
    names = {}
    for m in re.finditer(r'^SFX_(\w+)\s*=\s*(\d+)', src, re.M):
        if m.group(1) != 'COUNT':
            names[int(m.group(2))] = m.group(1)
    env = {}
    for m in re.finditer(r'^(SG_\w+|SOUNDS)\s*=\s*(\[.*\])\s*$', src, re.M):
        env[m.group(1)] = eval(m.group(2).replace('$', '0x'), {}, env)
    sig = {}
    for i, (_pri, ad, sr, pw, segs) in enumerate(env['SOUNDS']):
        key = (ad, sr, pw, segs[0][1], segs[0][2] >> 8)
        if key in sig:
            sys.exit('check_sfx.py: %s and %s share a fingerprint'
                     % (sig[key], names[i]))
        sig[key] = names[i]
    return sig


def decode(log, sig):
    """[(frame, name)] per effect started, and the voices that never
    released."""
    writes = []
    per = irqs_per_tick()
    for line in open(log):
        f, t, reg, val = line.split()
        writes.append((int(f), int(t) // per, int(reg, 16), int(val, 16)))
    out, stuck = [], []
    for i, (f, t, reg, val) in enumerate(writes):
        if reg > 20 or reg % 7 != 4 or val != 0x08:
            continue
        v = reg - 4
        ad = sr = pw = ctrl = fhi = None
        end = None
        again = False
        for f2, t2, r2, v2 in writes[i + 1:]:
            if r2 == v + 4 and v2 == 0x08:
                again = ctrl is None            # restarted before it played
                end = t2                        # or cut short: also an end
                break
            if r2 == v + 5 and ad is None:
                ad = v2
            elif r2 == v + 6 and sr is None:
                sr = v2
            elif r2 == v + 3 and pw is None:
                pw = v2
            elif r2 == v + 4 and ctrl is None:
                ctrl = v2
            elif r2 == v + 1 and ctrl is not None and fhi is None:
                fhi = v2
            elif r2 == v + 4 and not v2 & 1:
                end = t2
                break
        if again:
            continue                    # the same effect twice in a frame
        name = sig.get((ad, sr, pw, ctrl, fhi), '?%s' % ((ad, sr, pw, ctrl,
                                                           fhi),))
        out.append((f, name))
        if ctrl is not None and (end is None or end - t > 120):
            if end is not None or writes[-1][1] - t > 120:
                stuck.append((f, name))
    return out, stuck


def run(prg, level, frame, keys, target=False):
    with tempfile.NamedTemporaryFile(suffix='.log', delete=False) as tf:
        log = tf.name
    cmd = [sys.executable, os.path.join(HERE, 'prgsim', 'runsim.py'), prg,
           '--demo', '--frame-ms=32', '--stop-after=%d' % frame,
           '--level=' + level, '--sid-log=' + log] + keys + (
               [] if target else ['--notarget'])
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stdout.write(r.stdout)
        sys.stderr.write(r.stderr)
        sys.exit('check_sfx.py: the simulated run failed')
    return log


def main():
    prg = level = None
    for a in sys.argv[1:]:
        if a.startswith('--prg='):
            prg = a[6:]
        elif a.startswith('--level='):
            level = a[8:]
        else:
            sys.exit('check_sfx.py: unknown argument %s' % a)
    if not (prg and level):
        sys.exit('check_sfx.py: --prg and --level are required')
    sig = sounds()
    v = Verdict()

    def played(frame, keys, what, target=False):
        log = run(prg, level, frame, keys, target)
        got, stuck = decode(log, sig)
        os.unlink(log)
        names = [n for _, n in got]
        print('     %s: %s' % (what, ' '.join(
            '%s@%d' % (n, f) for f, n in got) or '(silence)'))
        v.check(not stuck, '%s: every effect released its gate%s'
                % (what, '' if not stuck else ' (%s)' % stuck))
        v.check(not [n for n in names if n.startswith('?')],
                '%s: every start decodes to an effect' % what)
        return names

    n = played(340, KILL, 'kill')
    v.check(n.count('SHOT') == 3, 'three shotgun blasts (%d)'
            % n.count('SHOT'))
    v.check(n.count('MPAIN') == 1 and n.count('MDIE') == 1,
            'the grunt flinched once and died once')
    v.check(n.index('MPAIN') < n.index('MDIE') if 'MPAIN' in n and 'MDIE'
            in n else False, 'pain before death')
    v.check(n.count('ITEM') == 1, 'the backpack was an ITEM')
    v.check(n.count('DOOR') >= 1 and n.count('BUTTON') == 1,
            'the route opened doors and pressed the button')

    n = played(110, RESPAWN, 'respawn')
    v.check(n.count('PAIN') == 4 and n.count('DIE') == 1,
            'four hurts and a death: PAIN x4, DIE x1 (%d, %d)'
            % (n.count('PAIN'), n.count('DIE')))

    n = played(120, DOG_WARP, 'dog', target=True)
    v.check(n.count('SIGHT') >= 1, 'the dog saw the player (SIGHT)')
    v.check(n.count('BITE') >= 1 and n.count('PAIN') >= 1,
            'it bit, and the player felt it')

    n = played(90, SWIMOUT, 'swim')
    v.check(n.count('SPLASH') == 2, 'in the pool and out again: two'
            ' SPLASHes (%d)' % n.count('SPLASH'))
    v.check(n.count('SECRET') == 1, 'the underwater secret (%d)'
            % n.count('SECRET'))

    n = played(60, ['--key=SPACE:20-30'], 'jump')
    v.check(n.count('JUMP') == 1, 'one press, one JUMP (%d)'
            % n.count('JUMP'))
    return 1 if v.bad else 0


if __name__ == '__main__':
    sys.exit(main())

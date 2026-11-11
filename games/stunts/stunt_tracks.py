#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
The eight tracks of the league: their centre lines, heights, gaps,
drawbridges, the AI's hints, and the chunk meshes gpu64 draws them with.

Every track is 256 points round, SEG world units apart, so the physics
constants of gpu64_demo_stunt.a hold on all of them and a car's place on a
track is still one byte of segment and two of fraction.

A track is laid out by a turtle: straights and arcs, with two of the
straights left free and solved for so that the loop closes exactly. The
whole loop is then scaled to N * SEG. Heights are keys along the point
index, eased by a cosine unless a key asks to be linear (a ramp's lip has
to keep climbing right to its edge). Gaps are segments with no road. A
drawbridge is eight segments the C64 moves: two decks hinged at their outer
ends, whose heights it rewrites in the track tables every frame.

The layouts are the original's: their plans and their features read off
its own track previews, then fitted to the one length every track here
has.
"""

import math

N      = 256
SEG    = 1.5
HALFW  = 1.5
THICK  = 0.3
CHUNK  = 16
NCHUNK = N // CHUNK
L      = N * SEG

DECK_SEGS = 4               # a drawbridge is two decks of four segments
DECK_STEPS = 16             # the C64 raises it in this many steps
DECK_MAX = 22               # binary degrees (256 a turn): about 31 degrees

# flTab bits
FL_GAP = 0x01               # no road here
FL_DECK = 0x02              # a drawbridge segment: the C64 owns its height

# aiFlg bits
AI_BOOST = 0x01             # use the boost here
AI_SINGLE = 0x02            # stay on the centre line: no overtaking lane


# ------------------------------------------------------------------ turtle
def layout(pieces, h0=0.0):
    """pieces: ('S', length or None) / ('A', degrees, radius). Exactly two
    straights may be None; they are solved so the loop closes. Returns a
    function s -> (x, z, heading) over [0, L), recentred on the origin,
    and the start arc length of every piece, in points."""
    # headings at the start of every piece
    hs, h = [], h0
    for p in pieces:
        hs.append(h)
        if p[0] == 'A':
            h += math.radians(p[1])
    assert abs(math.remainder(h - h0, 2 * math.pi)) < 1e-9, "turns must add to whole turns"

    def arc_disp(h, deg, r):
        a = math.radians(deg)
        k = r * abs(a) / a
        return k * (math.cos(h) - math.cos(h + a)), k * (math.sin(h + a) - math.sin(h))

    fx = fz = 0.0
    free = []
    for p, h in zip(pieces, hs):
        if p[0] == 'S':
            if p[1] is None:
                free.append(h)
            else:
                fx += math.sin(h) * p[1]
                fz += math.cos(h) * p[1]
        else:
            dx, dz = arc_disp(h, p[1], p[2])
            fx += dx
            fz += dz
    lens = []
    if free:
        assert len(free) == 2
        a, b = free
        det = math.sin(a) * math.cos(b) - math.sin(b) * math.cos(a)
        assert abs(det) > 0.1, "free straights are parallel"
        la = (-fx * math.cos(b) + fz * math.sin(b)) / det
        lb = (-math.sin(a) * fz + math.cos(a) * fx) / det
        assert la > 0 and lb > 0, (la, lb)
        lens = [la, lb]
    else:
        assert abs(fx) < 1e-6 and abs(fz) < 1e-6, (fx, fz)

    # resolved pieces, then the scale that makes the loop L long
    res = []
    for p in pieces:
        if p[0] == 'S':
            res.append(('S', lens.pop(0) if p[1] is None else p[1]))
        else:
            res.append(('A', p[1], p[2]))
    total = sum(p[1] if p[0] == 'S' else abs(math.radians(p[1])) * p[2] for p in res)
    k = L / total

    # walk it, densely
    pts, starts = [], []
    x = z = 0.0
    h = h0
    s = 0.0
    STEP = 0.05
    for p in res:
        starts.append(s / SEG)
        if p[0] == 'S':
            ln = p[1] * k
            n = max(1, int(round(ln / STEP)))
            for i in range(n):
                pts.append((s + ln * i / n, x + math.sin(h) * ln * i / n,
                            z + math.cos(h) * ln * i / n, h))
            x += math.sin(h) * ln
            z += math.cos(h) * ln
            s += ln
        else:
            a = math.radians(p[1])
            r = p[2] * k
            ln = abs(a) * r
            n = max(1, int(round(ln / STEP)))
            cx_, cz_ = x, z
            for i in range(n):
                hh = h + a * (i + 0.5) / n
                pts.append((s + ln * i / n, cx_, cz_, h + a * i / n))
                cx_ += math.sin(hh) * ln / n
                cz_ += math.cos(hh) * ln / n
            x, z = cx_, cz_
            h += a
            s += ln
    assert abs(x) < 0.05 and abs(z) < 0.05, ("loop does not close", x, z)
    xs = [p[1] for p in pts]
    zs = [p[2] for p in pts]
    ox = (min(xs) + max(xs)) / 2
    oz = (min(zs) + max(zs)) / 2

    def at(sq):
        sq %= L
        lo, hi = 0, len(pts) - 1
        while lo < hi:
            mid = (lo + hi + 1) // 2
            if pts[mid][0] <= sq:
                lo = mid
            else:
                hi = mid - 1
        s0, x0, z0, h0_ = pts[lo]
        d = sq - s0
        return (x0 + math.sin(h0_) * d - ox, z0 + math.cos(h0_) * d - oz, h0_)
    return at, starts


def smooth(a, b, t):
    t = max(0.0, min(1.0, t))
    return a + (b - a) * (1 - math.cos(t * math.pi)) / 2


# -------------------------------------------------------------- the eight
class Track:
    """name, pieces, keys(P) -> [(point, height[, 'lin'])], gaps(P) ->
    segments, humps(P) -> [(first, last, amplitude, period)], deck(P) ->
    first segment of a drawbridge or None, ai(P, tables) for anything the
    automatic hints miss."""
    def __init__(self, name, pieces, keys, gaps=lambda P: [], humps=lambda P: [],
                 deck=lambda P: None, h0=0.0, side=None):
        self.name, self.pieces, self.keys_fn = name, pieces, keys
        self.gaps_fn, self.humps_fn, self.deck_fn, self.h0 = gaps, humps, deck, h0


def r(x):
    return int(round(x))


def at(P, k, f):
    """The point a fraction f of the way along piece k."""
    return r(P[k] + (P[k + 1] - P[k]) * f)


E, W = math.pi / 2, -math.pi / 2        # h0: the first straight heads east / west

# The plans are read off the original's own track previews, seen from the
# south as it shows them: the near straight is the bottom of its picture.
TRACKS = [
    # 1 -- Division 4. A teardrop: the near straight runs west to a tight
    # tip, the far side climbs back diagonally over the little ramp, and
    # the wide east end is the high ground.
    Track("LITTLE RAMP",
          [('S', None), ('A', 155, 9), ('S', None), ('A', 205, 33)],
          lambda P: little_keys(P),
          humps=lambda P: crests(P, [(0, 0.3), (0, 0.62)]),
          gaps=lambda P: range(little_gap(P), little_gap(P) + 2), h0=W),

    # 2 -- Division 4. A rounded rectangle on stilts. Along the near
    # straight the road is three blocks with gaps of air between them, each
    # with a lip: too slow and the next block is above you.
    Track("STEPPING STONES",
          [('S', 120), ('A', -90, 14), ('S', 26), ('A', -90, 14),
           ('S', None), ('A', -90, 14), ('S', None), ('A', -90, 14)],
          lambda P: stones_keys(P),
          humps=lambda P: crests(P, [(0, 0.3), (0, 0.62)]),
          gaps=lambda P: stones_gaps(P), h0=W),

    # 3 -- Division 3. An oval with one great hump in the near straight:
    # taken flat out it throws you. The far side is a raised run that ends
    # in a step down onto the west bend.
    Track("HUMP BACK",
          [('S', None), ('A', -190, 36), ('S', None), ('A', -170, 32)],
          lambda P: hump_keys(P),
          humps=lambda P: crests(P, [(2, 0.35), (2, 0.7)]), h0=E),

    # 4 -- Division 3. Bumps down the near straight, then up onto a long
    # plateau along the far side that ends in the big ramp: a lip, three
    # segments of nothing, and a landing ramp running down to the east bend.
    Track("BIG RAMP",
          [('S', 130), ('A', 180, 30), ('S', None), ('A', 30, 30), ('S', None), ('A', 150, 16)],
          lambda P: bigramp_keys(P),
          gaps=lambda P: range(bigramp_gap(P), bigramp_gap(P) + 3), h0=W),

    # 5 -- Division 2. A long thin loop: a climb all the way up one side
    # to a hairpin in the sky, then the slope down the other, a kicker and
    # a long flight to a landing that is still going downhill.
    Track("SKI JUMP",
          [('S', 150), ('A', -180, 9), ('S', 150), ('A', -180, 9)],
          lambda P: ski_keys(P),
          humps=lambda P: crests(P, [(0, 0.45), (0, 0.75)]),
          gaps=lambda P: range(ski_gap(P), ski_gap(P) + 4), h0=math.radians(70)),

    # 6 -- Division 2. A flat near straight between two high loops, and
    # back along the middle the drawbridge. Down, it is road. Rising, it
    # is a ramp. Up, it is a gap you jump with everything you have.
    Track("DRAW BRIDGE",
          [('S', 130), ('A', -90, 14), ('S', None), ('A', -180, 14), ('S', 8), ('A', 90, 10),
           ('S', None), ('A', -180, 10)],
          lambda P: draw_keys(P),
          humps=lambda P: crests(P, [(0, 0.3), (0, 0.5), (0, 0.7)]),
          deck=lambda P: draw_deck(P), h0=E),

    # 7 -- Division 1. An oval with the high jump in the near straight: a
    # ramp, a tower standing in the gap, and a landing ramp beyond it.
    Track("HIGH JUMP",
          [('S', 70), ('A', -60, 45), ('S', None), ('A', -120, 18), ('S', None), ('A', -180, 32)],
          lambda P: high_keys(P),
          humps=lambda P: crests(P, [(0, 0.4), (0, 0.75), (2, 0.5)]),
          gaps=lambda P: high_gaps(P), h0=W),

    # 8 -- Division 1. The near straight climbs to the top of the sky, the
    # east bend turns up there, and the far side comes down in dips and
    # crests to a serpentine of high hairpins in the west.
    Track("ROLLER COASTER",
          [('S', None), ('A', -170, 22), ('S', None), ('A', -180, 10), ('S', 30), ('A', 180, 10),
           ('S', 30), ('A', -190, 12)],
          lambda P: roller_keys(P),
          humps=lambda P: crests(P, [(0, 0.35), (0, 0.7), (2, 0.55)]), h0=E),
]


def crests(P, spots, amp=0.9, per=10):
    """Sharp crests, a half sine each, at (piece, fraction): taken at
    cruise the car follows one; flat out (vHi past about 53) it flies."""
    return [(at(P, k, f) - per // 4, at(P, k, f) - per // 4 + per // 2, amp, per) for k, f in spots]


def little_gap(P):
    return at(P, 2, 0.45)


def little_keys(P):
    a = little_gap(P)
    return [(0, 3.0), (r(P[1]) - 4, 3.0), (a - 16, 3.0), (a, 5.6, 'lin'), (a + 3, 4.9),
            (a + 5, 4.8), (a + 20, 3.4), (r(P[3]) + 8, 4.6), (r(P[3]) + 40, 4.6),
            (256 - 12, 3.0), (256, 3.0)]


def stones_keys(P):
    a = at(P, 4, 0.3)
    ks = [(0, 3.6), (r(P[1]) - 8, 3.6), (r(P[1]) + 6, 4.6), (r(P[3]) + 8, 4.6),
          (a - 2, 4.4), (a, 4.6, 'lin')]
    i = a + 2                       # gap, block, gap, block, gap, block, gap
    for h in (4.2, 4.6, 4.3):
        ks += [(i + 1, h), (i + 4, h), (i + 5, h + 0.3, 'lin')]   # a lip at its end
        i += 7
    ks += [(i + 1, 4.4), (r(P[5]) + 6, 4.6), (r(P[7]) - 2, 4.6), (r(P[7]) + 12, 3.6), (256, 3.6)]
    return ks


def stones_gaps(P):
    a = at(P, 4, 0.3)
    return [a + 7 * k + j for k in range(4) for j in range(2)]


def hump_keys(P):
    m = at(P, 0, 0.55)
    s = r(P[3])
    return [(0, 3.0), (m - 26, 3.0), (m, 7.6), (m + 26, 3.0), (r(P[1]) + 10, 3.4),
            (at(P, 2, 0.2), 6.6), (at(P, 2, 0.5), 6.4), (at(P, 2, 0.9), 5.6),
            (s - 1, 5.4, 'lin'), (s + 2, 3.0, 'lin'), (256, 3.0)]


def bigramp_gap(P):
    return at(P, 2, 0.95)


def bigramp_keys(P):
    g = bigramp_gap(P)
    ks = [(0, 3.0)]
    for f in (0.2, 0.4, 0.6, 0.8):
        c = at(P, 0, f)
        ks += [(c - 4, 3.0), (c, 4.2), (c + 4, 3.0)]
    return ks + [(r(P[1]) + 4, 3.0), (r(P[2]) - 8, 8.0), (g - 2, 8.0), (g, 8.6, 'lin'),
                 (g + 3, 6.8), (g + 5, 6.6, 'lin'), (at(P, 4, 0.9), 3.4), (256, 3.0)]


def ski_gap(P):
    return at(P, 2, 0.55)


def ski_keys(P):
    g = ski_gap(P)
    return [(0, 3.0), (at(P, 0, 0.1), 3.0), (r(P[1]), 12.5), (r(P[2]) + 4, 12.5),
            (g - 14, 9.5), (g - 6, 6.5, 'lin'), (g, 7.2, 'lin'),       # the slope, then the kicker
            (g + 4, 5.0), (g + 6, 4.6, 'lin'), (g + 18, 3.0), (256, 3.0)]


def draw_deck(P):
    return at(P, 6, 0.5) - DECK_SEGS


def draw_keys(P):
    d = draw_deck(P)
    return [(0, 3.0), (r(P[1]) - 6, 3.0), (r(P[2]) + 4, 7.5), (r(P[4]), 7.5),
            (d - 10, 5.0), (d + 2 * DECK_SEGS + 10, 5.0), (r(P[7]) - 2, 10.5), (r(P[7]) + 4, 10.5),
            (256, 3.0)]


def high_keys(P):
    a = at(P, 4, 0.5)
    return [(0, 3.0), (a - 18, 3.0), (a, 8.0, 'lin'),
            (a + 4, 7.0), (a + 6, 7.0),                         # the tower
            (a + 11, 6.8), (a + 13, 6.4, 'lin'), (a + 30, 3.0), (256, 3.0)]


def high_gaps(P):
    a = at(P, 4, 0.5)
    return [a + k for k in range(4)]          # the tower is the landing's top


def roller_keys(P):
    return [(0, 3.0), (6, 3.0), (r(P[1]) - 2, 12.5), (r(P[2]) + 2, 12.5),
            (at(P, 2, 0.3), 7.0), (at(P, 2, 0.55), 10.0), (at(P, 2, 0.8), 6.0),
            (r(P[3]) + 6, 8.0), (r(P[5]), 5.0), (r(P[6]) + 4, 7.0), (r(P[7]), 4.0), (256, 3.0)]


# ------------------------------------------------------------- one track
def q88(v):
    rr = int(round(v * 256))
    assert -32768 <= rr <= 32767, v
    return rr


def ang8(rad):
    return int(round(rad * 128 / math.pi)) & 0xff


class Built:
    pass


def build(t):
    at, starts = layout(t.pieces, t.h0)
    P = starts
    keys = sorted(t.keys_fn(P), key=lambda k: k[0])
    gaps = set(t.gaps_fn(P))
    humps = t.humps_fn(P)
    deck = t.deck_fn(P)
    deckset = set(range(deck, deck + 2 * DECK_SEGS)) if deck is not None else set()

    def height(i):
        y = keys[-1][1]
        for ka, kb in zip(keys, keys[1:]):
            a, ha, b, hb = ka[0], ka[1], kb[0], kb[1]
            if a <= i <= b and b > a:
                tt = (i - a) / (b - a)
                y = ha + (hb - ha) * tt if len(kb) > 2 else smooth(ha, hb, tt)
                break
        for h0, h1, amp, per in humps:
            if h0 <= i <= h1:
                y += amp * math.sin((i - h0) * 2 * math.pi / per)
        return y

    pts = []
    for i in range(N):
        x, z, hd = at(i * SEG)
        pts.append((x, z, hd, height(i)))

    b = Built()
    b.name = t.name
    b.pts = pts
    b.gaps = gaps
    b.deck = deck
    b.deckset = deckset
    b.cx = [q88(p[0]) for p in pts]
    b.cz = [q88(p[1]) for p in pts]
    b.cy = [q88(p[3]) for p in pts]
    b.hd16 = [int(round(p[2] * 32768 / math.pi)) & 0xffff for p in pts]
    b.dh16 = [((b.hd16[(i + 1) % N] - b.hd16[i] + 32768) & 0xffff) - 32768 for i in range(N)]
    b.pt, b.fl = [], []
    for i in range(N):
        j = (i + 1) % N
        dx, dz = (b.cx[j] - b.cx[i]) / 256, (b.cz[j] - b.cz[i]) / 256
        dy = (b.cy[j] - b.cy[i]) / 256
        b.pt.append(ang8(math.atan2(-dy, math.hypot(dx, dz))))
        b.fl.append((FL_GAP if i in gaps else 0) | (FL_DECK if i in deckset else 0))

    # where the road crosses itself, the upper one is a deck on the lower
    # (dilated a segment each way, so no wall slopes down across the lower road)
    ov = [over_road(pts, i) for i in range(N)]
    b.over = [ov[i] or ov[(i - 1) % N] or ov[(i + 1) % N] for i in range(N)]
    for i in range(N):
        if b.over[i]:
            for j in range(N):
                if min((i - j) % N, (j - i) % N) >= 8 and \
                   math.hypot(pts[i][0] - pts[j][0], pts[i][1] - pts[j][1]) < HALFW + 1.5:
                    assert pts[i][3] - pts[j][3] > 4.0, (t.name, "bridge clearance", i, j)

    # the minimum radius, for the steering
    b.minr = min(SEG / max(1e-9, abs(b.dh16[i]) * math.pi / 32768) for i in range(N))
    b.ai_spd, b.ai_flg = ai_tables(b)
    b.chunks = chunk_meshes(b)
    b.decks = deck_meshes(b) if deck is not None else []
    return b


def over_road(pts, i):
    for j in range(N):
        d = min((i - j) % N, (j - i) % N)
        if d < 8:
            continue
        # the footprints overlap: both half-widths, plus the obliquity margin
        if math.hypot(pts[i][0] - pts[j][0], pts[i][1] - pts[j][1]) < 2 * HALFW + 3.0 \
           and pts[j][3] < pts[i][3] - 0.5:
            return True
    return False


# ------------------------------------------------------------------ the AI
CRUISE = 0x3c
FLAT_OUT = 0x60
CREST = 0x30
BEND_JUMP = 0x30            # a jump on a bend: the car flies straight while
AI_STEER, GRIP_K = 320, 12000   # gpu64_demo_stunt.a's
BEND_SLOW = 900             # |dh16| from which a bend caps the autopilot
BEND_DH = 300               #   the road turns under it, so no faster than it
                            #   takes to clear the gap (|dh16| past BEND_DH)


def ai_tables(b):
    """aiSpd: the speed (vHi) the autopilot holds at each segment, 0 for
    its cruise. aiFlg: boost here, and no overtaking lane here. Run-ups to
    every gap or drawbridge are flat out on the boost and single file,
    except a jump on a bend, taken just fast enough;
    crests are capped, because flying off every one breaks the car."""
    spd = [0] * N
    flg = [0] * N
    jumps = sorted(i for i in range(N) if b.fl[i] & (FL_GAP | FL_DECK))
    for g in jumps:
        if b.fl[(g - 1) % N] & (FL_GAP | FL_DECK):
            continue                        # only the first segment of each
        bent = any(abs(b.dh16[(g + k) % N]) > BEND_DH for k in range(-2, 5))
        v = BEND_JUMP if bent else FLAT_OUT
        for k in range(1, 18):
            i = (g - k) % N
            spd[i] = v
            flg[i] |= AI_BOOST | AI_SINGLE
        for k in range(0, 8):
            i = (g + k) % N
            spd[i] = v
            flg[i] |= AI_SINGLE
    # tight bends: no faster than the autopilot can turn (AI_STEER a frame
    # against the road's dh * vHi / 256) or the tyres hold (GRIP_K / vHi at
    # full lock), with a margin, and from far enough ahead to brake
    for i in range(N):
        dh = max(abs(b.dh16[(i + k) % N]) for k in range(0, 10))
        if dh < BEND_SLOW:
            continue
        v = int(min(0.85 * AI_STEER * 256 / dh,
                    math.sqrt(0.85 * GRIP_K * 2 * 120 / 256 * 256 / dh)))
        spd[i] = min(spd[i], v) if spd[i] else v
    # crests: where the road drops away ahead faster than the car falls
    for i in range(N):
        if spd[i]:
            continue
        # curvature of the height profile, per segment squared
        y0, y1, y2 = (b.cy[(i + k) % N] / 256 for k in (0, 2, 4))
        bend = (y2 - 2 * y1 + y0) / 4
        if bend < -0.035:
            for k in range(-6, 1):
                j = (i + k) % N
                if not (flg[j] & AI_BOOST):
                    spd[j] = CREST
    return spd, flg


# ------------------------------------------------------------------ meshes
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

FLAT = 0x02
DOUBLE = 0x01
UNLIT = 0x04

TEX_ROAD, TEX_START, TEX_SIDE = 1, 2, 3
UNDER_COL = None            # set by gen_stunt: the deck's underside colour


class Mesh:
    def __init__(self):
        self.v = []
        self.f = []
    def vert(self, p):
        self.v.append(p)
        return len(self.v) - 1
    def tri(self, a, b, c, uva, uvb, uvc, tex, flags, out, ref=None):
        pa, pb, pc = ref if ref else (self.v[a], self.v[b], self.v[c])
        n = cross(sub(pb, pa), sub(pc, pa))
        if dot(n, out) < 0:
            b, c, uvb, uvc = c, b, uvc, uvb
        self.f.append((a, b, c, uva, uvb, uvc, tex, flags))
    def quad(self, a, b, c, d, uv, tex, flags, out, ref=None):
        uv = uv or [(0, 0)] * 4
        r1 = (ref[0], ref[1], ref[2]) if ref else None
        r2 = (ref[0], ref[2], ref[3]) if ref else None
        self.tri(a, b, c, uv[0], uv[1], uv[2], tex, flags, out, r1)
        self.tri(a, c, d, uv[0], uv[2], uv[3], tex, flags, out, r2)
    def vblob(self):
        out = []
        for p in self.v:
            for c in p:
                rr = q88(c)
                assert -128 * 256 < rr < 128 * 256, p
                out += [rr & 0xff, (rr >> 8) & 0xff]
        assert len(self.v) <= 256, len(self.v)
        return bytes(out)
    def fblob(self):
        out = []
        for (a, b, c, ua, ub, uc, t, fl) in self.f:
            for uv in (ua, ub, uc):
                assert 0 <= uv[0] <= 255 and 0 <= uv[1] <= 255, uv
            out += [a, b, c, ua[0], ua[1], ub[0], ub[1], uc[0], uc[1], t, fl, 0]
        assert len(self.f) <= 255, len(self.f)
        return bytes(out)


def right(pts, i):
    hd = pts[i % N][2]
    return (math.cos(hd), 0.0, -math.sin(hd))


def chunk_meshes(b):
    pts, cx, cz, cy = b.pts, b.cx, b.cz, b.cy
    hole = lambda i: (i % N) in b.gaps or (i % N) in b.deckset
    chunks = []
    for c in range(NCHUNK):
        i0 = c * CHUNK
        xs = [pts[(i0 + k) % N][0] for k in range(CHUNK + 1)]
        zs = [pts[(i0 + k) % N][1] for k in range(CHUNK + 1)]
        ox = round((min(xs) + max(xs)) / 2)
        oz = round((min(zs) + max(zs)) / 2)
        m = Mesh()
        sec = []
        for k in range(CHUNK + 1):
            i = (i0 + k) % N
            x, z = cx[i] / 256 - ox, cz[i] / 256 - oz
            y = cy[i] / 256
            rv = right(pts, i)
            tl = m.vert((x - HALFW * rv[0], y, z - HALFW * rv[2]))
            tr = m.vert((x + HALFW * rv[0], y, z + HALFW * rv[2]))
            yb = y - THICK if b.over[i] else 0.0
            bl = m.vert((x - HALFW * rv[0], yb, z - HALFW * rv[2]))
            br = m.vert((x + HALFW * rv[0], yb, z + HALFW * rv[2]))
            sec.append((tl, tr, bl, br, rv))
        for k in range(CHUNK):
            i = i0 + k
            if hole(i):
                continue
            tl0, tr0, bl0, br0, r0 = sec[k]
            tl1, tr1, bl1, br1, r1 = sec[k + 1]
            rv = ((r0[0] + r1[0]) / 2, 0, (r0[2] + r1[2]) / 2)
            lv = (-rv[0], 0, -rv[2])
            v0, v1 = 8 * k, 8 * k + 8
            tx = TEX_START if i == 0 else TEX_ROAD
            m.quad(tl0, tr0, tr1, tl1, [(0, v0), (31, v0), (31, v1), (0, v1)], tx, 0, (0, 1, 0))
            m.quad(bl0, br0, br1, bl1, None, UNDER_COL, FLAT, (0, -1, 0))
            suv = [(v0 % 32, 0), (v0 % 32 + 8, 0), (v0 % 32 + 8, 31), (v0 % 32, 31)]
            m.quad(tl0, tl1, bl1, bl0, suv, TEX_SIDE, 0, lv)
            m.quad(tr0, tr1, br1, br0, suv, TEX_SIDE, 0, rv)
        # end caps where the road stops
        for k in range(CHUNK + 1):
            i = i0 + k
            before, after = hole(i - 1), hole(i)
            if before == after or k == CHUNK and not after or k == 0 and not before:
                continue
            tl, tr, bl, br, rv = sec[k]
            fwd = (math.sin(pts[i % N][2]), 0, math.cos(pts[i % N][2]))
            out = fwd if after else (-fwd[0], 0, -fwd[2])
            m.quad(tl, tr, br, bl, [(0, 0), (31, 0), (31, 31), (0, 31)], TEX_SIDE, 0, out)
        chunks.append((ox, oz, m))
    return chunks


def deck_meshes(b):
    """Two decks, each in its own axes: the hinge at the origin, +z along
    the road. Deck 1 reaches forward from its hinge, deck 2 back from its
    own, so raising both is a negative pitch on one and a positive one on
    the other."""
    out = []
    ln = DECK_SEGS * SEG
    for sign in (1, -1):
        m = Mesh()
        z0, z1 = (0.0, ln) if sign > 0 else (-ln, 0.0)
        c = {}
        for ix, x in enumerate((-HALFW, HALFW)):
            for iy, y in enumerate((-THICK, 0.0)):
                for iz, z in enumerate((z0, z1)):
                    c[ix, iy, iz] = m.vert((x, y, z))
        # the road on top, the stripes running along it
        m.quad(c[0, 1, 0], c[1, 1, 0], c[1, 1, 1], c[0, 1, 1],
               [(0, 0), (31, 0), (31, 31), (0, 31)], TEX_ROAD, 0, (0, 1, 0))
        m.quad(c[0, 0, 0], c[1, 0, 0], c[1, 0, 1], c[0, 0, 1], None, UNDER_COL, FLAT, (0, -1, 0))
        for ix, nx in ((0, -1), (1, 1)):
            m.quad(c[ix, 0, 0], c[ix, 1, 0], c[ix, 1, 1], c[ix, 0, 1],
                   [(0, 24), (0, 31), (31, 31), (31, 24)], TEX_SIDE, 0, (nx, 0, 0))
        for iz, nz in ((0, -1), (1, 1)):
            m.quad(c[0, 0, iz], c[1, 0, iz], c[1, 1, iz], c[0, 1, iz], None, UNDER_COL, FLAT, (0, 0, nz))
        out.append(m)
    return out


def deck_tables():
    """Per raise step: the deck's pitch (binary degrees) and the rise of
    one segment along it, 8.8."""
    pitch, rise = [], []
    for k in range(DECK_STEPS):
        p = int(round(DECK_MAX * k / (DECK_STEPS - 1)))
        pitch.append(p)
        rise.append(q88(SEG * math.tan(p * math.pi / 128)))
    return pitch, rise


if __name__ == "__main__":
    for t in TRACKS:
        b = build(t)
        ys = [v / 256 for v in b.cy]
        print("%-16s minR %5.1f  y %4.1f..%4.1f  gaps %s  deck %s  faces %d  verts %s"
              % (t.name, b.minr, min(ys), max(ys), sorted(b.gaps), b.deck,
                 sum(len(m.f) for _, _, m in b.chunks),
                 max(len(m.v) for _, _, m in b.chunks)))

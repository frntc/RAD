# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
stunt_pics.py - stunts.reu: the game's HDMI pictures, in a level pack.

The pack holds one level, named STNT, with no geometry at all: just the
game's palette and the pictures the title draws with LEVEL_PICTURE ($1A).
The player selects stunts.reu as the REU image in the RAD menu, with the
PRG; gpu64 reads levels and pictures from that image and nowhere else.

Pictures (indices are the PIC_* the title is assembled against):

  0 PIC_LOGO   296x96  the HONDANI logo: index LOGO_BASE + C64 cell column
                       where lit, 0 elsewhere -- the title sweeps it with
                       PAL_LOAD, as Quake's does (tools/hondani.py)
  1 PIC_START  320x200 the start screen, after the 1989 box art's layout:
                       the name in chunky letters over the track, a car in
                       the air off the ramp. Our own drawing.
  2 PIC_CREDS  320x200 the credits, in a chequered frame like the
                       original's track preview screen
  3 PIC_FIRE   306x18  "PRESS FIRE TO START", keyed: 255 is transparent
  4 PIC_MENU   320x200 every menu's background: the chequered frame round
                       a plain PANEL, which the menus' text is drawn on
  5 PIC_TRACK  +t      160x96 track t seen from above and to the south,
                       on PANEL, for the pre-race screen

Everything is in the game's palette (gen_stunt.py's 8 hues x 32 levels),
so PIC_START and PIC_CREDS are drawn with flag bit 1, which installs it.
Text is the C64 character ROM (tools/prgsim/gpu64font.py), as gpu64's own
HDMI text is.
"""
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'tools'))
sys.path.insert(0, os.path.join(HERE, '..', 'tools', 'prgsim'))

import hondani                  # noqa: E402
import pack_levels              # noqa: E402
from gpu64font import FONT, ASCII_TO_SCREEN     # noqa: E402

W, H = 320, 200
KEY = 255
PACK_NAME = 'STNT'
PIC_LOGO, PIC_START, PIC_CREDS, PIC_FIRE, PIC_MENU, PIC_TRACK = range(6)
TRK_W, TRK_H = 160, 96

# gen_stunt.py's hues
GRASS, GREY, SKY, RED, BLUE, YELLOW, KHAKI, WHITE = range(8)


def col(h, l):
    # 255 (white, level 31) is the transparent key: white stops at 30
    return min(254, h * 32 + max(0, min(31, int(l))))


# ------------------------------------------------------------- drawing
class Canvas:
    def __init__(self, w, h, fill=0):
        self.w, self.h = w, h
        self.px = bytearray([fill]) * (w * h)

    def put(self, x, y, c):
        if 0 <= x < self.w and 0 <= y < self.h:
            self.px[y * self.w + x] = c

    def get(self, x, y):
        return self.px[y * self.w + x]

    def rect(self, x0, y0, x1, y1, c):
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                self.px[y * self.w + x] = c(x, y) if callable(c) else c

    def poly(self, pts, c):
        """Even-odd scanline fill at pixel centres; c is an index or
        c(x, y) -> index."""
        ys = [p[1] for p in pts]
        n = len(pts)
        for y in range(max(0, int(math.floor(min(ys)))),
                       min(self.h, int(math.ceil(max(ys))) + 1)):
            yc = y + 0.5
            xs = []
            for i in range(n):
                (xa, ya), (xb, yb) = pts[i], pts[(i + 1) % n]
                if (ya <= yc < yb) or (yb <= yc < ya):
                    xs.append(xa + (yc - ya) * (xb - xa) / (yb - ya))
            xs.sort()
            for a, b in zip(xs[0::2], xs[1::2]):
                for x in range(max(0, int(math.ceil(a - 0.5))),
                               min(self.w, int(math.ceil(b - 0.5)))):
                    self.px[y * self.w + x] = c(x, y) if callable(c) else c


def glyph(ch):
    return FONT[0][ASCII_TO_SCREEN[0][ord(ch)]]


def text_mask(s, sx=1, sy=None):
    """The string in the C64 font as a set of lit (x, y), scaled."""
    sy = sy or sx
    lit = set()
    for i, ch in enumerate(s):
        g = glyph(ch)
        for r in range(8):
            for b in range(8):
                if g[r] & (0x80 >> b):
                    for dy in range(sy):
                        for dx in range(sx):
                            lit.add((i * 8 * sx + b * sx + dx, r * sy + dy))
    return lit


def outline(lit, d=1):
    out = set()
    for (x, y) in lit:
        for dy in range(-d, d + 1):
            for dx in range(-d, d + 1):
                out.add((x + dx, y + dy))
    return out - lit


def text(cv, s, x, y, face, sx=1, sy=None, edge=None, shadow=None):
    """Draw s at (x, y). face: index or face(dx, dy) over the text's own
    box; edge: a 1-pixel outline; shadow: (depth, colour(k)) extruded
    down-right, the box art's 3D letters."""
    lit = text_mask(s, sx, sy)
    if shadow:
        depth, sc = shadow
        for k in range(depth, 0, -1):
            sh = {(px + k, py + k) for (px, py) in lit}
            if edge is not None and k == depth:
                for (px, py) in outline(sh):
                    cv.put(x + px, y + py, edge)
            for (px, py) in sh:
                cv.put(x + px, y + py, sc(k))
    if edge is not None:
        for (px, py) in outline(lit):
            if not shadow or (px, py) not in lit:
                cv.put(x + px, y + py, edge)
    for (px, py) in lit:
        cv.put(x + px, y + py, face(px, py) if callable(face) else face)


def text_w(s, sx=1):
    return len(s) * 8 * sx


# ---------------------------------------------------------- the scene
CAM_H, CAM_P = 7.0, 0.07            # camera height, pitch down
TX0, TZ0, TX1, TZ1 = 20.0, 13.0, -24.0, 34.0    # the track's two ends
class Cam:
    """A pinhole camera at (0, ch, 0) looking down +z, pitched down by
    `pitch` radians; screen y grows down."""
    def __init__(self, ch, pitch, f, cx, cy):
        self.ch, self.f, self.cx, self.cy = ch, f, cx, cy
        self.cp, self.sp = math.cos(pitch), math.sin(pitch)

    def view(self, p):
        x, y, z = p[0], p[1] - self.ch, p[2]
        return (x, y * self.cp + z * self.sp, -y * self.sp + z * self.cp)

    def proj(self, p):
        x, y, z = self.view(p)
        return (self.cx + self.f * x / z, self.cy - self.f * y / z)


def shade(base, lit):
    return lambda x, y: col(base, lit)


def scene(cv):
    """The start screen's world: sky, hills, ground, the elevated track
    with its ramp and gap, and the car in the air over it."""
    HOR = 118
    # sky: lighter toward the horizon, as on the box
    cv.rect(0, 0, W, HOR, lambda x, y: col(SKY, 15 + 16 * y / HOR))
    # hills: two grey ridges
    for (amp, per, ph, base, lev) in ((9, 70.0, 0.4, HOR - 4, 17),
                                      (6, 41.0, 2.1, HOR, 21)):
        pts = [(0, HOR + 2)]
        for x in range(0, W + 8, 8):
            pts.append((x, base - amp * (0.6 + 0.4 * math.sin(x / per + ph))
                        * (0.7 + 0.3 * math.sin(x / 17.0 + ph * 3))))
        pts.append((W, HOR + 2))
        cv.poly(pts, col(GREY, lev))
    # ground: olive khaki, darker in the distance, a few furrows
    cv.rect(0, HOR, W, H, lambda x, y: col(
        GRASS, 19 + 8 * (y - HOR) / (H - HOR)
        - (1 if (y * 7 + (x >> 4)) % 11 == 0 else 0)))

    cam = Cam(ch=CAM_H, pitch=CAM_P, f=260.0, cx=160.0, cy=HOR + 260.0 * math.tan(CAM_P))
    # The track: centreline from near right, away and to the left; up a
    # ramp to the lip, a gap, and down again on the far side.
    HALF = 1.6
    segs = []

    def centre(s):
        return (TX0 + (TX1 - TX0) * s, TZ0 + (TZ1 - TZ0) * s)

    def height(s):
        if s < 0.12:
            return 1.2
        if s < 0.34:                    # up the ramp to the lip
            t = (s - 0.12) / 0.22
            return 1.2 + 3.6 * t * t
        if s < 0.45:                    # the landing, a little lower
            return 4.2
        return 4.2 - 2.4 * (s - 0.45) / 0.55

    N = 44
    GAP = (15, 19)                      # segments with no road
    for i in range(N):
        s0, s1 = i / N, (i + 1) / N
        if GAP[0] <= i < GAP[1]:
            continue
        c0, c1 = centre(s0), centre(s1)
        dx, dz = c1[0] - c0[0], c1[1] - c0[1]
        ln = math.hypot(dx, dz)
        nx, nz = -dz / ln * HALF, dx / ln * HALF
        h0, h1 = height(s0), height(s1)
        if i == GAP[0] - 1:
            h1 = h0 + 0.35               # the lip kicks up
        L0 = (c0[0] - nx, h0, c0[1] - nz)
        R0 = (c0[0] + nx, h0, c0[1] + nz)
        L1 = (c1[0] - nx, h1, c1[1] - nz)
        R1 = (c1[0] + nx, h1, c1[1] + nz)
        depth = (c0[1] + c1[1]) / 2
        segs.append((depth, i, L0, R0, L1, R1))

    polys = []
    for depth, i, L0, R0, L1, R1 in segs:
        panel = WHITE if (i // 2) % 2 == 0 else RED
        plev = 29 if panel == WHITE else 22
        # the near wall: the right-hand side faces the camera
        for (A, B, side_l) in ((R0, R1, 0), (L0, L1, 4)):
            q = [A, B, (B[0], 0.0, B[2]), (A[0], 0.0, A[2])]
            polys.append((depth + (0.01 if side_l else 0), q,
                          col(panel, plev - side_l), True))
        # the road, with its edge lines
        polys.append((depth - 0.02, [L0, R0, R1, L1],
                      col(KHAKI, 27 - 4 * depth / 40), False))
        edge = YELLOW if (i // 2) % 2 == 0 else RED
        for (A, B, C, D) in ((L0, L1, None, None), (R0, R1, None, None)):
            inw = 0.18 if A is L0 else -0.18
            dxA = (R0[0] - L0[0]) / (2 * HALF) * inw
            dzA = (R0[2] - L0[2]) / (2 * HALF) * inw
            q = [A, B, (B[0] + dxA, B[1], B[2] + dzA),
                 (A[0] + dxA, A[1], A[2] + dzA)]
            polys.append((depth - 0.03, q, col(edge, 28 if edge == YELLOW
                                               else 18), False))

    # the car, in the air over the gap, nose up
    cs = (GAP[0] + 2.2) / N
    cc = centre(cs)
    cy0 = height(cs) + 0.3
    yaw = math.atan2(TX1 - TX0, TZ1 - TZ0)
    pitch = 0.28
    K = 1.8                             # the car, larger than life

    def car_pt(lx, ly, lz):
        # car axes: x right, y up, z forward along the track
        lx, ly, lz = lx * K, ly * K, lz * K
        y1 = ly * math.cos(pitch) + lz * math.sin(pitch)
        z1 = -ly * math.sin(pitch) + lz * math.cos(pitch)
        x2 = lx * math.cos(yaw) + z1 * math.sin(yaw)
        z2 = -lx * math.sin(yaw) + z1 * math.cos(yaw)
        return (cc[0] + x2, cy0 + y1, cc[1] + z2)

    def box(x0, x1, y0, y1, z0, z1, hue, lev, top=None, front=None):
        q = [
            ([(x0, y1, z0), (x1, y1, z0), (x1, y1, z1), (x0, y1, z1)],
             top if top is not None else col(hue, lev)),
            ([(x0, y0, z0), (x1, y0, z0), (x1, y1, z0), (x0, y1, z0)],
             col(hue, lev - 3)),
            ([(x1, y0, z0), (x1, y0, z1), (x1, y1, z1), (x1, y1, z0)],
             col(hue, lev - 5)),
            ([(x0, y0, z0), (x0, y0, z1), (x0, y1, z1), (x0, y1, z0)],
             col(hue, lev - 5)),
            ([(x0, y0, z1), (x1, y0, z1), (x1, y1, z1), (x0, y1, z1)],
             front if front is not None else col(hue, lev - 2)),
            ([(x0, y0, z0), (x1, y0, z0), (x1, y0, z1), (x0, y0, z1)],
             col(hue, lev - 8)),
        ]
        return [([car_pt(*p) for p in f], c) for f, c in q]

    carparts = []
    carparts += box(-0.75, 0.75, 0.30, 0.70, -1.7, 1.7, RED, 26,
                    top=col(RED, 29))
    carparts += box(-0.60, 0.60, 0.70, 1.10, -0.9, 0.5, RED, 23,
                    top=col(RED, 28), front=col(GREY, 7))
    for wx in (-1.10, 0.75):
        for wz in (-1.45, 0.75):
            carparts += box(wx, wx + 0.3, 0.0, 0.7, wz, wz + 0.7, GREY, 13,
                            top=col(GREY, 15))
    # the car's faces sort among themselves by their own depth
    cam_polys = []
    for q, c in carparts:
        v = [cam.view(p) for p in q]
        # back-face cull in view space
        a, b, cpt = v[0], v[1], v[2]
        e1 = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        e2 = (cpt[0] - a[0], cpt[1] - a[1], cpt[2] - a[2])
        nrm = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
               e1[0] * e2[1] - e1[1] * e2[0])
        cen = [sum(p[k] for p in v) / 4 for k in range(3)]
        if nrm[0] * cen[0] + nrm[1] * cen[1] + nrm[2] * cen[2] >= 0:
            continue
        cam_polys.append((sum(p[2] for p in q) / 4, q, c))
    # its shadow on the ground
    sh = [car_pt(x, 0, z) for x, z in ((-1, -1.8), (1, -1.8), (1, 1.8),
                                       (-1, 1.8))]
    sh = [(p[0] + 1.0, 0.0, p[2] + 0.8) for p in sh]
    cv.poly([cam.proj(p) for p in sh], col(GRASS, 12))

    # track: far to near
    for depth, q, c, wall in sorted(polys, key=lambda t: -t[0]):
        pts = [cam.proj(p) for p in q]
        cv.poly(pts, c)
        if wall:                        # a dark seam at the panel's edge
            (x0, y0), (x1, y1) = pts[0], pts[3]
            for t in range(16):
                cv.put(int(x0 + (x1 - x0) * t / 15),
                       int(y0 + (y1 - y0) * t / 15), col(GREY, 6))
    # the car is in front of everything behind the gap and behind the near
    # stretch, which it never overlaps on screen: drawn last
    for _, q, c in sorted(cam_polys, key=lambda t: -t[0]):
        cv.poly([cam.proj(p) for p in q], c)


def start_picture():
    cv = Canvas(W, H)
    scene(cv)
    # "STUNT CAR": yellow letters, a red 3D side, a black edge
    s = 'STUNT CAR'
    text(cv, s, (W - text_w(s, 4)) // 2 - 2, 6,
         lambda dx, dy: col(YELLOW, 31 - dy * 6 // 32),
         sx=4, edge=0, shadow=(4, lambda k: col(RED, 24 - 2 * k)))
    # "RACER": larger, white over blue
    s = 'RACER'
    text(cv, s, (W - text_w(s, 6)) // 2 - 3, 44,
         lambda dx, dy: col(WHITE, 31 - dy * 8 // 48),
         sx=6, sy=6, edge=0, shadow=(5, lambda k: col(BLUE, 26 - 2 * k)))
    return cv


def credits_picture():
    cv = Canvas(W, H, col(BLUE, 5))
    # the chequered frame
    for y in range(H):
        for x in range(W):
            if x < 8 or x >= W - 8 or y < 8 or y >= H - 8:
                cv.px[y * W + x] = col(WHITE, 30) \
                    if ((x >> 3) + (y >> 3)) & 1 else 0
    s = 'STUNT CAR RACER'
    text(cv, s, (W - text_w(s, 2)) // 2, 18,
         lambda dx, dy: col(YELLOW, 31 - dy // 2), sx=2,
         edge=0, shadow=(2, lambda k: col(RED, 22)))
    lines = [
        (40, 'GAME AND GPU64', col(SKY, 31)),
        (50, 'HONDANI', col(WHITE, 31)),
        (66, 'MUSIC', col(SKY, 31)),
        (76, 'LED STORM BY TIM FOLLIN', col(WHITE, 31)),
        (86, 'C64 VERSION BY MICHAL HOFFMANN', col(WHITE, 31)),
        (96, '(SMALLTOWN BOY) MULTISTYLE LABS 2003', col(GREY, 24)),
        (112, 'AFTER THE ORIGINAL BY', col(SKY, 31)),
        (122, 'GEOFF CRAMMOND, 1988', col(WHITE, 31)),
        (138, 'JOYSTICK IN PORT 2', col(YELLOW, 30)),
        (148, 'OR W S A D AND SPACE', col(YELLOW, 30)),
        (178, '(C) 2026 HONZA / HONDANI - GPL V3', col(GREY, 22)),
    ]
    for y, s, c in lines:
        text(cv, s, (W - text_w(s)) // 2, y, c)
    return cv


def fire_picture():
    s = 'PRESS FIRE TO START'
    cv = Canvas(text_w(s, 2) + 2, 18, KEY)
    text(cv, s, 1, 1, lambda dx, dy: col(YELLOW, 31 - dy // 3), sx=2, edge=0)
    return cv


def panel():
    return col(BLUE, 5)


def menu_picture():
    cv = Canvas(W, H, panel())
    for y in range(H):
        for x in range(W):
            if x < 8 or x >= W - 8 or y < 8 or y >= H - 8:
                cv.px[y * W + x] = col(WHITE, 30) \
                    if ((x >> 3) + (y >> 3)) & 1 else 0
    return cv


def track_picture(b):
    """Track b (stunt_tracks.Built) from above and to the south: the road,
    its walls down to the ground, the gaps as gaps. Painter's order, far
    first, a quad per segment and side."""
    cv = Canvas(TRK_W, TRK_H, panel())
    xs = [v / 256 for v in b.cx]
    zs = [v / 256 for v in b.cz]
    ys = [v / 256 for v in b.cy]
    span = max(max(xs) - min(xs), (max(zs) - min(zs)) * 0.55 + 14)
    k = (TRK_W - 16) / span
    ox = (max(xs) + min(xs)) / 2
    oz = (max(zs) + min(zs)) / 2

    def P(x, y, z):
        return (TRK_W / 2 + (x - ox) * k, TRK_H / 2 + 6 - (z - oz) * k * 0.5 - y * k * 0.9)
    # the ground
    gr = [P(min(xs) - 4, 0, min(zs) - 4), P(max(xs) + 4, 0, min(zs) - 4),
          P(max(xs) + 4, 0, max(zs) + 4), P(min(xs) - 4, 0, max(zs) + 4)]
    cv.poly(gr, col(GRASS, 20))
    import stunt_tracks as T
    polys = []
    for i in range(T.N):
        if i in b.gaps:
            continue
        j = (i + 1) % T.N
        ri, rj = T.right(b.pts, i), T.right(b.pts, j)
        y0, y1 = ys[i], ys[j]
        if i in b.deckset:
            y1 = y0 = ys[b.deck]
        a = [(xs[i] - T.HALFW * ri[0], y0, zs[i] - T.HALFW * ri[2]),
             (xs[i] + T.HALFW * ri[0], y0, zs[i] + T.HALFW * ri[2])]
        c = [(xs[j] + T.HALFW * rj[0], y1, zs[j] + T.HALFW * rj[2]),
             (xs[j] - T.HALFW * rj[0], y1, zs[j] - T.HALFW * rj[2])]
        zc = (zs[i] + zs[j]) / 2
        yb = lambda p, o: (p[0], (p[1] - 0.3) if o else 0.0, p[2])
        low = b.over[i] or i in b.deckset
        # the near wall (the one facing south, i.e. -z), then the road
        for side in ((a[0], c[1]), (a[1], c[0])):
            p, q = side
            polys.append((zc + 0.01, [P(*p), P(*q), P(*yb(q, low)), P(*yb(p, low))],
                          col(RED, 20) if (i // 1) % 2 else col(WHITE, 26)))
        polys.append((zc, [P(*a[0]), P(*a[1]), P(*c[0]), P(*c[1])],
                      col(WHITE, 30) if i == 0 else col(KHAKI, 27)))
    for _, q, c in sorted(polys, key=lambda t: -t[0]):
        cv.poly(q, c)
    return cv


# The menus' headings, keyed, 2x, after the credits' title. HEADINGS[k] is
# picture PIC_HEAD + k; the league's divisions are 1-4 by k - HEAD_DIV.
HEADINGS = ["STUNT CAR RACER", "PRACTISE", "DIVISION 1", "DIVISION 2",
            "DIVISION 3", "DIVISION 4", "RACE RESULT", "END OF SEASON",
            "CONTROLS"]
HEAD_H = 20


def heading_picture(s):
    cv = Canvas(text_w(s, 2) + 4, HEAD_H, KEY)
    text(cv, s, 1, 1, lambda dx, dy: col(YELLOW, 31 - dy // 2), sx=2,
         edge=0, shadow=(2, lambda k: col(RED, 22)))
    return cv


# The menus' text: every C64 screen code 0-63 (@, A-Z, the punctuation,
# 0-9) as its own keyed 9x9 picture, the glyph and a one-pixel shadow down
# and right, once per colour. Glyph c of set k is PIC_GLY + 64 * k + c.
GLYPH_SETS = [col(WHITE, 30), col(YELLOW, 30), col(SKY, 31)]


def glyph_picture(code, face):
    cv = Canvas(9, 9, KEY)
    g = FONT[0][code]
    lit = [(b, r) for r in range(8) for b in range(8) if g[r] & (0x80 >> b)]
    for (x, y) in lit:
        cv.put(x + 1, y + 1, col(BLUE, 1))
    for (x, y) in lit:
        cv.put(x, y, face)
    return cv


def pictures(tracks=()):
    w, h, logo = hondani.hdmi_pixels()
    out = [(w, h, logo)]
    for cv in (start_picture(), credits_picture(), fire_picture(), menu_picture()):
        out.append((cv.w, cv.h, bytes(cv.px)))
    for b in tracks:
        cv = track_picture(b)
        out.append((cv.w, cv.h, bytes(cv.px)))
    assert len(out) == PIC_TRACK + len(tracks)
    for s in HEADINGS:
        cv = heading_picture(s)
        out.append((cv.w, cv.h, bytes(cv.px)))
    for face in GLYPH_SETS:
        for code in range(64):
            cv = glyph_picture(code, face)
            out.append((cv.w, cv.h, bytes(cv.px)))
    assert len(out) <= 256
    return out


def pic_head(ntracks):
    return PIC_TRACK + ntracks


def pic_gly(ntracks):
    return pic_head(ntracks) + len(HEADINGS)
# ---------------------------------------------------------- the files
LEVEL_VERSION = 8


def level(pal, pics):
    """A .g64lev v8 with nothing but the palette and the pictures: no start,
    no body (the game never sends LOAD_LEVEL)."""
    blob = bytearray()

    def put(b):
        o = len(blob)
        blob.extend(b)
        return o
    palo = put(bytes(pal[:768]))
    stro = put(b'\0')
    tab = [struct.pack('<HHI', w, h, put(px)) for w, h, px in pics]
    sec = struct.pack('<HH', len(pics), 0) + b''.join(tab)
    pico = put(sec)
    hdr_fmt = '<4sHH' '4H' 'HHI' '5I' '2I' '3iH4BH'
    base = struct.calcsize(hdr_fmt)
    hdr = struct.pack(hdr_fmt, b'G64L', LEVEL_VERSION, 1,
                      0, 0, 0, 0, 0, 0, 0,
                      base, palo, stro, 0, 0, pico, len(sec),
                      0, 0, 0, 0, 0, 0, 0, 0, 0)
    return bytes(hdr + blob)


def image(pal, tracks=(), ppm_dir=None):
    """The level pack: stunts.reu's first part. gen_stunt.py appends the
    game's own data after it."""
    pics = pictures(tracks)
    lev = level(pal, pics)
    img = pack_levels.pack([(PACK_NAME, lev)])
    pack_levels.unpack(img)
    if ppm_dir:
        os.makedirs(ppm_dir, exist_ok=True)
        for i, (w, h, px) in enumerate(pics):
            with open(os.path.join(ppm_dir, 'stunt_pic%d.ppm' % i), 'wb') as f:
                f.write(b'P6 %d %d 255\n' % (w, h))
                f.write(bytes(v for p in px for v in
                              ((255, 0, 255) if p == KEY and i != PIC_LOGO and px is not None
                               else (232, 44, 32) if p and i == PIC_LOGO
                               else tuple(pal[p * 3:p * 3 + 3]))))
    return img, len(lev), pics

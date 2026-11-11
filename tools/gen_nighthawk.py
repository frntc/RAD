#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
Builds the theatre of gpu64_demo_nighthawk.a: the heightmap and tile map the
6502 flies over and generates its terrain from, the palette, the textures,
and -- with --probe -- a class-1 frame stream that scenesim can render
without any 6502 at all. See dev/milestone19_nighthawk_design.md.

SCALE: 1 world unit = 100 m. The theatre is 256 x 256 units, 64 x 64 cells
of 4 units, 8 x 8 tiles of 8 x 8 cells.

THE MAPS (what the 6502 keeps)

  hMap  65 x 65 corner heights, one byte each: y = (h - SEA) / 16 units, so
        h = SEA is the waterline, 1/16 unit = 6.25 m a step.
  tMap  64 x 64 cell types, one per byte here (the 6502 copy packs them).

A TILE (what the 6502 generates and uploads, 64 times)

  81 vertices, the 9 x 9 corners of its cells, in rows of increasing z, each
  row in increasing x, relative to the tile centre -- so x and z are exact
  multiples of 4 in -16..16 and y is (h - SEA) * 16 in 8.8, i.e. the height
  byte shifted: nothing on the 6502 needs a multiply.
  Two triangles per cell that is neither sea nor airbase, split along the
  same diagonal everywhere; UVs 0..32 on a 32 x 32 texture. tile_blobs() is
  the reference the 6502 generator is checked against byte for byte.

Winding: class 1 draws a face whose (b-a)x(c-a) points at the viewer, in
its own left-handed axes. A heightfield's faces all point up, so the tile
triangles are built from an up hint once and the same index order holds for
every cell -- the 6502 copies the two templates this file proves.
"""

import math, os, random, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "games", "nighthawk", "gpu64_demo_nighthawk.inc")

W      = 256        # theatre, world units
CELL   = 4          # cell size, units
NC     = W // CELL  # 64 cells a side
TILEC  = 8          # cells per tile side
NT     = NC // TILEC  # 8 tiles a side
TILE   = TILEC * CELL  # 32 units
SEA    = 16         # height byte of the waterline
HSTEP  = 16         # height bytes per world unit

# ---------------------------------------------------------------- palette
# 8 hues x 32 levels, level 31 the hue itself, level 0 black -- the layout
# BUILD_COLORMAP's lighting ramps want, as in stunt.
HUES = [
    (100, 176, 60),     # 0 grass; level 31 is the HUD green
    (150, 150, 158),    # 1 grey: rock, concrete, the F-117
    (130, 170, 225),    # 2 sky
    (230, 60, 36),      # 3 red: fire, roofs, lights
    (40, 92, 150),      # 4 sea
    (236, 200, 90),     # 5 sand / yellow
    (130, 96, 62),      # 6 earth
    (244, 244, 244),    # 7 white
]
GRASS, GREY, SKY, RED, SEAC, SAND, EARTH, WHITE = range(8)
def col(h, l): return h * 32 + max(0, min(31, l))

# ------------------------------------------------------------ cell types
T_SEA, T_GRASS, T_FARMA, T_FARMB, T_FOREST, T_ROCK, T_SAND, T_TOWN, T_HOLE = range(9)
TEX_OF = {T_GRASS: 1, T_FARMA: 2, T_FARMB: 3, T_FOREST: 4,
          T_ROCK: 5, T_SAND: 6, T_TOWN: 7}
TEX_SEA = 8

# ------------------------------------------------------------ the theatre
rnd = random.Random(117)

def lattice(n, seed):
    r = random.Random(seed)
    return [[r.random() for _ in range(n + 1)] for _ in range(n + 1)]

def value_noise(lat, n, x, y):
    """x, y in 0..1 over a lattice of n cells, smoothstepped."""
    x, y = min(max(x, 0.0), 0.999999), min(max(y, 0.0), 0.999999)
    fx, fy = x * n, y * n
    ix, iy = min(int(fx), n - 1), min(int(fy), n - 1)
    tx, ty = fx - ix, fy - iy
    tx, ty = tx * tx * (3 - 2 * tx), ty * ty * (3 - 2 * ty)
    a = lat[iy][ix] + (lat[iy][ix + 1] - lat[iy][ix]) * tx
    b = lat[iy + 1][ix] + (lat[iy + 1][ix + 1] - lat[iy + 1][ix]) * tx
    return a + (b - a) * ty

OCT = [(lattice(4, 1), 4, 1.0), (lattice(8, 2), 8, 0.5),
       (lattice(16, 3), 16, 0.25), (lattice(32, 4), 32, 0.12)]

def fbm(x, y):
    return sum(value_noise(l, n, x, y) * a for l, n, a in OCT) / 1.87

COAST = lattice(6, 5)

# Airbases, in cell coordinates: (x0, z0, x1, z1) exclusive, and the height
# byte they are flattened to. The friendly field sits on the south-west
# coastal plain, the enemy's inland to the north-east.
BASES = [(20, 20, 22, 30, SEA + 5), (44, 42, 46, 52, SEA + 30)]

def land_mask(x, y):
    """0 at sea, 1 inland: a coast running NW-SE across the south-west
    third, a bay cut into it, and sea again all round the theatre's rim."""
    rim = min(x, y, 1 - x, 1 - y)
    edge = max(0.0, min(1.0, (rim - 0.04) / 0.10))
    coast = (x + y) / 2 - 0.30 + 0.16 * (value_noise(COAST, 6, x, y) - 0.5)
    m = max(0.0, min(1.0, coast * 12)) * edge
    bay = math.hypot(x - 0.62, y - 0.20) / 0.09
    if bay < 1.0:
        m *= max(0.0, bay * 1.5 - 0.5)
    return m

hMap = [[0] * (NC + 1) for _ in range(NC + 1)]
for j in range(NC + 1):
    for i in range(NC + 1):
        x, y = i / NC, j / NC
        m = land_mask(x, y)
        n = fbm(x, y)
        hills = max(0.0, (x + y - 0.7)) * 1.4 * n * n
        h = SEA + (-12 + 30 * m) + m * (150 * hills + 10 * n)
        hMap[j][i] = max(0, min(255, int(round(h))))

for (x0, z0, x1, z1, hb) in BASES:
    for j in range(z0 - 1, z1 + 2):
        for i in range(x0 - 1, x1 + 2):
            if (x0 <= i <= x1 and z0 <= j <= z1):
                hMap[j][i] = hb
            else:                       # a one-cell apron eases into it
                hMap[j][i] = (hMap[j][i] + hb) // 2

tMap = [[T_SEA] * NC for _ in range(NC)]
def forest_n(x, y): return value_noise(OCT[2][0], 16, x, y)
for j in range(NC):
    for i in range(NC):
        hs = [hMap[j][i], hMap[j][i + 1], hMap[j + 1][i], hMap[j + 1][i + 1]]
        if max(hs) <= SEA:
            t = T_SEA
        elif min(hs) <= SEA + 2:
            t = T_SAND
        elif max(hs) - min(hs) > 14 or min(hs) > SEA + 110:
            t = T_ROCK
        elif forest_n((i + .5) / NC, (j + .5) / NC) > 0.62:
            t = T_FOREST
        else:
            t = (T_FARMA, T_FARMB, T_GRASS)[(i // 3 + j // 2 * 5 + (i * j) // 7) % 3]
        tMap[j][i] = t
# towns: a handful of 2x2 clusters on flat, low land
for (i, j) in ((26, 18), (30, 30), (18, 36), (38, 24), (52, 30)):
    for dj in (0, 1):
        for di in (0, 1):
            if tMap[j + dj][i + di] not in (T_SEA, T_SAND, T_ROCK):
                tMap[j + dj][i + di] = T_TOWN
for (x0, z0, x1, z1, hb) in BASES:
    for j in range(z0, z1):
        for i in range(x0, x1):
            tMap[j][i] = T_HOLE

def ground_y(x, z):
    """World height at world (x, z), bilinear -- the probe's terrain follow."""
    fx, fz = max(0, min(NC - 1e-6, x / CELL)), max(0, min(NC - 1e-6, z / CELL))
    i, j = int(fx), int(fz)
    tx, tz = fx - i, fz - j
    a = hMap[j][i] + (hMap[j][i + 1] - hMap[j][i]) * tx
    b = hMap[j + 1][i] + (hMap[j + 1][i + 1] - hMap[j + 1][i]) * tx
    return (a + (b - a) * tz - SEA) / HSTEP

# --------------------------------------------------------------- textures
def tex(fn, w=32, h=32):
    return bytes(fn(u, v) for v in range(h) for u in range(w))   # class 1: v*w + u

def tn(seed):
    """A tileable 32x32 value-noise field: an 8-cell lattice that wraps."""
    r = random.Random(seed)
    lat = [[r.random() for _ in range(8)] for _ in range(8)]
    def f(u, v):
        fu, fv = u / 4, v / 4
        iu, iv = int(fu), int(fv)
        tu, tv = fu - iu, fv - iv
        g = lambda a, b: lat[b % 8][a % 8]
        a = g(iu, iv) + (g(iu + 1, iv) - g(iu, iv)) * tu
        b = g(iu, iv + 1) + (g(iu + 1, iv + 1) - g(iu, iv + 1)) * tu
        return a + (b - a) * tv
    return f

# Low contrast on purpose: there is no mip-mapping, so at 10 km a texel is
# a fraction of a pixel and anything contrasty sparkles as the camera moves.
N1, N2, N3 = tn(11), tn(12), tn(13)
def t_grass(u, v): return col(GRASS, 21 + int(4 * N1(u, v)))
def t_farmA(u, v):
    return col(GRASS if (v // 8) % 2 else SAND, (24 if (v // 8) % 2 else 22) + int(3 * N2(u, v)))
def t_farmB(u, v):
    return col(EARTH, 22 + int(3 * N3(u, v)) + (2 if u % 4 == 0 else 0))
def t_forest(u, v): return col(GRASS, 11 + int(8 * N1((u * 2) % 32, (v * 2) % 32)))
def t_rock(u, v): return col(GREY, 17 + int(8 * N2(u, v)))
def t_sand(u, v): return col(SAND, 25 + int(3 * N3(u, v)))
def t_town(u, v):
    bu, bv = u % 16, v % 16
    if bu < 3 or bv < 3:
        return col(GREY, 16)                        # streets
    return col(RED, 18 + (u // 16 + v // 16) % 2 * 4) if (bu + bv) % 13 else col(GREY, 22)
def t_sea(u, v): return col(SEAC, 22 + int(5 * N1(u, v)))

def pack_tex(data):
    """Sixteen colours and four bits a texel, low nibble first: no texture
    here has more than eight colours, and eleven of them at 1 KB each were
    a fifth of the image. uploadTex unpacks each into texBuf."""
    pal = sorted(set(data))
    assert len(pal) <= 16
    ix = {c: k for k, c in enumerate(pal)}
    return (pal + [0] * 16)[:16] + [ix[data[k]] | ix[data[k + 1]] << 4 for k in range(0, len(data), 2)]

TEXTURES = [
    ("texGrass", 1, tex(t_grass)), ("texFarmA", 2, tex(t_farmA)),
    ("texFarmB", 3, tex(t_farmB)), ("texForest", 4, tex(t_forest)),
    ("texRock", 5, tex(t_rock)), ("texSand", 6, tex(t_sand)),
    ("texTown", 7, tex(t_town)), ("texSea", TEX_SEA, tex(t_sea)),
]

# ------------------------------------------------------------------ meshes
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]

def q88(v):
    r = int(round(v * 256))
    assert -32768 <= r <= 32767, v
    return r

FLAT, DOUBLE, UNLIT = 0x02, 0x01, 0x04

# The cell's two triangles, as (corner, corner, corner) with corners
# 0 = (x, z), 1 = (x+1, z), 2 = (x, z+1), 3 = (x+1, z+1). Fixed here, proved
# against the up hint below for every cell of every tile.
CELL_TRIS = ((0, 2, 1), (1, 2, 3))
CORNER_UV = ((0, 0), (32, 0), (0, 32), (32, 32))

def tile_blobs(tx, tz):
    """(vertex blob, face blob, faces) for tile (tx, tz); None if empty."""
    vb = []
    for j in range(TILEC + 1):
        for i in range(TILEC + 1):
            h = hMap[tz * TILEC + j][tx * TILEC + i]
            for c in ((i * CELL - TILE // 2) * 256, (h - SEA) * 16, (j * CELL - TILE // 2) * 256):
                vb += [c & 0xff, (c >> 8) & 0xff]
    fb, nf = [], 0
    for j in range(TILEC):
        for i in range(TILEC):
            t = tMap[tz * TILEC + j][tx * TILEC + i]
            if t in (T_SEA, T_HOLE):
                continue
            corner = (j * 9 + i, j * 9 + i + 1, j * 9 + i + 9, j * 9 + i + 10)
            for tri in CELL_TRIS:
                idx = [corner[k] for k in tri]
                p = [(i * CELL + (k & 1) * CELL, 0, j * CELL + (k >> 1) * CELL) for k in tri]
                assert cross(sub(p[1], p[0]), sub(p[2], p[0]))[1] > 0   # faces up
                uv = [CORNER_UV[k] for k in tri]
                fb += idx + [uv[0][0], uv[0][1], uv[1][0], uv[1][1], uv[2][0], uv[2][1],
                             TEX_OF[t], 0, 0]
                nf += 1
    if nf == 0:
        return None
    assert len(vb) == 81 * 6 and nf <= 128
    return vb, fb, nf

def ocean_blobs():
    """8 x 8 cells of 30 units, 128 faces, centred on the node."""
    n, s = 8, 30
    vb = []
    for j in range(n + 1):
        for i in range(n + 1):
            for c in ((i - n / 2) * s, 0, (j - n / 2) * s):
                r = q88(c)
                vb += [r & 0xff, (r >> 8) & 0xff]
    fb = []
    for j in range(n):
        for i in range(n):
            corner = (j * 9 + i, j * 9 + i + 1, j * 9 + i + 9, j * 9 + i + 10)
            for tri in CELL_TRIS:
                uv = [CORNER_UV[k] for k in tri]
                fb += [corner[k] for k in tri] + [uv[0][0], uv[0][1], uv[1][0], uv[1][1],
                                                  uv[2][0], uv[2][1], TEX_SEA, 0, 0]
    return vb, fb, n * n * 2
OCEAN_STEP = 30

def palette():
    pal = []
    for (r, g, b) in HUES:
        for l in range(32):
            pal += [round(r * l / 31), round(g * l / 31), round(b * l / 31)]
    return pal

# ------------------------------------------------------- object textures
def t_runway(u, v):
    if u in (1, 2, 29, 30):
        return col(WHITE, 28)                       # edge lines
    if u in (15, 16) and v < 16:
        return col(WHITE, 26)                       # centre dashes
    return col(GREY, 8 + int(3 * N2(u, v)))
def t_thresh(u, v):
    if u in (1, 2, 29, 30):
        return col(WHITE, 28)
    if 4 <= v < 24 and 4 <= u < 28 and (u // 2) % 2 == 0:
        return col(WHITE, 29)                       # piano keys
    return col(GREY, 8 + int(3 * N2(u, v)))
def t_wall(u, v):
    """Hangar and block walls: a panel pattern, a band of windows."""
    if 6 <= v % 16 < 11 and 2 <= u % 8 < 6:
        return col(SKY, 10)                         # windows
    return col(GREY, 18 + (u % 8 == 0) * 3 + int(2 * N3(u, v)))

TEXTURES += [("texRunway", 9, tex(t_runway)), ("texThresh", 10, tex(t_thresh)),
             ("texWall", 11, tex(t_wall))]
TEX_RUNWAY, TEX_THRESH, TEX_WALL = 9, 10, 11

# ----------------------------------------------------------- object meshes
class Mesh:
    """Stunt's builder: every triangle is wound from an OUTWARD hint."""
    def __init__(self):
        self.v, self.f = [], []
    def vert(self, p):
        self.v.append(p)
        return len(self.v) - 1
    def tri(self, a, b, c, uva, uvb, uvc, tex, flags, out):
        pa, pb, pc = self.v[a], self.v[b], self.v[c]
        if dot(cross(sub(pb, pa), sub(pc, pa)), out) < 0:
            b, c, uvb, uvc = c, b, uvc, uvb
        self.f.append((a, b, c, uva, uvb, uvc, tex, flags))
    def quad(self, a, b, c, d, uv, tex, flags, out):
        uv = uv or [(0, 0)] * 4
        self.tri(a, b, c, uv[0], uv[1], uv[2], tex, flags, out)
        self.tri(a, c, d, uv[0], uv[2], uv[3], tex, flags, out)
    def vblob(self):
        out = []
        for p in self.v:
            for c in p:
                r = q88(c)
                out += [r & 0xff, (r >> 8) & 0xff]
        assert len(self.v) <= 256
        return out
    def fblob(self):
        out = []
        for (a, b, c, ua, ub, uc, t, fl) in self.f:
            for uv in (ua, ub, uc):
                assert 0 <= uv[0] <= 255 and 0 <= uv[1] <= 255, uv
            out += [a, b, c, ua[0], ua[1], ub[0], ub[1], uc[0], uc[1], t, fl, 0]
        assert len(self.f) <= 255
        return out

UP = (0, 1, 0)

# The airbase fills its hole in the terrain exactly: two cells wide, ten
# long, its surface the flattened height, so nothing in it is coplanar with
# anything. A grass strip each side, the runway down the middle, and upright
# unlit lights along both edges -- which is also all a night landing sees.
RWY_HALF = 0.3
def airbase():
    m = Mesh()
    bx0, bz0, bx1, bz1, hb = BASES[0]
    hw, hl = (bx1 - bx0) * CELL / 2, (bz1 - bz0) * CELL / 2
    xs = (-hw, -RWY_HALF, RWY_HALF, hw)
    nseg = bz1 - bz0
    grid = [[m.vert((x, 0.0, -hl + k * CELL)) for x in xs] for k in range(nseg + 1)]
    for k in range(nseg):
        for c in range(3):
            a, b = grid[k][c], grid[k][c + 1]
            d, e = grid[k + 1][c], grid[k + 1][c + 1]
            if c == 1:
                t = TEX_THRESH if k in (0, nseg - 1) else TEX_RUNWAY
                uv = [(0, 0), (31, 0), (31, 32), (0, 32)]
                if k == nseg - 1:                   # the far threshold, turned round
                    uv = [(31, 32), (0, 32), (0, 0), (31, 0)]
            else:
                t, uv = 1, [(0, 0), (int(abs(xs[c + 1] - xs[c]) * 8), 0),
                            (int(abs(xs[c + 1] - xs[c]) * 8), 32), (0, 32)]
            m.quad(a, b, e, d, uv, t, 0, UP)
    lw, lh = 0.05, 0.06
    for k in range(nseg + 1):
        z = -hl + k * CELL
        for x in (-RWY_HALF - 0.04, RWY_HALF + 0.04):
            shade = col(RED, 31) if k == 0 else col(GRASS, 31) if k == nseg else col(SAND, 31)
            q = [m.vert((x, 0.004, z - lw)), m.vert((x, 0.004, z + lw)),
                 m.vert((x, lh, z + lw)), m.vert((x, lh, z - lw))]
            m.quad(*q, None, shade, FLAT | UNLIT | DOUBLE, (1, 0, 0))
    return m

# The pitch ladder: world-levelled, placed at the camera with (yaw, 0, 0),
# so its rungs stand at true elevations whatever the aircraft does. Radius
# 1 unit so the thin strokes are whole 8.8 steps; unlit HUD green.
HUD = None
def ladder():
    m = Mesh()
    R, t = 1.0, 0.005
    for deg in range(-30, 31, 5):
        th = math.radians(deg)
        cy, cz = R * math.sin(th), R * math.cos(th)
        ty, tz = t * math.cos(th), -t * math.sin(th)
        if deg == 0:
            segs = [(0.06, 0.40)]
        elif deg > 0:
            segs = [(0.06, 0.16)]
        else:
            segs = [(0.06, 0.10), (0.12, 0.16)]      # dashed below the horizon
        for (x0, x1) in segs:
            for sx in (-1, 1):
                q = [m.vert((sx * x0, cy - ty, cz - tz)), m.vert((sx * x1, cy - ty, cz - tz)),
                     m.vert((sx * x1, cy + ty, cz + tz)), m.vert((sx * x0, cy + ty, cz + tz))]
                m.quad(*q, None, HUD, FLAT | UNLIT | DOUBLE, (0, 0, -1))
    return m

def facet_hull(m, pts, faces, shade, centre):
    """pts: named points; faces: tuples of names (tris or quads), each
    wound from centroid - centre."""
    idx = {k: m.vert(p) for k, p in pts.items()}
    for f in faces:
        ps = [pts[k] for k in f]
        cen = tuple(sum(p[i] for p in ps) / len(ps) for i in range(3))
        out = sub(cen, centre)
        s = shade(f) if callable(shade) else shade
        if len(f) == 3:
            m.tri(*[idx[k] for k in f], (0, 0), (0, 0), (0, 0), s, FLAT, out)
        else:
            m.quad(*[idx[k] for k in f], None, s, FLAT, out)

# The F-117: faceted, dark grey, 0.2 units long. Origin on the belly, so the
# 6502's y is the belly and on the ground it is the ground plus the gear.
def f117():
    m = Mesh()
    P = {
        'N': (0, 0.010, 0.100), 'C': (0, 0.034, 0.030), 'B': (0, 0.026, -0.050),
        'L': (-0.066, 0.006, -0.060), 'R': (0.066, 0.006, -0.060),
        'TL': (-0.030, 0.008, -0.095), 'TR': (0.030, 0.008, -0.095), 'M': (0, 0.010, -0.070),
        'SL': (-0.018, 0.018, 0.020), 'SR': (0.018, 0.018, 0.020),
        'nb': (0, 0.0, 0.080), 'lb': (-0.050, 0.0, -0.050), 'rb': (0.050, 0.0, -0.050),
        'mb': (0, 0.0, -0.070),
        'FL0': (-0.012, 0.024, -0.050), 'FL1': (-0.038, 0.052, -0.098), 'FL2': (-0.018, 0.022, -0.080),
        'FR0': (0.012, 0.024, -0.050), 'FR1': (0.038, 0.052, -0.098), 'FR2': (0.018, 0.022, -0.080),
    }
    top = [('N', 'SL', 'C'), ('N', 'C', 'SR'), ('SL', 'B', 'C'), ('SR', 'C', 'B'),
           ('N', 'L', 'SL'), ('N', 'SR', 'R'), ('SL', 'L', 'B'), ('SR', 'B', 'R'),
           ('L', 'TL', 'B'), ('R', 'B', 'TR'), ('TL', 'M', 'B'), ('TR', 'B', 'M')]
    bot = [('N', 'nb', 'L'), ('N', 'R', 'nb'), ('nb', 'lb', 'L'), ('nb', 'R', 'rb'),
           ('nb', 'mb', 'lb'), ('nb', 'rb', 'mb'), ('lb', 'mb', 'TL'), ('rb', 'TR', 'mb'),
           ('L', 'lb', 'TL'), ('R', 'TR', 'rb'), ('TL', 'mb', 'M'), ('TR', 'M', 'mb')]
    shade = lambda f: col(GREY, 7 + (hash(f) % 4) if f in top else 4)
    facet_hull(m, P, top + bot, shade, (0, 0.012, -0.02))
    for a, b, c in (('FL0', 'FL1', 'FL2'), ('FR0', 'FR1', 'FR2')):
        m.tri(m.vert(P[a]), m.vert(P[b]), m.vert(P[c]), (0, 0), (0, 0), (0, 0),
              col(GREY, 6), FLAT | DOUBLE, (1, 0, 0))
    return m

def box(m, x0, x1, y0, y1, z0, z1, tex_side, tex_top, uvs=32):
    """An axis-aligned box standing on y0, no bottom face (it would be
    coplanar with the ground)."""
    c = {(ix, iy, iz): m.vert((x, y, z)) for ix, x in enumerate((x0, x1))
         for iy, y in enumerate((y0, y1)) for iz, z in enumerate((z0, z1))}
    def uvq(w, h):
        return [(0, 0), (min(255, int(w * uvs)), 0), (min(255, int(w * uvs)), min(255, int(h * uvs))),
                (0, min(255, int(h * uvs)))]
    w, d, h = x1 - x0, z1 - z0, y1 - y0
    m.quad(c[0, 1, 0], c[1, 1, 0], c[1, 1, 1], c[0, 1, 1], None if tex_top >= 32 else uvq(w, d),
           tex_top, FLAT if tex_top >= 32 else 0, UP)
    for (q, out, ww) in (([(0, 1, 0), (1, 1, 0), (1, 0, 0), (0, 0, 0)], (0, 0, -1), w),
                         ([(1, 1, 1), (0, 1, 1), (0, 0, 1), (1, 0, 1)], (0, 0, 1), w),
                         ([(0, 1, 1), (0, 1, 0), (0, 0, 0), (0, 0, 1)], (-1, 0, 0), d),
                         ([(1, 1, 0), (1, 1, 1), (1, 0, 1), (1, 0, 0)], (1, 0, 0), d)):
        m.quad(*[c[k] for k in q], uvq(ww, h), tex_side, 0, out)

def flat_box(m, x0, x1, y0, y1, z0, z1, shade, flags=FLAT):
    """box() in one flat colour, no bottom face."""
    c = (x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2
    P = {(a, b, d): (x, y, z) for a, x in enumerate((x0, x1)) for b, y in enumerate((y0, y1))
         for d, z in enumerate((z0, z1))}
    faces = [((0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1)),
             ((0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0)), ((0, 0, 1), (1, 0, 1), (1, 1, 1), (0, 1, 1)),
             ((0, 0, 0), (0, 0, 1), (0, 1, 1), (0, 1, 0)), ((1, 0, 0), (1, 0, 1), (1, 1, 1), (1, 1, 0))]
    idx = {k: m.vert(p) for k, p in P.items()}
    for f in faces:
        ps = [P[k] for k in f]
        cen = tuple(sum(p[i] for p in ps) / 4 for i in range(3))
        m.quad(*[idx[k] for k in f], None, shade, flags, sub(cen, c))

def rod(m, a, b, r, shade, flags=FLAT):
    """A four-sided rod from a to b, capped, for launch rails and missiles."""
    d = sub(b, a)
    n = math.sqrt(dot(d, d))
    d = tuple(v / n for v in d)
    u = cross(d, (1, 0, 0)) if abs(d[0]) < 0.9 else cross(d, (0, 1, 0))
    un = math.sqrt(dot(u, u))
    u = tuple(v / un for v in u)
    w = cross(d, u)
    ring = lambda p: [tuple(p[k] + r * (u[k] * cu + w[k] * cw) for k in range(3))
                      for cu, cw in ((1, 0), (0, 1), (-1, 0), (0, -1))]
    ra, rb = ring(a), ring(b)
    ia, ib = [m.vert(p) for p in ra], [m.vert(p) for p in rb]
    mid = tuple((a[k] + b[k]) / 2 for k in range(3))
    for k in range(4):
        q = (ia[k], ia[(k + 1) % 4], ib[(k + 1) % 4], ib[k])
        cen = tuple((ra[k][i] + ra[(k + 1) % 4][i] + rb[k][i] + rb[(k + 1) % 4][i]) / 4 for i in range(3))
        m.quad(*q, None, shade, flags, sub(cen, mid))
    m.quad(*ia, None, shade, flags, tuple(-v for v in d))
    m.quad(*ib, None, shade, flags, d)

def ew_radar():
    m = Mesh()
    box(m, -0.16, 0.16, -0.10, 0.02, -0.16, 0.16, TEX_WALL, col(GREY, 12))
    box(m, -0.10, 0.02, 0.02, 0.14, -0.12, 0.00, TEX_WALL, col(GREY, 10))
    flat_box(m, -0.025, 0.025, 0.02, MAST[EW], 0.04, 0.09, col(GREY, 18))
    return m

def sam_site():
    m = Mesh()
    flat_box(m, -0.14, 0.14, -0.10, 0.01, -0.14, 0.14, col(EARTH, 14))     # the revetment
    flat_box(m, -0.05, 0.05, 0.01, 0.05, -0.08, 0.06, col(GRASS, 9))       # the launcher
    for x in (-0.03, 0.03):
        rod(m, (x, 0.05, -0.06), (x, 0.13, 0.05), 0.010, col(WHITE, 26))     # two rounds
    flat_box(m, 0.07, 0.10, 0.01, MAST[SAM], 0.07, 0.10, col(GREY, 14))    # the dish's mast
    return m

def dish(half, height):
    """A dish on its mast top, facing +z, bowed back at the edges."""
    m = Mesh()
    xs = (-half, -half / 3, half / 3, half)
    zs = (-half * 0.3, 0, 0, -half * 0.3)
    bot = [m.vert((x, 0.0, z)) for x, z in zip(xs, zs)]
    top = [m.vert((x, height, z + height * 0.3)) for x, z in zip(xs, zs)]
    for k in range(3):
        m.quad(bot[k], bot[k + 1], top[k + 1], top[k], None, col(WHITE, 22), FLAT | DOUBLE, (0, 0, 1))
    return m

def missile():
    m = Mesh()
    rod(m, (0, 0, -0.06), (0, 0, 0.04), 0.012, col(WHITE, 26))
    nose = m.vert((0, 0, 0.08))
    for k, (a, b) in enumerate((((0.012, 0), (0, 0.012)), ((0, 0.012), (-0.012, 0)),
                                ((-0.012, 0), (0, -0.012)), ((0, -0.012), (0.012, 0)))):
        pa, pb = m.vert((a[0], a[1], 0.04)), m.vert((b[0], b[1], 0.04))
        m.tri(nose, pa, pb, (0, 0), (0, 0), (0, 0), col(RED, 20), FLAT,
              ((a[0] + b[0]), (a[1] + b[1]), 0.02))
    for (dx, dy) in ((0.03, 0), (0, 0.03)):                                 # the exhaust
        q = [m.vert((-dx, -dy, -0.06)), m.vert((dx, dy, -0.06)), m.vert((0, 0, -0.16))]
        m.tri(*q, (0, 0), (0, 0), (0, 0), col(SAND, 31), FLAT | UNLIT | DOUBLE, (dy, dx, 0))
    return m

def bomb():
    m = Mesh()
    rod(m, (0, 0, -0.04), (0, 0, 0.04), 0.010, col(GREY, 10))
    for (dx, dy) in ((0.025, 0), (0, 0.025)):                               # fins
        q = [m.vert((-dx, -dy, -0.04)), m.vert((dx, dy, -0.04)), m.vert((0, 0, -0.01))]
        m.tri(*q, (0, 0), (0, 0), (0, 0), col(GREY, 6), FLAT | DOUBLE, (dy, dx, 0))
    return m

def octahedron(r, shade, flags):
    m = Mesh()
    P = {'t': (0, r, 0), 'b': (0, -r, 0), 'e': (r, 0, 0), 'w': (-r, 0, 0), 'n': (0, 0, r), 's': (0, 0, -r)}
    faces = [(y, a, b) for y in 'tb' for a, b in (('e', 'n'), ('n', 'w'), ('w', 's'), ('s', 'e'))]
    idx = {k: m.vert(p) for k, p in P.items()}
    for n, f in enumerate(faces):
        ps = [P[k] for k in f]
        m.tri(*[idx[k] for k in f], (0, 0), (0, 0), (0, 0), shade(n), flags,
              tuple(sum(p[i] for p in ps) for i in range(3)))
    return m

# A MiG-29-ish: faceted fuselage, cranked delta, twin fins, and an unlit
# afterburner that is most of what a night pursuit shows. Nose on +z.
def mig():
    m = Mesh()
    P = {'N': (0, 0.020, 0.120), 'T': (0, 0.040, 0.010), 'U': (0, 0.002, 0.010),
         'L': (-0.022, 0.020, 0.000), 'R': (0.022, 0.020, 0.000), 'E': (0, 0.022, -0.100)}
    faces = [('N', 'L', 'T'), ('N', 'T', 'R'), ('N', 'U', 'L'), ('N', 'R', 'U'),
             ('T', 'L', 'E'), ('T', 'E', 'R'), ('U', 'E', 'L'), ('U', 'R', 'E')]
    facet_hull(m, P, faces, lambda f: col(GREY, 13 if 'T' in f else 7), (0, 0.020, 0.0))
    for sx in (-1, 1):
        a, b, c = (sx * 0.015, 0.020, 0.040), (sx * 0.095, 0.018, -0.070), (sx * 0.015, 0.020, -0.085)
        m.tri(m.vert(a), m.vert(b), m.vert(c), (0, 0), (0, 0), (0, 0), col(GREY, 10), FLAT | DOUBLE, UP)
        a, b, c = (sx * 0.020, 0.030, -0.060), (sx * 0.030, 0.075, -0.105), (sx * 0.020, 0.030, -0.100)
        m.tri(m.vert(a), m.vert(b), m.vert(c), (0, 0), (0, 0), (0, 0), col(GREY, 9), FLAT | DOUBLE, (1, 0, 0))
    q = [m.vert((-0.012, 0.022, -0.100)), m.vert((0.012, 0.022, -0.100)), m.vert((0, 0.022, -0.170))]
    m.tri(*q, (0, 0), (0, 0), (0, 0), col(SAND, 31), FLAT | UNLIT | DOUBLE, UP)
    return m

def pier(m, x, y0, y1, h, shade):
    """A slab across the span's width, one DOUBLE quad: a pier seen from
    along the bridge is a line either way."""
    q = [m.vert((x, y0, -h)), m.vert((x, y0, h)), m.vert((x, y1, h)), m.vert((x, y1, -h))]
    m.quad(*q, None, shade, FLAT | DOUBLE, (1, 0, 0))

def deck(m, x0, x1):
    """The bridge's roadway from x0 to x1: its top, both sides and the
    underside, which is what a low run sees."""
    y0, y1, h = 0.40, 0.50, 0.30
    v = {(a, b, d): m.vert((x, y, z)) for a, x in enumerate((x0, x1)) for b, y in enumerate((y0, y1))
         for d, z in enumerate((-h, h))}
    m.quad(v[0, 1, 0], v[1, 1, 0], v[1, 1, 1], v[0, 1, 1], None, col(GREY, 12), FLAT, UP)
    m.quad(v[0, 0, 0], v[1, 0, 0], v[1, 1, 0], v[0, 1, 0], None, col(GREY, 8), FLAT, (0, 0, -1))
    m.quad(v[0, 0, 1], v[1, 0, 1], v[1, 1, 1], v[0, 1, 1], None, col(GREY, 8), FLAT, (0, 0, 1))
    m.quad(v[0, 0, 0], v[1, 0, 0], v[1, 0, 1], v[0, 0, 1], None, col(GREY, 5), FLAT, (0, -1, 0))

def lamp(m, x, y, z):
    a, b = m.vert((x - 0.05, y, z)), m.vert((x + 0.05, y, z))
    m.tri(a, b, m.vert((x, y + 0.12, z)), (0, 0), (0, 0), (0, 0), col(SAND, 31), FLAT | UNLIT | DOUBLE, (0, 0, 1))

# The bridge: eighteen units of roadway across the mouth of the bay on two
# piers, with unlit lamps -- all a night run sees of it. Broken, the middle
# span lies in the water between two stubs.
def bridge(broken=False):
    m = Mesh()
    if broken:
        deck(m, -9.0, -1.4)
        deck(m, 1.6, 9.0)
        q = [m.vert((-1.3, 0.42, -0.3)), m.vert((-1.3, 0.42, 0.3)), m.vert((1.2, -0.3, 0.3)), m.vert((1.2, -0.3, -0.3))]
        m.quad(*q, None, col(GREY, 6), FLAT | DOUBLE, UP)
    else:
        deck(m, -9.0, 9.0)
    for x in (-4.0, 4.0):
        pier(m, x, -1.0, 0.40, 0.2, col(GREY, 9))
    for x in ((-6.0, 6.0) if broken else (-6.0, 0.0, 6.0)):
        lamp(m, x, 0.50, 0.3)
    return m

# The factory: two halls, a chimney, and a row of lit windows. Broken, it
# is a burnt-out shell and a fire that burns all night.
def factory(broken=False):
    m = Mesh()
    if broken:
        flat_box(m, -0.70, 0.30, 0.0, 0.10, -0.45, 0.45, col(EARTH, 4))
        for (x, z) in ((-0.3, 0.0), (0.1, -0.2)):
            q = [m.vert((x - 0.12, 0.10, z)), m.vert((x + 0.12, 0.10, z)), m.vert((x, 0.40, z))]
            m.tri(*q, (0, 0), (0, 0), (0, 0), col(RED, 31), FLAT | UNLIT | DOUBLE, (0, 0, 1))
        return m
    box(m, -0.70, 0.30, 0.0, 0.35, -0.45, 0.45, TEX_WALL, col(GREY, 9))
    flat_box(m, 0.30, 0.70, 0.0, 0.22, -0.30, 0.10, col(GREY, 11))
    rod(m, (0.52, 0.0, 0.18), (0.52, 0.95, 0.18), 0.06, col(RED, 12))
    for z in (-0.46, 0.46):
        q = [m.vert((-0.60, 0.20, z)), m.vert((0.20, 0.20, z)), m.vert((0.20, 0.26, z)), m.vert((-0.60, 0.26, z))]
        m.quad(*q, None, col(SAND, 31), FLAT | UNLIT, (0, 0, z))
    return m

def rubble():
    m = Mesh()
    flat_box(m, -0.16, 0.16, -0.10, 0.01, -0.16, 0.16, col(EARTH, 5))
    rod(m, (-0.02, 0.03, 0.06), (0.30, 0.03, -0.10), 0.03, col(GREY, 6))  # the fallen mast
    flat_box(m, -0.12, -0.02, 0.01, 0.05, -0.12, 0.0, col(GREY, 5))
    return m

def hangar():
    m = Mesh()
    box(m, -0.3, 0.3, 0, 0.18, -0.4, 0.4, TEX_WALL, col(GREY, 14))
    return m

def tower():
    m = Mesh()
    box(m, -0.06, 0.06, 0, 0.30, -0.06, 0.06, TEX_WALL, col(GREY, 12))
    box(m, -0.11, 0.11, 0.30, 0.38, -0.11, 0.11, TEX_WALL, col(GREY, 10))
    return m

# ------------------------------------------------------- the enemy's sites
# Radars and SAM sites, in cells: (kind, i, j, range in units). Kind 0 is an
# early-warning radar -- a mast and a big dish -- and kind 1 a SAM launcher
# with a small one. Site 0 is mission 2's target, on the headland between
# the western sea and the bay; the SAM that guards the coast stands 32
# units off it, so a low run from the sea is inside the radar's reach and
# outside the launcher's.
#
# Kinds 2 and 3 are the night missions' targets, which see nothing (range
# 0) and carry no dish: mission 3's bridge over the mouth of the bay, and
# mission 4's factory in the hills south of the enemy's airbase.
EW, SAM, BRIDGE, FACTORY = 0, 1, 2, 3
SITES = [(EW, 30, 9, 48), (EW, 40, 40, 64),
         (SAM, 34, 16, 32), (SAM, 36, 30, 32), (SAM, 40, 44, 32), (SAM, 53, 40, 32),
         (BRIDGE, 40, 9, 0), (FACTORY, 48, 54, 0)]
TGT_SITE = 0                         # mission 2's; 3 and 4 have their own
TGT_M3, TGT_M4 = 6, 7
MAST = {EW: 0.40, SAM: 0.10, BRIDGE: 0.0, FACTORY: 0.0}   # the dish's height above the ground
DISH_OFF = {EW: (0.0, 0.065), SAM: (0.085, 0.085), BRIDGE: (0, 0), FACTORY: (0, 0)}
BRIDGE_POS = (160.0, 0.0, 38.0)      # the middle of the span, on the water

def ground_tri(x, z):
    """World height at (x, z) exactly as the 6502's groundAt and the tile
    meshes have it: the cell split on the corner-1 to corner-2 diagonal."""
    fx, fz = x / CELL, z / CELL
    i, j = int(fx), int(fz)
    fx, fz = fx - i, fz - j
    h0, h1, h2, h3 = hMap[j][i], hMap[j][i + 1], hMap[j + 1][i], hMap[j + 1][i + 1]
    if fx + fz <= 1:
        h = h0 + (h1 - h0) * fx + (h2 - h0) * fz
    else:
        h = h3 + (h2 - h3) * (1 - fx) + (h1 - h3) * (1 - fz)
    return max(0.0, (h - SEA) / HSTEP)

def site_pos(s):
    kind, i, j, rng = s
    if kind == BRIDGE:
        return BRIDGE_POS
    x, z = i * CELL + CELL / 2, j * CELL + CELL / 2
    assert tMap[j][i] not in (T_SEA, T_HOLE, T_TOWN), s
    return x, ground_tri(x, z), z

# The bomb's time of fall, in frames, from a height in 1/16 units: from
# rest, under G_BOMB 1/65536 units a frame a frame -- twice the real g, so
# a run at 100 m releases 600 m out and not 400. Quarter units were too
# coarse: at 125 m, a quarter of the height is 10% of the fall time.
G_BOMB = 20
def fall_table():
    return [min(255, int(round(math.sqrt(2 * (q / 16) / (G_BOMB / 65536))))) for q in range(256)]

# ------------------------------------------------------------ the emit
def base_centre(b):
    x0, z0, x1, z1, hb = b
    return (x0 + x1) * CELL / 2, (hb - SEA) / HSTEP, (z0 + z1) * CELL / 2

TILE_MESH = 64          # tile t (tz*8+tx) is mesh and node TILE_MESH + t
OCEAN_ID = 54          # was 2, texFarmA's id: one flat namespace, so the ocean replaced it
MESH_AIRBASE, MESH_LADDER, MESH_F117, MESH_HANGAR, MESH_TOWER = 40, 41, 42, 43, 44
MESH_EW, MESH_SAM, MESH_DISH, MESH_SDISH, MESH_MISSILE = 45, 46, 47, 48, 49
MESH_SMOKE, MESH_EXPL, MESH_BOMB, MESH_RUBBLE = 50, 51, 52, 53
MESH_MIG, MESH_BRIDGE, MESH_BRIDGEX, MESH_FACTORY, MESH_FACTORYX = 56, 57, 58, 59, 60
# each kind of site whole, and destroyed
SITE_MESH = {EW: (MESH_EW, MESH_RUBBLE), SAM: (MESH_SAM, MESH_RUBBLE),
             BRIDGE: (MESH_BRIDGE, MESH_BRIDGEX), FACTORY: (MESH_FACTORY, MESH_FACTORYX)}
# node ids of the moving parts; the statics are 10-17 and the tiles 64+.
# MIS_N slots fly: the first MIG_N are MiGs, the rest missiles of any side.
SITE_NODE, DISH_NODE, BOMB_NODE, EXPL_NODE, SMOKE_NODE, MIS_NODE = 20, 28, 38, 39, 40, 48
MIS_N, MIG_N, SMOKE_N = 8, 2, 8
assert len(SITES) <= 8 and SMOKE_NODE + SMOKE_N <= MIS_NODE and MIS_NODE + MIS_N <= TILE_MESH

# ------------------------------------------------------------- the HUD
# The cockpit is drawn on the HDMI screen by one mesh node that the C64
# re-poses on the camera every frame: its vertices are camera space. Its
# panel is HUD_ROWS x 40 characters at the bottom of the screen, cut into
# 4-character segments, each its own 32x8 texture that the 6502 renders from
# the VIC-II screen and re-uploads when that segment changes -- the mesh
# itself never changes. A texture upload leaks its 256 bytes (the arena is a
# bump allocator); a mesh upload of the same panel would leak about 6 KB.
NODE_HUD, MESH_HUD = 5, 55
HUD_ROWS, HUD_SEGC = 6, 10          # 6 rows of 10 segments
HUD_TEX0 = 128                      # segment textures 128..187: past the tiles
MAP_TEX = HUD_TEX0 + HUD_ROWS * HUD_SEGC
HUD_D = 18.0                        # model z; x SET_SCALE 1/256 = 0.070 world
PANEL_Y0 = 200 - 8 * HUD_ROWS
MAP_X0, MAP_Y0, MAP_PX = 252, 4, 64 # a 16x16 map texture, 4 pixels a cell

def fw_focal():
    """The firmware's own focal for FOV $3000 at 320 wide, off its sin table."""
    import re
    t = open(os.path.join(HERE, "..", "Source", "Firmware", "gpu64_3d_sintab.h")).read()
    tab = [int(x) for x in re.findall(r"-?\d+", t[t.index("{") + 1:t.rindex("}")])]
    half = 0x3000 // 2
    s, c = tab[half >> 6], tab[((half + 16384) & 0xffff) >> 6]
    return (160 * c * 65536 // s) / 65536

def hud_mesh():
    m = Mesh()
    f = fw_focal()
    def v(sx, sy):
        return m.vert(((sx - 160) * HUD_D / f, (100 - sy) * HUD_D / f, HUD_D))
    g = [[v(q * 32, PANEL_Y0 + r * 8) for q in range(HUD_SEGC + 1)] for r in range(HUD_ROWS + 1)]
    for r in range(HUD_ROWS):
        for q in range(HUD_SEGC):
            m.quad(g[r][q], g[r][q + 1], g[r + 1][q + 1], g[r + 1][q],
                   [(0, 0), (32, 0), (32, 8), (0, 8)], HUD_TEX0 + r * HUD_SEGC + q,
                   UNLIT | DOUBLE, (0, 0, -1))
    x0, y0, x1, y1 = MAP_X0, MAP_Y0, MAP_X0 + MAP_PX, MAP_Y0 + MAP_PX
    m.quad(v(x0, y0), v(x1, y0), v(x1, y1), v(x0, y1),
           [(0, 0), (16, 0), (16, 16), (0, 16)], MAP_TEX, UNLIT | DOUBLE, (0, 0, -1))
    return m

def hud_font():
    sys.path.insert(0, os.path.join(HERE, "prgsim"))
    from gpu64font import FONT
    return [b for code in range(64) for b in FONT[0][code]]

# C64 colour code -> palette index, for the panel's text and the map's marks
C64PAL = [0, col(WHITE, 31), col(RED, 31), col(SKY, 31), col(RED, 18), col(GRASS, 31),
          col(SEAC, 24), col(SAND, 31), col(RED, 22), col(EARTH, 20), col(SAND, 24),
          col(GREY, 8), col(GREY, 16), col(GRASS, 24), col(SKY, 22), col(GREY, 24)]
# cell type -> the map's colour for it
MAPPAL = [col(SEAC, 16), col(GRASS, 20), col(EARTH, 22), col(SAND, 18), col(GRASS, 11),
          col(GREY, 17), col(SAND, 28), col(GREY, 26), col(GREY, 11)] + [col(SEAC, 16)] * 7
HUD_BG = col(GREY, 3)

def u88(v):
    """World x/z: unsigned 8.8 units, the integer byte is the 16.16 hi word's low byte."""
    r = int(round(v * 256))
    assert 0 <= r <= 0xffff, v
    return r

def statics():
    """(node, mesh, x, y, z, yaw byte) -- everything that never moves."""
    out = []
    for k, b in enumerate(BASES):
        x, y, z = base_centre(b)
        out.append((10 + k * 4, MESH_AIRBASE, x, y, z, 0))
        out.append((11 + k * 4, MESH_HANGAR, x - 2.4, y, z - 6, 0))
        out.append((12 + k * 4, MESH_HANGAR, x - 2.4, y, z - 4, 0))
        out.append((13 + k * 4, MESH_TOWER, x + 2.4, y, z - 2, 0))
    return out

def emit():
    HUDc = col(GRASS, 31)
    global HUD
    HUD = HUDc
    o = []
    e = o.append
    def rows(label, data, per=16):
        e(label)
        for k in range(0, len(data), per):
            e("\t.byte " + ", ".join("$%02x" % (b & 0xff) for b in data[k:k + per]))
    e("; Generated by tools/gen_nighthawk.py -- do not edit by hand.")
    e("; The theatre, meshes and textures of gpu64_demo_nighthawk.a.")
    e("")
    for k, v in (("NC", NC), ("NT", NT), ("SEA", SEA), ("TEX_SEA", TEX_SEA),
                 ("TILE_MESH", TILE_MESH), ("OCEAN_ID", OCEAN_ID),
                 ("MESH_AIRBASE", MESH_AIRBASE), ("MESH_LADDER", MESH_LADDER),
                 ("MESH_F117", MESH_F117), ("SKY_COL", col(SKY, 26)),
                 ("NODE_HUD", NODE_HUD), ("MESH_HUD", MESH_HUD), ("HUD_ROWS", HUD_ROWS),
                 ("HUD_SEGS", HUD_ROWS * HUD_SEGC), ("HUD_TEX0", HUD_TEX0),
                 ("MAP_TEX", MAP_TEX), ("HUD_BG", HUD_BG),
                 ("NIGHT_COL", col(SKY, 3)), ("T_SEA", T_SEA), ("T_HOLE", T_HOLE),
                 ("T_TOWN", T_TOWN), ("T_ROCK", T_ROCK), ("T_FOREST", T_FOREST),
                 ("T_SAND", T_SAND)):
        e("%-13s = %d" % (k, v))
    for k, b in enumerate(BASES):
        x0, z0, x1, z1, hb = b
        cx, cy, cz = base_centre(b)
        e("BASE%d_X0 = %d\t\t; world units" % (k, x0 * CELL))
        e("BASE%d_X1 = %d" % (k, x1 * CELL))
        e("BASE%d_Z0 = %d" % (k, z0 * CELL))
        e("BASE%d_Z1 = %d" % (k, z1 * CELL))
        e("BASE%d_Y88 = %d\t; the surface, 8.8 units" % (k, q88(cy)))
        e("BASE%d_CX = %d" % (k, int(cx)))
    e("RWY_HALF88 = %d" % q88(RWY_HALF))
    e("")
    # resident: the maps
    rows("hMap", [hMap[j][i] for j in range(NC + 1) for i in range(NC + 1)], 65)
    packed = []
    for j in range(NC):
        for i in range(0, NC, 2):
            packed.append(tMap[j][i] | (tMap[j][i + 1] << 4))
    rows("tMap", packed, 32)
    e("texOfType\t; cell type -> texture id (0: no faces)")
    e("\t.byte " + ", ".join(str(TEX_OF.get(t, 0)) for t in range(16)))
    tiles = [tz * NT + tx for tz in range(NT) for tx in range(NT) if tile_blobs(tx, tz)]
    e("TILE_N = %d" % len(tiles))
    rows("tileList", tiles)
    st = statics()
    e("STATIC_N = %d" % len(st))
    for name, fn in (("stNode", lambda s: s[0]), ("stMesh", lambda s: s[1]),
                     ("stXLo", lambda s: u88(s[2]) & 0xff), ("stXHi", lambda s: u88(s[2]) >> 8),
                     ("stYLo", lambda s: q88(s[3]) & 0xff), ("stYHi", lambda s: (q88(s[3]) >> 8) & 0xff),
                     ("stZLo", lambda s: u88(s[4]) & 0xff), ("stZHi", lambda s: u88(s[4]) >> 8),
                     ("stYaw", lambda s: s[5])):
        e("%s\t.byte %s" % (name, ", ".join(str(fn(s_)) for s_ in st)))
    for k, v in (("SITE_N", len(SITES)), ("TGT_SITE", TGT_SITE), ("SITE_NODE", SITE_NODE),
                 ("DISH_NODE", DISH_NODE), ("MIS_NODE", MIS_NODE), ("BOMB_NODE", BOMB_NODE),
                 ("EXPL_NODE", EXPL_NODE), ("SMOKE_NODE", SMOKE_NODE), ("MIS_N", MIS_N),
                 ("SMOKE_N", SMOKE_N), ("MESH_EW", MESH_EW), ("MESH_SAM", MESH_SAM),
                 ("MESH_DISH", MESH_DISH), ("MESH_SDISH", MESH_SDISH),
                 ("MESH_MISSILE", MESH_MISSILE), ("MESH_SMOKE", MESH_SMOKE),
                 ("MESH_EXPL", MESH_EXPL), ("MESH_BOMB", MESH_BOMB),
                 ("MESH_RUBBLE", MESH_RUBBLE), ("G_BOMB", G_BOMB),
                 ("MIG_N", MIG_N), ("MESH_MIG", MESH_MIG), ("TGT_M3", TGT_M3),
                 ("TGT_M4", TGT_M4), ("K_EW", EW), ("K_SAM", SAM), ("K_BRIDGE", BRIDGE),
                 ("K_FACTORY", FACTORY)):
        e("%-13s = %d" % (k, v))
    sp = [site_pos(s_) for s_ in SITES]
    s16 = lambda v: int(round(v * 16))
    for name, vals in (
            ("siteKind", [s_[0] for s_ in SITES]),
            ("siteMeshA", [SITE_MESH[s_[0]][0] for s_ in SITES]),
            ("siteMeshD", [SITE_MESH[s_[0]][1] for s_ in SITES]),
            ("siteRange", [s_[3] for s_ in SITES]),
            ("siteXLo", [u88(p[0]) & 0xff for p in sp]), ("siteXHi", [u88(p[0]) >> 8 for p in sp]),
            ("siteYLo", [q88(p[1]) & 0xff for p in sp]), ("siteYHi", [(q88(p[1]) >> 8) & 0xff for p in sp]),
            ("siteZLo", [u88(p[2]) & 0xff for p in sp]), ("siteZHi", [u88(p[2]) >> 8 for p in sp]),
            ("dishXLo", [u88(p[0] + DISH_OFF[s_[0]][0]) & 0xff for p, s_ in zip(sp, SITES)]),
            ("dishZLo", [u88(p[2] + DISH_OFF[s_[0]][1]) & 0xff for p, s_ in zip(sp, SITES)]),
            ("dishYLo", [q88(p[1] + MAST[s_[0]]) & 0xff for p, s_ in zip(sp, SITES)]),
            ("dishYHi", [(q88(p[1] + MAST[s_[0]]) >> 8) & 0xff for p, s_ in zip(sp, SITES)]),
            # the radar's head and the ground under the site, in 1/16 units
            ("siteX16Lo", [s16(p[0]) & 0xff for p in sp]), ("siteX16Hi", [s16(p[0]) >> 8 for p in sp]),
            ("siteZ16Lo", [s16(p[2]) & 0xff for p in sp]), ("siteZ16Hi", [s16(p[2]) >> 8 for p in sp]),
            ("siteH16", [s16(p[1] + MAST[s_[0]]) for p, s_ in zip(sp, SITES)]),
            ("siteG16", [s16(p[1]) for p in sp])):
        assert all(0 <= v <= 255 for v in vals), name
        e("%s\t.byte %s" % (name, ", ".join(str(v) for v in vals)))
    rows("fallT\t; frames to fall a height in 1/16 units", fall_table())
    rows("hudFont\t; the C64 character ROM, screen codes $00-$3f", hud_font())
    e("c64Pal\t.byte " + ", ".join(str(c) for c in C64PAL))
    e("mapPal\t.byte " + ", ".join(str(c) for c in MAPPAL))
    e("")
    # upload-once: everything below is dead after start-up, and the tile
    # generator's scratch buffer is laid over it
    e("uploadOnce")
    rows("palData", palette())
    for label, tid, data in TEXTURES:
        rows(label, pack_tex(data), 32)
    e("TEX_N = %d" % len(TEXTURES))
    e("texTab\t\t; id, address")
    for label, tid, data in TEXTURES:
        e("\t.word %d, %s" % (tid, label))
    meshes = [(MESH_AIRBASE, "Airbase", airbase()), (MESH_LADDER, "Ladder", ladder()),
              (MESH_F117, "F117", f117()), (MESH_HANGAR, "Hangar", hangar()),
              (MESH_TOWER, "Tower", tower()), (MESH_EW, "Ew", ew_radar()),
              (MESH_SAM, "Sam", sam_site()), (MESH_DISH, "Dish", dish(0.16, 0.12)),
              (MESH_SDISH, "SDish", dish(0.06, 0.05)), (MESH_MISSILE, "Missile", missile()),
              (MESH_SMOKE, "Smoke", octahedron(0.05, lambda n: col(WHITE, 18 + n % 2 * 4), FLAT)),
              (MESH_EXPL, "Expl", octahedron(0.10, lambda n: col(SAND, 31) if n % 2 else col(RED, 31),
                                             FLAT | UNLIT)),
              (MESH_BOMB, "Bomb", bomb()), (MESH_RUBBLE, "Rubble", rubble()),
              (MESH_HUD, "Hud", hud_mesh()), (MESH_MIG, "Mig", mig()),
              (MESH_BRIDGE, "Bridge", bridge()), (MESH_BRIDGEX, "BridgeX", bridge(True)),
              (MESH_FACTORY, "Factory", factory()), (MESH_FACTORYX, "FactoryX", factory(True))]
    for mid, name, m in meshes:
        rows("mv" + name, m.vblob())
        rows("mf" + name, m.fblob())
    e("MESH_N = %d" % len(meshes))
    e("meshTab\t\t; id, vertex blob, its length, face blob, its length, faces")
    for mid, name, m in meshes:
        e("\t.word %d, mv%s, %d, mf%s, %d, %d" % (mid, name, len(m.v) * 6, name, len(m.f) * 12, len(m.f)))
    e("uploadEnd")
    open(OUT, "w").write("\n".join(o) + "\n")
    print("-> %s: %d tiles, %d statics, meshes %s" % (os.path.relpath(OUT), len(tiles), len(st),
          ", ".join("%s %dv/%df" % (n, len(m.v), len(m.f)) for _, n, m in meshes)))

# ------------------------------------------------------------------ probe
def ang16(deg): return int(round(deg * 65536 / 360)) & 0xffff
def fx16(v): return int(round(v * 65536))

def probe(outdir, frames):
    """A scenesim stream of the bare theatre, flown by a scripted camera:
    a low pass over the coast, a climb, then orbits at altitude looking at
    as much of the theatre as the far plane allows."""
    os.makedirs(outdir, exist_ok=True)
    lines = ["# nighthawk probe: generated by tools/gen_nighthawk.py --probe", "RESET"]
    def blob(name, data):
        open(os.path.join(outdir, name), "wb").write(bytes(b & 0xff for b in data))
        return name
    lines.append("PAL " + blob("pal.bin", palette()))
    lines.append("COLORMAP")
    for label, tid, data in TEXTURES:
        lines.append("TEX %d %s 5 5" % (tid, blob("t%d.bin" % tid, data)))
    tiles = []
    tri_total = 0
    for tz in range(NT):
        for tx in range(NT):
            r = tile_blobs(tx, tz)
            if r is None:
                continue
            mid = 200 + tz * NT + tx
            lines.append("MESH %d %s %s" % (mid, blob("m%dv.bin" % mid, r[0]), blob("m%df.bin" % mid, r[1])))
            tiles.append((mid, tx, tz))
            tri_total += r[2]
    ov, of, _ = ocean_blobs()
    lines.append("MESH 199 %s %s" % (blob("oceanv.bin", ov), blob("oceanf.bin", of)))

    # the path: (frame) -> (x, y, z, yaw deg, pitch deg)
    def cam(f):
        if f < 150:     # low level, 150 m AGL, from the sea towards the hills
            t = f / 150
            x, z = 40 + t * 60, 40 + t * 60
            return x, ground_y(x, z) + 1.5, z, 45, 3
        if f < 250:     # climb to 1500 m
            t = (f - 150) / 100
            x, z = 100 + t * 30, 100 + t * 30
            return x, ground_y(x, z) * (1 - t) + 1.5 * (1 - t) + 15 * t, z, 45, 3 + 7 * t
        a = (f - 250) * 360 / 200       # orbit radius 60 round the centre
        x, z = 128 + 60 * math.sin(math.radians(a)), 128 - 60 * math.cos(math.radians(a))
        return x, 15, z, a - 90 + 360 * 0, 10

    for f in range(frames):
        x, y, z, yaw, pitch = cam(f)
        lines += ["FRAME %d %d" % (f + 1, f & 1),
                  "ST vp 0 0 320 200",
                  "ST persp %d %d %d" % (ang16(70), 16384, 8388352),
                  "ST light -60 200 -90 10",
                  "ST bg %d 1" % col(SKY, 26),
                  "CAM 1"]
        slot = 0
        def node(nid, typ, px, py, pz, yw, pt, rl, mesh):
            nonlocal slot
            lines.append("N %d %d %d 1 %d %d %d %d %d %d 256 %d 0 0 0 0 0 0"
                         % (slot, nid, typ, fx16(px), fx16(py), fx16(pz),
                            ang16(yw), ang16(pt), ang16(rl), mesh))
            slot += 1
        node(1, 2, x, y, z, yaw, pitch, 0, 0)
        ox = math.floor(x / OCEAN_STEP + 0.5) * OCEAN_STEP
        oz = math.floor(z / OCEAN_STEP + 0.5) * OCEAN_STEP
        node(2, 1, ox, 0, oz, 0, 0, 0, 199)
        for (mid, tx, tz) in tiles:
            node(10 + mid - 200, 1, tx * TILE + TILE / 2, 0, tz * TILE + TILE / 2, 0, 0, 0, mid)
        lines.append("ENDFRAME")
    open(os.path.join(outdir, "frames.txt"), "w").write("\n".join(lines) + "\n")
    print("probe: %d tiles, %d terrain triangles, %d frames -> %s"
          % (len(tiles), tri_total, frames, outdir))

def mapdump():
    ch = {T_SEA: "~", T_GRASS: ".", T_FARMA: ":", T_FARMB: ";", T_FOREST: "T",
          T_ROCK: "^", T_SAND: "_", T_TOWN: "#", T_HOLE: "="}
    for j in reversed(range(NC)):
        print("".join(ch[tMap[j][i]] for i in range(NC)))
    print("height bytes: min %d max %d  (%.0f m max)"
          % (min(map(min, hMap)), max(map(max, hMap)), (max(map(max, hMap)) - SEA) / HSTEP * 100))

if __name__ == "__main__":
    if len(sys.argv) == 1:
        emit()
    if "--map" in sys.argv:
        mapdump()
    if "--probe" in sys.argv:
        d = sys.argv[sys.argv.index("--probe") + 1]
        probe(d, 450)

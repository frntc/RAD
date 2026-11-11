#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
Builds stunts.reu and gpu64_demo_stunt.inc: everything the game uploads,
fetches and drives by.

STUNTS.REU

  0                   the level pack (stunt_pics.py): the palette and the
                      pictures LEVEL_PICTURE draws -- the title, the menus'
                      background, a preview of every track
  GAME                the game's own data, from the first 256-byte page
                      after the pack:
    header page       "STNT" and a build id; the PRG checks this page
                      before it trusts anything else in the image
    palette           768 bytes, 8 hues x 32 levels
    textures          32x32, one byte a texel
    meshes            every vertex and face blob, each stored once however
                      many meshes share it
    fonts             the menus' glyphs, opaque on the menu panel colour:
                      small 9x9 cells (8x8 and a shadow) a 128-byte
                      stride, big 18x18 cells a 512-byte stride
    track blocks      eight of them, 15 pages each, page-aligned: the
                      tables the physics reads, fetched whole into $9000
                      before a race

All of it reaches gpu64 as SPACE_REU descriptors, so none of it crosses
the bus byte by byte. The track blocks come into C64 RAM through class 0
MAT_TRANSPOSE ($84) used as a 256-byte copy, one page at a time, each
checked against a (sum, xor) pair in the PRG and fetched again until it
matches.

A TRACK BLOCK (at TRK in C64 RAM)

  page 0-1   cxLo, cxHi    the centre line, signed 8.8 world units
  page 2-3   czLo, czHi
  page 4-5   cyLo, cyHi    the road height
  page 6-7   hdLo, hdHi    heading at each point, 16-bit binary angle
  page 8-9   dhLo, dhHi    how much it turns to the next point
  page 10    ptTab         segment pitch, binary degrees, + = nose down
  page 11    flTab         bit 0 no road, bit 1 a drawbridge segment
  page 12    aiSpd         the autopilot's speed here (vHi), 0 = cruise
  page 13    aiFlg         bit 0 boost here, bit 1 single file
  page 14    params        the chunk nodes (staticTab) and the drawbridge

Writes games/stunts/gpu64_demo_stunt.inc and games/stunts/stunts.reu. Regenerate with:
    python3 games/stunts/gen_stunt.py
"""

import math, os, random, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "gpu64_demo_stunt.inc")
REU = os.path.join(HERE, "stunts.reu")
sys.path.insert(0, HERE)

import stunt_tracks as T
import stunt_pics
import stunt_cockpit as K
import stunt_mountains as MT
from stunt_pics import FONT

N, CHUNK, NCHUNK = T.N, T.CHUNK, T.NCHUNK
q88 = T.q88
Mesh = T.Mesh
FLAT, DOUBLE, UNLIT = T.FLAT, T.DOUBLE, T.UNLIT

TEX_ROAD, TEX_START, TEX_SIDE, TEX_GRASS = 1, 2, 3, 4
TEX_MTN = 5                 # MT.PANELS of them: the mountains
TEX_BAL = TEX_MTN + 16      # BAL_N of them: the balloons

# palette: 8 hues x 32 levels, level 31 the hue itself, level 0 black
HUES = [               # the original's: flat blue sky, khaki everywhere
    (156, 150, 104),    # 0 the ground, olive khaki
    (150, 150, 162),    # 1 grey: the distant hills, the cars' dark parts
    (76, 148, 252),     # 2 sky
    (232, 44, 32),      # 3 red: the player, the wall panels, the edge line
    (40, 84, 236),      # 4 blue: the opponent, the menus
    (252, 224, 40),     # 5 yellow: the edge line
    (210, 200, 150),    # 6 khaki: the road
    (244, 244, 244),    # 7 white: the wall panels
]
def col(h, l): return h * 32 + l

GRASS, GREY, SKY, RED, BLUE, YELLOW, EARTH, WHITE = range(8)
KHAKI = EARTH
T.UNDER_COL = col(GREY, 8)

# --------------------------------------------------------------- textures
rnd = random.Random(64)

def tex(fn, w=32, h=32):
    return bytes(fn(u, v) for v in range(h) for u in range(w))   # class 1: v*w + u

def edge(u, v):
    """The road's two edge lines: yellow, broken by dark red, a segment each."""
    if u == 0 or u == 31:
        return col(YELLOW, 30) if (v // 8) % 2 else col(RED, 13)
    return None

def road(u, v):                                             # khaki, a little grain
    return edge(u, v) or col(KHAKI, 27 + (rnd.randrange(5) == 0))

def start(u, v):                                            # a chequered band
    if edge(u, v):
        return edge(u, v)
    if 10 <= v < 22:
        return col(WHITE, 30) if ((u // 3) + (v // 3)) % 2 else col(GREY, 3)
    return road(u, v)

def side(u, v):
    """The embankment walls: red and white panels, a segment each, split by
    a dark line -- the original's, from the road down to the ground."""
    if u % 8 == 0:
        return col(RED, 10)
    if (u // 8) % 2:
        return col(WHITE, 28 + rnd.randrange(2))
    return col(RED, 22 + rnd.randrange(2))

def grass(u, v):                                            # olive khaki ground
    n = rnd.randrange(4)
    if (u * 7 + v * 13) % 29 == 0:
        return col(GRASS, 20)
    return col(GRASS, 24 + n)

textures = [                # (id, w, h, texels)
    (TEX_ROAD, 32, 32, tex(road)),
    (TEX_START, 32, 32, tex(start)),
    (TEX_SIDE, 32, 32, tex(side)),
    (TEX_GRASS, 32, 32, tex(grass)),
] + K.build(col)                # the cockpit's sprites

# the balloons: the C64 User's Guide's sprite (UP, UP, AND AWAY), 24x21,
# feet at the bottom of a 32x32 texture. The envelope in a hue of its own,
# the C= logo (the holes it encloses) in a second, the ropes grey and the
# basket brown; whatever is left is index 0, the sky through it.
BALLOON = [0, 127, 0, 1, 255, 192, 3, 255, 224, 3, 231, 224, 7, 217, 240, 7, 223, 240,
           7, 217, 240, 3, 231, 224, 3, 255, 224, 3, 255, 224, 2, 255, 160, 1, 127, 64,
           1, 62, 64, 0, 156, 128, 0, 156, 128, 0, 73, 0, 0, 73, 0, 0, 62, 0,
           0, 62, 0, 0, 62, 0, 0, 28, 0]
BAL_HUES = [(RED, WHITE), (YELLOW, RED), (BLUE, WHITE)]
BAL_N = len(BAL_HUES)

def balloon(hue, logo):
    bit = [[(BALLOON[r * 3 + c // 8] >> (7 - c % 8)) & 1 for c in range(24)] for r in range(21)]
    outside, todo = set(), [(r, c) for r in range(21) for c in (0, 23)]
    while todo:                                 # the holes the outline encloses
        r, c = todo.pop()
        if 0 <= r < 21 and 0 <= c < 24 and not bit[r][c] and (r, c) not in outside:
            outside.add((r, c))
            todo += [(r + 1, c), (r - 1, c), (r, c + 1), (r, c - 1)]
    t = bytearray(32 * 32)
    for r in range(21):
        run = ''.join('#' if b else '.' for b in bit[r])
        for c in range(24):
            if bit[r][c]:
                if r >= 17:
                    v = col(EARTH, 11)                      # the basket
                elif r >= 10 and not ('###' in run[max(0, c - 2):c + 3]):
                    v = col(GREY, 9)                        # a rope
                else:                                       # the envelope, lit
                    edge = c >= len(run.rstrip('.')) - 2    #   from the left
                    v = col(hue, 18 if r >= 10 or edge else 28)
            elif (r, c) not in outside and r < 10:
                v = col(logo, 30)                           # the C= logo
            else:
                continue
            t[(11 + r) * 32 + 4 + c] = v
    return bytes(t)

textures += [(TEX_BAL + k, 32, 32, balloon(h, l)) for k, (h, l) in enumerate(BAL_HUES)]

# where they can float: BAL_DIRS bearings round the camera, each with its
# own distance and height above the eye. Inside the mountains' ring (which
# hides whatever is beyond it), and 8-18 degrees up, clear of the cockpit.
BAL_DIRS, BAL_SIZE = 16, 15.0
balRnd = random.Random(1989)
balTab = []
for k in range(BAL_DIRS):
    a = 2 * math.pi * (k + balRnd.uniform(-0.3, 0.3)) / BAL_DIRS
    d = balRnd.uniform(44, 64)
    h = d * math.tan(math.radians(balRnd.uniform(8, 18)))
    balTab.append((q88(d * math.sin(a)), q88(h), q88(d * math.cos(a))))

# ----------------------------------------------------------------- tracks
tracks = [T.build(t) for t in T.TRACKS]

# ------------------------------------------------------------ the scenery
# ground: a grid the C64 keeps under the camera, snapped to whole cells so
# the grass texture never slides. +-112 units, so the camera is always at
# least 80 from its edge -- which is why the hills stand at 76.
GCELL, GN = 32, 7
ground = Mesh()
gv = {}
for a in range(GN + 1):
    for b in range(GN + 1):
        gv[a, b] = ground.vert((-GCELL * GN / 2 + a * GCELL, 0.0, -GCELL * GN / 2 + b * GCELL))
for a in range(GN):
    for b in range(GN):
        ground.quad(gv[a, b], gv[a + 1, b], gv[a + 1, b + 1], gv[a, b + 1],
                    [(0, 0), (255, 0), (255, 255), (0, 255)], TEX_GRASS, 0, (0, 1, 0))

# the mountains: a ring of panels round the camera, facing in, each with
# its slice of the rendered panorama, unlit so its sky is the sky's colour
hills = Mesh()
HR = MT.RING                    # inside the ground's reach -- see GCELL
SKIRT = 16.0                    # deeper than the highest track, 9
for k in range(MT.PANELS):
    a0 = 2 * math.pi * k / MT.PANELS
    a1 = 2 * math.pi * (k + 1) / MT.PANELS
    p = [hills.vert((HR * math.sin(a), y, HR * math.cos(a)))
         for a, y in ((a0, MT.Y1), (a1, MT.Y1), (a1, MT.Y0), (a0, MT.Y0))]
    am = (a0 + a1) / 2
    inward = (-math.sin(am), 0, -math.cos(am))
    hills.quad(p[0], p[1], p[2], p[3],
               [(0, 0), (MT.PW - 1, 0), (MT.PW - 1, MT.H - 1), (0, MT.H - 1)],
               TEX_MTN + k, UNLIT, inward)
    # the skirt: the bottom row drawn on down, for a camera high above
    # the ground (the ring rides at its height, the ground does not)
    q = [hills.vert((HR * math.sin(a), MT.Y0 - SKIRT, HR * math.cos(a))) for a in (a1, a0)]
    hills.quad(p[3], p[2], q[0], q[1],
               [(0, MT.H - 1), (MT.PW - 1, MT.H - 1), (MT.PW - 1, MT.H - 1), (0, MT.H - 1)],
               TEX_MTN + k, UNLIT, inward)

# the car: origin on the road under its centre, +z forward, 1.1 long
def box(m, x0, x1, y0, y1, z0, z1, colr, taper_front=0.0, skip=()):
    """A box (or a wedge: taper_front lowers the top of the front edge)."""
    c = {}
    for ix, x in enumerate((x0, x1)):
        for iy, y in enumerate((y0, y1)):
            for iz, z in enumerate((z0, z1)):
                yy = y - (taper_front if (iy == 1 and iz == 1) else 0)
                c[ix, iy, iz] = m.vert((x, yy, z))
    faces = {
        'top':    ([(0, 1, 0), (1, 1, 0), (1, 1, 1), (0, 1, 1)], (0, 1, 0)),
        'bottom': ([(0, 0, 0), (1, 0, 0), (1, 0, 1), (0, 0, 1)], (0, -1, 0)),
        'left':   ([(0, 0, 0), (0, 1, 0), (0, 1, 1), (0, 0, 1)], (-1, 0, 0)),
        'right':  ([(1, 0, 0), (1, 1, 0), (1, 1, 1), (1, 0, 1)], (1, 0, 0)),
        'front':  ([(0, 0, 1), (0, 1, 1), (1, 1, 1), (1, 0, 1)], (0, taper_front * 2, 1)),
        'back':   ([(0, 0, 0), (0, 1, 0), (1, 1, 0), (1, 0, 0)], (0, 0, -1)),
    }
    for name, (q, out) in faces.items():
        if name in skip:
            continue
        colr_f = colr(name) if callable(colr) else colr
        m.quad(*[c[k] for k in q], None, colr_f, FLAT, out)

def car(body):
    m = Mesh()
    dark = col(GREY, 5)
    box(m, -0.34, 0.34, 0.12, 0.30, -0.55, 0.55, lambda n: col(body, 26 if n == 'top' else 22),
        taper_front=0.10, skip=('bottom',))
    box(m, -0.22, 0.22, 0.30, 0.50, -0.28, 0.08,
        lambda n: col(GREY, 6) if n in ('front', 'left', 'right') else col(body, 18), skip=('bottom',))
    box(m, -0.36, 0.36, 0.50, 0.56, -0.56, -0.40, col(body, 24), skip=())
    box(m, -0.05, 0.05, 0.30, 0.50, -0.50, -0.44, dark, skip=('top', 'bottom'))
    for sx in (-1, 1):
        for sz in (-1, 1):
            x0 = 0.34 if sx > 0 else -0.46
            z0 = 0.26 if sz > 0 else -0.46
            box(m, x0, x0 + 0.12, 0.0, 0.26, z0, z0 + 0.22, dark, skip=('bottom',))
    return m

carRed, carBlue = car(RED), car(BLUE)

MESH_GROUND, MESH_HILLS, MESH_CARP, MESH_CARAI = 120, 121, 130, 131
def mesh_chunk(t, c): return 1000 + t * 32 + c
def mesh_deck(t, k): return 1000 + t * 32 + 16 + k

# ------------------------------------------------------------------ fonts
PANEL = stunt_pics.panel()
SMALL = [("WHITE", col(WHITE, 30)), ("YELLOW", col(YELLOW, 30)),
         ("RED", col(RED, 29)), ("GREY", col(GREY, 20)), ("SKY", col(SKY, 31))]
BIG = [("YELLOW", col(YELLOW, 30)), ("WHITE", col(WHITE, 30))]
SM_CELL, SM_STRIDE = 9, 128
BG_CELL, BG_STRIDE = 18, 512

def font_sheet(face, scale, cell, stride):
    out = bytearray()
    for code in range(64):
        g = FONT[0][code]
        px = bytearray([PANEL]) * (cell * cell)
        lit = set()
        for r in range(8):
            for b in range(8):
                if g[r] & (0x80 >> b):
                    for dy in range(scale):
                        for dx in range(scale):
                            lit.add((b * scale + dx, r * scale + dy))
        for (x, y) in lit:
            px[(y + scale // 2 + scale % 2) * cell + x + scale // 2 + scale % 2] = 0
        for (x, y) in lit:
            px[y * cell + x] = face
        out += px + bytes(stride - cell * cell)
    return bytes(out)

# ------------------------------------------------------------- the image
pal = []
for (r, g, b) in HUES:
    for l in range(32):
        pal += [round(r * l / 31), round(g * l / 31), round(b * l / 31)]
K.patch_pal(pal)                # WHITE 1-11: the cockpit's own colours

pack, lev_bytes, pics = stunt_pics.image(pal, tracks)
img = bytearray(pack)

def align(n=256):
    img.extend(bytes(-len(img) % n))

align()
GAME = len(img)
img.extend(bytes(256))              # the header page, filled in at the end

def put(data, al=1):
    align(al)
    o = len(img)
    img.extend(data)
    return o

blobs = {}
def blob(data):
    data = bytes(data)
    if data not in blobs:
        blobs[data] = put(data)
    return blobs[data]

PAL_REU = put(bytes(pal))
textures += [(TEX_MTN + k, MT.PW, MT.H, d)
             for k, d in enumerate(MT.panels(MT.panorama(pal, col(SKY, 26))))]
texTab = [(tid, put(data, 256), w, h) for tid, w, h, data in textures]

meshTab = []        # (id, vaddr, vlen, faddr, flen, faces)
def add_mesh(mid, m):
    vb, fb = m.vblob(), m.fblob()
    assert len(m.f) <= 255, (mid, len(m.f))
    meshTab.append((mid, blob(vb), len(vb), blob(fb), len(fb), len(m.f)))

add_mesh(MESH_GROUND, ground)
add_mesh(MESH_HILLS, hills)
add_mesh(MESH_CARP, carRed)
add_mesh(MESH_CARAI, carBlue)
for t, b in enumerate(tracks):
    for c, (ox, oz, m) in enumerate(b.chunks):
        add_mesh(mesh_chunk(t, c), m)
    for k, m in enumerate(b.decks):
        add_mesh(mesh_deck(t, k), m)

fonts_small = [put(font_sheet(c, 1, SM_CELL, SM_STRIDE), 256) for _, c in SMALL]
fonts_big = [put(font_sheet(c, 2, BG_CELL, BG_STRIDE), 256) for _, c in BIG]

# the track blocks
DECK_PITCH, DECK_RISE = T.deck_tables()
TRK_PAGES = 15

def track_block(t, b):
    pg = [bytearray(256) for _ in range(TRK_PAGES)]
    for i in range(N):
        pg[0][i], pg[1][i] = b.cx[i] & 0xff, (b.cx[i] >> 8) & 0xff
        pg[2][i], pg[3][i] = b.cz[i] & 0xff, (b.cz[i] >> 8) & 0xff
        pg[4][i], pg[5][i] = b.cy[i] & 0xff, (b.cy[i] >> 8) & 0xff
        pg[6][i], pg[7][i] = b.hd16[i] & 0xff, b.hd16[i] >> 8
        pg[8][i], pg[9][i] = b.dh16[i] & 0xff, (b.dh16[i] >> 8) & 0xff
        pg[10][i] = b.pt[i]
        pg[11][i] = b.fl[i]
        pg[12][i] = b.ai_spd[i]
        pg[13][i] = b.ai_flg[i]
    p = pg[14]
    for c, (ox, oz, m) in enumerate(b.chunks):
        p[c * 10:c * 10 + 10] = struct.pack('<HHhhh', 10 + c, mesh_chunk(t, c),
                                            q88(ox), 0, q88(oz))
    p[0xa0:0xb0] = b.name.encode('ascii')[:16].ljust(16, b' ')
    if b.deck is not None:
        d = b.deck
        e = (d + 2 * T.DECK_SEGS) % N
        p[0xb0] = d
        p[0xb1], p[0xb2] = b.cy[d] & 0xff, (b.cy[d] >> 8) & 0xff
        p[0xb3] = (b.hd16[d] >> 8) & 0xff
        p[0xb4:0xb8] = struct.pack('<hh', b.cx[d], b.cz[d])
        p[0xb8:0xbc] = struct.pack('<hh', b.cx[e], b.cz[e])
        for k in range(T.DECK_STEPS):
            p[0xc0 + k] = DECK_RISE[k] & 0xff
            p[0xd0 + k] = (DECK_RISE[k] >> 8) & 0xff
            p[0xe0 + k] = DECK_PITCH[k]
    return [bytes(x) for x in pg]

trkAddr, trkPages = [], []
for t, b in enumerate(tracks):
    pages = track_block(t, b)
    trkAddr.append(put(b''.join(pages), 256))
    trkPages.append(pages)

align()
build_id = stunt_pics.pack_levels.fnv(bytes(img[GAME + 256:]))
hdr = bytearray(256)
hdr[0:4] = b'STNT'
hdr[4:8] = struct.pack('<I', build_id)
img[GAME:GAME + 256] = hdr
assert len(img) <= 1024 * 1024, len(img)
with open(REU, 'wb') as f:
    f.write(img)

# ------------------------------------------------------------------ write
out = []
emit = out.append

def rows(label, data, per=16):
    emit(label)
    for k in range(0, len(data), per):
        emit("\t.byte " + ", ".join("$%02x" % (v & 0xff) for v in data[k:k + per]))

def sumxor(page):
    s = x = 0
    for v in page:
        s = (s + v) & 0xff
        x ^= v
    return s, x

emit("; Generated by games/stunts/gen_stunt.py -- do not edit by hand.")
emit("; Constants and REU addresses; the data itself is in stunts.reu.")
emit("")
emit("TRK_N      = %d" % N)
emit("TRK_CHUNKS = %d" % NCHUNK)
emit("TRACKS     = %d" % len(tracks))
emit("TEX_ROAD   = %d" % TEX_ROAD)
emit("TEX_START  = %d" % TEX_START)
emit("TEX_SIDE   = %d" % TEX_SIDE)
emit("TEX_GRASS  = %d" % TEX_GRASS)
emit("SKY_COL    = %d" % col(SKY, 26))
emit("MTN_EYE    = %d" % MT.EYE)
emit("TEX_BAL    = %d\t\t; + 0..BAL_N-1: the balloons" % TEX_BAL)
emit("BAL_N      = %d" % BAL_N)
emit("BAL_DIRS   = %d" % BAL_DIRS)
emit("BAL_SIZE   = $%04x\t\t; SET_SPRITE width and height (8.8)" % q88(BAL_SIZE))
for i, nm in enumerate(("balX", "balY", "balZ")):
    rows(nm + "Lo", [b[i] & 0xff for b in balTab])
    rows(nm + "Hi", [(b[i] >> 8) & 0xff for b in balTab])
emit("PANEL_COL  = %d" % PANEL)
emit("MESH_GROUND = %d" % MESH_GROUND)
emit("MESH_HILLS  = %d" % MESH_HILLS)
emit("MESH_CARP   = %d" % MESH_CARP)
emit("MESH_CARAI  = %d" % MESH_CARAI)
emit("DECK_SEGS   = %d" % T.DECK_SEGS)
emit("DECK_STEPS  = %d" % T.DECK_STEPS)
emit("FL_GAP      = $%02x" % T.FL_GAP)
emit("FL_DECK     = $%02x" % T.FL_DECK)
emit("AI_BOOST    = $%02x" % T.AI_BOOST)
emit("AI_SINGLE   = $%02x" % T.AI_SINGLE)
emit("AI_CRUISE   = $%02x" % T.CRUISE)
emit("")
emit("; the track block, fetched into C64 RAM before every race")
emit("TRK       = $9000")
emit("TRK_PAGES = %d" % TRK_PAGES)
for i, name in enumerate(("cxLo", "cxHi", "czLo", "czHi", "cyLo", "cyHi", "hdLo", "hdHi",
                          "dhLo", "dhHi", "ptTab", "flTab", "aiSpd", "aiFlg", "trkParm")):
    emit("%-9s = TRK + $%03x" % (name, i * 256))
emit("staticTab = trkParm")
emit("deckS     = trkParm + $b0\t; 0: no drawbridge")
emit("deckYLo   = trkParm + $b1")
emit("deckYHi   = trkParm + $b2")
emit("deckYaw   = trkParm + $b3")
emit("deckH1    = trkParm + $b4\t; x, z of the first hinge (8.8)")
emit("deckH2    = trkParm + $b8\t; and the second")
emit("deckRiseLo = trkParm + $c0")
emit("deckRiseHi = trkParm + $d0")
emit("deckPitch = trkParm + $e0")
emit("STATIC_N  = %d" % NCHUNK)
emit("")
emit("; stunts.reu")
emit("GAME_REU  = $%06x\t; the header page" % GAME)
emit("BUILD_ID  = $%08x" % build_id)
emit("PAL_REU   = $%06x" % PAL_REU)
s, x = sumxor(hdr)
emit("HDR_SUM   = $%02x" % s)
emit("HDR_XOR   = $%02x" % x)
emit("SM_CELL   = %d" % SM_CELL)
emit("BG_CELL   = %d" % BG_CELL)
for i, (name, _) in enumerate(SMALL):
    emit("FNT_%-7s = %d" % (name, i))
for i, (name, _) in enumerate(BIG):
    emit("BIG_%-7s = %d" % (name, i))
rows("fntMid\t\t; small font sheets: REU address bits 8-15, 16-23", [a >> 8 for a in fonts_small])
rows("fntHi", [a >> 16 for a in fonts_small])
rows("bigMid", [a >> 8 for a in fonts_big])
rows("bigHi", [a >> 16 for a in fonts_big])
emit("")
emit("; the track blocks: REU address bits 8-23, and every page's sum and xor")
rows("trkMid", [(a >> 8) & 0xff for a in trkAddr])
rows("trkBank", [a >> 16 for a in trkAddr])
rows("trkSum", [sumxor(p)[0] for pages in trkPages for p in pages], TRK_PAGES)
rows("trkXor", [sumxor(p)[1] for pages in trkPages for p in pages], TRK_PAGES)
emit("trkNames\t; 16 characters each")
for b in tracks:
    emit('\t.text "%s"' % b.name.ljust(16))
emit("")
emit("TEX_N = %d" % len(texTab))
emit("texTab\t\t; id, REU address (3), length (2), w shift | h shift << 4")
for tid, a, w, h in texTab:
    ws, hs = w.bit_length() - 1, h.bit_length() - 1
    assert 1 << ws == w and 1 << hs == h and 3 <= ws <= 8 and 3 <= hs <= 8 and w * h < 65536
    emit("\t.byte %d, %d, $%02x, $%02x, $%02x, $%02x, $%02x, $%02x"
         % (tid & 255, tid >> 8, a & 0xff, (a >> 8) & 0xff, a >> 16, (w * h) & 0xff, (w * h) >> 8, ws | hs << 4))
emit("")
emit("; the cockpit (stunt_cockpit.py): view-space sprites, one WORLD_TICK actor each")
for name in ("TX_PIL_L", "TX_PIL_R", "TX_TOP", "TX_DASH", "TX_FLAME", "TX_CHAIN", "TX_WHEEL",
             "TX_CAR", "TX_TYRE", "TX_CRACK", "TX_BAR", "TX_DIG", "TX_SGN", "TX_PAIR", "BAR_N"):
    emit("%-9s = %d" % (name, getattr(K, name)))
ck = K.sprites()
NODE_CK = 64
emit("CK_N = %d" % len(ck))
emit("NODE_CK = %d\t\t; + CK_*: the cockpit's sprite nodes" % NODE_CK)
for k, sp in enumerate(ck):
    emit("CK_%-7s = %d" % (sp[0], k))
emit("ckTab\t\t; node, texture, flags $03, yaw/pitch/roll 0, x y z 16.16")
ckSum0 = 0
for (name, tid, x0, y0, w, h, m) in ck:
    (px, py, pz), _ = K.pose(x0, y0, w, h, m)
    b = [NODE_CK + ck.index((name, tid, x0, y0, w, h, m)), 0, tid & 0xff, tid >> 8, 3, 0, 0, 0] + list(struct.pack('<III', px, py, pz))
    emit("\t.byte " + ", ".join("$%02x" % v for v in b) + "\t; " + name)
    ckSum0 += sum(b) - b[2] - b[4]
# the block's sum less the two bytes a frame patches in every record (the
# texture's low byte and the flags) and the header: sendCockpit adds those
emit("CK_SUM0 = $%04x" % (ckSum0 & 0xffff))
emit("ckSize\t\t; SET_SPRITE width, height (8.8)")
for (name, tid, x0, y0, w, h, m) in ck:
    _, (sw, sh) = K.pose(x0, y0, w, h, m)
    emit("\t.word $%04x, $%04x" % (sw, sh))
emit("")
emit("MESH_N = %d" % len(meshTab))
emit("meshTab\t\t; id, vertex blob (3) and length, face blob (3) and length, faces, pad")
for (mid, va, vl, fa, fl, nf) in meshTab:
    emit("\t.byte $%02x, $%02x, $%02x, $%02x, $%02x, $%02x, $%02x, $%02x, $%02x, $%02x, $%02x, $%02x, %d, 0, 0, 0"
         % (mid & 0xff, mid >> 8, va & 0xff, (va >> 8) & 0xff, va >> 16, vl & 0xff, vl >> 8,
            fa & 0xff, (fa >> 8) & 0xff, fa >> 16, fl & 0xff, fl >> 8, nf))
emit("")
emit("; stunts.reu: LEVEL_PICTURE indices (stunt_pics.py)")
for i, name in enumerate(("PIC_LOGO", "PIC_START", "PIC_CREDS", "PIC_FIRE", "PIC_MENU")):
    emit("%s = %d\t\t; %dx%d" % (name, i, pics[i][0], pics[i][1]))
emit("PIC_TRACK = %d\t\t; + track, %dx%d" % (stunt_pics.PIC_TRACK, stunt_pics.TRK_W, stunt_pics.TRK_H))
emit("PIC_TRACK_W = %d" % stunt_pics.TRK_W)
emit("PIC_TRACK_H = %d" % stunt_pics.TRK_H)
emit("PIC_FIRE_W = %d" % pics[3][0])
ph = stunt_pics.pic_head(len(tracks))
emit("PIC_HEAD = %d\t\t; + HD_*: the menus' headings, keyed" % ph)
for k, h in enumerate(stunt_pics.HEADINGS):
    emit("HD_%-10s = %d" % (h.split()[0] if not h.startswith("DIVISION") else "DIV" + h[-1], k))
emit("headW\t\t; each heading's width")
emit("\t.word " + ", ".join(str(pics[ph + k][0]) for k in range(len(stunt_pics.HEADINGS))))
emit("PIC_GLY = %d\t\t; + 64 * GLY_* + screen code: 9x9 keyed glyphs" % stunt_pics.pic_gly(len(tracks)))
for k, n in enumerate(("WHITE", "YELLOW", "SKY")):
    emit("GLY_%s = %d" % (n, k))
emit("trkName\t\t; each track's name, ASCII, 0-terminated")
for k in range(len(tracks)):
    emit("\t.word trkName%d" % k)
for k, b in enumerate(tracks):
    emit("trkName%d\t.text \"%s\"\n\t.byte 0" % (k, b.name))
emit("")

open(OUT, "w").write("\n".join(out) + "\n")

for b in tracks:
    print("%-16s minR %5.1f  y %4.1f..%4.1f  gaps %-24s deck %s"
          % (b.name, b.minr, min(b.cy) / 256, max(b.cy) / 256,
             ",".join(map(str, sorted(b.gaps))) or "-", b.deck))
print("meshes %d, blobs %d (%d bytes)" % (len(meshTab), len(blobs), sum(map(len, blobs))))
print("-> %s" % os.path.relpath(OUT))
print("-> %s (%d bytes: pack %d, game data from $%06x)" % (os.path.relpath(REU), len(img), len(pack), GAME))

# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
The cockpit: the original's view from the driver's seat, as view-space
sprites (docs/class1-3d-mesh-reference.md, "View space").

Everything here is laid out in screen pixels of the 320x200 viewport and
turned into view-space poses by pose(): a sprite's position is its feet
(bottom centre), and at depth z = m * focal / 256 one screen pixel is m/256
of a unit, so an m-layer sprite W pixels wide is W * m in SET_SPRITE's 8.8.
Smaller m is nearer; view-space nodes are drawn over the world on a cleared
depth buffer, so the layers only order the cockpit's own parts.

  m=1    the readouts and the crack
  m=1.5  the steering wheel
  m=2    the frame: the roll cage, its struts, the cowl and the panel
  m=2.5  the boost flames, out of the exhaust stacks
  m=2.75 the two front tyres
  m=3    the car's front: the engine, the exhausts, the suspension
  m=4    the crane's chains, behind the car

The frame is drawn once as a 320x200 picture and cut into four textures:
the two side columns, the bar across the top and the dashboard. Its colours
are the original's, eleven palette entries of their own (patch_pal()).

The car's front is the original's, pixel for pixel (ENGINE). Its tyres
are sprites of their own, one texture per steering position and wheel
turn, so they steer with the player and roll with the speed.

A sprite's index 0 is a hole, so black here is col(GREY, 0), never 0.
Every texture id is 256-511: the C64 patches only an id's low byte, so no
group may cross a multiple of 256.
"""

import math
import random

FOV = 0x4600
FOCAL = 160 * math.cos(FOV / 2 * math.pi / 32768) / math.sin(FOV / 2 * math.pi / 32768)

GREY, WHITE = 1, 7

# The original's colours, in WHITE's dark levels 1-11: nothing else uses
# them (WHITE is only ever drawn at 23-31), the GREY hue covers the greys
# they were, and the colormap is a nearest-colour search, so lighting is
# unchanged.
CUSTOM = [
    (148, 74, 66),      # 1 R  brick red: the cage
    (198, 132, 123),    # 2 P  pink: its lit face
    (99, 99, 99),       # 3 G  dark grey: its edges, the cowl's lip
    (181, 181, 181),    # 4 L  light grey: the readout boxes, the wheel
    (140, 140, 140),    # 5 M  mid grey
    (74, 57, 173),      # 6 B  blue: the panel
    (132, 123, 222),    # 7 b  light blue
    (156, 107, 41),     # 8 N  brown: the speed bar
    (132, 198, 206),    # 9 C  cyan: the gauge
    (214, 222, 123),    # 10 Y yellow: the boost flames
    (181, 239, 148),    # 11 V light green: the header pipes
]


def patch_pal(pal):
    for k, rgb in enumerate(CUSTOM):
        i = (WHITE * 32 + 1 + k) * 3
        pal[i:i + 3] = list(rgb)


# texture ids: offsets from 256
_B = 256
TX_PIL_L, TX_PIL_R, TX_TOP, TX_DASH = _B + 0, _B + 1, _B + 2, _B + 3
TX_FLAME = _B + 4       # 2 frames
TX_CHAIN = _B + 6
TX_WHEEL = _B + 8       # 5 steering positions
TX_CAR = _B + 16        # the body: the engine and intakes
TX_TYRE = _B + 17       # + side * 20 + steer * 4 + turn: 40, left then right
TX_CRACK = _B + 56      # + 1-15: how far the crack has run (0 is not drawn)
TX_BAR = _B + 72        # + 0-24: the speed bar, 5 pixels a step
TX_DIG = _B + 97        # 0-9, 10 a dash
TX_SGN = _B + 108       # sign and thousands: +0..+9, then -0..-9
TX_PAIR = _B + 128      # 00-99, then 100 two dashes
BAR_N = 25
CRACK_N = 16
STEERS = 5
assert TX_PAIR + 100 < 512


def build(col):
    tex = []                                    # (id, w, h, bytes)
    C = palette(col)
    BK = C['.']

    def done(tid, c):
        tex.append((tid, len(c[0]), len(c), bytes(v or 0 for row in c for v in row)))

    F = frame(C)
    done(TX_PIL_L, cut(F, 0, 0, 32, 256))
    done(TX_PIL_R, cut(F, 288, 0, 32, 256))
    done(TX_TOP, cut(F, 32, 0, 256, 32))
    done(TX_DASH, cut(F, 32, 136, 256, 64))

    done(TX_CAR, car(C))
    for side in range(2):
        for s in range(STEERS):
            for t in range(4):
                done(TX_TYRE + side * 20 + s * 4 + t, tyre(C, side, s - 2, t))
    for s in range(STEERS):
        done(TX_WHEEL + s, wheel(C, s - 2))

    # ------------------------------------------- the crack, 256x16, m=1
    # One jagged line along the cage's top bar, and holes punched through
    # it as the damage mounts; stage k shows it k/15 of the way across.
    rnd = random.Random(1988)
    pts = []
    y = 6.0
    for x in range(0, 256):
        y += rnd.choice((-1, 0, 0, 1))
        y = min(11, max(2, y))
        pts.append((x, int(y)))
    holes = [(216, 4, 6), (52, 9, 10), (150, 6, 13)]    # x, y, from stage
    for k in range(1, CRACK_N):
        c = canvas(256, 16)
        lim = 256 * k // (CRACK_N - 1)
        for (x, yy) in pts:
            if x < lim:
                put(c, x, yy, BK)
                put(c, x, yy - 1, C['W'] if x % 3 else BK)
        for (hx, hy, st) in holes:
            if k >= st:
                for dy in range(-2, 3):
                    for dx in range(-3, 4):
                        if dx * dx + dy * dy * 2 <= 9:
                            put(c, hx + dx, hy + dy, BK)
        done(TX_CRACK + k, c)

    # ------------------------------------------- the flames, m=2.5
    for f in range(2):
        done(TX_FLAME + f, flames(C, f))

    # --------------------------------------- the chains, 256x128, m=4
    # From the crane, past the cage, to the front wheels' hubs.
    c = canvas(256, 128)
    for cx in (34, 218):
        for y in range(112):
            k = y % 12
            if (y // 12) % 2 == 0:              # a link face on
                if k in (0, 11):
                    rect(c, cx - 2, y, cx + 2, y + 1, col(GREY, 18))
                else:
                    put(c, cx - 3, y, col(GREY, 18))
                    put(c, cx + 2, y, col(GREY, 10))
            else:                               # a link edge on
                rect(c, cx - 1, y, cx + 1, y + 1, col(GREY, 14))
    done(TX_CHAIN, c)

    # ------------------------------------------------- the readouts, m=1
    # Dark on the boxes' light grey, opaque, a character a cell.
    for d in range(11):
        c = canvas(8, 8, C['L'])
        glyph(c, 0, "0123456789-"[d], BK)
        done(TX_DIG + d, c)
    for neg in range(2):
        for d in range(10):
            c = canvas(16, 8, C['L'])
            sign = "-" if neg else "+"
            if d:
                glyph(c, 0, sign, BK)
                glyph(c, 8, str(d), BK)
            else:
                glyph(c, 8, sign, BK)
            done(TX_SGN + neg * 10 + d, c)
    for n in range(101):
        c = canvas(16, 8, C['L'])
        s = "--" if n == 100 else "%02d" % n
        glyph(c, 0, s[0], BK)
        glyph(c, 8, s[1], BK)
        done(TX_PAIR + n, c)

    # ------------------------------------ the speed bar, 128x8, m=1
    # Rows 6-7 only: drawn at screen y 168, over the gauge's rows 174-175.
    for k in range(BAR_N):
        c = canvas(128, 8)
        rect(c, 0, 6, k * 5, 8, C['N'])
        done(TX_BAR + k, c)

    return tex


# ---------------------------------------------------------------- drawing

def palette(col):
    C = {k: col(WHITE, 1 + i) for i, k in enumerate("RPGLMBbNCYV")}
    C['.'] = col(GREY, 0)
    C['W'] = col(WHITE, 31)
    return C


def canvas(w, h, v=None):
    return [[v] * w for _ in range(h)]


def put(c, x, y, v):
    if 0 <= y < len(c) and 0 <= x < len(c[0]):
        c[y][x] = v


def rect(c, x0, y0, x1, y1, v):
    for y in range(y0, y1):
        for x in range(x0, x1):
            put(c, x, y, v)


def cut(F, x0, y0, w, h):
    c = canvas(w, h)
    for y in range(h):
        for x in range(w):
            if y0 + y < len(F):
                c[y][x] = F[y0 + y][x0 + x]
    return c


# The cowl's top edge: flat across the middle, falling a row every 8
# pixels out to the pillars.
def cowl_top(x):
    return 152 + max(0, 84 - x, x - 235) / 7.7


def frame(C):
    """The 320x200 picture: None where the windscreen is."""
    F = canvas(320, 200)
    BK = C['.']
    for y in range(200):
        for x in range(160):
            F[y][x] = F[y][319 - x] = frame_px(C, x, y)

    # ------------------------------------------------- the dashboard
    def box(x0, y0):
        rect(F, x0, y0, x0 + 48, y0 + 8, C['L'])
        for (cx, cy) in ((x0, y0), (x0 + 47, y0), (x0, y0 + 7), (x0 + 47, y0 + 7)):
            F[cy][cx] = BK
    for y0 in (178, 188):
        box(38, y0)
        box(234, y0)
    glyph(F, 38, "L", BK, 178)
    glyph(F, 62, "B", BK, 178)
    for y0 in (178, 188):
        glyph(F, 242, ":", BK, y0)
        glyph(F, 266, ".", BK, y0)

    # the gauge: ticks every 6, a number every third, the bar's track
    for k in range(21):
        x = 96 + 4 + 6 * k
        rect(F, x, 172, x + 2, 174, C['C'])
        if k % 3 == 0:
            rect(F, x, 170, x + 2, 171, C['b'])
            rect(F, x, 171, x + 2, 172, C['C'])
            rect(F, x, 176, x + 2, 178, C['C'])
            n = str(5 + k)
            gx = x + 1 - 3 * len(n) - (len(n) - 1)
            for ch in n:
                fat(F, gx, 179, ch, C['C'])
                gx += 8
    rect(F, 100, 185, 220, 186, C['b'])
    rect(F, 96, 186, 224, 187, C['C'])
    # the flag and the car, the two small plates under the gauge
    for (x0, chequer) in ((98, True), (208, False)):
        rect(F, x0, 189, x0 + 16, 198, BK)
        if chequer:
            rect(F, x0 + 3, 190, x0 + 4, 197, C['G'])
            for y in range(190, 194):
                for x in range(x0 + 4, x0 + 13):
                    if ((x - x0) // 2 + (y - 190) // 2) % 2 == 0:
                        F[y][x] = C['G']
        else:
            rect(F, x0 + 3, 192, x0 + 13, 195, C['G'])
            rect(F, x0 + 5, 191, x0 + 11, 192, C['G'])
            for wx in (x0 + 3, x0 + 11):
                rect(F, wx, 195, wx + 2, 197, C['M'])
    return F


def frame_px(C, x, y):
    """The left half of the frame; frame() mirrors it."""
    # the dashboard, from the cowl's edge down
    if x >= 32:
        o = y - cowl_top(x)
        if o >= 0:
            return dash_px(C, x, y, o)
    # the cage: d is how far into it, from the windscreen's edge
    if x >= 37 and y < 16:
        d = 16 - y
    elif x < 37 and y < 21:
        d = round(math.hypot(x + 0.5 - 37, y + 0.5 - 21) - 5)
    else:
        d = 32 - x
    if d <= 0:
        return None                             # the windscreen
    if x < 20 and y >= 84:
        s = strut_px(C, x, y)
        if s is not False:
            return s
    if d > 16:
        return C['.']
    if d <= 2:
        v = C['R']
        if 34 <= x < 37 and 10 <= y < 22 or x < 37 and 12 <= y < 15:
            v = C['W']                          # the glint on the corner
        return v
    if d <= 6:
        return C['P']
    if d <= 12:
        if y < 16 and 8 <= d <= 11 and x >= 37:
            k = x % 24                          # the bar's ribs
            if k in (14, 15) and d >= 9:
                return C['G']
            if k in (16, 17) and d <= 10:
                return C['P']
        return C['R']
    if y < 16 and x >= 37:
        k = x % 24
        if k in (22, 23):
            return C['.']
        if k in (14, 15) and d >= 14:
            return C['R']
    return C['G']


def strut_px(C, x, y):
    """The brace from the pillar out to the screen's edge, and the blue side
    panel under it. False: not part of it."""
    v = x + y
    if v < 98:
        return False if x >= 16 else C['.']
    xe = 22 - 0.6 * max(0, y - 100)             # its lower edge
    if x < xe:
        if v < 104:
            return C['G']
        if v < 114:
            return C['R']
        if v < 126:
            return C['P']
        return C['R']
    if x < xe + 2:
        return C['G']
    if x >= 16:
        return False
    if x >= 14 or y < 112:
        return C['.']
    return C['B'] if x >= 10 - (y - 170) * 0.4 else C['b']


def dash_px(C, x, y, o):
    if o < 2:
        return C['R']
    if o < 6:
        return C['P']
    if o < 11:
        return C['R']
    if o < 16:
        return C['G']
    if y >= 199:
        return C['C']
    if y >= 198:
        return C['b']
    xm = min(x, 319 - x)                        # the panel is symmetric
    if 170 <= y < 176 and xm >= 86 - (y - 170) * 7.6 and xm < 96:
        return C['B']                           # the wedge under the lip
    if y >= 176 and 90 <= xm < 96:
        return C['C'] if xm >= 94 and y >= 178 else C['B']
    if y >= 187 and xm >= 96:
        return C['B'] if abs(x - 159.5) > 30 + (y - 187) * 4 else C['.']
    return C['.']


# ------------------------------------------------------------- the car

# The car's front as the original draws it, from a screenshot of the C64
# game: screen rows 120-159, the left half (x 32-159), a letter per
# multicolour pixel (two screen pixels wide). It is mirror-symmetric about
# x 160, so car() mirrors it. The letters are palette()'s keys; ' ' is a
# hole. R and P on the last rows are the cowl's top, behind the dashboard.
#
# The left: the front tyre (TYRE_W pixels, cut out by tyre_mask() and
# steered as a sprite of its own), the four exhaust stacks' black mouths,
# the suspension arms; then the green-lit header pipes, the intake drum
# and the grille out to the breather pipe on the centre line.
ENGINE = [
    '',
    '',
    '',
    'W.W                                                 WWWWWWW',
    '.W.W                                               W.......W',
    '..W.W                       WWWW                   WWWWWWWWW',
    'W..W..                     W...b                   W......WW',
    '.W.W.W                     W....W                 W........W',
    '.W.....                WCCC.....W                 WWWWWWWWWW',
    '..W.W.W               W...CW....W                WW......WWC   .',
    '..W.M.M.             W.....W....W               W..........W   .',
    'W...M.MW             .......W...W               WW........WW   .',
    'W...M.M..       WbbWWW......W..WW               WWWWWWWWWWWCWWW.',
    'WW.MMWM..      W.....Wb.....WCWCW             bWWWWWCCCCCW. bbb.',
    'WM.MWWW..      .......b.....W.CCW           bbWWW........WW.....',
    '.M.WWWWW.     W........b....W.CCW          WbWW...........W WWW.',
    '.MWWWWWW..    .........W...WC.CCW       MMWVMWWW.........WW.....',
    '.MWWWWMMCCWCCC.........W..CWC.CCW       MWWVWVWWWWWWWWWWWWC.....',
    '.MWWWMMW......W........WCW.WC.C.W       .WVVWVW.CCWWWWCC..CMMM.C',
    'MWWWMMM........W.......W.C.CC...W       WWWWVV...CCCCCC...MMWW.C',
    'MWWMMMW........W.......W.C.CCC..b      bWV.WVVW..........C.....C',
    'WWWMM...........C.....WC.C.CWC..bb     WWVWVVWM.........CCMMMM.C',
    'WWWM.C..........W.....WC.C.CWC...b    WWVVWVVMMM..C....CC......C',
    'WWMM.C..........W....CCC..CCWC..WWb  bWWWWWVVMMM.CWCC...W......C',
    'WWM..C..........WCWCC.CC..CCWC.CCCCCW.WV.WVVWWVMMCWCC...WMMMMM.C',
    'WWM..C..........W.C.C.CWC.C.WC..C....VWMWWVVW.MM.CWCC...WMMWWW.C',
    'WM...C..........W.C.C.CWC.C.CWC...CCWWVMWVVWV..V.CWCC...W......C',
    'WM...C.........WC.C.C.CWC....WCC...WVWMWWVVWMM.M.CWCC.C.WMMMMM.C',
    'WM...CC........WC..CC.CWC..CCCWWWCWCWWMWVVWVMM...CWCC.C.WMMMMM.C',
    'WM...C........WWCC.CC.CWC..CWC..CCCVWWWWVVWWMMM..CWCC.C.W......C',
    'WM...C.W.....W.CWC.CC.CWC..CWWC..CWWV.WWVVM.VMM..CWCC.C.W......C',
    'M....C.CWWCCWC.CWC.C.C.CWC..CCCCWWVWMWWVVWW.MMM..CWCC.C.WMMWWW.C',
    'M....C.CWC..C..CW...CC.CW.RRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR',
    'M......CWC..C.CCW...C.RRRRRPRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR',
    'M.....CCWC..C.CCW.RRRRRRRRRPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP',
    '......C.WC..CCCRRRRRRRRPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP',
    '......C.WWCRRRRRRRRRPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP',
    '......CRRRRRRRRRPPPPPPPPPPPPRPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPPP',
    '...RRRRRRRRRPPPPPPPPPPPPPPPPRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR',
    'RRRRRRRRPPPPPPPPPPPPPPPPPRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRRR',
]

# The boost flames out of the right-hand stacks, from the same screenshot,
# rows 120-151; flames() mirrors them to the left.
FLAMES = [
    '',
    '',
    '',
    '',
    '                                     Y',
    '                                  Y  Y',
    '                                  YYY   Y',
    '                                  YWY   Y  Y',
    '                                 YYWY  YYYYY',
    '                                 YWWY  YYYY',
    '                                 YWY   WYWY',
    '                                WYWY  YWWW   YYY',
    '                                  WY  YWWY   YYYY',
    '                                      YWWY   YYYY',
    '                                     YWWWY  YYYWY',
    '                                    WYWWY   YWWWY',
    '                                      WWY   YYWYY    Y',
    '                                       WY  YYWWY     Y',
    '                                           YWWWY    YY',
    '                                          YYWWWY    YY',
    '                                         WYWWWWY    YY',
    '                                           WWWYY    YY',
    '                                          WWWWYY   YYW',
    '                                            WWYY  YYWW',
    '                                                  YYWW',
    '                                                  YYWW',
    '                                                W YYWW',
    '                                                  YYWW',
    '                                                 WYYWW',
    '                                                   YWW',
    '                                                    YY',
    '',
]

TYRE_X = (24, 264)              # the tyres' sprites' screen x0, left and right
TYRE_W = 16                     # texels: the tyre's sprite is 16x64 drawn 32x64


def engine_px(C, tx, y):
    """ENGINE at texel tx (0-127 across the car), row y; None: a hole."""
    if not 0 <= y < len(ENGINE):
        return None
    if tx >= 64:
        tx = 127 - tx
    row = ENGINE[y]
    ch = row[tx] if tx < len(row) else ' '
    return None if ch == ' ' else C[ch]


def tyre_mask(y):
    """How many texels of ENGINE row y, from the left, are the tyre."""
    if not 0 <= y < len(ENGINE):
        return 0
    n = 0
    for ch in ENGINE[y][:8]:
        if ch not in '.WM':
            break
        n += 1
    return n


def car(C):
    """128x64 texels drawn 256x64 at screen (32, 120): the engine, the
    exhausts and the suspension, without the tyres."""
    c = canvas(128, 64)
    for y in range(64):
        for tx in range(128):
            if min(tx, 127 - tx) >= tyre_mask(y):
                c[y][tx] = engine_px(C, tx, y)
    return c


def flames(C, f):
    """128x32 texels drawn 256x32 at screen (32, 120). Frame 1 is frame 0
    a row higher with every flame's tip licked back: the flicker."""
    c = canvas(128, 32)
    for y in range(32):
        sy = y + f
        row = FLAMES[sy] if sy < len(FLAMES) else ''
        for k, ch in enumerate(row):
            if ch == ' ':
                continue
            if f and ch == 'Y' and (k + y) % 3 == 0:
                continue
            c[y][64 + k] = c[y][63 - k] = C[ch]
    return c


def tyre(C, side, s, t):
    """A front tyre, TYRE_W x 64 texels drawn 32x64 at TYRE_X: ENGINE's
    tyre, slid s texels (-2..2, right positive) as it steers, its tread
    (the black and white chequer over its top) turned t (0-3) rows."""
    c = canvas(TYRE_W, 64)
    for y in range(64):
        n = tyre_mask(y)
        for k in range(n):
            v = engine_px(C, k, y)
            if v in (C['W'], C['.']) and y < 12:
                v = C['W'] if (k - y + t) % 2 else C['.']
            x = k + s + 4                       # 4: room to swing left
            if side:
                x = TYRE_W - 1 - (k - s + 4)
            put(c, x, y, v)
    return c


def wheel(C, s):
    """The steering wheel's rim, 128x16 at screen (96, 184): the top of a
    circle centred below the screen, its marker turned s * 20 degrees."""
    c = canvas(128, 16)
    cx, cy, a, b = 64, 32, 52, 27
    for y in range(16):
        for x in range(128):
            e = ((x + 0.5 - cx) / a) ** 2 + ((y + 0.5 - cy) / b) ** 2
            if 0.74 <= e <= 1.0:
                v = C['L']
                if e < 0.79:
                    v = C['.']
                elif e < 0.84:
                    v = C['M']
                if e > 0.95 and x < cx:
                    v = C['W']
                c[y][x] = v
    th = math.radians(s * 20)
    for k in range(-4, 5):                      # the marker, at the top
        u = th + k * 0.012
        mx = cx + a * 0.9 * math.sin(u)
        my = cy - b * 0.9 * math.cos(u)
        for dy in range(-2, 2):
            put(c, round(mx), round(my) + dy, C['W'] if abs(k) < 3 else C['.'])
    # the column, under the rim
    rect(c, 58, 13, 70, 16, C['L'])
    rect(c, 58, 13, 70, 14, C['W'])
    return c


# ------------------------------------------------------------- the fonts

# 5x6, one-pixel strokes, in an 8x8 cell from (1, 1)
FONT = {
    '0': [".###.", "#...#", "#...#", "#...#", "#...#", ".###."],
    '1': ["..#..", ".##..", "..#..", "..#..", "..#..", ".###."],
    '2': [".###.", "#...#", "...#.", "..#..", ".#...", "#####"],
    '3': ["####.", "....#", "..##.", "....#", "....#", "####."],
    '4': ["...#.", "..##.", ".#.#.", "#####", "...#.", "...#."],
    '5': ["#####", "#....", "####.", "....#", "....#", "####."],
    '6': [".###.", "#....", "####.", "#...#", "#...#", ".###."],
    '7': ["#####", "....#", "...#.", "..#..", ".#...", ".#..."],
    '8': [".###.", "#...#", ".###.", "#...#", "#...#", ".###."],
    '9': [".###.", "#...#", "#...#", ".####", "....#", ".###."],
    '-': [".....", ".....", "#####", ".....", ".....", "....."],
    '+': [".....", "..#..", "#####", "..#..", ".....", "....."],
    'L': ["#....", "#....", "#....", "#....", "#....", "#####"],
    'B': ["####.", "#...#", "####.", "#...#", "#...#", "####."],
    ':': [".....", "..#..", ".....", ".....", "..#..", "....."],
    '.': [".....", ".....", ".....", ".....", ".....", "..#.."],
}

# the gauge's numbers: 3x5, each pixel two wide
FAT = {
    '0': ["###", "#.#", "#.#", "#.#", "###"], '1': [".#.", ".#.", ".#.", ".#.", ".#."],
    '2': ["###", "..#", "###", "#..", "###"], '3': ["###", "..#", ".##", "..#", "###"],
    '4': ["#.#", "#.#", "###", "..#", "..#"], '5': ["###", "#..", "###", "..#", "###"],
    '6': ["###", "#..", "###", "#.#", "###"], '7': ["###", "..#", "..#", "..#", "..#"],
    '8': ["###", "#.#", "###", "#.#", "###"], '9': ["###", "#.#", "###", "..#", "###"],
}


def glyph(c, x0, ch, v, y0=0):
    for gy, row in enumerate(FONT[ch]):
        for gx, b in enumerate(row):
            if b == '#':
                put(c, x0 + 1 + gx, y0 + 1 + gy, v)


def fat(c, x0, y0, ch, v):
    for gy, row in enumerate(FAT[ch]):
        for gx, b in enumerate(row):
            if b == '#':
                put(c, x0 + 2 * gx, y0 + gy, v)
                put(c, x0 + 2 * gx + 1, y0 + gy, v)


# --------------------------------------------------------------- the sprites

# the readout boxes' cells: left boxes from x 38, right from 234; the top
# row at y 178, the bottom at 188
def sprites():
    """In WORLD_TICK actor order: (name, tex, screen x0, y0, w, h, m)."""
    return [
        ("PILL",   TX_PIL_L, 0, 0, 32, 256, 2),
        ("PILR",   TX_PIL_R, 288, 0, 32, 256, 2),
        ("TOP",    TX_TOP,   32, 0, 256, 32, 2),
        ("DASH",   TX_DASH,  32, 136, 256, 64, 2),
        ("CAR",    TX_CAR,   32, 120, 256, 64, 3),
        ("TYREL",  TX_TYRE + 8, TYRE_X[0], 120, 32, 64, 2.75),
        ("TYRER",  TX_TYRE + 28, TYRE_X[1], 120, 32, 64, 2.75),
        ("FLAME",  TX_FLAME, 32, 120, 256, 32, 2.5),
        ("CHAIN",  TX_CHAIN, 32, 16, 256, 128, 4),
        ("CRACK",  TX_CRACK + 1, 32, 0, 256, 16, 1),
        ("WHEEL",  TX_WHEEL + 2, 96, 184, 128, 16, 1.5),
        ("SPEED",  TX_BAR,   96, 168, 128, 8, 1),
        ("LAP",    TX_DIG,   46, 178, 8, 8, 1),
        ("BOOST",  TX_PAIR,  70, 178, 16, 8, 1),
        ("GAPS",   TX_SGN,   46, 188, 16, 8, 1),
        ("GAPP",   TX_PAIR,  62, 188, 16, 8, 1),
        ("GAPD",   TX_DIG,   78, 188, 8, 8, 1),
        ("TIMEM",  TX_DIG,   234, 178, 8, 8, 1),
        ("TIMES",  TX_PAIR,  250, 178, 16, 8, 1),
        ("TIMET",  TX_DIG,   274, 178, 8, 8, 1),
        ("BESTM",  TX_DIG,   234, 188, 8, 8, 1),
        ("BESTS",  TX_PAIR,  250, 188, 16, 8, 1),
        ("BESTT",  TX_DIG,   274, 188, 8, 8, 1),
        ("SPARE",  TX_DIG,   0, 0, 8, 8, 1),    # CK_N must be even
    ]


def pose(x0, y0, w, h, m):
    """-> (x, y, z) 16.16 view-space feet, and (w, h) 8.8 for SET_SPRITE."""
    assert w * m == int(w * m) and h * m == int(h * m)
    z = m * FOCAL / 256
    upp = z / FOCAL                             # units per screen pixel
    xc = x0 + w / 2
    yb = y0 + h
    return ((fx((xc - 160) * upp), fx((100 - yb) * upp), fx(z)), (int(w * m), int(h * m)))


def fx(v):
    return int(round(v * 65536)) & 0xffffffff

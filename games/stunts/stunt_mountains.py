# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""The mountains on the horizon: a 360-degree panorama, rendered.

A fractal mountain range (ridged multifractal noise) stands 3 to 30 km
round the track. It is ray-cast column by column as a heightfield, voxel
space style, from 300 m up: lit by a low sun, snow-capped above a ragged
snowline where the slope can hold it, forested in the valleys, and hazed
blue with distance. The colours are dithered into the game's palette, so it
is made of the same 256 colours as everything else. Above the ridge every
texel is the sky's own colour: the panels are drawn unlit, and the
mountains stand straight out of the flat sky fill.

panorama(pal, sky) -> W x H palette indices, rows top first, column 0 due
north (+z) and turning clockwise seen from above (towards +x). panels()
cuts it into PANELS textures of PW x H.

The ring it is drawn on: radius RING, from Y0 at the bottom to Y1 at the
top, seen from EYE units up.
"""
import math
import numpy as np

PANELS, PW, H = 16, 64, 64
W = PANELS * PW
RING, Y0, Y1, EYE = 76.0, -1.0, 19.0, 2.0

# the world the panorama looks at, in metres
CAM_H = 300.0
D_NEAR, D_FAR = 2500.0, 32000.0
SUN = np.array([-0.55, 0.42, 0.72])          # low, from the south-west
SUN = SUN / np.linalg.norm(SUN)

# the colours it may be dithered into: (hue, lowest level); hue 0 olive,
# 1 blue-grey, 2 sky, 6 khaki, 7 white (levels 1-11 are the cockpit's)
ALLOWED = [(0, 2), (1, 2), (2, 8), (6, 4), (7, 12)]
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]) / 16.0 - 0.47


def _hash(ix, iy, seed):
    h = (ix.astype(np.uint32) * np.uint32(374761393) + iy.astype(np.uint32) * np.uint32(668265263)
         + np.uint32(seed * 2246822519 & 0xffffffff))
    h = (h ^ (h >> np.uint32(13))) * np.uint32(1274126177)
    h = h ^ (h >> np.uint32(16))
    return (h & np.uint32(0xffffff)).astype(np.float64) / float(0xffffff)


def vnoise(x, z, seed):
    ix, iz = np.floor(x), np.floor(z)
    fx, fz = x - ix, z - iz
    fx = fx * fx * fx * (fx * (fx * 6 - 15) + 10)
    fz = fz * fz * fz * (fz * (fz * 6 - 15) + 10)
    ix, iz = ix.astype(np.int64), iz.astype(np.int64)
    a = _hash(ix, iz, seed)
    b = _hash(ix + 1, iz, seed)
    c = _hash(ix, iz + 1, seed)
    d = _hash(ix + 1, iz + 1, seed)
    return (a * (1 - fx) + b * fx) * (1 - fz) + (c * (1 - fx) + d * fx) * fz


def terrain(x, z):
    """Height in metres: ridged multifractal, rising out of the plain."""
    f = 1.0 / 5200.0
    s = np.zeros_like(x)
    w = np.ones_like(x)
    amp = 1.0
    for o in range(9):
        n = vnoise(x * f, z * f, 17 + o)
        n = 1.0 - np.abs(2 * n - 1)
        n = n * n
        s += n * amp * w
        w = np.clip(n * 1.6, 0, 1)
        f *= 2.03
        amp *= 0.48
    r = np.hypot(x, z)
    mask = np.clip((r - 2600.0) / 6000.0, 0, 1)
    mask = mask * mask * (3 - 2 * mask)
    big = 0.6 + 0.6 * vnoise(x / 14000.0, z / 14000.0, 5)   # some ranges higher
    return 60.0 + s * 1400.0 * mask * big


def _rows_elev():
    y = Y1 - (np.arange(H) + 0.5) * (Y1 - Y0) / H
    return np.arctan2(y - EYE, RING)                  # elevation of each row


def panorama(pal, sky):
    palc = np.array(pal, dtype=np.float64).reshape(256, 3)
    skyrgb = palc[sky]
    haze = skyrgb * 0.6 + np.array([235, 235, 235]) * 0.4

    th = 2 * math.pi * (np.arange(W) + 0.5) / W
    dx, dz = np.sin(th), np.cos(th)
    elev = _rows_elev()
    img = np.zeros((H, W, 3))
    done = np.zeros((H, W), dtype=bool)
    ybuf = np.full(W, H)                              # rows filled from here down

    t = D_NEAR
    e = 25.0
    while t < D_FAR:
        x, z = dx * t, dz * t
        h = terrain(x, z)
        hx = terrain(x + e, z) - h
        hz = terrain(x, z + e) - h
        n = np.stack([-hx, np.full_like(h, e), -hz])
        n /= np.linalg.norm(n, axis=0)
        lam = np.clip(n[0] * SUN[0] + n[1] * SUN[1] + n[2] * SUN[2], 0, 1)
        up = n[1]
        # the ground's own colour
        rock = np.array([128, 118, 108])
        rock2 = np.array([92, 88, 86])
        rk = vnoise(x / 180.0, z / 180.0, 99)[:, None]
        c = rock * (1 - rk) + rock2 * rk
        forest = np.array([44, 72, 22])
        meadow = np.array([118, 128, 52])
        veg = np.clip((1500.0 - h) / 500.0, 0, 1)[:, None] * np.clip((up - 0.55) / 0.2, 0, 1)[:, None]
        lowv = np.clip((500.0 - h) / 300.0, 0, 1)[:, None]
        green = forest * (1 - lowv) + meadow * lowv
        c = c * (1 - veg) + green * veg
        line = 1650.0 + 450.0 * (vnoise(x / 900.0, z / 900.0, 7) - 0.5)
        sn = np.clip((h - line) / 150.0, 0, 1) * np.clip((up - 0.5) / 0.18, 0, 1)
        c = c * (1 - sn[:, None]) + np.array([240, 242, 246]) * sn[:, None]
        light = 0.34 + 1.05 * lam
        shade_blue = np.array([0.9, 0.95, 1.1])      # the shadow side is lit by the sky
        c = c * light[:, None] * (1 - (1 - lam[:, None]) * 0.25 + (1 - lam[:, None]) * 0.25 * shade_blue)
        # air: blue haze with distance
        fog = 1 - np.exp(-t / 24000.0)
        fog = 0.02 + 0.9 * fog
        c = c * (1 - fog) + haze * fog
        # project and fill below the ridge seen so far
        ang = np.arctan2(h - CAM_H, t)
        rowtop = np.searchsorted(-elev, -ang)         # first row at or below ang
        for col in np.nonzero(rowtop < ybuf)[0]:
            r0, r1 = rowtop[col], ybuf[col]
            img[r0:r1, col] = c[col]
            done[r0:r1, col] = True
            ybuf[col] = r0
        t *= 1.012
        e = max(25.0, t / 400.0)

    # dither into the palette
    cands = np.array([hh * 32 + l for hh, lo in ALLOWED for l in range(lo, 32)])
    cp = palc[cands]
    d = BAYER[np.arange(H)[:, None] & 3, np.arange(W)[None, :] & 3] * 16.0
    c = img + d[:, :, None]
    flat = _ycc(c.reshape(-1, 3))
    cp = _ycc(cp)
    wts = np.array([1.0, 3.0, 3.0])                   # hue matters more than shade
    out = np.full(H * W, sky, dtype=np.uint8)
    idx = np.nonzero(done.reshape(-1))[0]
    for k in range(0, len(idx), 8192):
        blk = idx[k:k + 8192]
        dd = (((flat[blk, None, :] - cp[None, :, :]) ** 2) * wts).sum(axis=2)
        out[blk] = cands[np.argmin(dd, axis=1)]
    return bytes(out)


def _ycc(rgb):
    r, g, b = rgb[..., 0], rgb[..., 1], rgb[..., 2]
    y = 0.299 * r + 0.587 * g + 0.114 * b
    return np.stack([y, 0.564 * (b - y), 0.713 * (r - y)], axis=-1)


def panels(img):
    """The panorama cut into PANELS textures of PW x H, rows top first."""
    return [bytes(img[r * W + p * PW + u] for r in range(H) for u in range(PW))
            for p in range(PANELS)]

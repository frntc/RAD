#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
gpu64 rastercheck -- differential test of the class 2 raster core.

Renders the same randomised scenarios twice: once through the firmware's
Source/Firmware/gpu64_raster_core.cpp (compiled natively by the Makefile
here) and once through the reference model in tools/prgsim/gpu64model.py,
which was written from docs/class2-raster-reference.md rather than from the firmware.
Then it compares all 64000 pixels and the accepted/rejected/pixels counters.

A disagreement is a real finding either way round: either the firmware does
not do what the document says, or the document does not say what the
firmware does. Finding it here costs a second; finding the same thing at the
bench costs a card swap and a test round -- see CLAUDE.md.

The scenarios deliberately spend most of their randomness on the edges the
conformance PRG can only sample a few of: columns that start above the view
and so need v advanced across the clipped rows, negative dv, u past the
texture width, spans with a non-power-of-two texture (which must be
rejected), sprites clipped by clipY0/clipY1, masked draws, and lighting.

  python3 check.py            # 400 scenarios
  python3 check.py -n 5000    # more
  python3 check.py --seed 7   # reproduce one
  python3 check.py --dump 3   # write out/scn3-{core,model}.ppm on a failure
"""
import argparse
import math
import os
import random
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "prgsim"))

from gpu64model import (Gpu64Model, FB_W, FB_H, C64_PALETTE,  # noqa: E402
                        BATCH_CAM3D)

BIN = os.path.join(HERE, "rastercheck")
MAGIC = 0x4B433252
REC = 16
THING_DIRECTIONAL = 0x10
WALL2 = 32
POLY = 16


# ----------------------------------------------------------------------
# Scenario generation
# ----------------------------------------------------------------------

def make_texture(rnd, allow_npot_w):
    h = 1 << rnd.randint(1, 6)
    if allow_npot_w and rnd.random() < 0.35:
        w = rnd.choice([3, 5, 6, 7, 12, 20])
    else:
        w = 1 << rnd.randint(1, 6)
    # A pattern with no repeats inside a row or column, so a wrap that is
    # off by one texel shows up as a different byte rather than the same one.
    texels = bytearray(w * h)
    for u in range(w):
        for v in range(h):
            texels[u * h + v] = (u * 37 + v * 11 + 3) & 0xFF
    return w, h, bytes(texels)


def make_camera(rnd, walls):
    """A camera. Always generated, so every scenario carries one.

    For a wall scenario it is deliberately mostly legal -- an illegal camera
    rejects the whole batch and then the scenario tests one branch instead of
    the projection. The illegal ones are still generated, about one in ten,
    because that branch has to agree too.
    """
    if walls and rnd.random() < 0.1:
        return dict(x=0, y=0, ang=0, flags=0,
                    eye=rnd.choice([0, -256, 0x0400]),
                    ceil=rnd.choice([0x0200, 0]),
                    proj=rnd.choice([0, 0xA000]),
                    floorc=0, ceilc=0, horizon=0)
    return dict(
        x=rnd.randint(-0x2000, 0x2000),
        y=rnd.randint(-0x2000, 0x2000),
        ang=rnd.randint(0, 255),
        flags=rnd.randint(0, 1),
        eye=rnd.randint(0x20, 0x180),
        ceil=rnd.randint(0x200, 0x600),
        proj=rnd.randint(0x2000, 0xE000),
        floorc=rnd.randint(0, 255),
        ceilc=rnd.randint(0, 255),
        horizon=rnd.randint(-40, 40),
    )


def make_sectors(rnd, sectors):
    """The sector table. Generated for every scenario, empty one time in ten
    for a DRAW_SECTORS one -- an empty table rejects the whole batch and that
    branch has to agree too."""
    if not sectors:
        return []
    if rnd.random() < 0.1:
        return []
    n = rnd.randint(1, 6)
    out = []
    for _ in range(n):
        f = rnd.randint(-0x300, 0x300)
        out.append(dict(floor=f, ceil=f + rnd.randint(0x40, 0x500),
                        floorc=rnd.randint(0, 255), ceilc=rnd.randint(0, 255),
                        light=rnd.randint(0, 70), flags=rnd.randint(0, 1)))
    return out


def make_camera3(rnd, polys):
    """The milestone 9 camera. Illegal (proj 0) about one time in ten for a
    polygon scenario, for the same reason the 2D one is: the branch that
    rejects a whole batch has to agree too."""
    if polys and rnd.random() < 0.1:
        return dict(x=0, y=0, z=0, yaw=0, pitch=0, proj=0, flags=0)
    return dict(
        x=rnd.randint(-0x2000, 0x2000),
        y=rnd.randint(-0x2000, 0x2000),
        z=rnd.randint(-0x200, 0x200),
        yaw=rnd.randint(0, 255),
        pitch=rnd.choice([0, 0, rnd.randint(-40, 40)]),
        proj=rnd.randint(0x2000, 0xE000),
        flags=0,
    )


def clamp16(v):
    return max(-32768, min(32767, v))


def make_polys(rnd, cam3, ids):
    """A vertex pool, a texinfo table and a batch of face records.

    Faces are generated as convex n-gons in a random plane rather than as
    random vertices, because a random triple is a degenerate sliver almost
    every time and would test the clipper instead of the rasteriser. The
    malformed ones are generated deliberately and counted: an index past the
    pool, a vertex count of 0/1/17, and a texinfo a textured face does not
    have.
    """
    verts = []
    faces = bytearray()
    nfaces = rnd.randint(0, 12)

    ntexinfo = rnd.randint(0, 4)
    texinfo = []
    for _ in range(ntexinfo):
        texinfo.append(tuple([rnd.randint(-0x400, 0x400) for _ in range(3)]
                             + [rnd.randint(-2000, 2000)]
                             + [rnd.randint(-0x400, 0x400) for _ in range(3)]
                             + [rnd.randint(-2000, 2000)]))

    for _ in range(nfaces):
        n = rnd.randint(3, 6)
        ox = cam3["x"] + rnd.randint(-0x500, 0x500)
        oy = cam3["y"] + rnd.randint(-0x500, 0x500)
        oz = cam3["z"] + rnd.randint(-0x300, 0x300)
        e1 = [rnd.randint(-0x300, 0x300) for _ in range(3)]
        e2 = [rnd.randint(-0x300, 0x300) for _ in range(3)]
        first = len(verts)
        pts = []
        for k in range(n):
            ang = 2 * math.pi * k / n
            ca, sa = math.cos(ang), math.sin(ang)
            pts.append((clamp16(int(ox + ca * e1[0] + sa * e2[0])),
                        clamp16(int(oy + ca * e1[1] + sa * e2[1])),
                        clamp16(int(oz + ca * e1[2] + sa * e2[2]))))
        if rnd.random() < 0.5:
            pts.reverse()           # the other winding: a backface
        verts += pts

        nrec = n
        firstrec = first
        if rnd.random() < 0.15:
            # Malformed on purpose, one of the three ways a record can be.
            which = rnd.randint(0, 2)
            if which == 0:
                nrec = rnd.choice([0, 1, 2, 17, 255])
            elif which == 1:
                firstrec = rnd.randint(4000, 5000)
        texid = rnd.choice(ids)
        # Mostly a texinfo that exists -- a face rejected for naming one that
        # does not tests the guard, not the rasteriser.
        if ntexinfo and rnd.random() < 0.8:
            ti = rnd.randint(1, ntexinfo)
        else:
            ti = rnd.randint(0, max(ntexinfo, 1) + 1)
        faces += struct.pack("<HBBBBBBxxxxxxxx", firstrec & 0xFFFF, nrec,
                             ti, texid, rnd.randint(0, 255),
                             rnd.randint(0, 70), rnd.randint(0, 7))

    assert len(faces) == nfaces * POLY
    return verts, texinfo, nfaces, bytes(faces)


def make_things(rnd, cx, cy, cz, ids, n, dirbase=0):
    """A batch of thing records scattered around a camera at (cx, cy, cz).

    The spread is deliberately tight: things far off to the side clip away
    and test nothing, whereas things a few hundred units out straddle the
    near plane, the view edges and -- for the 3D camera -- the rows that
    pitch sweeps past."""
    recs = bytearray()
    for _ in range(n):
        texid = rnd.choice(ids)
        flags = rnd.randint(0, 15)
        facing = 0
        # Milestone 11: some records are eight-view sprites. dirbase is the
        # first of the eight ids and byte 13 is where the thing looks.
        if dirbase and rnd.random() < 0.4:
            texid = dirbase
            flags |= THING_DIRECTIONAL
            facing = rnd.randint(0, 255)
        recs += struct.pack("<hhhHHBBBBxx",
                            cx + rnd.randint(-0x600, 0x600),
                            cy + rnd.randint(-0x600, 0x600),
                            cz + rnd.randint(-0x400, 0x400),
                            rnd.choice([0, rnd.randint(1, 0x300)]),
                            rnd.choice([0, rnd.randint(1, 0x300)]),
                            texid,
                            rnd.randint(0, 70),
                            flags,
                            facing)
    assert len(recs) == n * REC
    return bytes(recs)


def make_lights(rnd, cam, cam3):
    """Milestone 12. Half the scenarios have no lights at all, because the
    unlit path is the one every shipped program takes and it has to stay
    byte-identical. When there are lights they are placed near one of the two
    cameras, with radii on the scale the geometry is generated at, so they
    actually reach the pixels being compared."""
    if rnd.random() < 0.5:
        return []
    out = []
    for slot in range(rnd.randint(1, 8)):
        c = rnd.choice([cam3, cam3, cam])
        cz = c.get("z", 0)
        out.append(dict(
            slot=rnd.choice([slot, rnd.randint(0, 7)]),
            x=clamp16(c["x"] + rnd.randint(-0x600, 0x600)),
            y=clamp16(c["y"] + rnd.randint(-0x600, 0x600)),
            z=clamp16(cz + rnd.randint(-0x300, 0x300)),
            # 0 radius and 0 strength are both "off": generated on purpose so
            # the disable path is exercised alongside the lit one.
            radius=rnd.choice([0, rnd.randint(0x100, 0x2000),
                               rnd.randint(0x100, 0x2000)]),
            strength=rnd.choice([0, rnd.randint(1, 63), rnd.randint(1, 63)]),
        ))
    return out


def make_scenario(rnd):
    scn = _make_scenario(rnd)
    scn["lights"] = make_lights(rnd, scn["cam"], scn["cam3"])
    if any(l["radius"] and l["strength"] for l in scn["lights"]):
        # A light can only move a pixel if there is a colormap to move it in,
        # and only if the base level is not already pinned at the darkest row.
        # Without this most lit scenarios rendered identically to unlit ones
        # and the comparison proved nothing: 2 of 47 differed before, 9 after.
        if scn["levels"] < 32 or rnd.random() < 0.5:
            scn["levels"] = rnd.choice([32, 64, 64])
            scn["cmap"] = bytes((i * 7 + (i >> 8) * 13) & 0xFF
                                for i in range(scn["levels"] * 256))
    return scn


def _make_scenario(rnd):
    # 8 and 9 are milestone 10: things projected through SET_CAMERA3D, alone
    # and then behind a polygon level sharing the same depth buffer.
    kind = rnd.choice([0, 0, 1, 2, 3, 3, 4, 4, 5, 5, 6, 6, 6, 7, 8, 8, 9, 9])

    # The view: full surface half the time, otherwise a rectangle that
    # actually clips something.
    if rnd.random() < 0.5:
        view = (0, 0, FB_W, FB_H)
    else:
        vx = rnd.randint(0, 60)
        vy = rnd.randint(0, 60)
        vw = rnd.randint(1, FB_W - vx)
        vh = rnd.randint(1, FB_H - vy)
        view = (vx, vy, vw, vh)

    levels = 0
    cmap = b""
    if rnd.random() < 0.5:
        levels = rnd.choice([1, 2, 8, 32, 64])
        cmap = bytes((i * 7 + (i >> 8) * 13) & 0xFF for i in range(levels * 256))

    ntex = rnd.randint(1, 4)
    texs = {}
    for i in range(ntex):
        texid = rnd.randint(1, 8)
        texs[texid] = make_texture(rnd, allow_npot_w=(kind == 1))

    # Milestone 11: a run of eight consecutive ids, so a DIRECTIONAL thing
    # resolves a real view whichever octant the camera ends up in. A quarter
    # of the time the run is left unregistered, which exercises the other
    # half of the contract -- a view that misses simply draws nothing.
    dirbase = rnd.randint(1, 240)
    if rnd.random() < 0.75:
        for i in range(8):
            texs.setdefault(dirbase + i, make_texture(rnd, False))

    fill = rnd.randint(0, 255)
    key = rnd.randint(0, 255)
    cam = make_camera(rnd, kind in (3, 4, 5, 7))
    sectors = make_sectors(rnd, kind in (4, 5, 7))
    cam3 = make_camera3(rnd, kind in (6, 7, 8, 9))
    ids_for_polys = sorted(texs) + [0, rnd.randint(9, 255)]
    verts, texinfo, npolys, polyrecs = make_polys(rnd, cam3, ids_for_polys)

    if kind == 2:
        texid = rnd.choice(sorted(texs))
        tw, th, _ = texs[texid]
        spr = dict(
            texid=texid,
            x=rnd.randint(-40, FB_W),
            y=rnd.randint(-40, FB_H),
            w=rnd.randint(1, 120),
            h=rnd.randint(1, 120),
            light=rnd.randint(0, 70),
            key=rnd.choice([key, (3 * 37 + 11) & 0xFF]),
            cy0=rnd.randint(-10, FB_H),
            cy1=rnd.randint(-10, FB_H + 10),
            flags=rnd.randint(0, 3),
        )
        return dict(kind=2, view=view, levels=levels, cmap=cmap,
                    texs=texs, fill=fill, key=key, cam=cam,
                    sectors=sectors, spr=spr, cam3=cam3, verts=verts,
                    texinfo=texinfo, polys=npolys, polyrecs=polyrecs)

    if kind in (6, 9):
        # The polygon batch IS the scenario here; the shared generator above
        # already built it. kind 9 hangs a batch of things off the back of it.
        n3 = rnd.randint(0, 20) if kind == 9 else 0
        t3 = make_things(rnd, cam3["x"], cam3["y"], cam3["z"],
                         sorted(texs) + [0, rnd.randint(9, 255)], n3,
                         dirbase)
        return dict(kind=kind, view=view, levels=levels, cmap=cmap, texs=texs,
                    fill=fill, key=key, cam=cam, sectors=sectors, cam3=cam3,
                    verts=verts, texinfo=texinfo, count=npolys,
                    recs=polyrecs, polys=0, polyrecs=b"",
                    things=n3, thingrecs=t3)

    if kind == 8:
        # Things alone, through the 3D camera. No geometry in the depth
        # buffer, so what this exercises is the projection itself and the
        # thing-against-thing ordering.
        n3 = rnd.randint(0, 60)
        t3 = make_things(rnd, cam3["x"], cam3["y"], cam3["z"],
                         sorted(texs) + [0, rnd.randint(9, 255)], n3,
                         dirbase)
        return dict(kind=8, view=view, levels=levels, cmap=cmap, texs=texs,
                    fill=fill, key=key, cam=cam, sectors=sectors, cam3=cam3,
                    verts=verts, texinfo=texinfo, count=n3, recs=t3,
                    polys=0, polyrecs=b"", things=0, thingrecs=b"")

    count = rnd.randint(0, 60)
    recs = bytearray()
    ids = sorted(texs) + [0, rnd.randint(9, 255)]
    for _ in range(count):
        texid = rnd.choice(ids)
        light = rnd.randint(0, 70)
        if kind in (4, 5, 7):
            # Same near-plane bias as the wall scenarios, plus sector ids
            # that are out of range about one time in six and a back sector
            # that is $FF -- one-sided -- about half the time.
            def near2(c):
                return c + rnd.randint(-0x600, 0x600)
            nsec = max(len(sectors), 1)
            def secid():
                return rnd.randint(0, nsec + 1)
            back = 0xFF if rnd.random() < 0.5 else secid()
            recs += struct.pack("<hhhhhhBBBBBBBBBBxxxxxxxxxx",
                                near2(cam["x"]), near2(cam["y"]),
                                near2(cam["x"]), near2(cam["y"]),
                                rnd.randint(-2000, 2000),
                                rnd.randint(-2000, 2000),
                                secid(), back, light, rnd.randint(0, 7),
                                texid, rnd.choice(ids), rnd.choice(ids),
                                rnd.randint(0, 255), rnd.randint(0, 255),
                                rnd.randint(0, 255))
        elif kind == 3:
            # Endpoints near the camera as often as far from it, so the near
            # plane, the one-sided test and the clipped-end interpolation all
            # get exercised rather than just the comfortable middle.
            def near(c):
                return c + rnd.randint(-0x600, 0x600)
            recs += struct.pack("<hhhhBBhhBB",
                                near(cam["x"]), near(cam["y"]),
                                near(cam["x"]), near(cam["y"]),
                                texid, light,
                                rnd.randint(-2000, 2000),
                                rnd.randint(-2000, 2000),
                                rnd.randint(0, 3), 0)
        elif kind == 0:
            x = rnd.randint(0, FB_W + 8)
            y0 = rnd.randint(-40, FB_H)
            y1 = y0 + rnd.randint(-4, 120)
            u = rnd.randint(0, 400)
            v = rnd.randint(-8000, 8000)
            dv = rnd.randint(-1200, 1200)
            flags = rnd.randint(0, 1)
            recs += struct.pack("<HhhBBHhhBB", x & 0xFFFF, y0, y1,
                                texid, light, u, v, dv, flags, 0)
        else:
            y = rnd.randint(-8, FB_H + 8)
            x0 = rnd.randint(-40, FB_W)
            x1 = x0 + rnd.randint(-4, 200)
            u = rnd.randint(-8000, 8000)
            v = rnd.randint(-8000, 8000)
            du = rnd.randint(-900, 900)
            dv = rnd.randint(-900, 900)
            recs += struct.pack("<hhhBBhhhh", y, x0, x1, texid, light,
                                u, v, du, dv)
    assert len(recs) == count * (WALL2 if kind in (4, 5, 7) else REC)

    # kind 5: the same wall batch, then a batch of things to be occluded by
    # it. Positions are drawn near the camera so a fair share of them land in
    # front of, behind and straddling the geometry rather than off the side.
    things = b""
    nthings = 0
    if kind == 5:
        nthings = rnd.randint(0, 20)
        things = make_things(rnd, cam["x"], cam["y"], 0, ids, nthings,
                             dirbase)

    return dict(kind=kind, view=view, levels=levels, cmap=cmap, texs=texs,
                fill=fill, key=key, cam=cam, sectors=sectors,
                count=count, recs=bytes(recs),
                things=nthings, thingrecs=things,
                cam3=cam3, verts=verts, texinfo=texinfo,
                polys=(npolys if kind == 7 else 0),
                polyrecs=(polyrecs if kind == 7 else b""))


def encode(scn):
    b = bytearray()
    b += struct.pack("<I", MAGIC)
    b += struct.pack("<HHHH", *scn["view"])
    b += struct.pack("<H", scn["levels"])
    b += scn["cmap"]
    b += struct.pack("<H", len(scn["texs"]))
    for texid, (w, h, texels) in sorted(scn["texs"].items()):
        b += struct.pack("<BHH", texid, w, h) + texels
    b += bytes([scn["fill"], scn["kind"], scn["key"]])
    c = scn["cam"]
    b += struct.pack("<hhBBhhHBBh", c["x"], c["y"], c["ang"], c["flags"],
                     c["eye"], c["ceil"], c["proj"], c["floorc"],
                     c["ceilc"], c["horizon"])
    b += struct.pack("<H", len(scn["sectors"]))
    for sec in scn["sectors"]:
        b += struct.pack("<hhBBBB", sec["floor"], sec["ceil"], sec["floorc"],
                         sec["ceilc"], sec["light"], sec["flags"])
    c3 = scn["cam3"]
    b += struct.pack("<hhhBbHB", c3["x"], c3["y"], c3["z"], c3["yaw"],
                     c3["pitch"], c3["proj"], c3["flags"])
    b += struct.pack("<H", len(scn["verts"]))
    for v in scn["verts"]:
        b += struct.pack("<hhhh", v[0], v[1], v[2], 0)
    b += struct.pack("<H", len(scn["texinfo"]))
    for ti in scn["texinfo"]:
        b += struct.pack("<8h", *ti)

    b += struct.pack("<H", len(scn["lights"]))
    for l in scn["lights"]:
        b += struct.pack("<BiiiHB", l["slot"], l["x"], l["y"], l["z"],
                         l["radius"], l["strength"])

    if scn["kind"] == 2:
        s = scn["spr"]
        b += struct.pack("<BiiIIBBiiB", s["texid"], s["x"], s["y"],
                         s["w"], s["h"], s["light"], s["key"],
                         s["cy0"], s["cy1"], s["flags"])
    else:
        b += struct.pack("<I", scn["count"]) + scn["recs"]
        if scn["kind"] in (5, 9):
            b += struct.pack("<I", scn["things"]) + scn["thingrecs"]
        if scn["kind"] == 7:
            b += struct.pack("<I", scn["polys"]) + scn["polyrecs"]
    return bytes(b)


# ----------------------------------------------------------------------
# The two renderers
# ----------------------------------------------------------------------

def run_core(scn, tmp):
    with open(tmp, "wb") as f:
        f.write(encode(scn))
    out = subprocess.run([BIN, tmp], stdout=subprocess.PIPE, check=True).stdout
    acc, rej, pix = struct.unpack("<HHI", out[:8])
    return (acc, rej, pix), out[8:]


def run_model(scn):
    m = Gpu64Model(lambda a: 0, lambda a, v: None, reu_size=1024)
    m.rview = scn["view"]
    if scn["levels"]:
        m.rcolormap = bytearray(scn["cmap"])
        m.rlevels = scn["levels"]
    for texid, (w, h, texels) in scn["texs"].items():
        m.rtex[texid] = (w, h, bytearray(texels))
    m.pages[m.draw_page][:] = bytes([scn["fill"]]) * (FB_W * FB_H)
    m.rbatch = [0, 0, 0]
    m.rcam = dict(scn["cam"])
    m.rsectors = [dict(x) for x in scn["sectors"]]
    m.rcam3 = dict(scn["cam3"])
    m.rverts = list(scn["verts"])
    m.rtexinfo = list(scn["texinfo"])
    for l in scn["lights"]:
        m.r_set_light(l["slot"], l["x"], l["y"], l["z"],
                      l["radius"], l["strength"])

    if scn["kind"] == 2:
        s = scn["spr"]
        m.ident = [s["texid"], 0]
        m.arg = [0] * 16
        m.arg[0:2] = list(struct.pack("<h", s["x"]))
        m.arg[2:4] = list(struct.pack("<h", s["y"]))
        m.arg[4:6] = list(struct.pack("<H", s["w"]))
        m.arg[6:8] = list(struct.pack("<H", s["h"]))
        m.arg[8] = s["light"]
        m.arg[9] = s["key"]
        m.arg[10:12] = list(struct.pack("<h", s["cy0"]))
        m.arg[12:14] = list(struct.pack("<h", s["cy1"]))
        m.arg[14] = s["flags"]
        m.r_sprite()
    elif scn["kind"] == 0:
        m.r_columns(scn["recs"], scn["count"], scn["key"])
    elif scn["kind"] == 1:
        m.r_spans(scn["recs"], scn["count"])
    elif scn["kind"] == 3:
        m.r_walls(scn["recs"], scn["count"], scn["key"])
    elif scn["kind"] in (6, 9):
        m.r_polys(scn["recs"], scn["count"], scn["key"])
        if scn["kind"] == 9:
            m.r_things(scn["thingrecs"], scn["things"], scn["key"],
                       BATCH_CAM3D)
    elif scn["kind"] == 8:
        m.r_things(scn["recs"], scn["count"], scn["key"], BATCH_CAM3D)
    else:
        m.r_sectors(scn["recs"], scn["count"], scn["key"])
        if scn["kind"] == 5:
            m.r_things(scn["thingrecs"], scn["things"], scn["key"])
        if scn["kind"] == 7:
            m.r_polys(scn["polyrecs"], scn["polys"], scn["key"])
    return tuple(m.rbatch), bytes(m.pages[m.draw_page])


# ----------------------------------------------------------------------

def write_ppm(path, pixels):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (FB_W, FB_H))
        rgb = bytearray()
        for c in pixels:
            r, g, b = C64_PALETTE[c % len(C64_PALETTE)]
            rgb += bytes((r, g, b))
        f.write(rgb)


def describe(scn):
    name = {0: "DRAW_COLUMNS", 1: "DRAW_SPANS", 2: "DRAW_SPRITE",
            3: "DRAW_WALLS", 4: "DRAW_SECTORS",
            5: "SECTORS+THINGS", 6: "DRAW_POLYS",
            7: "SECTORS+POLYS", 8: "THINGS3D",
            9: "POLYS+THINGS3D"}[scn["kind"]]
    bits = ["view=%d,%d %dx%d" % scn["view"], "levels=%d" % scn["levels"]]
    if scn["kind"] == 2:
        s = scn["spr"]
        bits.append("tex=%d at %d,%d %dx%d clip=%d..%d flags=%d"
                    % (s["texid"], s["x"], s["y"], s["w"], s["h"],
                       s["cy0"], s["cy1"], s["flags"]))
    else:
        bits.append("count=%d key=$%02X" % (scn["count"], scn["key"]))
    if scn["kind"] in (5, 9):
        bits.append("things=%d" % scn["things"])
    if scn["kind"] in (6, 7, 8, 9):
        c3 = scn["cam3"]
        bits.append("cam3=%d,%d,%d yaw=%d pitch=%d proj=%d verts=%d ti=%d"
                    % (c3["x"], c3["y"], c3["z"], c3["yaw"], c3["pitch"],
                       c3["proj"], len(scn["verts"]), len(scn["texinfo"])))
        if scn["kind"] == 7:
            bits.append("polys=%d" % scn["polys"])
    if scn["lights"]:
        live = [l for l in scn["lights"] if l["radius"] and l["strength"]]
        bits.append("lights=%d/%d" % (len(live), len(scn["lights"])))
    if scn["kind"] in (3, 5):
        c = scn["cam"]
        bits.append("cam=%d,%d ang=%d eye=%d ceil=%d proj=%d hor=%d fl=%d"
                    % (c["x"], c["y"], c["ang"], c["eye"], c["ceil"],
                       c["proj"], c["horizon"], c["flags"]))
    return "%-12s  %s" % (name, "  ".join(bits))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--dump", action="store_true",
                    help="write out/*.ppm for the first failure")
    args = ap.parse_args()

    if not os.path.exists(BIN):
        print("build it first: make -C tools/rastercheck")
        return 2

    tmp = os.path.join(HERE, ".scenario.bin")
    rnd = random.Random(args.seed)
    fails = 0
    kinds = {k: 0 for k in range(10)}
    for i in range(args.n):
        scn = make_scenario(rnd)
        kinds[scn["kind"]] += 1
        cstat, cpix = run_core(scn, tmp)
        mstat, mpix = run_model(scn)

        why = None
        if cstat != mstat:
            why = ("counters differ: core acc/rej/pix=%d/%d/%d "
                   "model=%d/%d/%d" % (cstat + mstat))
        elif cpix != mpix:
            bad = [j for j in range(FB_W * FB_H) if cpix[j] != mpix[j]]
            j = bad[0]
            why = ("%d pixels differ, first at (%d,%d): core=$%02X model=$%02X"
                   % (len(bad), j % FB_W, j // FB_W, cpix[j], mpix[j]))

        if why:
            fails += 1
            print("FAIL  scenario %d (seed %d)" % (i, args.seed))
            print("      %s" % describe(scn))
            print("      %s" % why)
            if args.dump and fails == 1:
                write_ppm(os.path.join(HERE, "out", "core.ppm"), cpix)
                write_ppm(os.path.join(HERE, "out", "model.ppm"), mpix)
                with open(os.path.join(HERE, "out", "scenario.bin"), "wb") as f:
                    f.write(encode(scn))
                print("      wrote out/core.ppm, out/model.ppm, out/scenario.bin")
            if fails >= 5:
                print("... stopping after 5")
                break

    if os.path.exists(tmp):
        os.unlink(tmp)

    print("%d scenarios (%d columns, %d spans, %d sprites, %d walls, "
          "%d sectors, %d things, %d polys, %d sectors+polys, "
          "%d things3d, %d polys+things3d), %d disagreements"
          % (args.n, kinds[0], kinds[1], kinds[2], kinds[3], kinds[4],
             kinds[5], kinds[6], kinds[7], kinds[8], kinds[9], fails))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())

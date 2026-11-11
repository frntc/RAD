#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
Builds the class-1 mesh blobs for gpu64_demo_quake3d from the same room
gpu64_demo_quake.a draws with class 2.

The two layers do not share a coordinate system. Class 2 is a map renderer:
x east, y north, z up. Class 1 is left-handed with x right, y UP and z away
from the camera. So a class-2 point (x,y,z) is the class-1 point (x, z, y) --
a swap of two axes. Class 1 draws a face when it is clockwise on screen, so
what matters is not the swap's handedness in the abstract but where the
camera stands: FACES below is wound to be clockwise as seen from INSIDE the
room, which is why the room is solid from the inside and see-through from
the outside. The vertex order is passed through unchanged.

Texcoords are per-corner bytes here, not the class-2 texinfo axes, so the
axes are evaluated at the corners instead: eight texels to the world unit,
the same density texinfoTab uses. A byte wraps, so a face whose raw
coordinate goes negative is biased by a multiple of the 32-texel texture --
which shifts it by a whole number of tiles and so cannot change how it lines
up with its neighbours.

Writes games/quake/gpu64_demo_quake3d.inc. Regenerate with:
    python3 tools/gen_quakemesh.py
"""

WU = lambda n: int(round(n * 256))

Z_TOP   = 3 * 256
Z_RAMP  = 384
Z_SLABB = 256
Z_SLABT = 320

TEX_BRICK = 1
TEX_STONE = 2

# texture axis kinds, matching texinfoTab: eight texels to the world unit,
# t running with -z so the texture is the right way up.
WALLX, WALLY, FLAT = 'wallx', 'wally', 'flat'

TEXELS_PER_UNIT = 8.0
TEXW = 32

def uv(kind, p):
    x, y, z = (c / 256.0 for c in p)
    if kind == WALLX: return ( x * TEXELS_PER_UNIT, -z * TEXELS_PER_UNIT )
    if kind == WALLY: return ( y * TEXELS_PER_UNIT, -z * TEXELS_PER_UNIT )
    return ( x * TEXELS_PER_UNIT, y * TEXELS_PER_UNIT )

# --- the room, in class-2 map coordinates, face by face -----------------
# Each entry: name, kind, texture, list of (x,y,z) in the demo's own winding.
FACES = [
 ("floor",  FLAT,  TEX_STONE, [(WU(0),WU(-5),0), (WU(0),WU(5),0), (WU(10),WU(5),0), (WU(10),WU(-5),0)]),
 ("ceil",   FLAT,  TEX_STONE, [(WU(0),WU(-5),Z_TOP), (WU(10),WU(-5),Z_TOP), (WU(10),WU(5),Z_TOP), (WU(0),WU(5),Z_TOP)]),
 ("walls",  WALLX, TEX_BRICK, [(WU(10),WU(-5),Z_TOP), (WU(0),WU(-5),Z_TOP), (WU(0),WU(-5),0), (WU(10),WU(-5),0)]),
 ("wallw",  WALLY, TEX_BRICK, [(WU(0),WU(-5),Z_TOP), (WU(0),WU(5),Z_TOP), (WU(0),WU(5),0), (WU(0),WU(-5),0)]),
 ("walln",  WALLX, TEX_BRICK, [(WU(0),WU(5),Z_TOP), (WU(10),WU(5),Z_TOP), (WU(10),WU(5),0), (WU(0),WU(5),0)]),
 ("walle",  WALLY, TEX_BRICK, [(WU(10),WU(5),Z_TOP), (WU(10),WU(-5),Z_TOP), (WU(10),WU(-5),0), (WU(10),WU(5),0)]),
 ("ramp",   FLAT,  TEX_BRICK, [(WU(6),WU(-2),0), (WU(6),WU(2),0), (WU(9),WU(2),Z_RAMP), (WU(9),WU(-2),Z_RAMP)]),
 ("plat",   FLAT,  TEX_BRICK, [(WU(9),WU(-2),Z_RAMP), (WU(9),WU(2),Z_RAMP), (WU(10),WU(2),Z_RAMP), (WU(10),WU(-2),Z_RAMP)]),
 ("ramps",  WALLX, TEX_STONE, [(WU(9),WU(-2),Z_RAMP), (WU(6),WU(-2),0), (WU(9),WU(-2),0)]),
 ("plats",  WALLX, TEX_STONE, [(WU(10),WU(-2),Z_RAMP), (WU(9),WU(-2),Z_RAMP), (WU(9),WU(-2),0), (WU(10),WU(-2),0)]),
 ("rampn",  WALLX, TEX_STONE, [(WU(6),WU(2),0), (WU(9),WU(2),Z_RAMP), (WU(9),WU(2),0)]),
 ("platn",  WALLX, TEX_STONE, [(WU(9),WU(2),Z_RAMP), (WU(10),WU(2),Z_RAMP), (WU(10),WU(2),0), (WU(9),WU(2),0)]),
 ("slabb",  FLAT,  TEX_BRICK, [(WU(2),WU(-5),Z_SLABB), (WU(4),WU(-5),Z_SLABB), (WU(4),WU(5),Z_SLABB), (WU(2),WU(5),Z_SLABB)]),
 ("slabt",  FLAT,  TEX_STONE, [(WU(2),WU(-5),Z_SLABT), (WU(2),WU(5),Z_SLABT), (WU(4),WU(5),Z_SLABT), (WU(4),WU(-5),Z_SLABT)]),
 ("slabw",  WALLY, TEX_BRICK, [(WU(2),WU(-5),Z_SLABT), (WU(2),WU(5),Z_SLABT), (WU(2),WU(5),Z_SLABB), (WU(2),WU(-5),Z_SLABB)]),
 ("slabx",  WALLY, TEX_BRICK, [(WU(4),WU(5),Z_SLABT), (WU(4),WU(-5),Z_SLABT), (WU(4),WU(-5),Z_SLABB), (WU(4),WU(5),Z_SLABB)]),
]

verts = []          # class-1 model space, 8.8
tris  = []          # (i0,i1,i2, u0,v0,u1,v1,u2,v2, texid, flags)

for name, kind, tex, poly in FACES:
    raw = [uv(kind, p) for p in poly]
    # bias each axis by whole tiles until the whole face is inside a byte
    def bias(vals):
        lo = min(vals)
        b = 0
        while lo + b < 0:
            b += TEXW
        return b
    bu = bias([u for u, v in raw])
    bv = bias([v for u, v in raw])
    tc = []
    for u, v in raw:
        iu, iv = int(round(u + bu)), int(round(v + bv))
        assert 0 <= iu <= 255 and 0 <= iv <= 255, (name, iu, iv)
        tc.append((iu, iv))

    base = len(verts)
    for (x, y, z) in poly:
        verts.append((x, z, y))         # map -> class 1: y is up

    n = len(poly)
    # No reversal. The list below is authored so that each face is already
    # clockwise ON SCREEN for a class-1 camera standing inside the room --
    # which is what class 1 calls front-facing -- once (x,y,z) has become
    # (x,z,y). Reversing it here (the first cut did, reasoning only about the
    # axis swap's handedness and not about where the viewer is) culled 24 of
    # the 30 triangles and rendered the room as a few stray slivers.
    order = list(range(n))
    for k in range(1, n - 1):
        a, b, c = order[0], order[k], order[k + 1]
        tris.append((base + a, base + b, base + c,
                     tc[a][0], tc[a][1], tc[b][0], tc[b][1], tc[c][0], tc[c][1],
                     tex, 0, name))

assert len(verts) <= 256, len(verts)

def s16(v):
    v &= 0xffff
    return v & 0xff, v >> 8

out = []
out.append("; Generated by tools/gen_quakemesh.py -- do not edit by hand.")
out.append("; The room of gpu64_demo_quake.a as a class-1 mesh: %d vertices, %d triangles."
           % (len(verts), len(tris)))
out.append("")
out.append("NVERT3D  = %d" % len(verts))
out.append("NFACE3D  = %d" % len(tris))
out.append("VBLOBLEN = %d" % (len(verts) * 6))
out.append("FBLOBLEN = %d" % (len(tris) * 12))
out.append("")
out.append("vertBlob")
for (x, y, z) in verts:
    b = []
    for c in (x, y, z):
        b.extend(s16(c))
    out.append("\t.byte " + ", ".join("$%02x" % v for v in b))
out.append("")
out.append("faceBlob")
for t in tris:
    i0, i1, i2, u0, v0, u1, v1, u2, v2, tex, flags, name = t
    out.append("\t.byte %3d, %3d, %3d, %3d, %3d, %3d, %3d, %3d, %3d, %d, $%02x, 0\t; %s"
               % (i0, i1, i2, u0, v0, u1, v1, u2, v2, tex, flags, name))
out.append("")

open("games/quake/gpu64_demo_quake3d.inc", "w").write("\n".join(out))

# The same bytes again for tools/hostsim, so what the PC renders is the blob
# the C64 uploads and not a second transcription of the room that can drift
# from it.
h = []
h.append("/* Generated by tools/gen_quakemesh.py -- do not edit by hand. */")
h.append("/* The room of gpu64_demo_quake.a as class-1 mesh blobs. */")
h.append("#define QUAKE_NVERT %d" % len(verts))
h.append("#define QUAKE_NFACE %d" % len(tris))
h.append("")
h.append("static const unsigned char g_QuakeVertBlob[ %d ] = {" % (len(verts) * 6))
for (x, y, z) in verts:
    b = []
    for c in (x, y, z):
        b.extend(s16(c))
    h.append("\t" + ", ".join("0x%02x" % v for v in b) + ",")
h.append("};")
h.append("")
h.append("static const unsigned char g_QuakeFaceBlob[ %d ] = {" % (len(tris) * 12))
for t in tris:
    i0, i1, i2, u0, v0, u1, v1, u2, v2, tex, flags, name = t
    h.append("\t%3d, %3d, %3d, %3d, %3d, %3d, %3d, %3d, %3d, %d, 0x%02x, 0,\t/* %s */"
             % (i0, i1, i2, u0, v0, u1, v1, u2, v2, tex, flags, name))
h.append("};")
h.append("")
open("tools/hostsim/quake_level.h", "w").write("\n".join(h))

print("%d verts, %d tris -> games/quake/gpu64_demo_quake3d.inc, tools/hostsim/quake_level.h"
      % (len(verts), len(tris)))

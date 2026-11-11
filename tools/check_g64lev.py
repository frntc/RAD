#!/usr/bin/env python3
# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""Validate a .g64lev level file produced by tools/gen_quakelevel.py.

Checks the header, that every table and blob offset lands inside the file,
and that every mesh blob obeys the class-1 wire format the firmware enforces
in gpu64_3dBuildMesh(): vertex stride 6, face stride 12, at most 256 verts,
and no face index past the mesh's own vertex count. Those are exactly the
conditions that come back as BAD_ARGS from UPLOAD_MESH, so a file that
passes here cannot be rejected for its shape.

  python3 tools/check_g64lev.py build/e1m1.g64lev
"""
import os, struct, sys

VERT_STRIDE = 6
FACE_STRIDE = 12
MAX_VERTS = 256

HDR = '<4sHH4HHHI5I2I3iH4BH'
HDR_LEN = struct.calcsize(HDR)
TEX_REC = '<HBBII'
MESH_REC = '<IIII'
NODE_REC = '<HiiiH'
ENT_REC = '<6H2h3i3iBBH'
ENT_STRIDE = struct.calcsize(ENT_REC)
PLANE_REC = '<3hiBB'
HULL_REC = '<2i6ii'
HULL_STRIDE = struct.calcsize(HULL_REC)
CLIP_REC = '<HhhH'


def fail(msg):
    print('FAIL  ' + msg)
    sys.exit(1)


def main(path):
    d = open(path, 'rb').read()
    if len(d) < HDR_LEN + 4:
        fail('file is shorter than its header')

    (magic, ver, scale, ntex, nmesh, nnode, nent, nplane, nhull, nclip,
     base, palo, stro, viso, visl, pico, picl, sx, sy, sz, syaw,
     step_up, eye, feet, waist, sflags) = struct.unpack_from(HDR, d, 0)
    if magic != b'G64L':
        fail('magic is %r, not G64L' % magic)
    if ver != 8:
        fail('version %d is not 8' % ver)
    print('header  ver %d  scale %d qu/wu  tex %d  mesh %d  node %d  ent %d'
          % (ver, scale, ntex, nmesh, nnode, nent))
    print('        collision  %d planes  %d hulls  %d clipnodes'
          % (nplane, nhull, nclip))
    print('        start %s  body step %d eye %d feet %d waist %d'
          % ('%.2f %.2f %.2f yaw %04x' % (sx / 65536.0, sy / 65536.0,
                                         sz / 65536.0, syaw)
             if sflags & 1 else 'none', step_up, eye, feet, waist))

    off = HDR_LEN
    tex_off, off = off, off + ntex * struct.calcsize(TEX_REC)
    mesh_off, off = off, off + nmesh * struct.calcsize(MESH_REC)
    node_off, off = off, off + nnode * struct.calcsize(NODE_REC)
    ent_off, off = off, off + nent * ENT_STRIDE
    plane_off, off = off, off + nplane * struct.calcsize(PLANE_REC)
    hull_off, off = off, off + nhull * HULL_STRIDE
    clip_off, off = off, off + nclip * struct.calcsize(CLIP_REC)
    if off != base:
        fail('tables end at %d but the header says the blob area starts at %d'
             % (off, base))
    if base > len(d):
        fail('blob area starts past the end of the file')
    print('tables  end at %d == base  OK   blob area %d B' % (base, len(d) - base))

    def blob(o, n, what):
        if o + n > len(d) - base:
            fail('%s runs past the end of the blob area (%d+%d > %d)'
                 % (what, o, n, len(d) - base))
        return d[base + o: base + o + n]

    pal = blob(palo, 768, 'palette')
    if len(pal) != 768:
        fail('palette is %d bytes, not 768' % len(pal))

    texels = 0
    for i in range(ntex):
        qid, ws, hs, o, ln = struct.unpack_from(TEX_REC, d, tex_off + i * 12)
        if not (3 <= ws <= 8 and 3 <= hs <= 8):
            fail('texture %d has shifts %d,%d outside the 3..8 UPLOAD_TEXTURE range'
                 % (i, ws, hs))
        if ln != (1 << ws) * (1 << hs):
            fail('texture %d: len %d != %dx%d' % (i, ln, 1 << ws, 1 << hs))
        blob(o, ln, 'texture %d' % i)
        texels += ln
    print('tex     %d records, %d texels, all shifts 3..8 and len == w*h  OK'
          % (ntex, texels))

    vmax = fmax = 0
    tris = 0
    for i in range(nmesh):
        vo, vl, fo, fl = struct.unpack_from(MESH_REC, d, mesh_off + i * 16)
        if vl == 0 or vl % VERT_STRIDE:
            fail('mesh %d: vertex len %d is not a nonzero multiple of %d'
                 % (i, vl, VERT_STRIDE))
        if fl == 0 or fl % FACE_STRIDE:
            fail('mesh %d: face len %d is not a nonzero multiple of %d'
                 % (i, fl, FACE_STRIDE))
        nv = vl // VERT_STRIDE
        nf = fl // FACE_STRIDE
        if nv > MAX_VERTS:
            fail('mesh %d has %d verts, past the 1-byte index limit of %d'
                 % (i, nv, MAX_VERTS))
        vb = blob(vo, vl, 'mesh %d verts' % i)
        fb = blob(fo, fl, 'mesh %d faces' % i)
        for f in range(nf):
            w = fb[f * FACE_STRIDE: (f + 1) * FACE_STRIDE]
            if max(w[0], w[1], w[2]) >= nv:
                fail('mesh %d face %d indexes vertex %d of %d'
                     % (i, f, max(w[0], w[1], w[2]), nv))
            if w[11] != 0:
                fail('mesh %d face %d has pad byte %02x, not 0' % (i, f, w[11]))
            # A textured face's texid is a one-byte resource id, so it must
            # name a texture this file actually carries; the renderer's only
            # other reading of it is "palette index", which is what
            # FLAT_COLOUR selects.
            if not (w[10] & 0x02) and w[9] >= ntex:
                fail('mesh %d face %d is textured with resource %d of %d'
                     % (i, f, w[9], ntex))
        vmax = max(vmax, nv); fmax = max(fmax, nf); tris += nf
    print('mesh    %d records, max %d verts / %d faces, %d tris, indices and pad  OK'
          % (nmesh, vmax, fmax, tris))

    for i in range(nnode):
        mi, x, y, z, model = struct.unpack_from(NODE_REC, d, node_off + i * 16)
        if mi >= nmesh:
            fail('node %d points at mesh %d of %d' % (i, mi, nmesh))
        for c, n in zip((x, y, z), 'xyz'):
            if abs(c) > 128 * 65536:
                fail('node %d origin %s = %.1f wu is past the 8.8 model range'
                     % (i, n, c / 65536.0))
    print('node    %d records, mesh ids and origins in range  OK' % nnode)

    # The string blob's extent is implied: it starts at stro and every offset
    # into it must land on a byte that follows a NUL, which is what makes a
    # C-string read from it terminate inside the blob.
    if d[base + stro] != 0:
        fail('the string blob does not start with the NUL that offset 0 means')
    nul = d.index(b'\0\0', base + stro) if b'\0\0' in d[base + stro:] else None
    strend = len(d) - base
    names = {}
    unknown = set()
    catalog = None
    for i in range(nent):
        (nameOff, yaw, sf, mdl, tgt, tnm, p0, p1, x, y, z,
         ox, oy, oz, kind, _pad, _rsv) = \
            struct.unpack_from(ENT_REC, d, ent_off + i * ENT_STRIDE)
        for o, what in ((nameOff, 'classname'), (tgt, 'target'), (tnm, 'targetname')):
            if stro + o >= strend:
                fail('entity %d %s offset %d is past the string blob' % (i, what, o))
            e = d.index(b'\0', base + stro + o)
            if e >= len(d):
                fail('entity %d %s is unterminated' % (i, what))
        if mdl and mdl >= 256:
            fail('entity %d names brush model %d, which no BSP has' % (i, mdl))
        nm = d[base + stro + nameOff: d.index(b'\0', base + stro + nameOff)].decode()
        names[nm] = names.get(nm, 0) + 1
        if not nm:
            fail('entity %d has an empty classname' % i)
        # A door whose travel is zero is a door that cannot open, and the
        # only way to find that out later is at the bench. kinds 11-14 are
        # the four brush movers; see ENT_KIND in tools/gen_quakelevel.py.
        # (E1M3's and E1M4's 4-unit buttons get no travel from Quake's own
        # formula; the converter gives them a token unit so they are kept.)
        if kind in (11, 12, 13, 14) and (ox, oy, oz) == (0, 0, 0):
            fail('entity %d (%s, kind %d) is a mover with no travel' % (i, nm, kind))
        if kind == 0:
            unknown.add(nm)
        if nm == 'gpu64_catalog':
            catalog = (i, sf, p0, p1, kind)
    starts = names.get('info_player_start', 0)
    if starts != 1:
        fail('%d info_player_start entities; the game needs exactly one' % starts)
    print('ent     %d records, %d classnames, strings terminated, 1 player start  OK'
          % (nent, len(names)))
    # Not a failure: a classname the kind table has no opinion about is
    # decoration as far as a game is concerned. Printed because a *lot* of
    # them means the table has fallen behind the maps being converted.
    if unknown:
        print('ent     %d classnames have kind 0: %s'
              % (len(unknown), ' '.join(sorted(unknown))))
    # --- milestone 20's actor catalogue --------------------------------
    #
    # Optional: a --no-actors level has none. When present it must be the
    # last entity and must describe this file, and when the game's include
    # is at hand every id it compiles in must name something that exists --
    # a stale include against a fresh level is a dog wearing a gun's frames.
    if catalog is not None:
        ci, chash, cmesh, ctex, ckind = catalog
        if ci != nent - 1 or ckind != 9:
            fail('gpu64_catalog is entity %d kind %d; it must be the last (%d) '
                 'with kind 9' % (ci, ckind, nent - 1))
        if (cmesh, ctex) != (nmesh, ntex):
            fail('gpu64_catalog says %d meshes %d textures; the file has %d %d'
                 % (cmesh, ctex, nmesh, ntex))
        inc = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                           'games', 'quake', 'gpu64_quake_actors.inc')
        if os.path.exists(inc):
            sym = {}
            for ln in open(inc):
                ln = ln.split(';')[0].strip()
                if '=' in ln:
                    k, v = (t.strip() for t in ln.split('=', 1))
                    sym[k] = int(v[1:], 16) if v.startswith('$') else int(v)
            # Only the hash: the actors come first in every level now, so
            # one include serves E1M1 and E1M2 alike. CAT_ENT is the level's
            # own entity count and differs between them; nothing reads it.
            if sym.get('CAT_HASH') != chash:
                fail('%s is stale: hash $%04x ent %s, the level has $%04x ent %d'
                     % (inc, sym.get('CAT_HASH', 0), sym.get('CAT_ENT'), chash, ci))
            for k, v in sym.items():
                if k.startswith('T_') and not v < ntex:
                    fail('%s = %d is not a texture of this file' % (k, v))
                if k.startswith('M_') and not v < nmesh:
                    fail('%s = %d is not a mesh of this file' % (k, v))
                if k.startswith('N_') and k.replace('N_', 'M_', 1) in sym \
                        and sym[k.replace('N_', 'M_', 1)] + v > nmesh:
                    fail('%s runs past the mesh table' % k)
            print('actors  catalogue hash $%04x, %d include symbols in range  OK'
                  % (chash, len(sym)))
        else:
            print('actors  catalogue hash $%04x (no include to cross-check)' % chash)
    # --- the collision hulls ------------------------------------------
    #
    # Shape checks first, then the one check that actually says the
    # conversion is right: walk hull 1 and ask what is at the player start.
    # A sign error in a plane normal, a missed axis swap, or a dist that did
    # not get scaled all leave the tables perfectly well-formed and put the
    # player inside a wall -- which the renderer cannot tell you, because
    # the renderer never looks at a clipnode.
    planes = [struct.unpack_from(PLANE_REC, d, plane_off + i * 12)
              for i in range(nplane)]
    for i, (nx, ny, nz, dist, ty, pad) in enumerate(planes):
        if ty > 3 or pad != 0:
            fail('plane %d has type %d pad %d' % (i, ty, pad))
        # 1.15 normals of a unit vector: the square sum must land on 1.0 to
        # within rounding, or the trace's distances are wrong by that factor.
        mag = (nx * nx + ny * ny + nz * nz) / (32767.0 * 32767.0)
        if not 0.99 < mag < 1.01:
            fail('plane %d normal is not unit: |n|^2 = %.4f' % (i, mag))
    clips = [struct.unpack_from(CLIP_REC, d, clip_off + i * 8)
             for i in range(nclip)]
    for i, (pn, c0, c1, pad) in enumerate(clips):
        if pn >= nplane:
            fail('clipnode %d names plane %d of %d' % (i, pn, nplane))
        for c in (c0, c1):
            if c >= nclip:
                fail('clipnode %d has child %d of %d' % (i, c, nclip))
            if c < -16:
                fail('clipnode %d has contents %d, which Quake has no name for'
                     % (i, c))
        if pad != 0:
            fail('clipnode %d has pad %d' % (i, pad))
    hulls = [struct.unpack_from(HULL_REC, d, hull_off + i * HULL_STRIDE)
             for i in range(nhull)]
    for i, h in enumerate(hulls):
        for k in range(3):
            if h[2 + k] > h[5 + k]:
                fail('hull %d axis %d has mins %d > maxs %d'
                     % (i, k, h[2 + k], h[5 + k]))
    print('hull    %d planes unit-length, %d clipnodes in range, %d models  OK'
          % (nplane, nclip, nhull))

    def contents(node, pt):
        """Quake's hull point test, on the converted tables. Returns the
        CONTENTS_* the point lands in: -1 empty, -2 solid, -3.. liquid."""
        while node >= 0:
            pn, c0, c1, _ = clips[node]
            nx, ny, nz, dist, _, _ = planes[pn]
            # 1.15 normal against a 16.16 position gives 16.16 after the
            # >>15, which is the unit dist is in.
            dot = ((nx * pt[0] + ny * pt[1] + nz * pt[2]) >> 15) - dist
            node = c1 if dot < 0 else c0
        return node

    start = None
    for i in range(nent):
        r = ent_off + i * ENT_STRIDE
        nm = d[base + stro + struct.unpack_from('<H', d, r)[0]:]
        if nm[:nm.index(b'\0')] == b'info_player_start':
            start = struct.unpack_from('<3i', d, r + 16)
    if start is None:
        fail('no info_player_start to test the hull with')

    root = hulls[0][0]                  # worldspawn, hull 1 (the player hull)
    eye = (start[0], start[1] + (22 << 16) // scale, start[2])
    c = contents(root, eye)
    if c != -1:
        fail('the player start is in contents %d, not empty space -- the hull'
             ' conversion has an axis or a sign wrong' % c)

    # And a point that must NOT be empty, or "empty" proves nothing:
    # straight down from the start is floor. How far down is the level's
    # business -- E1M8 starts the player 636 units above the floor (its
    # low-gravity drop), and E1M5/E1M6 put thin floors over rooms -- so walk
    # down in 4-unit steps to the first non-empty point instead of probing
    # one fixed depth. 1024 units of nothing is not a floor.
    def floor_below(node, top):
        for qu in range(4, 1028, 4):
            pt = (top[0], top[1] - (qu << 16) // scale, top[2])
            k = contents(node, pt)
            if k != -1:
                return qu, k
        return None, -1
    dq, cb = floor_below(root, start)
    if dq is None:
        fail('1024 Quake units below the player start are all empty -- the'
             ' hull is not describing this level')
    print('trace   player start is empty (%d), %dqu below is %d  OK'
          % (c, dq, cb))

    # Hull 0, the point hull the shots trace against, is built by the
    # converter rather than carried from the BSP, so it gets the same pair
    # of tests at the eye: empty there, something under the floor.
    root0 = hulls[0][8]
    if not 0 <= root0 < nclip:
        fail('worldspawn hull 0 head %d is not a clipnode' % root0)
    c0 = contents(root0, eye)
    dq0, cb0 = floor_below(root0, eye)
    if c0 != -1 or dq0 is None:
        fail('hull 0: eye contents %d, nothing below it for 1024qu' % c0)
    print('trace   hull 0: eye is empty (%d), %dqu below the eye is %d  OK'
          % (c0, dq0, cb0))

    # --- visibility (v6) ------------------------------------------------
    #
    # gen_quakelevel.py's build_vis() has the layout. The one check that
    # says it is right: the drawing BSP puts the player-start eye in an open
    # leaf, and that leaf's row is neither empty (the firmware would draw
    # nothing) nor all ones (it would cull nothing).
    if visl == 0:
        fail('no visibility section')
    if viso + visl > len(d) - base:
        fail('visibility section %d+%d runs past the blob area' % (viso, visl))
    vo = base + viso
    nleafs, rowb, nworld, nvn, head, vpad = struct.unpack_from('<6H', d, vo)
    if 12 + nvn * 8 + nleafs * rowb != visl:
        fail('visibility section is %d bytes, its header adds up to %d'
             % (visl, 12 + nvn * 8 + nleafs * rowb))
    if rowb * 8 < nworld or nworld > nnode or head >= nvn:
        fail('visibility header: rows %d B for %d world nodes of %d, head %d/%d'
             % (rowb, nworld, nnode, head, nvn))
    for j in range(nworld):
        if struct.unpack_from('<H', d, node_off + j * 16 + 14)[0] != 0:
            fail('node %d is covered by the vis rows but is not the world' % j)
    vnodes = [struct.unpack_from('<HhhH', d, vo + 12 + i * 8)
              for i in range(nvn)]
    for i, (pn, a0, a1, _) in enumerate(vnodes):
        if pn >= nplane or a0 >= nvn or a1 >= nvn:
            fail('vis node %d out of range: plane %d children %d %d'
                 % (i, pn, a0, a1))
    n, steps = head, 0
    while n >= 0 and steps < 96:
        pn, a0, a1, _ = vnodes[n]
        nx, ny, nz, dist, _, _ = planes[pn]
        n = a1 if ((nx * eye[0] + ny * eye[1] + nz * eye[2]) >> 15) - dist < 0 \
            else a0
        steps += 1
    leaf = -1 - n
    if n >= 0 or not 0 < leaf < nleafs:
        fail('the player-start eye is in no visible leaf (%d)' % n)
    row = d[vo + 12 + nvn * 8 + leaf * rowb:][:rowb]
    seen = sum(1 for j in range(nworld) if row[j >> 3] & (1 << (j & 7)))
    if not 0 < seen < nworld:
        fail('the player start sees %d of %d world chunks' % (seen, nworld))
    print('vis     %d leafs x %d B, %d BSP nodes; start leaf %d sees %d of %d'
          ' world chunks  OK' % (nleafs, rowb, nvn, leaf, seen, nworld))

    # --- pictures (v7) --------------------------------------------------
    #
    # The firmware's own rule: the section's length is exactly its table,
    # and every picture's w*h lies inside the blob area. LEVEL_PICTURE then
    # draws with a straight copy.
    if picl:
        sec = blob(pico, picl, 'picture section')
        npic = struct.unpack_from('<H', sec, 0)[0]
        if 4 + npic * 8 != picl:
            fail('picture section is %d bytes, %d pictures need %d'
                 % (picl, npic, 4 + npic * 8))
        px = 0
        for i in range(npic):
            w, h, o = struct.unpack_from('<HHI', sec, 4 + i * 8)
            if not w or not h:
                fail('picture %d is %dx%d' % (i, w, h))
            blob(o, w * h, 'picture %d' % i)
            px += w * h
        print('pics    %d pictures, %d B of pixels  OK' % (npic, px))
    else:
        print('pics    none (a level built with --no-actors)')

    print('PASS  %s (%.1f KB)' % (path, len(d) / 1024.0))


if __name__ == '__main__':
    main(sys.argv[1] if len(sys.argv) > 1 else 'build/e1m1.g64lev')

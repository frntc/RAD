# Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
"""
Milestone 20's assets: the things E1M1 is populated with, as opposed to the
level itself. gen_quakelevel.py imports this and appends what it returns to
the same texture and mesh tables the level uses, so the loader builds every
one of them as an ordinary resource and no firmware learns a new format.

Three sources, all inside PAK0.PAK:

  progs/*.mdl     monsters, view-model guns, pickups -- alias models
  maps/b_*.bsp    the health / ammo boxes, which Quake draws as tiny BSPs
  gfx.wad         the status bar: digits, faces, icons, the bar itself

An animation frame is a mesh. What makes that affordable is that the loader
COPIES a mesh's vertex and face blobs into the arena (gpu64_3dBuildMesh), so
any number of mesh records may point at the same face blob. A model therefore
costs one face blob plus 6 bytes per vertex per frame, and switching frames
is a resource-id change on the node -- which is what WORLD_TICK's actor
record carries anyway.

Local axes. The level maps Quake (x, y, z) to gpu64 (x, z, y). An actor mesh
instead maps it to (-y, z, x), so the model's forward, Quake +x, becomes local
+z, which is gpu64's forward at yaw 0. The node then takes the entity's yaw
exactly as yaw_of() derives it. Both mappings have determinant -1, so the
winding argument in gen_quakelevel.py's coordinate comment holds here too and
no triangle is reversed. Vertices stay relative to the model origin -- the
point Quake positions the entity by -- rather than being re-centred as level
chunks are, so an actor's node position IS its entity origin.
"""

import math, struct, zlib
from collections import OrderedDict

QU = 32.0                       # Quake units per gpu64 world unit
MAX_VERTS = 256


def q88(v):
    n = int(round(v * 256.0))
    return max(-32768, min(32767, n))


def mdl_axes(p):
    """Quake model space -> gpu64 local, in world units."""
    return (-p[1] / QU, p[2] / QU, p[0] / QU)


def pow2_near(n, lo=8, hi=256):
    """The power of two nearest n on a log scale, clamped to what a texture
    shift can say (3..8)."""
    s = int(round(math.log2(max(1, n))))
    return max(lo, min(hi, 1 << s))


def resample(px, w, h, nw, nh):
    """Nearest-neighbour resize of an 8bpp indexed image. Filtering is not an
    option on indexed colour, and Quake's skins are low-frequency enough that
    nearest looks like Quake."""
    out = bytearray(nw * nh)
    for y in range(nh):
        sy = min(h - 1, (y * h) // nh)
        row = sy * w
        for x in range(nw):
            out[y * nw + x] = px[row + min(w - 1, (x * w) // nw)]
    return bytes(out)


SKIN_PAD = 208                  # Quake palette pure blue (0,0,255)


def bleed(px, w, h, pad=SKIN_PAD, passes=4):
    """Grow the art into the pure-blue padding some skins (v_axe, v_shot,
    v_shot2) are painted on. Shrinking a 308-wide skin to 256 lands UV edges
    on padding texels, and the renderer then paints bright blue slivers along
    every seam -- levelsim showed them on the super shotgun. Only index 208
    is treated as padding: the other skins' corner colours are also used
    inside the art, and bleeding those would repaint it."""
    a = bytearray(px)
    for _ in range(passes):
        src = bytes(a)
        for y in range(h):
            for x in range(w):
                if src[y * w + x] != pad:
                    continue
                for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                    nx, ny = x + dx, y + dy
                    if 0 <= nx < w and 0 <= ny < h and src[ny * w + nx] != pad:
                        a[y * w + x] = src[ny * w + nx]
                        break
    return bytes(a)


# ---------------------------------------------------------------------------
# MDL
# ---------------------------------------------------------------------------

class Mdl:
    def __init__(self, data, name):
        self.name = name
        if data[:4] != b'IDPO':
            raise ValueError('%s: not an alias model' % name)
        ver, = struct.unpack_from('<i', data, 4)
        if ver != 6:
            raise ValueError('%s: MDL version %d' % (name, ver))
        self.scale = struct.unpack_from('<3f', data, 8)
        self.trans = struct.unpack_from('<3f', data, 20)
        (nskins, self.sw, self.sh, nverts, ntris, nframes) = \
            struct.unpack_from('<6i', data, 48)
        o = 84
        self.skins = []
        for _ in range(nskins):
            grp, = struct.unpack_from('<i', data, o); o += 4
            if grp == 0:
                self.skins.append(data[o:o + self.sw * self.sh])
                o += self.sw * self.sh
            else:                           # skin group: keep the first
                n, = struct.unpack_from('<i', data, o); o += 4 + 4 * n
                self.skins.append(data[o:o + self.sw * self.sh])
                o += n * self.sw * self.sh
        self.st = []
        for _ in range(nverts):
            self.st.append(struct.unpack_from('<3i', data, o)); o += 12
        self.tris = []
        for _ in range(ntris):
            self.tris.append(struct.unpack_from('<4i', data, o)); o += 16
        self.frames = []                    # (name, [(x,y,z) quake units])
        for _ in range(nframes):
            ty, = struct.unpack_from('<i', data, o); o += 4
            if ty == 0:
                nm, vs, o = self._simple(data, o, nverts)
                self.frames.append((nm, vs))
            else:
                n, = struct.unpack_from('<i', data, o)
                o += 4 + 8 + 4 * n          # count, min, max, times[n]
                first = None
                for k in range(n):
                    nm, vs, o = self._simple(data, o, nverts)
                    if first is None:
                        first = (nm, vs)
                self.frames.append(first)
        self.nverts = nverts

    def _simple(self, data, o, nverts):
        o += 8                              # bboxmin, bboxmax trivertx
        nm = data[o:o + 16].split(b'\0')[0].decode('latin-1'); o += 16
        vs = []
        for i in range(nverts):
            x, y, z, _ = data[o + i * 4: o + i * 4 + 4]
            vs.append((self.scale[0] * x + self.trans[0],
                       self.scale[1] * y + self.trans[1],
                       self.scale[2] * z + self.trans[2]))
        return nm, vs, o + nverts * 4

    def groups(self):
        """Frame names grouped by their alphabetic stem, in file order:
        'run1'..'run8' -> ('run', [indices])."""
        g = OrderedDict()
        for i, (nm, _) in enumerate(self.frames):
            stem = nm.rstrip('0123456789')
            g.setdefault(stem, []).append(i)
        return g


def mdl_faces(m, texid, tw, th):
    """The model's one shared face blob. UVs are per face corner, which is
    what makes Quake's seam vertices free: a back-facing triangle on a seam
    vertex just gets s + skinwidth/2 in its own corner."""
    fb = bytearray()
    for front, a, b, c in m.tris:
        fb += bytes((a, b, c))
        for vi in (a, b, c):
            onseam, s, t = m.st[vi]
            if not front and onseam:
                s += m.sw // 2
            u = min(255, int((s + 0.5) * tw / m.sw))
            v = min(255, int((t + 0.5) * th / m.sh))
            fb += bytes((u, v))
        fb += bytes((texid, 0, 0))
    return bytes(fb)


def mdl_verts(vs):
    vb = bytearray()
    for p in vs:
        for c in mdl_axes(p):
            vb += struct.pack('<h', q88(c))
    return bytes(vb)


# Which MDLs, and which of their animations. None means every frame in file
# order. These are the FIXED catalogue: every level carries them at the same
# mesh indices, so the game is assembled against their M_* symbols.
MODELS = [
    # key       file                      animations
    ('v_axe',   'progs/v_axe.mdl',    None),
    ('v_shot',  'progs/v_shot.mdl',   None),
    ('v_shot2', 'progs/v_shot2.mdl',  None),
    ('v_nail',  'progs/v_nail.mdl',   None),
    ('g_shot',  'progs/g_shot.mdl',   None),
    ('g_nail',  'progs/g_nail.mdl',   None),
    ('armor',   'progs/armor.mdl',    None),
    ('quad',    'progs/quaddama.mdl', None),
    ('suit',    'progs/suit.mdl',     None),
    ('spike',   'progs/spike.mdl',    None),
    ('backpack','progs/backpack.mdl', None),
    # The monsters' missiles (games/quake/gpu64_game_proj.inc).
    ('grenade', 'progs/grenade.mdl',  None),
    ('w_spike', 'progs/w_spike.mdl',  None),
    ('zom_gib', 'progs/zom_gib.mdl',  None),
]

# The monster bank. Unlike the fixed catalogue, a level carries only the
# monsters its own map places, so their mesh indices vary by level and the
# game reads them from the level instead: one gpu64_bank entity per monster
# kind (bank_entities()), scanned before anything it describes.
#
# Eight animation slots, the ones the game's AI plays; a monster without a
# use for one leaves it empty (count 0). DEATHB may repeat DEATH. A slot
# given as (animation, n) plays only that animation's first n full-rate
# frames, sharing its meshes: the zombie's DEATH is the fall of its knock-
# down (paine1-12) and stays lying, its DEATHB the whole knockdown, which
# the game plays as the get-up-again, not as a death.
BANK_SLOTS = ['STAND', 'RUN', 'MELEE', 'LEAP', 'SHOOT', 'PAIN', 'DEATH',
              'DEATHB']
BANK_KIND = 18                  # the bank record's entity kind
# kind: (key, file, frame step, {slot: animation}, {slot: hit frame})
# The hit frame is a FULL-rate frame index into that animation -- where
# Quake's own code deals the damage -- and is divided by the step. A step of
# 2 keeps every other frame, and the game plays those at half rate, so an
# animation lasts as long as Quake's; the soldier and dog keep every frame
# (E1M1 is unchanged), and the rest would not fit beside them in the 255
# mesh indices a one-byte frame number reaches.
MONSTERS = OrderedDict([
    (51, ('soldier', 'progs/soldier.mdl', 1,
          {'STAND': 'stand', 'RUN': 'run', 'SHOOT': 'shoot', 'PAIN': 'pain',
           'DEATH': 'death', 'DEATHB': 'deathc'},
          {'SHOOT': 4})),
    (52, ('dog', 'progs/dog.mdl', 1,
          {'STAND': 'stand', 'RUN': 'run', 'MELEE': 'attack', 'LEAP': 'leap',
           'PAIN': 'pain', 'DEATH': 'death', 'DEATHB': 'deathb'},
          {'MELEE': 3})),
    (53, ('ogre', 'progs/ogre.mdl', 2,          # ogre_swing's chainsaw,
          {'STAND': 'stand', 'RUN': 'run', 'MELEE': 'swing',  # ogre_nail4's
           'SHOOT': 'shoot', 'PAIN': 'pain', 'DEATH': 'death',  # grenade
           'DEATHB': 'bdeath'},
          {'MELEE': 6, 'SHOOT': 3})),
    (54, ('knight', 'progs/knight.mdl', 2,
          {'STAND': 'stand', 'RUN': 'runb', 'MELEE': 'attackb',
           'PAIN': 'pain', 'DEATH': 'death', 'DEATHB': 'deathb'},
          {'MELEE': 6})),
    (55, ('zombie', 'progs/zombie.mdl', 2,      # atta13's thrown gib
          {'STAND': 'stand', 'RUN': 'run', 'SHOOT': 'atta',
           'PAIN': 'paina', 'DEATH': ('paine', 12), 'DEATHB': 'paine'},
          {'SHOOT': 12})),
    (56, ('wizard', 'progs/wizard.mdl', 2,      # magatt's two spikes
          {'STAND': 'hover', 'RUN': 'fly', 'SHOOT': 'magatt',
           'PAIN': 'pain', 'DEATH': 'death', 'DEATHB': 'death'},
          {'SHOOT': 4})),
    (57, ('demon', 'progs/demon.mdl', 2,
          {'STAND': 'stand', 'RUN': 'run', 'MELEE': 'attacka',
           'LEAP': 'leap', 'PAIN': 'pain', 'DEATH': 'death',
           'DEATHB': 'death'},
          {'MELEE': 4})),
    (58, ('shambler', 'progs/shambler.mdl', 2,  # smash10, magic's bolt
          {'STAND': 'stand', 'RUN': 'run', 'MELEE': 'smash',
           'SHOOT': 'magic', 'PAIN': 'pain', 'DEATH': 'death',
           'DEATHB': 'death'},
          {'MELEE': 9, 'SHOOT': 5})),
])
BANK_REC = 24                   # payload bytes: origin and ofs, 3 x s32 each

# Brush pickups: Quake draws these as tiny BSP models whose origin is the
# item's corner, the same point the entity's origin names.
BRUSH_ITEMS = [
    ('bh10',   'maps/b_bh10.bsp'),
    ('bh25',   'maps/b_bh25.bsp'),
    ('bh100',  'maps/b_bh100.bsp'),
    ('shell0', 'maps/b_shell0.bsp'),
    ('shell1', 'maps/b_shell1.bsp'),
    ('nail0',  'maps/b_nail0.bsp'),
    ('nail1',  'maps/b_nail1.bsp'),
]

# Status-bar pics. Sprites treat texel index 0 as a hole
# (gpu64_3d_raster.cpp), so the art is remapped: Quake's transparent 255
# becomes 0, and Quake's real black 0 moves to the darkest other index.
HUD_PICS = (['NUM_%d' % i for i in range(10)] + ['NUM_MINUS'] +
            ['ANUM_%d' % i for i in range(10)] + ['ANUM_MINUS'] +
            ['FACE%d' % i for i in range(1, 6)] +
            ['FACE_P%d' % i for i in range(1, 6)] +
            ['FACE_QUAD', 'SB_ARMOR1', 'SB_ARMOR2', 'SB_SHELLS', 'SB_NAILS',
             'SB_QUAD', 'SB_SUIT'])
SBAR_TILES = 5                      # 320x24 -> five 64x32 tiles


def read_wad(data):
    magic, n, dirofs = struct.unpack_from('<4sii', data, 0)
    if magic != b'WAD2':
        raise ValueError('gfx.wad is not WAD2')
    out = {}
    for i in range(n):
        fp, dsz, sz, ty, cmp, _, nm = struct.unpack_from('<iiibbh16s', data,
                                                         dirofs + i * 32)
        nm = nm.split(b'\0')[0].decode('latin-1').upper()
        if ty == 0x42 and cmp == 0:
            w, h = struct.unpack_from('<ii', data, fp)
            out[nm] = (w, h, data[fp + 8: fp + 8 + w * h])
    return out


def darkest_nonzero(pal):
    best, bi = None, 1
    for i in range(1, 255):
        r, g, b = pal[i * 3: i * 3 + 3]
        s = r + g + b
        if best is None or s < best:
            best, bi = s, i
    return bi


def sprite_pic(w, h, px, tw, th, black):
    """Centre a pic in a tw x th hole-filled texture, remapped for sprites."""
    out = bytearray(tw * th)
    ox, oy = (tw - w) // 2, (th - h) // 2
    for y in range(h):
        for x in range(w):
            c = px[y * w + x]
            c = 0 if c == 255 else (black if c == 0 else c)
            out[(y + oy) * tw + x + ox] = c
    return bytes(out)


def crosshair(black):
    """8x8: a small white plus with a dark outline, hole elsewhere."""
    WHITE, n = 254, 8
    out = bytearray(n * n)
    arm = set()
    for i in (1, 2, 5, 6):
        arm |= {(i, 3), (i, 4), (3, i), (4, i)}
    for x, y in arm:                        # outline first, arms on top
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                if 0 <= x + dx < n and 0 <= y + dy < n:
                    out[(y + dy) * n + x + dx] = black
    for x, y in arm:
        out[y * n + x] = WHITE
    for x in (3, 4):                        # keep the centre open
        for y in (3, 4):
            out[y * n + x] = 0
    return bytes(out)


# ---------------------------------------------------------------------------
# The whole catalogue
# ---------------------------------------------------------------------------

def build(pak_f, pak_ents, pak_read, Bsp, collect_fn, pal, tex_base, mesh_base,
          stats):
    """Returns (textures, meshes, symbols).

      textures  [(name, wshift, hshift, pixels)] -- ids tex_base + i
      meshes    [(vblob, fblob)]                  -- indices mesh_base + i
      symbols   OrderedDict name -> value, for the 64tass include

    Face blobs are shared by identity: the caller dedupes equal bytes objects
    when it lays out the file."""
    texs, meshes, sym = [], [], OrderedDict()

    def add_tex(name, w, h, px):
        ws, hs = int(math.log2(w)), int(math.log2(h))
        assert (1 << ws) == w and (1 << hs) == h and 3 <= ws <= 8 and 3 <= hs <= 8
        assert len(px) == w * h
        texs.append((name, ws, hs, px))
        tid = tex_base + len(texs) - 1
        if tid > 255:
            raise ValueError('texture ids ran past 255: a face texid is one byte')
        return tid

    # --- alias models -------------------------------------------------------
    for key, path, anims in MODELS:
        m = Mdl(pak_read(pak_f, pak_ents, path), path)
        if m.nverts > MAX_VERTS:
            raise ValueError('%s: %d vertices' % (path, m.nverts))
        skin_ids, tw, th = model_skin(m, key, add_tex)
        sym['T_%s' % key.upper()] = skin_ids[0]
        if len(skin_ids) > 1:
            for si, t in enumerate(skin_ids):
                sym['T_%s_%d' % (key.upper(), si)] = t
        fb = mdl_faces(m, skin_ids[0], tw, th)
        # Alternate skins (armor has three) need their own face blob, since
        # the texture id lives in the face record.
        fbs = [fb] + [mdl_faces(m, t, tw, th) for t in skin_ids[1:]]
        groups = m.groups()
        if anims is None:
            order = [('', list(range(len(m.frames))))]
        else:
            order = []
            for a in anims:
                if a not in groups:
                    raise ValueError('%s has no "%s" frames (%s)'
                                     % (path, a, ', '.join(groups)))
                order.append((a, groups[a]))
        for si, sfb in enumerate(fbs):
            skey = key.upper() + ('' if si == 0 else '_S%d' % si)
            first = mesh_base + len(meshes)
            sym['M_%s' % skey] = first
            for a, idxs in order:
                if a:
                    sym['M_%s_%s' % (skey, a.upper())] = mesh_base + len(meshes)
                    sym['N_%s_%s' % (skey, a.upper())] = len(idxs)
                for fi in idxs:
                    meshes.append((mdl_verts(m.frames[fi][1]), sfb))
            sym['N_%s' % skey] = mesh_base + len(meshes) - first
        stats['mdl_frames'] += sum(len(i) for _, i in order) * len(fbs)
        stats['mdl_tris'] += len(m.tris)

    # --- brush pickups ------------------------------------------------------
    for key, path in BRUSH_ITEMS:
        b = Bsp(pak_read(pak_f, pak_ents, path))
        tris = collect_fn(b, b.models[0], False, stats)
        texmap = {}
        for _, _, t, _ in tris:
            if t in texmap:
                continue
            tx = b.textures[t]
            texmap[t] = add_tex('%s_%s' % (key, tx['name']), tx['w'], tx['h'],
                                tx['px'][:tx['w'] * tx['h']])
        # collect() produced level axes (x, z, y); re-express in actor axes
        # (-y, z, x) = level (-z, y, x) so a pickup yaws like everything else.
        order, index = [], {}
        fb = bytearray()
        for pts, uvs, t, flags in tris:
            idx = []
            for p in pts:
                lp = (-p[2], p[1], p[0])
                if lp not in index:
                    index[lp] = len(order); order.append(lp)
                idx.append(index[lp])
            fb += bytes(idx)
            for u, v in uvs:
                fb += bytes((u & 0xff, v & 0xff))
            fb += bytes((texmap[t], flags, 0))
        if len(order) > MAX_VERTS:
            raise ValueError('%s: %d vertices' % (path, len(order)))
        vb = b''.join(struct.pack('<3h', *(q88(c) for c in p)) for p in order)
        sym['M_%s' % key.upper()] = mesh_base + len(meshes)
        meshes.append((vb, bytes(fb)))
        stats['brush_items'] += 1

    # --- status bar ---------------------------------------------------------
    wad = read_wad(pak_read(pak_f, pak_ents, 'gfx.wad'))
    black = darkest_nonzero(pal)
    for nm in HUD_PICS:
        w, h, px = wad[nm]
        tw, th = max(8, 1 << (w - 1).bit_length()), max(8, 1 << (h - 1).bit_length())
        sym['T_%s' % nm] = add_tex(nm.lower(), tw, th,
                                   sprite_pic(w, h, px, tw, th, black))
    w, h, px = wad['SBAR']
    tw = w // SBAR_TILES
    for i in range(SBAR_TILES):
        tile = bytearray()
        for y in range(h):
            tile += px[y * w + i * tw: y * w + (i + 1) * tw]
        t = add_tex('sbar%d' % i, 64, 32, sprite_pic(tw, h, bytes(tile), 64, 32,
                                                     black))
        if i == 0:
            sym['T_SBAR'] = t
    sym['T_CROSSHAIR'] = add_tex('crosshair', 8, 8, crosshair(black))
    stats['hud_textures'] = len(HUD_PICS) + SBAR_TILES + 1
    return texs, meshes, sym


def model_skin(m, key, add_tex):
    """Every skin of m as a texture; returns (ids, tw, th)."""
    tw, th = pow2_near(m.sw), pow2_near(m.sh)
    ids = []
    for si, sk in enumerate(m.skins):
        ids.append(add_tex('%s_skin%d' % (key, si), tw, th,
                           resample(bleed(sk, m.sw, m.sh), m.sw, m.sh, tw, th)))
    return ids, tw, th


def build_bank(pak_f, pak_ents, pak_read, kinds, tex_base, mesh_base, stats):
    """The level's own monsters: every kind in `kinds` that MONSTERS knows.
    Returns (textures, meshes, banks, symbols); banks is [(kind, payload)],
    the BANK_REC bytes of each gpu64_bank entity:

      0..15   per BANK_SLOTS: first mesh index (u8), frame count (u8)
      16      the MELEE hit frame, 17 the SHOOT hit frame (kept frames)
      18      the frame step (1 or 2)
      19..23  0
    """
    texs, meshes, banks, sym = [], [], [], OrderedDict()

    def add_tex(name, w, h, px):
        texs.append((name, int(math.log2(w)), int(math.log2(h)), px))
        tid = tex_base + len(texs) - 1
        if tid > 255:
            raise ValueError('texture ids ran past 255: a face texid is one byte')
        return tid

    for kind, (key, path, step, slots, hits) in MONSTERS.items():
        if kind not in kinds:
            continue
        m = Mdl(pak_read(pak_f, pak_ents, path), path)
        if m.nverts > MAX_VERTS:
            raise ValueError('%s: %d vertices' % (path, m.nverts))
        ids, tw, th = model_skin(m, key, add_tex)
        fb = mdl_faces(m, ids[0], tw, th)
        groups = m.groups()
        rec = bytearray(BANK_REC)
        done = {}
        for si, slot in enumerate(BANK_SLOTS):
            a = slots.get(slot)
            if a is None:
                continue
            part = None
            if isinstance(a, tuple):
                a, part = a
            if a not in groups:
                raise ValueError('%s has no "%s" frames (%s)'
                                 % (path, a, ', '.join(groups)))
            if a not in done:
                done[a] = (mesh_base + len(meshes), len(groups[a][::step]))
                for fi in groups[a][::step]:
                    meshes.append((mdl_verts(m.frames[fi][1]), fb))
            first, n = done[a]
            if part is not None:
                n = len(groups[a][:part][::step])
            if first > 255:
                raise ValueError('%s: monster frames ran past mesh index 255'
                                 % path)
            rec[si * 2] = first
            rec[si * 2 + 1] = n
            sym['LB_M_%s_%s' % (key.upper(), slot)] = first
            sym['LB_N_%s_%s' % (key.upper(), slot)] = n
        for k, slot in ((16, 'MELEE'), (17, 'SHOOT')):
            if slot in hits:
                rec[k] = hits[slot] // step
                assert rec[k] < rec[BANK_SLOTS.index(slot) * 2 + 1]
        rec[18] = step
        banks.append((kind, bytes(rec)))
        stats['bank_monsters'] += 1
        stats['bank_frames'] += sum(n for _, n in done.values())
    return texs, meshes, banks, sym


def catalogue_hash(sym):
    """16 bits the game compares against its compiled-in include, so a level
    file from an older converter run is refused instead of drawing a gun
    with a dog's frames."""
    s = ';'.join('%s=%d' % kv for kv in sym.items()).encode()
    return zlib.crc32(s) & 0xffff


def write_inc(path, sym, cat_ent, cat_hash, bank_sym=None, map_name=''):
    with open(path, 'w') as f:
        f.write('; Generated by tools/gen_quakelevel.py -- do not edit.\n')
        f.write(';\n; Milestone 20 actor catalogue. M_* are level mesh INDICES\n'
                '; (add MESH_BASE for a resource id), N_* frame counts,\n'
                '; T_* texture resource ids. CAT_ENT is the entity index of the\n'
                '; gpu64_catalog record; its spawnflags must equal CAT_HASH.\n\n')
        f.write('CAT_ENT = %d\nCAT_HASH = $%04x\n\n' % (cat_ent, cat_hash))
        for k, v in sym.items():
            f.write('%s = %d\n' % (k, v))
        if bank_sym:
            # Not hashed and not for the game, which reads every level's
            # own bank: the PC checkers' view of this one level's monsters.
            f.write('\n; %s\'s monster bank, for the PC checkers only.\n'
                    % map_name.upper())
            for k, v in bank_sym.items():
                f.write('%s = %d\n' % (k, v))

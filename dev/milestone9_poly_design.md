# gpu64 milestone 9 design — the polygon layer (class 2, `DRAW_POLYS`)

Rationale and as-built decisions. What a developer needs in order to *use*
these opcodes is in [api_design.md](api_design.md); this document does not
restate it.

## Why, in one sentence

Class 2 as milestone 8 left it renders a Doom level; a Quake level is not a
Doom level with more polygons in it, and the difference is exactly three
things: **surfaces that are not vertical or horizontal**, **a camera that can
look up and down for real**, and **geometry stacked over other geometry**.

`DRAW_SECTORS` cannot express any of the three, and no amount of adding
fields to a wall record will make it. A wall record *is* a vertical segment;
a sector *is* a pair of horizontal planes. Both facts are load-bearing in the
projection: a wall column is one `u` for the whole column, a flat row is one
`z` for the whole row, and that is what makes them cheap.

So milestone 9 adds the primitive that has none of those assumptions in it:
**a convex polygon in world space, arbitrarily oriented, perspective-textured
and depth-tested per pixel.** It is Quake's `msurface_t`, and one of them can
be a floor, a ceiling, a wall, a ramp, a slanted skylight or the underside of
a bridge over the room you are standing in.

## What stays

`DRAW_WALLS` and `DRAW_SECTORS` are not migrated onto it and not deprecated.
They are cheaper per pixel, they are proven on hardware, and a game that wants
a Doom-shaped level should keep using them. The precedent is milestone 8c,
which kept `DRAW_WALLS` when `DRAW_SECTORS` superseded it: *a regression
traded for tidiness is a bad trade with bench time this scarce.*

What is shared is the thing that has to be shared — **the per-pixel depth
buffer**. `DRAW_POLYS`, `DRAW_SECTORS` and `DRAW_THINGS` all test and write
the same 320x200 `u16` z buffer, so a sprite is occluded by a polygon exactly
as it is by a sector wall, and a level may legitimately be half one and half
the other.

## The four new opcodes and why the work is split that way

| Op | Name | Sent |
|---|---|---|
| $07 | `SET_CAMERA3D` | once per frame, 12 register writes |
| $12 | `UPLOAD_VERTS` | once per level |
| $13 | `UPLOAD_TEXINFO` | once per level |
| $26 | `DRAW_POLYS` | once per frame, one batch |

The split follows the rule the whole class is built on: **what changes every
frame crosses the bus every frame, and nothing else does.** A Quake level's
vertices and texture axes are constants; a face record names them by index.
A face record is therefore 16 bytes with 8 of them spare, and 300 visible
faces are 4800 bytes a frame — the size `DRAW_SECTORS` already carries on
hardware.

**The vertex pool is the reason a face record fits in 16 bytes at all.** A
triangle carries three positions; at 8.8 x/y/z that is 18 bytes for the
geometry alone before anything says what texture it wears. Indexing a pool
also matches how a level is actually built: adjacent faces share vertices, so
the pool is smaller than the faces that reference it, and a vertex welded
once cannot crack.

**Texinfo is a second table for the same reason and one more.** Quake's
texture axes are two 3-vectors plus two offsets — 16 bytes, the whole of a
face record — and they are shared by every face on a wall, which is why
Quake has the indirection too. It also gives texture alignment for free: two
faces that name one texinfo are guaranteed to align, whatever their shapes.

**`SET_CAMERA3D` is register-inline and not a blob**, unlike the two tables.
It is per-frame, and twelve `STA`s beat a descriptor plus a DMA fetch. It is
a separate opcode from `SET_CAMERA` rather than an extension of it because
`SET_CAMERA`'s fifteen argument bytes are full, and because the two cameras
mean different things: `SET_CAMERA`'s `horizon` is a y-shear, which is what a
sector renderer can afford, and `SET_CAMERA3D`'s `pitch` is a rotation.

## Conventions this pins down

The axis, winding and fixed-point conventions this milestone settled are
developer-facing API material now, not rationale — see
[docs/class2-raster-reference.md](../docs/class2-raster-reference.md#conventions).

## The rasteriser, and what it deliberately does not do

Per face: transform to view space, clip against the near plane, project,
cull, scan-convert. Per pixel: one depth compare, and if it passes, two
divides for the perspective-correct texel and one for the depth written.

**Three divides per pixel is Quake's own arithmetic without Quake's
optimisation.** Quake interpolated `1/z`, `s/z` and `t/z` linearly and
divided once every sixteen pixels, then linearly interpolated the texture
coordinates between those exact points. That is a 16x saving on the divides
and it is a *different picture* — an approximation, tuned to what a Pentium's
FPU could overlap with its integer unit. This layer divides per pixel because
a 1.2 GHz ARM can, and because an exact answer is the one a Python model can
be diffed against pixel for pixel. If a frame turns out too slow on hardware,
the subdivision is the optimisation to reach for and it is a local change to
one loop.

**Near clipping is Sutherland–Hodgman in view space**, on `z >= 0.25`, the
same near plane the wall projection uses. It runs on the world-space
attributes `s` and `t` as well as the position, which is exact: `s` and `t`
are affine functions of world position, so they interpolate linearly along an
edge in view space.

**Scan conversion re-derives each row's span from the edge list** rather than
walking active edges incrementally. It is O(vertices) per row instead of
O(1), which for a 16-vertex face is a rounding error against the per-pixel
work, and it removes the whole class of bugs that live in an active-edge
table. Rows and columns are sampled at their top-left corner and the rule is
half-open — a pixel belongs to the polygon whose interior contains its sample
point — so two faces sharing an edge neither double-draw it nor leave a seam.

**Both texture dimensions must be powers of two**, as `DRAW_SPANS` already
requires and for the same reason: `u` and `v` both walk arbitrarily, so both
are masked per pixel. `UPLOAD_TEXTURE` only enforces it on `h`, so a face
naming a texture with a non-power-of-two width rejects the record — it is not
an upload error, because the same texture is legal for `DRAW_COLUMNS`.

## The depth buffer's lifecycle, restated

`DRAW_POLYS` **does not clear the depth buffer.** `DRAW_SECTORS` does, at the
start of every batch, because a Doom frame is exactly one sector batch. A
Quake frame is not: it may be several polygon batches (the world, then the
detail, then a moving platform), and a batch that cleared would erase the one
before it.

`FILL_VIEW` is what clears it — milestone 8d already made that true and
already documented it as "the op a frame starts with". A frame that skips
`FILL_VIEW` because it draws a sky over every pixel must send
`FILL_VIEW` anyway, or the sky pass sees last frame's depth.

## Limits, and where they came from

The limit values and their rationale are now in
[docs/class2-raster-reference.md](../docs/class2-raster-reference.md#limits),
alongside the rest of class 2's limits table.

## Not in this milestone

- **`DRAW_THINGS` still projects through `SET_CAMERA`,** not the 3D camera.
  A Quake game therefore has no monsters yet. It is the next step and it is
  small: the thing record's fields already mean what they need to mean, and
  the projection is the one this layer just wrote.
- **Lightmaps.** Lighting is per face plus distance, as everywhere else in
  class 2. Quake's per-surface lightmap needs a second texture unit and a
  second set of coordinates and is a milestone of its own.
- **Sky, water and any translucency.** A sky is a texture on a face today.
- **Visibility.** The C64 sends the faces it wants drawn; nothing here builds
  or walks a BSP tree, and per-pixel depth is what makes that safe.

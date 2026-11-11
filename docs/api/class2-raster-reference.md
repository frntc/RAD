# Class 2 — the raster layer

Set `CMD_HI = 2`. This is the column/span/sprite/polygon layer a
first-person renderer needs: instead of one command per shape, you fill an
array of 16-byte (32-byte for sector-walls) records in your own RAM and hand
gpu64 the whole array in a single dispatch. A frame of several hundred
primitives costs one command and one bulk fetch, not several thousand
register writes.

See also: [dev/milestone8_raster_design.md](../../dev/milestone8_raster_design.md)
for why this is a class of its own and what it is aimed at, and
[dev/milestone9_poly_design.md](../../dev/milestone9_poly_design.md) for
the rationale behind the polygon layer and its rasteriser.

Every class 2 primitive is clipped to the **view rectangle**, which starts
as the whole surface. Records draw into the current draw page, so class 0's
`SET_DRAW_PAGE` and `PAGE_FLIP` work exactly as they do for 2D drawing, and
class 0 and class 2 can draw into the same page in the same frame.

## Conventions

Two coexisting geometry models share this class: `DRAW_SECTORS`/`DRAW_WALLS`
work in a 2.5D, Doom-shaped world (a ground plane plus separate floor/ceiling
heights); `DRAW_POLYS` works in a full 3D, Quake-shaped world (arbitrary
planes). Both share one depth buffer and one set of world-space axis
conventions:

- **World is right-handed-ish and Doom-shaped**: `x`,`y` are the ground
  plane, and **`z` is up**, measured the same way the sector table's
  absolute floor heights are. A thing standing on a sector floor at 0 and a
  polygon vertex at z = 0 are at the same height, so the two layers compose.
- **Yaw 0 looks along +x**, 256 to the circle — `SET_CAMERA`'s `angle` and
  `SET_CAMERA3D`'s `yaw` share this convention.
- **Pitch is positive looking up.** For `SET_CAMERA3D` it is a real rotation
  about the view's right axis, applied after yaw, so the horizon bends the
  way it should when you look at a floor edge-on rather than sliding as a
  shear does (`SET_CAMERA`'s `horizon` is a y-shear, not a rotation, which is
  the cheaper thing a sector renderer can afford).
- **Front-facing is clockwise on screen** after projection, y downwards.
  That is the same convention `DRAW_WALLS` states as "drawn only from the
  side that projects it left to right": extrude such a wall upward and wind
  the quad top-left, top-right, bottom-right, bottom-left and it is
  clockwise. A level ported from sector walls to polygons keeps its winding.
- Everything is 8.8 signed, so the world is **±127.99 units** across with a
  resolution of 1/256. Scale a level to fit it.

## Limits

| Limit | Value | Why |
|---|---|---|
| sectors | 128 | `SET_SECTORS` table |
| vertices in the polygon pool | 4096 | 32 KB of static store; a Quake start map is ~1200 |
| texinfos | 255 | one byte in a polygon record |
| vertices per polygon face | 16 | Quake's own limit before it subdivides; the near clip can add one, so the working buffer is 20 |
| faces (records) per batch | 4096, or 65536 bytes total whichever is smaller | the staging buffer, 16 bytes a record (32 for sector-walls) |
| resident world slots (`UPLOAD_POLYS`) | 2048 | see [level-scale-visibility.md](level-scale-visibility.md) |
| textures | 255 live ids | bump-allocated arena, see below |
| dynamic lights | 8 | `SET_LIGHT` slots |
| colormap levels | 64 | `SET_COLORMAP` |

## System — $00–$0F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $00 | `RASTER_RESET` | 0 | — | Frees every texture, view back to the whole surface, colormap back to identity, counters cleared. Also happens on a session reset, so a new program never inherits another one's texture ids. |
| $01 | `SET_VIEW` | 8 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` w, `ARG6-7` h | The clip rectangle every class 2 primitive obeys. `w` or `h` of 0, or a rectangle running off the surface, is `BAD_ARGS`. |
| $02 | `SET_COLORMAP` | 7 | `ARG0-5` blob descriptor, `ARG6` levels | Loads a `levels × 256` lighting table: a pixel of palette index `c` at light level `L` is drawn as `map[L * 256 + c]`. `levels = 0` removes it and returns to drawing raw indices. Max 64 levels; `len` must equal `levels * 256`. |
| $03 | `RASTER_STATS` | 6 | `ARG0-5` destination descriptor | Writes the 16-byte stats block (below) — what became of the **last** batch. `len` must be ≥ 16. |
| $04 | `FILL_VIEW` | 1 | `ARG0` palette index | Fills the view rectangle with one raw index. No colormap, no clipping beyond the view. **Also empties the class 2 depth buffer over the same rectangle** — a pixel just painted background has nothing in it, so this is the op a frame starts with. |
| $05 | `SET_CAMERA` | 15 | `ARG0-1` x, `ARG2-3` y, `ARG4` angle, `ARG5` flags, `ARG6-7` eye height, `ARG8-9` ceiling height, `ARG10-11` projection, `ARG12` floor colour, `ARG13` ceiling colour, `ARG14` horizon | The camera `DRAW_WALLS` projects through. Position and heights are signed 8.8 world units; angle is binary, 256 to the circle, 0 looking along +x. Projection is the distance to the projection plane in pixels as 8.8 — 160.0 (`$A000`) across a 320-wide view is a 90° field of view. Horizon is a signed pixel offset from the view's centre row. Flags bit 0 (`CAM_PAINT`) has each wall column also fill its own ceiling above and floor below in the two colours. A projection of 0, an eye height at or below 0, or a ceiling at or below the eye is `BAD_ARGS`. For `DRAW_SECTORS` the eye height is an **absolute** world height rather than a height above the floor, the ceiling height is not read at all (send the level's tallest, so the validation passes), and the two flat colours are not read either — they come from the sector. |
| $06 | `SET_SECTORS` | 8 | `ARG0-5` blob descriptor, `ARG6-7` count | Loads the sector table `DRAW_SECTORS` reads heights and flat colours from — see the sector record below. `count = 0` drops the table. Max 128 sectors; `len` must equal `count * 8`. A sector whose ceiling is at or below its own floor rejects the **whole** upload with `BAD_ARGS` and leaves the table that was already there untouched. |
| $07 | `SET_CAMERA3D` | 11 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` z, `ARG6` yaw, `ARG7` pitch, `ARG8-9` projection, `ARG10` flags | The camera `DRAW_POLYS` projects through — a free camera in three dimensions, with a real pitch rather than `SET_CAMERA`'s horizon shear. Position is signed 8.8 world units, `z` upwards. Yaw is binary, 256 to the circle, 0 looking along +x, and turns the same way `SET_CAMERA`'s angle does. Pitch is **signed** binary, $00 level, positive looking **up**; $40 is straight up and $C0 straight down, though a Quake game normally clamps well inside that. Projection is as `SET_CAMERA`: distance to the projection plane in pixels, 8.8, so 160.0 (`$A000`) across a 320-wide view is 90°. A projection of 0 is `BAD_ARGS`; there is no other validation, because there is no floor to be under. `ARG10` is reserved, write 0. This camera is entirely separate from `SET_CAMERA`'s: both can be live at once and a frame may use both. |
| $08 | `SET_LIGHT` | 10 | `ARG0` slot, `ARG1-2` x, `ARG3-4` y, `ARG5-6` z, `ARG7-8` radius, `ARG9` strength | Sets one of **8** dynamic point lights, or clears it. Position is signed 8.8 world units on the same axes as `SET_CAMERA3D`; radius is **unsigned** 8.8 and is how far the light reaches; strength is how many colormap levels it lifts a pixel at its own centre. A slot of 8 or more is `BAD_ARGS`. A radius of 0 or a strength of 0 clears the slot, and a slot is cleared by `RASTER_RESET`. Arguments are **inline** — no blob, so a light can be moved or raised on the frame it is wanted. Lights apply to `DRAW_POLYS` and `DRAW_THINGS`; see [Dynamic lights](#dynamic-lights) below. |

## Resources — $10–$1F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $10 | `UPLOAD_TEXTURE` | 11 | `ARG0-5` blob descriptor, `ARG6-7` w, `ARG8-9` h, `ARG10` flags; **`ID`** = texture id | Copies `w * h` bytes into gpu64's texture arena under id `1..255`. **`h` must be a power of two** (it is masked per pixel); `w` need not be, unless the texture is used by `DRAW_SPANS` or `DRAW_POLYS`, which mask both. Either dimension 0 or over 1024 is `BAD_ARGS`, as is a `w * h` over 65536; `len` must equal `w * h`. `ID = 0` is `BAD_ID`, and so is an id past 255. Re-uploading a live id replaces it. Arena exhausted is `OUT_OF_MEMORY`. |
| $11 | `FREE_TEXTURE` | 0 | **`ID`** = texture id | Releases the id. An id that is not live is `BAD_ID`. |
| $12 | `UPLOAD_VERTS` | 8 | `ARG0-5` blob descriptor, `ARG6-7` count | Loads the vertex pool `DRAW_POLYS` indexes into — see the vertex record below. `count = 0` drops the pool. Max 4096 vertices; `len` must equal `count * 8`. |
| $13 | `UPLOAD_TEXINFO` | 8 | `ARG0-5` blob descriptor, `ARG6-7` count | Loads the texture-axis table a textured polygon names — see the texinfo record below. `count = 0` drops the table. Max 255 entries; `len` must equal `count * 16`. |

The texture arena is a bump allocator: freeing an id, or re-uploading over a
live one, releases the **id** but not the bytes. Load a level's textures
once rather than swapping them per frame, and use `RASTER_RESET` — or a
session reset — to get the space back. If a level needs more distinct
textures than the arena has room for at once, group them so each batch of
levels/rooms that share screen time also shares an arena load, and budget a
`RASTER_RESET` (or per-texture `FREE_TEXTURE`) at the transition — there is
no automatic eviction under `OUT_OF_MEMORY`, so an upload that doesn't fit
simply fails and the old set stays live until you free something yourself.

Textures are stored **column-major**: `texel(u, v)` is byte `u * h + v`. That
is the order a wall column reads them in, which is what makes a column one
sequential walk instead of `h` scattered reads. `ARG10` bit 0 set says your
source is row-major and asks gpu64 to transpose it on the way in — pay it
once at upload rather than per pixel.

## Batches — $20–$2F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $20 | `DRAW_COLUMNS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` column records. |
| $21 | `DRAW_SPANS` | 9 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags | Draws `count` span records. |
| $22 | `DRAW_SPRITE` | 15 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` w, `ARG6-7` h, `ARG8` light, `ARG9` key, `ARG10-11` clipY0, `ARG12-13` clipY1, `ARG14` flags; **`ID`** = texture id | Scales one texture into the screen rectangle `x,y,w,h`, clipped to the view **and** to the inclusive row range `clipY0..clipY1`. Flags: bit0 mask on `key`, bit1 flip horizontally. `w` or `h` of 0 draws nothing (and counts as one rejected primitive); an unknown id is `BAD_ID`. |
| $23 | `DRAW_WALLS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` wall records — see below. |
| $24 | `DRAW_SECTORS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` **32-byte** sector-wall records — see below. `BAD_ARGS` if `SET_SECTORS` has not been sent. |
| $25 | `DRAW_THINGS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` 16-byte thing records — billboards in world space, depth-tested against the buffer `DRAW_SECTORS` or `DRAW_POLYS` filled. `ARG8` bit 1 (`BATCH_CAM3D`) projects the batch through `SET_CAMERA3D` instead of `SET_CAMERA`; with that bit set it is `BAD_ARGS` if `SET_CAMERA3D` has not been sent. See below. |
| $26 | `DRAW_POLYS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` 16-byte polygon records — arbitrary convex polygons in world space, through `SET_CAMERA3D`, depth-tested per pixel. See below. `BAD_ARGS` if `SET_CAMERA3D` or `UPLOAD_VERTS` has not been sent. |
| $27 | `DRAW_WORLD` | 4 | `ARG0-1` first, `ARG2-3` count, `ARG9` key | Draws polygons `first .. first+count-1` of the resident world uploaded by `UPLOAD_POLYS`. Pixel for pixel this is `DRAW_POLYS` over the same records — same clipping, texturing, lighting and depth test — but **no blob is transferred**: a frame is ten register writes however big the level is. `BAD_ARGS` if the range runs past what has been uploaded, or if `SET_CAMERA3D` or `UPLOAD_VERTS` has not been sent. `count = 0` is a no-op, not an error, so a room with nothing visible costs a command and nothing else. See [level-scale-visibility.md](level-scale-visibility.md). |

`count = 0` is a no-op, not an error. Otherwise `len` must be exactly
`count * 16` — `count * 32` for `DRAW_SECTORS` — plus 2 more if the checksum
flag is set, and no more than 65536 bytes in total — anything else is
`BAD_ARGS`.

**`ARG8` bit 0 — the batch checksum.** With it set, the two bytes following
the records are a plain 16-bit little-endian sum of the record bytes. gpu64
recomputes it and answers `BAD_ARGS`, drawing nothing, if it disagrees. A
6502 cannot afford to sum thousands of bytes every frame, so use it where it
is free: on a batch that is built once and re-sent unchanged, the sum is
computed once too. For a batch rebuilt every frame, the `rejected` counter
in the stats block is the cheaper watch — see
[getting-started.md](../guide/getting-started.md#what-a-command-costs-the-c64).

**`ARG8` bit 1 — the 3D camera (`DRAW_THINGS` only).** With it set, the
batch projects through `SET_CAMERA3D` rather than `SET_CAMERA`, which is
what puts monsters into a level drawn with `DRAW_POLYS`. It is a property of
the batch and not of a record because a frame's things all belong to one
world: a game that has moved to the 3D camera has moved every one of them. A
frame may still send both kinds — one batch flagged and one not — and both
land in the same depth buffer. Every other opcode ignores this bit.

### Wall record — 16 bytes

A wall segment in **world** coordinates. gpu64 does the projection, the
perspective texture mapping, the distance lighting and the depth sorting, so
the C64 sends the same unchanged bytes every frame and spends its own time
on the game. This is a geometry opcode rather than a pixels opcode, and it is
where the per-column divide a 1 MHz 6502 cannot afford went.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x1 | first endpoint, signed 8.8 world units |
| 2-3 | y1 | |
| 4-5 | x2 | second endpoint |
| 6-7 | y2 | |
| 8 | texid | texture id, or 0 for a solid colour taken from the low byte of `u1` |
| 9 | light | base light level, before distance is added |
| 10-11 | u1 | texture u at the first endpoint, signed 8.8 texels |
| 12-13 | u2 | texture u at the second endpoint |
| 14 | flags | bit0 mask on `key`; bit1 use `light` unchanged, with no darkening by distance |
| 15 | — | reserved, write 0 |

**Winding decides which side is the front.** A wall is drawn only from the
side that projects it left to right; seen from the other side it is one
rejected record and no pixels. Walls are one-sided, exactly as Doom's are,
and this is what makes a closed room cost nothing to look at from outside.

**u1 and u2 rather than a length and a scale**, because a length needs a
square root. Give the texture coordinate at each end and the interpolation
is perspective-correct with no root anywhere. Since `u` is signed 8.8 texels
it reaches 127.99, so tile a long wall as several segments — which is what a
cell-based level gives you for free, one record per cell face.

**Depth is sorted for you.** Every column is tested against a 1/z buffer that
`DRAW_WALLS` clears at the start of each batch, so records may arrive in any
order. Doom needed a BSP tree to avoid that sort; here it is a comparison.

**What is not here:** walls run floor to ceiling at one height for the
whole level, the height `SET_CAMERA` gives. Varying floor and ceiling
heights, two-sided walls and windows are `DRAW_SECTORS`, below; this opcode
is kept as it is because it is simpler to drive and its per-column depth
buffer is cheaper.

Distance darkening adds `z / 2` light levels to the record's `light`, `z`
being the column's distance in world units, unless bit 1 says otherwise. The
result is clamped to the colormap's last level, so a level that runs out of
levels goes as dark as it can rather than failing.

Rejections a wall record can earn, none of which fail the batch: both
endpoints behind the near plane (0.25 world units), an unknown `texid`, or a
back face. A camera that cannot be projected rejects every record in the
batch.

### Sector record — 8 bytes

The vertical shape of a level. `DRAW_SECTORS`' wall records name two of
these by one-byte id instead of carrying four heights each, which is what
keeps the wall record down to 32 bytes.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | floorH | floor height, signed 8.8 **absolute** world units |
| 2-3 | ceilH | ceiling height; must be above the floor |
| 4 | floorCol | palette index the floor is painted in |
| 5 | ceilCol | palette index the ceiling is painted in |
| 6 | light | base light level for this sector's floor and ceiling |
| 7 | flags | bit0 (`SEC_SKY`) — the ceiling is not a surface |

**`SEC_SKY`** means the ceiling is neither painted nor given depth, and a
wall whose front sector has it draws no upper band. So a courtyard's wall
stops at the sky instead of growing a lintel across it, and whatever the
page already held shows through — put a `FILL_VIEW`, or a sky texture, there.

### Sector-wall record — 32 bytes

`DRAW_SECTORS`' record. Everything the wall record does, plus the two things
a level with steps in it needs: floor and ceiling heights that come from the
sector table, and a **two-sided** wall that draws a band above the far
ceiling and a band below the far floor and leaves the middle see-through.

Thirty-two bytes rather than sixteen because a two-sided wall names two
sectors and three textures and none of that fits beside the geometry. It
costs nothing per frame: a level is uploaded once and its checksum computed
once.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x1 | first endpoint, signed 8.8 world units |
| 2-3 | y1 | |
| 4-5 | x2 | second endpoint |
| 6-7 | y2 | |
| 8-9 | u1 | texture u at the first endpoint, signed 8.8 texels |
| 10-11 | u2 | texture u at the second endpoint |
| 12 | frontSec | sector id on the side this record is seen from |
| 13 | backSec | sector id on the far side, or `$FF` for a solid wall |
| 14 | light | base light level for all three bands |
| 15 | flags | bit0 mask on `key`; bit1 use `light` unchanged; bit2 (`WALL_NOFLATS`) this wall's columns paint no floor and no ceiling whatever the camera says |
| 16 | texMid | texture for a solid wall, floor to ceiling; 0 for a flat colour |
| 17 | texUpper | texture for the band from this ceiling down to the far one |
| 18 | texLower | texture for the band from the far floor down to this one |
| 19 | colMid | palette index used when `texMid` is 0 |
| 20 | colUpper | likewise for `texUpper` |
| 21 | colLower | likewise for `texLower` |
| 22-31 | — | reserved, write 0 |

Winding, `u1`/`u2`, distance darkening, the near plane and the rejection
rules are all exactly as for the wall record above; a bad sector id rejects
a record the same way a bad texture id does. `v` walks each band's texture
over that band's own full height on screen, so an upper band carries the
whole texture squeezed into it.

**A two-sided wall is still one-sided per record.** The level carries the
other side as a second record with the endpoints swapped and the sectors
exchanged — which is also how the far side's bands get their own textures
without this record knowing anything about them.

**Depth is per PIXEL here, not per column.** It has to be: a window means a
column is no longer owned by one wall — the near wall owns the band above
the window and the band below it, and something further away owns the
middle. The buffer is cleared at the start of each batch, so records may
still arrive in any order, and Doom's BSP tree, which exists to produce that
order, is still not needed.

**Floors and ceilings are drawn per column, by the walls.** With
`CAM_PAINT` set, each wall column fills from the view's top down to where
its front sector's ceiling cuts the column, and from where its floor cuts it
down to the view's bottom, in that sector's colours. Every such row has its
own distance, so a floor depth-tests correctly against a wall standing on
it, and a two-sided wall also paints the far sector's flats inside its
window — nothing else would, because a corridor's own side walls seen
end-on cover almost no columns.

One correction is applied to that: **the depth a flat writes is never nearer
than the wall whose column painted it.** A plane is infinite and a sector's
floor is not, so without the clamp a low ceiling two rooms away wins the
rows above the wall that hides it. It is the cheap stand-in for Doom's
visplane clipping, which needs a front-to-back order this design does not
have. Lighting still uses the row's true distance, so the clamp is
invisible. One consequence: it only reaches one sector through a portal — a
sector two portals away can leave a hole.

**What is not here:** flat (floor and ceiling) textures, sloped floors, and
flats seen through two portals in a row. Sprites that depth-test against
this buffer are `DRAW_THINGS`, below.

### Thing record — 16 bytes

A billboard at a **world** position, projected by the batch's camera —
`SET_CAMERA` by default, or `SET_CAMERA3D` if `ARG8` bit 1 is set — and
depth-tested per pixel against the buffer `DRAW_SECTORS` or `DRAW_POLYS`
filled, which is what lets a monster stand behind a wall.

`DRAW_SPRITE` cannot do this. It takes a screen rectangle, so the C64 has to
do the projection, and it has no argument room left for a depth. Keep
`DRAW_SPRITE` for what is genuinely at a screen position and must never be
occluded: the weapon in the player's hands, the status bar, a full-screen
flash.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x | world position, signed 8.8 |
| 2-3 | y | |
| 4-5 | base | **absolute** height of the bottom of the sprite, signed 8.8 — the same measure the sector table's floor heights use, so a thing standing on the floor of sector *n* carries that sector's floor height |
| 6-7 | h | height in world units, unsigned 8.8 |
| 8-9 | w | width in world units, unsigned 8.8 |
| 10 | texture id | must name a live texture; 0 is a rejected record. With `THING_DIRECTIONAL`, the first of **eight** consecutive ids |
| 11 | light | base light level, darkened by distance unless `THING_FLATLIT` |
| 12 | flags | bit0 mask on the batch `key`; bit1 flip horizontally; bit2 (`THING_NODEPTH`) ignore the depth buffer entirely; bit3 (`THING_FLATLIT`) use `light` unchanged; bit4 (`THING_DIRECTIONAL`) pick one of eight views from byte 13 |
| 13 | facing | which way the thing itself is pointing, in the camera's 256-to-the-circle units. Read only with `THING_DIRECTIONAL`; write 0 otherwise |
| 14-15 | — | reserved, write 0 |

**`THING_DIRECTIONAL` — eight views.** A monster that looks the same from
behind as it does from the front is the one thing that gives a billboard
away. Set bit 4 and byte 10 becomes the first of eight consecutive texture
ids: the thing seen from the front, then every 45° round it the way the
angle units run. Byte 13 says which way the thing is facing, in the same
units as `SET_CAMERA`'s `angle` and `SET_CAMERA3D`'s `yaw`, and gpu64 works
out from where the camera is standing which of the eight you are looking at.
A thing facing straight at the camera draws id + 0; one walking away draws
id + 4.

The 6502 sends the facing it already keeps for the monster's AI and nothing
else — no arctangent, no view index, no per-frame table. The camera used is
the batch's: under `BATCH_CAM3D` it is `SET_CAMERA3D`'s position that
decides the view. **All eight ids must be live**; a view whose texture is
missing is a rejected record, so upload the whole set together. Turning the
camera on the spot does not change the view — only moving does, which is
correct, and is what makes a strafing player see a monster's flank swing
round.

The card always faces the camera, so `w` is measured straight across the
view and there is no rotation to send. A thing is `w` wide and `h` tall in
world units at any distance; the screen rectangle is whatever the projection
makes of that, centred on the column the world position projects to.

Under the 3D camera the card stays **upright on the screen** — it does not
tilt with pitch, which is what a billboard is for — but its two ends are
projected separately, so looking up at a tall thing standing close makes it
taller on screen and pushes it down the view, as the walls beside it do.
`SET_CAMERA`'s horizon offset is not applied to a `BATCH_CAM3D` batch:
`SET_CAMERA3D`'s pitch is the whole of the vertical aim. Nothing else in the
record changes meaning — `base` is still the absolute world height of the
thing's feet, now measured on `SET_CAMERA3D`'s `z` axis.

**Depth is written as well as tested**, at drawn pixels only — so a masked
texel is a hole in the depth too, and a batch of things is order-independent
exactly as a batch of walls is. `THING_NODEPTH` opts out of both, which is
how a muzzle flash gets painted over the world; things drawn that way *do*
depend on the order they arrive in.

**Send this batch after the level's geometry for the same frame** —
`DRAW_SECTORS`, or `DRAW_POLYS`/`DRAW_WORLD` for a `BATCH_CAM3D` batch. The
depth buffer is shared and persists: it is emptied by `FILL_VIEW`, by
`DRAW_SECTORS` (over the view, at the start of each batch) and by
`RASTER_RESET`, and by nothing else. Things sent before the level's walls
would be occluded by whatever the buffer still held.

A record with `w` or `h` of 0, an unknown or zero texture id, or a position
behind the near plane is **rejected**; one that is well formed and clips
away entirely, or is too far off to cover a pixel, is **accepted** and draws
nothing. That distinction is the whole value of the counters: rejected means
the record was wrong, not that it missed.

### Vertex record — 8 bytes

The pool `UPLOAD_VERTS` loads and polygon records index into. A vertex is
shared by every face that touches it, so a level is uploaded once and the
per-frame batch is nothing but indices.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x | world position, signed 8.8 |
| 2-3 | y | |
| 4-5 | z | upwards |
| 6-7 | — | reserved, write 0 |

The two pad bytes are there so a vertex is 8 bytes and the 6502 reaches
index *n* with three shifts instead of a multiply.

Coordinates are 8.8, so the world is ±128 units across and no vertex may be
more than 127.99 units from the origin — the same far plane the rest of the
3D pipeline works in. Pick a scale where a room is a few units wide rather
than a few hundred.

### Texinfo record — 16 bytes

Where a texture's texels land in the world, as a pair of axes — the same
idea Quake's `texinfo` lump carries. A face names one entry and gpu64
derives its own texture coordinates from the vertex positions, so a wall and
the floor it meets stay aligned however the face is split.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | sx | s axis, signed 8.8 |
| 2-3 | sy | |
| 4-5 | sz | |
| 6-7 | sOff | s offset in texels, signed 8.8 |
| 8-9 | tx | t axis, signed 8.8 |
| 10-11 | ty | |
| 12-13 | tz | |
| 14-15 | tOff | t offset in texels, signed 8.8 |

For a vertex at world position `P`:

    s = ((P · sAxis) >> 8) + sOff
    t = ((P · tAxis) >> 8) + tOff

both in 8.8 texels, and both wrap by masking, so an axis of length 1.0
(`$0100`) gives one texel per world unit and `$0400` gives four. The axes
need not be perpendicular and need not lie in the face's plane — only the
values at the vertices matter, and everything between them is interpolated
in perspective.

Entries are numbered **from 1**: a polygon record's `texinfo` byte of 1 is
the first record you uploaded. 0 means "none", which is only legal on an
untextured face.

### Polygon record — 16 bytes

A convex polygon in world space, drawn through `SET_CAMERA3D` and
depth-tested per pixel. This is the Quake-shaped layer: arbitrary planes at
arbitrary angles, rather than `DRAW_SECTORS`' vertical walls and horizontal
flats.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | first | index of the polygon's first vertex in the pool |
| 2 | nVerts | 3 to 16, taken consecutively from `first` |
| 3 | texinfo | 1-based texinfo index; ignored when `tex` is 0 |
| 4 | tex | texture id, or **0 for a flat-shaded face** drawn in `col` |
| 5 | col | palette index used when `tex` is 0 |
| 6 | light | base light level, darkened by distance unless `POLY_FLATLIT` |
| 7 | flags | bit0 (`POLY_MASKED`) skip texels equal to the batch `key`; bit1 (`POLY_FLATLIT`) use `light` unchanged; bit2 (`POLY_TWOSIDED`) no backface cull |
| 8-15 | — | reserved, write 0 |

The vertices must be **convex** and listed so that the face reads
**clockwise on screen** when you are looking at its front — the same
convention `DRAW_WALLS` states as "drawn only from the side that projects it
left to right". A face seen from behind is culled and counts as rejected,
which is what makes a sealed room cost only the polygons facing you; set
`POLY_TWOSIDED` for a face that has no back, such as a grate or a banner.

A textured face needs **both dimensions a power of two** — polys mask `u`
and `v` alike, so a texture that is legal for `DRAW_COLUMNS` may still be
rejected here.

**Depth is written as well as tested**, at drawn pixels only, into the same
buffer `DRAW_SECTORS` and `DRAW_THINGS` use. That means a batch of polygons
is order-independent, a masked texel is a hole in the depth too, and a Quake
level and a Doom level can be drawn into one frame if you want them to be.

**`DRAW_POLYS` does not clear the depth buffer.** `DRAW_SECTORS` clears it
because a Doom frame is one batch; a Quake frame is usually several — the
world, then the moving brushes, then the sprites — so the clear belongs to
`FILL_VIEW`, which is the op that starts the frame. Send `FILL_VIEW` first
or you are depth-testing against last frame. See
[known-gaps.md](known-gaps.md) for the no-clear contract spelled out with an
example.

A record is **rejected** if `nVerts` is outside 3..16, if `first + nVerts`
runs past the uploaded pool, if a non-zero `tex` names a dead texture or one
whose width is not a power of two, if a non-zero `tex` carries a `texinfo`
of 0 or past the uploaded table, if it is culled as backfacing, or if it
lies entirely behind the near plane. It is **accepted** if it is well formed
and merely clips away off the sides of the view, or is edge-on and covers no
pixels. Rejected means the record was wrong; accepted-and-invisible means it
missed.

### Dynamic lights

`SET_LIGHT` gives the class 2 layer eight point lights on top of the
colormap. They do not replace a record's `light` field — they *brighten*
what it and distance darkening already produced:

```
level = litLevel( record light, distance )        as before
for each live light within its radius:
    d2   = squared distance from the pixel to the light, world units
    sub += strength * ( radius² - d2 ) / radius²  truncated per light
level = max( 0, level - sub )
colour = colormap[ level * 256 + texel ]
```

Falloff is linear in the *square* of the distance, which costs no square
root and gives a soft edge: a light is at full strength only at its exact
centre and reaches 0 exactly at its radius, so a light going out of range
never pops. Each light truncates to a whole level **before** it is added, so
two strength-4 lights at one point brighten by 8 levels at the centre but
slightly less than one strength-8 light elsewhere. Lower level is brighter —
lights subtract.

Where they are evaluated differs by opcode, and this is the cost/quality
trade:

- **`DRAW_POLYS`** evaluates every live light **per pixel**, at that pixel's
  true world position recovered from the perspective interpolation. A light
  on a wall is a round pool that follows the geometry, and it works on a
  `POLY_FLATLIT` face too.
- **`DRAW_THINGS`** evaluates them **once per record**, at the billboard's
  centre. A sprite is a flat cut-out, so a gradient across it would be wrong
  anyway; what this gives is a monster that lights up as it walks into a
  torch.
- **The Doom layer — `DRAW_SECTORS`, `DRAW_WALLS`, `DRAW_COLUMNS` and
  `DRAW_SPANS` — ignores dynamic lights entirely.** Those opcodes take their
  light per column or per span by design and their cost model depends on it.
  Use the record's `light` field there.

Eight slots are always scanned when any light is live, and a whole frame
with no light live costs one test. There is no per-light distance culling:
if you have more lights in a level than slots, pick the ones near the camera
yourself and re-send the slots each frame — that is ten register writes per
light and no upload, which is what makes it affordable.

### Column record — 16 bytes

Doom's `R_DrawColumn`: one textured vertical strip.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | x — screen column, unsigned |
| 2 | 2 | y0 — first row, **signed** |
| 4 | 2 | y1 — last row, inclusive, **signed** |
| 6 | 1 | texture id, or 0 for solid colour |
| 7 | 1 | light level |
| 8 | 2 | u — texture column (wrapped modulo `w` once per record); with texid 0, the low byte is the palette index |
| 10 | 2 | v — texture row at `y0`, **8.8 signed** |
| 12 | 2 | dv — v step per screen row, **8.8 signed** |
| 14 | 1 | flags: bit0 = skip source texels equal to the batch `key` (the masked/see-through column) |
| 15 | 1 | reserved, write 0 |

`y0` and `y1` are signed and are *meant* to run off the view: a wall close
enough is thousands of pixels tall. gpu64 clips them **and advances `v`
across the rows it discarded**, so a texture does not slide as you walk into
a wall. A record with `y1 < y0`, an `x` outside the view, or an unknown
texture id is **rejected** — counted, not fatal, and the rest of the batch
still draws.

### Span record — 16 bytes

Doom's `R_DrawSpan`: one horizontal strip, with `u` and `v` both stepping.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | y — screen row, **signed** |
| 2 | 2 | x0 — first column, **signed** |
| 4 | 2 | x1 — last column, inclusive, **signed** |
| 6 | 1 | texture id, or 0 for solid colour |
| 7 | 1 | light level |
| 8 | 2 | u — **8.8 signed** |
| 10 | 2 | v — **8.8 signed** |
| 12 | 2 | du — u step per screen column, **8.8 signed** |
| 14 | 2 | dv — v step per screen column, **8.8 signed** |

Both coordinates are masked every pixel, so a **textured** span needs a
texture whose width *and* height are powers of two; one whose width is not
rejects the record. Solid-colour spans (texid 0) have no such restriction
and are the cheap way to lay down a lit floor and ceiling gradient — one
record per row.

### Stats block

`RASTER_STATS` writes 16 bytes describing the **last** batch dispatched:

| Offset | Size | Contents |
|---|---|---|
| 0 | 2 | `"R2"` |
| 2 | 2 | accepted — records that were drawn or clipped away |
| 4 | 2 | rejected — records that were malformed or named an unknown texture |
| 6 | 2 | requested — the `count` the command asked for |
| 8 | 4 | pixels written |
| 12 | 2 | live textures |
| 14 | 2 | free arena space, KB |

`requested` against `accepted + rejected` is the length readback the bus
asks for, and `rejected` is the counter that moves if a batch arrives
damaged. Reading it back costs one dispatch and a 16-byte write, which is
cheap enough to do every frame.

## Class 2 error codes

Class 2 adds three codes to the [shared error code table](error-codes.md):
`$08 OUT_OF_MEMORY` (the texture arena is full), `$0A BAD_ID` (a texture id
of 0, past 255, or not live), and — from a batch op on a build without the
class compiled in — `$02 BAD_CLASS`.

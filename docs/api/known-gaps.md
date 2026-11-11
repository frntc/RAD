# Known gaps

A read-through of this reference from the point of view of someone actually
shipping a game — Doom-, Quake-, CAD-, or flight-sim-shaped — surfaces a
handful of things worth knowing before you design around gpu64, rather than
discovering them by trial and error on hardware. None of these are bugs;
they're the edges of a small, real 3D pipeline.

## #1: there is no level-scale visibility system yet

If your level is bigger than what fits comfortably in one frame's draw
calls, this is the gap that matters most. `UPLOAD_POLYS`/`DRAW_WORLD` give
you a resident polygon table and a batch-less draw (`DRAW_WORLD` costs ten
register writes regardless of level size) — but there is no PVS, no portal
culling, and no frustum culling done for you. `DRAW_WORLD`'s `first`/`count`
range is drawn exactly as given; picking *which* range is visible from the
current camera position is still your problem, on the 6502, this session.
The one exception is a level loaded from the Pi's SD card (`LOAD_LEVEL`):
its world nodes are culled by the level file's baked PVS, with no work on
your side. See "Loading a level from the Pi's SD card" in
[class1-3d-mesh-reference.md](class1-3d-mesh-reference.md).
See [level-scale-visibility.md](level-scale-visibility.md) for the frozen
requirements this was scoped against, and size your level (or your own
coarse cell/room culling) accordingly rather than assuming gpu64 will skip
what's off-screen.

## Things the examples don't cover yet

[examples.md](../guide/snippets.md) covers the three most common single-purpose
programs. It does not yet walk through:

- **Multi-batch frames** — a scene whose walls/things/polys don't fit in
  one batch's blob, and how to split it across multiple `DRAW_*` calls
  against the one shared depth buffer without the second batch clobbering
  the first's rejects/stats interpretation.
- **Texture eviction under `OUT_OF_MEMORY`** — the texture arena is a bump
  allocator: freeing or re-uploading an id releases the id but not the
  bytes, and **there is no automatic eviction**. An upload that doesn't fit
  fails outright. If your level has more textures than fit at once, you
  need your own scheme for grouping them per area and calling
  `RASTER_RESET`/`FREE_TEXTURE` at the transition — this needs a worked
  example, not just the warning in
  [class2-raster-reference.md](class2-raster-reference.md).
- **The dynamic-light budget in a populated scene** — only 8 point lights
  exist (`SET_LIGHT` slots 0-7); a 9th call is `BAD_ARGS`, not queued. A
  real level needs a strategy for which 8 lights are live near the camera
  at any moment, and that strategy isn't demonstrated anywhere yet.
- **Class-1 point lights are still positioned affinely** — a mesh's
  texture coordinates are perspective correct, but the view-space position
  the point lights are evaluated at is interpolated affinely across the
  triangle (`px`/`py`/`pz` in `gpu64_3dRasterTriangle()`). The lit patch is
  in the right place on a face seen roughly head-on and drifts by a few
  pixels on a large one at a grazing angle; `tools/hostsim`'s "point light
  orbit" checks print that error directly. Class 2 already corrects it, the
  same way and in the same place it corrects `s`/`t`.
- **`THING_DIRECTIONAL`'s eight-consecutive-texture-id requirement** — a
  directional thing's texture id is the *first* of eight ids that must all
  be live and consecutive; there's no worked example of uploading and
  managing a directional sprite set.
- **Camera-per-batch vs. camera-per-record for `DRAW_THINGS`** — `ARG8` bit
  1 (`BATCH_CAM3D`) projects an entire batch through `SET_CAMERA3D` instead
  of the flat `SET_CAMERA`; nothing currently shows both call shapes side
  by side so the difference is visible.
- **`GPU64_FB_PAGES`/`SET_DRAW_PAGE` bounds** — the info block reports how
  many framebuffer pages exist; nothing demonstrates reading it and picking
  a safe page range instead of hardcoding page counts.

## Visual-quality facts easy to miss in a table

- **`SET_LIGHT` falls off linear in distance *squared***, not distance —
  full strength only at the light's exact centre, exactly 0 at its radius.
  That avoids a square root, but it also means a light's *apparent* size
  doesn't scale the way a linear falloff would; test radius/strength
  together, not separately.
- **Not every draw opcode is lit.** `DRAW_POLYS` evaluates lights per pixel;
  `DRAW_THINGS` once per record at the billboard's centre; the Doom layer
  (`DRAW_SECTORS`/`DRAW_WALLS`/`DRAW_COLUMNS`/`DRAW_SPANS`) **ignores
  dynamic lights entirely, by design** — it takes its light per column/span
  from the record's own `light` field instead. Mixing the Doom layer and
  `DRAW_POLYS`/`DRAW_THINGS` in one scene means two different lighting
  models sharing one frame.
- **`DRAW_POLYS`/`FILL_VIEW`'s depth-clear contract**: `FILL_VIEW` and
  `DRAW_SECTORS` both clear the depth buffer; `DRAW_POLYS` **does not** —
  it depth-tests against whatever the buffer already holds. Skip
  `FILL_VIEW` at the top of a frame and you're depth-testing against last
  frame's geometry, silently.
- **8.8 fixed point bounds world space to roughly ±128 units** on each
  axis (the format's own integer range). That's the hard ceiling for how
  large a single coordinate space can be — a level bigger than that needs
  chunking (multiple coordinate spaces stitched at load boundaries), and
  nothing here designs that for you yet.

## Setting expectations

This is a small, real pipeline: backface and near-plane rejection happen,
but there is no on-device frustum or occlusion culling, and (per #1 above)
no level-scale visibility structure. Size a first level around a room- or
cell-based structure you cull yourself, not around the assumption that
gpu64 will skip what's off-screen for you.

See also: [level-scale-visibility.md](level-scale-visibility.md) for the
one gap above that's a real design item rather than a documentation gap,
and [dev/gap_filling_plan.md](../../dev/gap_filling_plan.md) for
where that work is actually tracked.

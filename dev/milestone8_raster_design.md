# Milestone 8 — the raster layer (class 2), and why Doom needs it

The target this milestone serves is stated in
[project_description.md](project_description.md): **Doom, running on a stock
1 MHz C64, with gpu64 doing the drawing.** This document is the rationale and
the as-built record. The developer-facing reference is the class 2 section of
[api_design.md](api_design.md).

## What Doom actually asks a renderer for

Doom's renderer is not a triangle pipeline, and class 1 — transform, cull,
light, rasterise triangles — is the wrong shape for it. Doom decomposes every
frame into exactly three primitives:

| Doom's own name | What it is | Count per frame |
|---|---|---|
| `R_DrawColumn` | a **vertical run** of one screen column, sampling one texture column at a constant texel-per-row step | 320 walls + masked sprite columns |
| `R_DrawSpan` | a **horizontal run** of one screen row, sampling a flat with a constant `du`/`dv` per column | one per visplane row |
| masked column | the same as `R_DrawColumn` but skipping a key index | sprites, grates, fences |

Everything above those three — BSP traversal, the seg/visplane split, clipping
arrays, sprite ordering — is *decision* work: comparisons, table lookups and
16-bit fixed-point arithmetic on a few hundred objects. That half a 6502 can
do. What it cannot do is the *pixel* half: at 320x200 a Doom frame writes
around 64000 texels through a colormap, which is several seconds of 6502 time
per frame and the reason "Doom on a C64" has never meant a real port.

So the split this milestone commits to is:

> **The C64 decides what to draw. gpu64 draws it.** The C64 emits a batch of
> column and span records; gpu64 turns them into pixels.

That is a much better fit for this hardware than either alternative. Sending
the *level* to gpu64 and letting the Pi run the whole renderer would make the
C64 a keyboard, which is not the project. Sending individual draw commands
would not work at all — see the next section.

## Why batches, and why this is the interesting part

The binding constraint is not the Pi's speed. It is that **a command halts the
C64 for its duration** (api_design.md) and costs a handful of IO2 register
writes to set up. A Doom frame needs ~320 wall columns plus a few hundred span
records plus sprite columns: call it 800 primitives.

At one command per primitive, staging ~10 `ARG` bytes each, that is 8000
IO2 writes per frame, before any pixels are drawn. At roughly a microsecond
per 6502 store that is 8 ms of pure protocol per frame — and it multiplies the
exposure to the known IO2 sampling floor (~1 in 180000 command writes never
lands, [CLAUDE.md](../CLAUDE.md)) by 800.

A batch collapses that to **one command**. The C64 builds an array of 16-byte
records in its own RAM (or pushes it to REU with the REU's own DMA, which
costs it nothing), then issues a single `DRAW_COLUMNS` naming the array. gpu64
pulls the whole array in one bounded DMA burst — the same path a `BLIT`
already uses and the fastest way bytes cross this bus — and executes it.

Three consequences that shaped the format:

- **16 bytes per record, fixed.** A power-of-two stride is what makes the
  6502 side cheap: indexing record *n* is a shift, not a multiply. The wasted
  byte or two is worth more than it costs.
- **`DRAW_SPANS` and `DRAW_COLUMNS` share the record size** so the two
  builders on the C64 side are the same loop with different fields.
- **The batch is the checksum unit.** Rule 3 of [CLAUDE.md](../CLAUDE.md) says
  any bulk upload through this path needs a checksum or a length readback.
  A per-record checksum would be absurd; a per-batch one is 2 bytes and one
  16-bit sum, and it is *optional* (a flag bit) so a program that would rather
  spend the 6502 cycles on gameplay can decline it.

## Why not a new class-0 opcode

Class 0 is the 2D drawing API and is hardware-verified. Class 2 brings its own
resource type (column-major textures), its own resource arena, its own view
rectangle and its own lighting table — that is a subsystem, not an opcode.
Putting it behind `CMD_HI = 2` also means a program can discover it: bit 2 of
the `GET_INFO` class bitmap is set only when this build has it.

## Why not class 1

Class 1 is the triangle pipeline. It shares nothing useful with a column
renderer except the idea of a texture, and it differs in the two places that
matter: its textures are row-major and power-of-two on both axes (an affine
mapper walks rows), and its dispatch is staged through the core-1 ring.
Class 2 runs **synchronously on core 0** inside the dispatch window, exactly
like a class 0 draw op — the C64 is halted anyway, nothing else can touch the
framebuffer, and every milestone-6a contention question is therefore out of
scope. See *If this ever moves to core 1* below.

## The as-built decisions

### Textures are column-major

`texel(u,v)` lives at `pTexels[u * h + v]`. This is Doom's own storage order
and it is not a coincidence: a column draw walks one *u* and every *v*, so
column-major makes the inner loop a linear walk through one cache line after
another instead of a stride-`w` scatter. `UPLOAD_TEXTURE`'s flag bit 0 lets
the C64 hand over row-major bytes and have the firmware transpose once at
upload — a convenience for tooling, never in the per-frame path.

**`h` must be a power of two; `w` need not be.** The column loop wraps `v`
every row, so that has to be a mask. `u` is wrapped once per record, so a
modulo is affordable there, and *not* constraining `w` is what lets a sprite
be its natural width instead of padded. The one place the relaxation bites is
`DRAW_SPANS`, which wraps `u` per pixel too — a span record naming a texture
whose `w` is not a power of two is rejected, and the reference says so.

### Texture IDs are 1..255, and 0 means "solid colour"

A record has 16 bytes and a full 16-bit ID would cost two of them for a
namespace no Doom-sized level needs. One byte it is. Spending id 0 on *solid
colour* — fill the run with the palette index in the record's `u` field, still
mapped through the colormap — pays for itself immediately: sky, untextured
floors, the fallback when a texture failed to upload, and the whole first
bring-up of a batch renderer before any texture exists.

### A bad record is rejected, not fatal

A record whose `x` is outside the view, whose `y1 < y0`, or which names a
texture that does not exist, is **skipped and counted**; the command still
returns `OK`. The alternative — fail the batch — means one stale record from
last frame blanks the screen, which is a worse debugging experience and a
worse game. `RASTER_STATS` is how a program finds out, and it is also the
length readback rule 3 asks for: a batch that lost bytes in transit shows up
as a rejected count that should have been zero.

### Lighting is a colormap, not a shader

`SET_COLORMAP` uploads *levels* x 256 bytes: `colour = map[light * 256 + c]`.
This is exactly Doom's `colormaps` lump, so a real Doom palette and its 34
light levels transfer unchanged, and it costs one table lookup per pixel. The
`light` byte is per record, which is the granularity Doom lights at (per seg,
per visplane, per sprite).

Class 1 has its own colormap (`BUILD_COLORMAP`, generated from the palette).
They are deliberately separate tables: class 1 generates one from a light
direction, class 2 is handed one. Sharing them would mean one subsystem's
reset stomping the other's.

### The view rectangle

`SET_VIEW` clips every class 2 primitive. Doom's 3D view is a window above a
status bar, and without a clip the C64 would have to clamp every record it
emits — per record, on a 6502, per frame. One rectangle set once a level is
strictly better. It also bounds the cache clean: only the view's rows are
cleaned after a batch, not the whole page.

### `DRAW_SPRITE` is a scaled blit, not a column list

Doom draws sprites through `R_DrawColumn` with a per-column scale, and a
program *can* do exactly that here — masked columns are in the record format
precisely so sprites work that way when they need per-column depth clipping.
But the common case is a sprite that is wholly in front of everything it
overlaps, and for that, computing 40 column records on a 6502 to draw one
imp is waste. `DRAW_SPRITE` takes a destination rectangle in screen pixels
and does the stepping itself, with a scalar top/bottom clip for the case
where the sprite is standing behind a wall edge.

Destination-rectangle semantics rather than Doom's `iscale` because the C64
already knows the rectangle it wants — it computed the sprite's screen extent
to decide it was visible at all — and turning that back into a step is a
division the firmware can do for free and the 6502 cannot.

## What this does not do, and what Doom still needs

Being honest about the gap, because "one step towards" is the claim:

- **No depth information crosses the API.** Painter's ordering and the
  clipping arrays stay on the C64, which is where Doom puts them anyway.
- **No BSP, no visplanes, no sprite clipping arrays.** All C64-side work, and
  all of it still to be written.
- **No performance figure on real hardware.** Nothing in this milestone has
  executed on the Pi. The per-frame cost is dominated by the batch DMA pull,
  which is the same path as a `BLIT` and therefore has a known shape but no
  number at this size.
- **The C64 side of a real Doom port is not started.** This milestone is the
  drawing half, and only the drawing half.

## If this ever moves to core 1

It does not need to — the C64 is halted for the dispatch, so core 0 has
nothing better to be doing. If a future phase wants the render to overlap C64
execution, milestone 6a's constraint applies unchanged and is hard: **at most
7 consecutive cache lines written before a yield, design to 4.** A 320-byte
scanline is inside that, and a column run of 200 pixels down a 320-byte pitch
touches 200 separate lines — so the column loop would need a yield roughly
every 4 rows, not every 4 pixels. That is a real design question and it is
deliberately not answered here.

## Verification

Per [CLAUDE.md](../CLAUDE.md), nothing goes to the bench unverified:

- `tools/prgsim/gpu64model.py` gained a class 2 model **written from the
  reference document**, not from the firmware — the same second-opinion
  discipline the class 0 suite uses.
- `tests/gpu64_test_raster.a` is the conformance program: it draws
  through every class 2 opcode and reads the result back with class 0's
  `READ_RECT`, so correctness is a byte comparison rather than a look at the
  screen.
- `tools/rastercheck/` closes the gap the class 0 suite has to leave open: it
  compiles the **firmware's own** portable raster core with a native g++ and
  renders the same batches the Python model renders, then diffs the two
  images. A disagreement between firmware and reference is found on a PC
  instead of at the bench.
- `Source/Demos/gpu64_demo_raycast.a` is the developer-facing demo and
  renders to `Source/Demos/out/raycast.ppm`.

# Level-scale visibility — superseded by class 1

**This section is frozen, not actionable.** It records a gap identified
before 2026-08-28, when class 2's resident-polygon-table work
(`UPLOAD_POLYS`/`DRAW_WORLD`) was the forward path for level-scale 3D. On
2026-08-28 that decision changed: **class 1's retained scene graph is now
gpu64's forward path for 3D**, and none of the items below are planned to
be built inside class 2. They are kept here, unchanged in substance, so a
reader who finds a reference to this gap elsewhere in the project's history
can see what it meant and where it went — not as a live roadmap.

If you are looking for gpu64's actual plan for level-scale visibility, it
is in [dev/gap_filling_plan.md](../../dev/gap_filling_plan.md), and
its current API surface is [class1-3d-mesh-reference.md](class1-3d-mesh-reference.md).
Class 1's scene graph gives most of the structure these items were reaching
for **by construction** — a retained tree of nodes and meshes doesn't need
a bespoke visibility opcode to know what exists, the way a per-frame batch
protocol did.

## What shipped under this heading

- **[IMPLEMENTED]** `UPLOAD_POLYS` ($14) / `DRAW_WORLD` ($27) — a resident,
  whole-level polygon table, uploaded once and redrawn every frame without
  re-sending geometry. This is what made the remaining items below tractable
  in principle: a resident table is a precondition for visibility
  determination that doesn't repeat per-frame transfer cost.

## What was still open when this froze

These are preserved as originally scoped, for historical reference only —
read them as "what class 2 would have needed", not as class 1's design:

1. All per-frame visibility determination has to run in RPi firmware,
   never on the 6502 — the whole point is that a 1MHz CPU cannot afford to
   decide what's visible every frame.
2. A precomputed, offline-baked visibility structure (a portal graph or a
   potentially-visible-set table) needs its own upload opcode, parallel to
   `UPLOAD_POLYS`.
3. Firmware needs to determine the camera's current cell or leaf from the
   position `SET_CAMERA3D` last received, to know which precomputed
   visibility set applies.
4. Full frustum culling, scoped by that visibility set, needs to run
   against the persistent polygon table before rasterizing.
5. A batch-less draw opcode is needed so that a level-scale frame costs one
   fixed-size command, regardless of how much of the level is actually
   visible — the way `DRAW_WORLD` already doesn't require re-listing which
   polygons to draw.
6. A memory and frame-time budget needs working out against the Pi 3A+'s
   actual headroom, once 1-5 have a concrete shape to measure.

See also: [dev/progress_tracker.md](../../dev/progress_tracker.md) for
the campaign history around milestone 13 and the 2026-08-28 decision, and
[dev/milestone9_poly_design.md](../../dev/milestone9_poly_design.md)
for the class-2 polygon layer this heading originally described.

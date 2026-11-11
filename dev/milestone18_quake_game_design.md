# Milestone 18 — the Quake game

Step 4 of the v1 plan: build a real game on the retained scene, and so find
out whether the frozen v1 surface is enough to build one with. A hand-authored
room cannot answer that. E1M1 can.

This document is the as-designed record; `dev/progress_tracker.md` carries
status, and `docs/` carries whatever a user of the API ends up needing.

## What is being proved, and what is not

The claim under test is narrow and worth stating precisely: **a C64 program
can drive a textured, lit, 3D game world through the class-1 retained scene at
playable rates, sending on the order of ten register writes a frame.**

What that does *not* claim:

- It is not a claim that the C64 can hold the level. It cannot — E1M1 is
  554 KB converted, and its collision hulls alone are 65 KB.
- It is not a claim that the v1 opcode set is sufficient *unchanged*. It is
  not, and the freeze anticipated this: "anything added later takes a new
  opcode number rather than a new argument on an existing one"
  (`docs/README.md`). Two new opcodes are proposed below. The freeze is a
  promise about not breaking existing meanings, not a promise to stop.
- It is not a bench result. Everything here is verified on a PC until it is
  verified on hardware, per the project's own testing rule.

## The shape of the split

The governing constraint is the bus, not the Pi. A gpu64 command halts the
C64 for the duration and the register window is one byte wide, so the
question for every piece of game state is *which side of the bus does it live
on*, and the answer is driven by how many bytes a frame it would otherwise
cost.

| Lives on the Pi | Lives on the C64 |
|---|---|
| Level geometry, textures, palette, colormap | Player position, angles, velocity |
| The scene graph (one node per chunk, plus entity nodes) | Which entities are awake, and their game state |
| Collision hulls, and the trace against them | Game rules: health, keys, doors opened, triggers fired |
| The camera transform | Input, HUD, sound |

The C64 keeps the *authoritative game state*; the Pi keeps the *world* and
answers questions about it. That division is what keeps the per-frame traffic
at about a dozen writes: a camera pose, a commit, and whatever handful of
entity nodes actually moved.

## The level file

`tools/gen_quakelevel.py` converts a Quake BSP out of `PAK0.PAK` into a
`.g64lev` file. It is not a format the API knows about — it is a file the
*firmware* reads off its own SD card, the same way it already reads
`config.txt` and the C64 font.

Three hard limits shape the conversion, and all three come from the class-1
mesh format rather than from Quake:

- **256 vertices per mesh**, because a face index is one byte.
- **±128 world units per mesh**, because vertices are signed 8.8 model space.
- **255 texels of UV span per face**, because UV bytes wrap at 256.

So the level is cut into spatial chunks — 108 of them for E1M1, mean 87
vertices — each chunk a mesh placed by a node transform. Chunking spatially
rather than by BSP leaf is what makes per-object culling worth anything
later, and it is why the node count (108) has to be read against
`GPU64_3D_MAX_NODES` (256): the world uses 108 of the scene's 256 slots and
the game gets the remaining 148 for monsters, items and doors.

Two conversion details cost a full debugging round each and are recorded in
the converter's own comments:

- **Winding survives the axis swap unchanged.** Quake is right-handed with
  z up; gpu64 is left-handed with y up; the mapping is `(x, z, y)`. The
  obvious argument — "the swap flips handedness, so reverse the winding" —
  is wrong, because the swap *is* the conversion: it turns Quake's
  counter-clockwise-from-front into gpu64's clockwise-from-outside. Written
  the wrong way round first, the level rendered see-through, with three of
  eight directions from the player start showing nothing at all.
- **Yaw is `90 - angle`, derived, not guessed.** gpu64's forward is column 2
  of `gpu64_3dMatFromEuler()`, `(sin y, 0, cos y)`; Quake's is `(cos t, 0,
  sin t)` after the swap. A quarter-turn error here still renders a perfectly
  good picture of the wrong wall, which is exactly the kind of bug a bench
  photograph cannot settle.

Face texture ids are **dense and byte-wide**, remapped from BSP miptex
indices, because `gpu64_3d_render.cpp` resolves a face's texture as
`pLookup(ctx, pF->texid)` with `texid` a `u8`. A texture the converter cannot
accept — E1M1 has one, `slip1` at 128x192, not a power of two — leaves its 18
faces flat grey rather than pointing them at whatever resource happens to
occupy that number.

`tools/check_g64lev.py` validates a written file against every one of those
invariants, so a file that passes cannot be rejected by `UPLOAD_MESH` for its
shape.

## Rendering it before the bench

`tools/hostsim/levelsim` loads a `.g64lev` and renders it through
`gpu64_3dSceneRender()` — the same function core 1 runs — placing a camera at
`info_player_start`. It is the third sim beside `hostsim` (immediate mode) and
`scenesim` (replays a demo's real command stream), and it exists because
neither of those can check a converter whose consumer does not exist yet.

It is what caught both conversion bugs above, and it is the gate before any
bench trip: a level that does not render here will not render there.

`Source/Demos/gpu64_demo_level.a` closes the rest of the gate. It is the
program the bench actually runs — two commands and a polling loop to put E1M1
on the HDMI screen, then one camera update and one `SCENE_COMMIT` per frame —
and `tools/examples.sh level` runs it under `tools/prgsim` (which models
`LOAD_LEVEL`/`LEVEL_STEP` through the same tables `UPLOAD_MESH` and
`CREATE_OBJECT` use) and renders every frame of the resulting command stream
through `scenesim`. Three defects came out of that before any bench time was
spent: a `cmd1` that returned the right byte in `A` with the wrong Z flag, an
input layer reading `$91` where prgsim models the CIA matrix, and a
`LOOP_START` whose mode argument was a leftover from `LOAD_LEVEL`.

Its key schedule is a tour that **returns** — every walk paired with the walk
that undoes it, and the long stretches are yaw sweeps in place. Nothing in
gpu64 stops the camera leaving the level until `CLIP_MOVE` exists, and the
first schedule tried flew out through a wall by frame 500 and spent the
remaining thousand frames photographing the black outside of the map.

## The two new opcodes

### `LOAD_LEVEL` — the level crosses on the Pi's side of the bus

620 KB cannot come across the register window, and it should not have to: the
Pi has an SD card and already reads files from it. `LOAD_LEVEL` ($13) takes a
mesh id base, a node id base and a camera id, and the Pi builds every texture,
every mesh, the palette, the colormap and the world's object nodes from its
own card.

**As built, it is two opcodes and it runs on core 0.** Three constraints ruled
out the design above, and each of them is worth keeping written down:

- **The SD read cannot happen anywhere near `reuUsingPolling()`.** EMMC is
  MMIO, and MMIO traffic from any core wrecks core 0's per-cycle bus timing.
  So `gpu64_levelPreload()` reads `SD:RAD/level.g64lev` **once, before the
  polling loop ever runs**, into a 1 MB BSS buffer, and parses it there so a
  bad file is a line in the boot log rather than a one-byte ERRCODE at the
  bench. `LOAD_LEVEL` itself only ever parses memory.
- **It cannot run on core 1.** The arena allocator is core-0-owned and
  unlocked; building 66 textures and 108 meshes out of it from the render core
  is exactly the race the allocator's design excludes.
- **It cannot be a one-shot command.** Building 282 items under a single DMA
  hold is a multi-second halt of the C64.

So the load is **sliced on core 0 under a bounded budget**: `LOAD_LEVEL` sets
the work up and returns, and `LEVEL_STEP` ($14) spends ~16 KB of source bytes
per call and returns the percentage in `RESULT`, or `$FF` when the whole level
is registered. E1M1 finishes in **29 steps**. The slicing is not only about
hold length — it buys a loading bar, and it makes a failure report *which*
item failed instead of collapsing 282 of them into one ERRCODE.

Four details the client has to know:

- **`LOAD_LEVEL` is keyed.** It is destructive — it clears the resource table
  and the scene — so it needs the one-shot `$A5` in ARG15 like `SCENE_RESET`
  does. `LEVEL_STEP` is not keyed; it only continues work already authorised.
- **Both refuse with BUSY while the loop is running.** Not left to the
  dispatcher's drain: a step creates nodes in `s_Scene`, which is what core 1
  reads, and the drain proves only that the frame *in flight* has finished.
- **Texture ids are forced to `0..nTex-1`.** `pF->texid` in the mesh wire
  format is a `u8` handed straight to `lookupTexture()`, so the level's
  textures take the dense low ids and the caller's mesh and node bases are the
  only free choices.
- **The camera is created from the file.** `info_player_start` is in the
  entity table, and a C64 that had to find it would need the entity table on
  its own side. Passing 0 for the camera id opts out.

### `CLIP_MOVE` — the Pi answers "can I walk here?"

Collision is the one piece of game logic that cannot live on the C64: E1M1's
clipnodes are 43 KB and its planes another 36 KB. But the Pi already holds
the level, and Quake's hull trace is a few dozen lines against data the level
file can carry.

So `CLIP_MOVE` takes a desired delta and returns the resolved position plus
contact flags (on ground, hit wall, in liquid). The C64 stays authoritative:
it decides what to *do* with the answer — whether that was a landing, a step
up, or a wall to slide along.

This is the same division `DRAW_WALLS` already established, where the 6502
sends ten bytes of camera a frame and gpu64 does the projection. It is the
division that makes the API useful rather than merely capable.

**As built (2026-09-21).** The opcode is `$15`; the block layout and the
client's obligations are in
[docs/class1-3d-mesh-reference.md](../docs/class1-3d-mesh-reference.md) under
"Walking in a level". Four decisions worth keeping the reasons for:

- **Everything travels in one 56-byte block, not in the ARG registers.** It
  does not fit in them — a position and a displacement in 16.16 are 24 bytes
  — but the better reason is that a block can carry a checksum and a register
  file cannot. The opcode is called every frame, forever, which is exactly
  the exposure the ~1/180000 sampling defect needs; a bad block is rejected
  whole and the player simply does not move for one frame.
- **Both halves are magic'd as well as checksummed.** A checksum alone
  accepts an all-zero block, which is what an uninitialised buffer looks
  like, and `gpu64-getinfo-readback-was-unfenced` is the run that made that
  concrete. The C64 side poisons the output half before the call for the same
  reason.
- **Not keyed, and not refused while the loop runs.** `CLIP_MOVE` reads the
  level file — immutable once loaded — and writes the caller's own memory. It
  mutates no gpu64 state, so it is the one non-render opcode exempt from
  `gpu64_3dDispatch()`'s pre-execute drain (`isDrainExempt()`). Making a
  movement query wait for a frame to finish would put a whole render on the
  critical path of every step the player takes.
- **Gravity, friction and jumping are NOT in it.** It answers a geometric
  question. The demo's gravity is one constant subtracted from the
  displacement's y before the call, which is four lines of 6502.

The trace itself is `gpu64_levelTrace()`/`gpu64_levelMove()` in
`Source/Firmware/gpu64_level.cpp` — Quake's `SV_RecursiveHullCheck` and
`SV_FlyMove` in 16.16, with time folded into the displacement so there is no
velocity or frame duration on the wire. Two gates cover it on a PC:
`tools/hostsim/hulltest` drives the trace directly against E1M1 (a 64-heading
sweep and a 20000-frame wander, with the assertions that a collision system
which blocks *everything* would fail — the `L=0/0` trap), and `tools/prgsim`
answers the opcode for `gpu64_demo_level` by calling the firmware's own trace
through `tools/hostsim/libclipmove.so`, so the protocol is modelled and the
geometry is not re-implemented.

## Sequence

1. Converter: geometry, textures, palette, entities. **Done, PC-verified.**
2. Converter: collision hulls (clipnodes, planes, per-model headnodes).
   **Done, PC-verified** — `tools/hostsim/hulltest` traces against them.
3. Firmware: the `.g64lev` loader and `LOAD_LEVEL`/`LEVEL_STEP`, sliced on
   core 0. **Done, PC-verified, hardware-unverified.**
4. Firmware: the hull trace, and `CLIP_MOVE`. **Done, PC-verified,
   hardware-unverified.** `gpu64_demo_level` walks on the floor, climbs
   E1M1's stairs and cannot leave the map.
5. C64: the game — movement, doors, monsters as sprite nodes, HUD.
   - 5a. The two firmware prerequisites: `LEVEL_ENT` ($16), so the C64 can
     read the entity list one digested record at a time, and a **mover list**
     in the `CLIP_MOVE` block, so a closed door blocks. **Done, PC-verified,
     hardware-unverified** — progress_tracker section 65.
   - 5b. The C64 side of doors: the entity tables, the trigger/button wiring
     by id, and the animation as absolute `SET_POSITION` on a node run.
     **Done and run on hardware** — `gpu64_demo_game` opens E1M1's doors, and
     `tools/check_game.py` asserts that the node actually moved
     (progress_tracker section 66). Bench run 37 put it on a real C64 and it
     reported `THE DOORS WORK`, but with 61% of that run's `CLIP_MOVE`
     answers rejected by the bus the player barely moved, so the *game* is
     hardware-verified and the *walk* is not — progress_tracker section 68.
   - 5c. Monsters, items and a HUD.
6. Bench.

Steps 3 and 4 are the ones that touch the polling loop's neighbourhood and
therefore the ones that get the careful reading; steps 1, 2 and 5 are
verifiable on a PC in full.

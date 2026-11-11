# Class 1 — 3D mesh reference

This is what a byte means for `CMD_HI = 1`, gpu64's retained/immediate-mode
3D mesh pipeline: textured, perspective-correct triangles, flat-lit per face,
over a real z-buffer. **This is gpu64's general-purpose 3D layer — the
"OpenGL for the C64" one** — not a pipeline scoped to any one game genre;
a Doom/Quake-style first-person renderer is one thing it can draw, not
what it's for. It supersedes [class 2](class2-raster-reference.md)
(deprecated, frozen but still working) as gpu64's forward path for 3D.

See also: [dev/milestone6_3d_design.md](../../dev/milestone6_3d_design.md)
for the architecture rationale (why a second core, why a generated colormap
instead of RGB, the store-burst budget) and the phase-1 build history, and
[dev/gap_filling_plan.md](../../dev/gap_filling_plan.md) for what is
staged next to close the gap between this section and "Status" below.

## Status: retained scene graph, real core-1 overlap, handshake loop live

**Read this before the opcode table below — it changes what several rows
actually do today.**

The design targets a retained scene graph rendered autonomously by a second
core, with the C64 only ever moving nodes and committing frames. As of
stage 16 (2026-08-29), that is live for **handshake mode**:

- `CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` run on **core 1**, not core 0 —
  `gpu64_3dDispatch()` queues the draw and returns without waiting for it to
  finish, so the C64 is free to issue its next command while core 1 is still
  rasterising. `RESULT`/`ERRCODE` for these three catch up to the draw's own
  outcome at the next observation point rather than at return time — see
  "Deferred RESULT" below, it changes how you read them back. Node state
  (position, orientation, scale, visibility) is retained across commands;
  drawing it is still an explicit per-frame call. **These three are refused
  with `BUSY` while the autonomous loop is running** — see "Frame lifecycle"
  below for why, and use `DRAW_NODE`'s scene-graph siblings instead.
- `LOOP_START`/`LOOP_STOP`/`SCENE_COMMIT` ($06-$08) are implemented for
  **handshake mode** (`LOOP_START`'s `ARG0` = 0) and are **hardware-verified
  as of 2026-09-08** — a full `gpu64_loop_test` run (setup, 600 animation
  frames, teardown, `VERDICT PASS`) with the C64 alive at the end. The
  derails that made earlier builds unusable were an async DMA hold, not the
  protocol; see [dev/progress_tracker.md](../../dev/progress_tracker.md)
  § 16b and § 16c. Two things that run teaches a calling program:
  **commit once per ready frame, not flat out** (poll `STATUS` bit4 —
  committing flat out is refused `BUSY` about 80% of the time, by
  construction, since the loop runs at the frame clock), and **check
  `SEQACK`**: at roughly 0.13 lost writes per accepted flip, a per-frame
  command is dropped every several frames, so re-issue on a mismatch rather
  than assuming it landed. Free-running mode (`ARG0` = 1, vsync-driven
  rather than `SCENE_COMMIT`-driven) answers `UNSUPPORTED` — staged in
  [dev/gap_filling_plan.md](../../dev/gap_filling_plan.md).
- Scene-node and transform opcodes, **$20-$24 and $30-$36**, are **live**:
  a node created with `CREATE_OBJECT`/`CREATE_CAMERA` persists in a 256-node
  table until `DESTROY_NODE` or a session reset, and `DRAW_NODE` ($42) draws
  it using whatever the transform opcodes last set — no re-staging a
  position/orientation/scale on every draw the way `DRAW_MESH` needs. Every
  opcode in these ranges runs synchronously on core 0, but as of 15b each one
  first waits for any *immediate-mode* render it queued to finish draining on
  core 1 — it is about to mutate state that render also reads. **While the
  autonomous loop is running, these same opcodes instead write a shadow copy
  and return without waiting at all** — see "Frame lifecycle" below; the
  drain-before-mutate behaviour just described is what still happens when the
  loop is not running.
- `ARENA_STATUS` ($09) exists and was not in the original design sketch —
  see below.

In other words: **a class 1 program has two ways to drive a frame.**
Immediate mode calls `DRAW_MESH` directly (transform staged fresh every
call), or builds persistent nodes with `CREATE_OBJECT`/`CREATE_CAMERA` and
draws them explicitly with `DRAW_NODE` — every frame's draws are calls the
C64 makes itself, each one queuing on core 1 and returning without waiting
for its own draw to finish (something that actually needs the draw finished
— the next state-mutating opcode, a page flip, a class 0 framebuffer op —
pays that wait). The autonomous loop (`LOOP_START`/`SCENE_COMMIT`) instead
has core 1 render the whole scene every frame on its own: the C64's
per-frame work collapses to one small command per object that actually moved
this frame, then `SCENE_COMMIT` — a fire-and-forget trigger that publishes
those edits and returns immediately, never blocking on the render itself; it
finds out the next frame is ready by polling `STATUS` bit4. See "Frame
lifecycle" below for the full protocol.

## Opcode table

`CMD_HI = 1`. `ARG` offsets are relative to `ARG0` ($DF56), exactly as in
class 0, and every opcode reads exactly the byte count in its row.

### System and loop — $00-$0F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $00 | `SCENE_RESET` | 0 or 1 | `ARG15` = `$A5` | Destroys every node, stops the loop, leaves uploaded resources alone. `ARG15` must be `$A5` on this dispatch or the call is `BAD_ARGS` and nothing is destroyed — see "The key byte". |
| $01 | `SET_VIEWPORT` | 8 | x, y, w, h (16-bit each) | Places the 3D viewport in the page. `w*h*3 > GPU64_3D_BUDGET` (196608, provisional) is `OUT_OF_RANGE`; a viewport not wholly inside 320x200 is `BAD_ARGS`. |
| $02 | `SET_PERSPECTIVE` | 6 | `ARG0-1` fov (binary angle), `ARG2-3` near, `ARG4-5` far (8.8) | Projection for the active camera. |
| $03 | `SET_LIGHT` | 7 | `ARG0-5` direction x,y,z (8.8), `ARG6` ambient level 0-15 | The single directional light. Face shade = ambient + N.L, clamped to 0-15, indexing the colormap. |
| $04 | `BUILD_COLORMAP` | 0 | — | Regenerates the 16-level colormap from the current palette. Needed after a palette change; costs milliseconds — never call it per frame. |
| $05 | `SET_BACKGROUND` | 1 | `ARG0` palette index | What the viewport is cleared to. |
| $06 | `LOOP_START` | 1 | `ARG0` 0 = handshake, 1 = free-running | Starts the autonomous render loop. `ARG0` must be 0 — free-running is `UNSUPPORTED`, staged next. `BUSY` if the loop is already running; `NO_CAMERA` if no camera is active at the moment of the call (checked once, here — not re-checked per frame after). Queues the loop's first frame and returns immediately; see "Frame lifecycle". **Optional** in handshake mode — `SCENE_COMMIT` starts a stopped loop by itself, so this opcode's lasting purpose is selecting the mode. |
| $07 | `LOOP_STOP` | 0 or 1 | `ARG15` = `$A5` | Stops the loop. `ARG15` must be `$A5` on this dispatch or the call is `BAD_ARGS` and the loop keeps running — see "The key byte". Keyed, it answers `OK` even against a loop that is already stopped, so a teardown retry never fails. A frame already queued when this arrives still finishes on core 1; it is discarded, not cancelled. |
| $08 | `SCENE_COMMIT` | 0 | — | Publishes every shadow-redirected edit made since `LOOP_START`/the last commit, arms the next vblank flip the same way `PAGE_FLIP` does, and queues the loop's next frame. `RESULT` = the page the just-finished frame is in — the same page the flip this call just armed is about to show. If the loop is **not** running, this re-starts it (same checks as `LOOP_START`) and answers `OK` without committing — see "A stopped loop repairs itself". `UNSUPPORTED` if the flip subsystem is not calibrated yet; `BUSY` if a `PAGE_FLIP` is already pending. See "Frame lifecycle". |
| $09 | `ARENA_STATUS` | 0 | — | `RESULT` = free resource RAM in 128 KB units (0..256 for the 32 MB phase-1 arena). Not in the original design; added because "you get `OUT_OF_MEMORY` eventually" is truthful and undebuggable without it. |

### Resources and the level — $10-$1F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $10 | `UPLOAD_MESH` | 12 | `ARG0-5` vertex blob descriptor, `ARG6-11` face blob descriptor | Uploads into resource RAM under the staged `ID`. Re-upload to a live ID replaces it in place — see Resource lifecycle below. `RESULT` = face count. |
| $11 | `UPLOAD_TEXTURE` | 8 | `ARG0-5` blob descriptor, `ARG6` w shift, `ARG7` h shift | Dimensions are `1 << shift`, 3..8 (8 to 256 px, power of two). `len` must equal `w*h`; anything else is `BAD_ARGS`. |
| $12 | `FREE_RESOURCE` | 0 | `ARG15` = `$A5` | Frees the staged `ID`'s table slot. Keyed — see "The key byte"; `BAD_ARGS` without it, and nothing is freed. In phase 1 this does not reclaim arena bytes — see Resource lifecycle. |
| $13 | `LOAD_LEVEL` | 6 | `ARG0-1` mesh ID base, `ARG2-3` node ID base, `ARG4-5` camera ID, `ARG15` = `$A5` | Begins building a whole level out of the REU image selected in the RAD menu. Clears the resource table and the scene first, so it is keyed — see "The key byte". Does no building itself: call `LEVEL_STEP` until it says done. `BUSY` while the loop is running. See "Loading a level" below. |
| $14 | `LEVEL_STEP` | 0 | — | Builds the next slice of the level begun by `LOAD_LEVEL`. `RESULT` = percent complete 0..99, or `$FF` when the level is fully built — and once done it stays `$FF`, so a lost `RESULT` read can simply step again. `BAD_ARGS` if no load is in progress. If a slice fails, the error is whatever that item's build returned (`OUT_OF_MEMORY`, `BAD_ARGS`, …) and `RESULT` carries the **low byte of the failing item's index**; the load is abandoned. `BUSY` while the loop is running. |
| $15 | `CLIP_MOVE` | 6 | `ARG0-5` descriptor of a 56-byte block in C64 RAM or REU | Asks where a move actually ends: you send a position and the displacement you want, and gpu64 traces it against the level's Quake clip hulls and answers with the position you end up in. Everything travels in the block, checksummed both ways — see "Walking in a level". `BAD_ARGS` if `len < 56`, if no level is loaded, if the block's magic or checksum is wrong, or if the hull or model is not one this level has. **Not** keyed and **not** `BUSY` while the loop runs: it changes no gpu64 state, so it answers mid-frame. |
| $16 | `LEVEL_ENT` | 6 | `ARG0-5` descriptor of a 96-byte block in C64 RAM or REU | Hands back one entity of the loaded level — where it is, what kind of thing it is, which scene nodes draw it, and how far it travels if it is a door. Same block discipline as `CLIP_MOVE`: input bytes 0-3, output 16-95, magic and checksum both ways. `RESULT` = the entity's `kind`. `BAD_ARGS` if `len < 96`, if no level is loaded, or if the block's magic or checksum is wrong; `OUT_OF_RANGE` past the last entity. Also not keyed and not `BUSY` while the loop runs. |
| $17 | `LEVEL_PALETTE` | 0 | — | Puts the loaded level's palette back. Compares the live palette with the level file's and rewrites only the entries that differ; if any did, pushes the palette to the display and rebuilds the colormap. `RESULT` = entries repaired, saturated at 255 — 0 is the normal answer and costs nothing but the compare. For a refresh ring: the level's palette is the one piece of state your program never held a copy of. `BAD_ARGS` until a level load has finished. Not keyed; fine while the loop runs. |
| $1A | `LEVEL_PICTURE` | 6 | `ARG0` picture index, `ARG1-2` x, `ARG3-4` y (signed, clipped), `ARG5` flags: bit 0 = keyed (index 255 is transparent), bit 1 = install the level file's palette first; other bits must be 0 | Draws one of the level file's 2D pictures (title, menu and intermission art — see "Pictures" below) onto the **class 0 draw page**; `PAGE_FLIP` shows it. Reads level slot 0 of the REU image, so it works **before** `LOAD_LEVEL`, which is what a title screen needs. `BAD_ARGS` for an index past the table, bad flag bits, or an REU image that is not a level pack — which is also how a program checks that the player selected its image. `BUSY` while the loop is running — the loop owns the pages — which also makes a phantom one harmless mid-game. Not keyed. |
| $1C | `LEVEL_NODE` | 1 | `ARG0` bit 0 = also restore the position; other bits must be 0 | Puts one level node back the way `LEVEL_STEP` built it: an object, instancing the level's own mesh, visible, scale 1.0, no rotation — and, with bit 0, at the level file's position. Leave bit 0 clear for a node you move yourself (a door). Staged `ID` = the node id. `RESULT` bit 0 = the node itself was wrong, bit 1 = its position was; 0 is the normal answer. `BAD_ID` past the level's last node, which is also how a program finds where the run ends; `BAD_ARGS` until a level load has finished. If the node had become the active camera, it is not one any more. Not keyed; fine while the loop runs. |

### Scene nodes — $20-$2F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $20 | `CREATE_OBJECT` | 2 | `ARG0-1` mesh resource ID | Creates a node instancing that mesh, under the staged node `ID`. Re-creating over a live `ID` replaces it — same convention as the resource table's re-upload. `OUT_OF_MEMORY` if the 256-node table is full. |
| $21 | `CREATE_CAMERA` | 0 | — | Creates a camera node under the staged `ID`. Same replace-on-live-ID and `OUT_OF_MEMORY` behaviour as `CREATE_OBJECT`. |
| $22 | `DESTROY_NODE` | 0 | `ARG15` = `$A5` | Destroys the staged `ID`. Keyed — see "The key byte"; `BAD_ARGS` without it, and nothing is destroyed. `BAD_ID` if it does not exist. Destroying the active camera clears it — a later node reusing that same numeric `ID` is not silently treated as the camera again. |
| $23 | `SET_ACTIVE_CAMERA` | 0 | — | The staged `ID` becomes the camera `DRAW_NODE` and the loop render from. `BAD_ID` unless it names a live camera node. |
| $24 | `SET_VISIBLE` | 1 | `ARG0` 0 or 1 | Skips the node without destroying it. Any other `ARG0` value is `BAD_ARGS`. |
| $25 | `CREATE_SPRITE` | 2 | `ARG0-1` texture resource ID | Creates a billboard node under the staged `ID`. Starts one world unit square, masked and screen-upright, so a sprite you never call `SET_SPRITE` on is still a sprite you can see. Same replace-on-live-`ID` and `OUT_OF_MEMORY` behaviour as `CREATE_OBJECT`. |
| $26 | `CREATE_LIGHT` | 0 | — | Creates a point-light node under the staged `ID`, **off**: strength and radius both start at zero and it lights nothing until `SET_POINT_LIGHT` gives it both. Position it with the ordinary transform opcodes — it is a node like any other. |
| $27 | `SET_SPRITE` | 5 | `ARG0-1` width, `ARG2-3` height (8.8 world units), `ARG4` flags | `BAD_ID` unless the staged `ID` is a live sprite node. Flags: bit0 directional (`ARG0` of `CREATE_SPRITE` then names the **first of eight** consecutive texture IDs and the view drawn follows the camera, exactly as class 2's `DRAW_THINGS` does), bit1 depth-test but do not write, bit2 opaque (index 0 is a colour, not a hole), bit3 unlit. |
| $28 | `SET_POINT_LIGHT` | 3 | `ARG0` strength, `ARG1-2` radius (8.8 world units) | `BAD_ID` unless the staged `ID` is a live light node. Strength is in colormap levels added at the light's centre, falling off to nothing at `radius`. **Either being zero turns the light off** without destroying the node — which, with `SET_VISIBLE`, is the cheap way to switch one. |

### Transforms — $30-$3F

All act on the staged node `ID`. Every node type carries a
position/orientation, so these are how a camera is aimed, a sprite is put
somewhere and turned to face a direction (its yaw is what the directional
flag reads), and a light is moved — there is no separate "set light
position" opcode. `SET_SCALE` is the exception: it is used only by object
nodes, since a sprite is sized by `SET_SPRITE` and a light has no size.

Outside the loop these write into the retained node table, not a shadow copy
— an immediate-mode `DRAW_NODE` sees the change on its very next call. While
the loop is running they are redirected to the shadow scene and take effect
at the next `SCENE_COMMIT`; see Frame lifecycle.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $30 | `SET_POSITION` | 12 | x, y, z (16.16) | Absolute, world space. |
| $31 | `SET_ORIENTATION` | 6 | yaw, pitch, roll (binary angle) | Absolute — replaces, does not compose with, whatever was there. |
| $32 | `MOVE_LOCAL` | 6 | dx, dy, dz (8.8) | Translate along the node's own axes, i.e. the delta is rotated by the node's current orientation before being added — "forward 1.5" means forward for *this* node, however it's currently facing. |
| $33 | `MOVE_WORLD` | 6 | dx, dy, dz (8.8) | Translate along world axes — added directly, no rotation. |
| $34 | `ROTATE_LOCAL` | 6 | dyaw, dpitch, droll (binary angle) | Added to the current yaw/pitch/roll independently, each wrapping mod 65536 — "add a turn rate and let it wrap", same as any other binary-angle field. |
| $35 | `SET_SCALE` | 2 | s (unsigned 8.8) | Uniform. Zero is `BAD_ARGS` — same rejection `DRAW_MESH` makes at draw time, caught here instead so the opcode that caused it is the one that fails. |
| $36 | `GET_TRANSFORM` | 6 | `ARG0-5` destination descriptor | Writes 18 bytes — position (12, s32 16.16 x/y/z) + yaw/pitch/roll (6, u16 each), all little-endian — back to C64 RAM or REU, for collision and gameplay logic. Reads back the accumulated angles, including anything `ROTATE_LOCAL` has added since the last `SET_ORIENTATION`. Destination `len < 18` is `BAD_ARGS`. |

### Immediate mode — $40-$4F

Legal only with the loop stopped. While the autonomous loop runs, all three
answer `BUSY` — it owns the framebuffer and the scene for as long as it is
running.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $40 | `CLEAR_VIEWPORT` | 0 | — | Fills the viewport with the background index and clears the z-buffer. Call this once per frame before drawing — nothing else clears the z-buffer for you (see Depth buffer below). |
| $41 | `DRAW_MESH` | 14 | `ARG0-5` position x,y,z (8.8), `ARG6-11` orientation, `ARG12-13` scale; mesh resource in `ID` | Transforms, lights, clips and rasterises one mesh into the draw page's viewport, z-tested. Queues on core 1 and returns immediately — see "Deferred RESULT" below for what that means for reading the outcome back. Ignores the scene graph entirely — draws in world space unless a camera has been applied by a preceding `DRAW_NODE` call this session (`SET_PERSPECTIVE`'s projection still applies either way). `RESULT` = triangle count drawn, saturated to a byte — **a mesh that draws 0 triangles and a winding-order mistake that culls every face look identical**, so check `RESULT` after your first upload of any new mesh. |
| $42 | `DRAW_NODE` | 0 | — | Draws the staged object node `ID` using its retained position/orientation/scale — no re-staging a transform, unlike `DRAW_MESH`. Queues on core 1 and returns immediately, same as `DRAW_MESH` — see "Deferred RESULT" below. Applies the active camera (if any) fresh on every call, so moving the camera between two `DRAW_NODE`s in one frame is seen by both — "every call" means every call as actually run on core 1, in the order queued, not the order a deferred `RESULT` is later read. `BAD_ID` if the staged `ID` is not a live object node (a camera `ID` included) — this much is still checked on core 0 before the call returns, since it can be. An invisible node (`SET_VISIBLE 0`) is skipped silently: `RESULT` = 0, `ERRCODE` = `OK`, same "0 is informative, not an error" convention `DRAW_MESH` uses for full culling. |

### The world — $60-$6F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $6C | `WORLD_TICK` | 6 | `ARG0-5` descriptor of a block in C64 RAM or REU | One block a frame for a whole game. It poses up to 32 existing nodes (absolute position, yaw/pitch/roll, mesh or sprite texture, visible, view space), traces up to 24 shots or lines of sight, and moves up to 24 monsters through the level's hulls. Everything travels in the block, checked at both ends; see "A world in one block". `BAD_ARGS` and nothing changed if the block is malformed or does not check. `RESULT` = actor records refused, saturated at 255. Not keyed, not `BUSY` while the loop runs: the poses go to the shadow scene like `SET_POSITION`'s. Not strict-exempt. |

### Deferred RESULT

As of stage 15b (2026-08-29), `CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` queue
onto core 1 and return without waiting for their own draw to finish — that
overlap, not a stall on every one, is the point: a frame of `DRAW_NODE` calls
now queues and runs while the C64 goes on to its next command instead of
halting for each one in turn. The trade is that `ERRCODE` on return only
reflects what core 0 could check before queuing (a bad `ID`, no active
camera, an argument out of range) — never the draw's own outcome — and
`RESULT` is **not** updated to that draw's triangle count at return time.

`RESULT` is valid **as of the last observation point** instead: it catches up
to the most recently queued `DRAW_MESH`/`DRAW_NODE` the next time anything
forces the ring to finish draining, which happens automatically and often —
every other class 1 opcode does it before it runs (they mutate state a queued
draw also reads), and so does a `PAGE_FLIP`/`VBLANK_SYNC` and any class 0
opcode that touches the framebuffer. In practice this means: **the debugging
workflow "check `RESULT` after your first upload of any new mesh" still
works exactly as written** — issue the `DRAW_MESH`, then issue essentially
any other command (a `SET_*`, a class 0 `CLEAR`, a `PAGE_FLIP`) and read
`RESULT` — no dedicated "wait" opcode is needed because ordinary programs
already call something else next. Two things to not use for this: another
`DRAW_MESH`/`DRAW_NODE` does **not** reliably flush the previous one's
`RESULT` — the render ops only drain when the ring is full, not on every
call, so back-to-back draws can leave the first one's `RESULT` unread for
longer than one call; and an opcode that defines its own `RESULT`
(`ARENA_STATUS`, `UPLOAD_MESH`) overwrites the just-flushed value with its
own the instant it runs, so it never actually surfaces the draw's triangle
count even though it does force the wait. A program with truly nothing else
to issue should use `NOP` ($00, class 0) instead: it is genuinely
side-effect-free and forces the same wait — see `gpu64_3dSync()`'s comment
in `gpu64_3d.h`.

A queued-but-not-yet-drained ring is also why `QUEUE_FULL` is reachable for
`CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` far less often than it used to be:
pushing against a full ring waits (bus held) for core 1 to make room rather
than rejecting outright, with `WORKER_TIMEOUT` as the backstop against a
wedged core 1 — see the `ERRCODE` table below. A genuinely full ring only
still rejects outright for the other class 1 opcodes, which is the design's
"a failed dispatch does nothing" rule doing its job on a command with no
useful way to wait.

## Frame lifecycle

The autonomous loop (handshake mode, `LOOP_START ARG0=0`, live as of
stage 16) is the model the whole class 1 design has been building toward:
core 1 renders a whole frame on its own, every commit, instead of the C64
issuing each draw itself. The frame is, in order:

1. clear the viewport (colour and z-buffer),
2. apply the active camera,
3. apply the live point lights,
4. every visible `OBJECT` node, in scene order,
5. every visible `SPRITE` node, in scene order.

Objects before sprites, and not one interleaved pass, because sprites are
depth-tested against geometry that is by then already in the z-buffer. A
node whose mesh or texture ID does not resolve is skipped, not an error —
one stale ID must not blank an otherwise-good frame.

**`LOOP_START` is optional.** In handshake mode you never have to send it:
`SCENE_COMMIT` against a stopped loop starts it, with the same checks, and
answers `OK` without committing (see "A stopped loop repairs itself"). So the
opcode survives only as a **mode selector** — the place where you will one day
ask for free-running instead of handshake. The practical rule that falls out
of this: **never branch on whether the loop is running.** There is no register
that tells you, `SEQACK` does not cover the argument bytes that would carry
the answer, and a program that decides "the loop must already be started, so
I will not commit" simply stops drawing. Send `SCENE_COMMIT` every frame and
let it sort itself out.

**Starting it.** `LOOP_START` snapshots the live scene/render state into a
shadow copy and queues the first frame. From that point on, every opcode
that would normally mutate the live scene graph or per-frame render state
(`SET_VIEWPORT`/`SET_PERSPECTIVE`/`SET_LIGHT`/`BUILD_COLORMAP`/
`SET_BACKGROUND`, every $20-$28/$30-$36 scene-node opcode) instead writes the
**shadow** copy and returns immediately, without waiting on the ring at
all — that's what makes moving ten objects between two commits cheap: none
of those ten calls can race the frame core 1 is currently rendering, because
neither one touches what the other reads. `CLEAR_VIEWPORT`/`DRAW_MESH`/
`DRAW_NODE` (immediate mode) are refused with `BUSY` for as long as the loop
runs — they read/write the live scene/framebuffer directly, exactly what the
loop's own render is doing on core 1 at the same time.

**Each frame.** Move whatever needs to move with the ordinary scene-node
opcodes, then call `SCENE_COMMIT`. It publishes every shadow edit since the
last commit into the live scene (what the *next* frame renders from), arms
a page flip the same way `PAGE_FLIP` does (so the *just-finished* frame
actually reaches the screen), and queues the next frame on core 1 — all
without the C64 waiting for that next frame to actually finish rendering.
`RESULT` after `SCENE_COMMIT` returns is the page the frame just flipped to
was rendered into.

**Polling for the next one — wait on two bits, not one.** The predicate a
frame loop must poll is `FRAME_READY` **set and `BUSY` clear**:

```asm
    lda STATUS
    and #$11                ; FRAME_READY | BUSY
    cmp #$10
    bne poll                ; not ready, or a flip is still pending
```

`STATUS` bit4 (`GPU64_STATUS_FRAME_READY`) goes high once core 1 has finished
a frame **and** gpu64 has looked. It looks at two moments: inside the vblank
handler that retires the previous flip, and at the start of every command
dispatch. A frame that finishes *before* that flip retires is therefore seen
on the frame clock with no command traffic at all. A frame that finishes
*after* it — a heavy view that takes longer than one vblank — is seen only at
the next command. A read-only poll loop then waits for its whole timeout.

**While waiting, send a cheap command every ~2 ms.** Any class 1 command
harvests the frame; `SET_BACKGROUND` with the colour you already use
changes nothing and does not drain the ring. The Quake game does exactly this
(`waitReady`, `WR_PUMP_MS`): about 7 extra commands in a 45 ms wait.
A per-vblank harvest in firmware would not be better: it would raise the
bit only at the *next* vblank, so the commit would miss that vblank and the
frame would be shown one refresh later than with the pump.

Bit4 says nothing about the page flip the
*previous*
`SCENE_COMMIT` armed: that flip only retires at the next vblank, and until it
does, `SCENE_COMMIT` answers `BUSY` rather than arming a second flip over an
un-presented one. Bit0 (`GPU64_STATUS_BUSY`) is cleared by the same vsync
handler that clears the pending flip, so the two-bit test is exactly the
question "may I commit now?".

Polling `FRAME_READY` alone is not incorrect — it never commits against a
still-in-flight render — it is just wasteful: the loop spins issuing commits
that are refused, and each refusal costs a DMA hold that stops the 6510. At
the bench on 2026-09-08 the Quake demo drew 6099 accepted frames against
15838 `BUSY` answers, 72% of its commit traffic thrown away, while still
rendering at the full 60 Hz the flip rate allows.

**Stopping it.** `LOOP_STOP` with `ARG15` = `$A5` stops the loop. A frame
already queued when it arrives still finishes on core 1 — there is no way
to cancel a frame in flight — but nothing further reads its result once the
loop is stopped. `SCENE_RESET` also stops the loop (and drops the shadow
copy) as part of destroying every node, and takes the same key.

**The key byte.** Four opcodes are *destructive* — they throw away something
the firmware cannot reconstruct — and all four demand `ARG15` = `$A5` on the
same dispatch, answering `BAD_ARGS` and changing nothing without it:

| Opcode | What an unwanted one destroys |
|---|---|
| `$00 SCENE_RESET` | every node, every transform, the shadow copy, the loop |
| `$07 LOOP_STOP` | the loop, i.e. the picture |
| `$22 DESTROY_NODE` | one node — and if it is the active camera, the ability to render at all |
| `$12 FREE_RESOURCE` | an uploaded mesh or texture |

This is not ceremony. Register writes from the C64 are occasionally
mis-sampled, and a corrupted `CMD_LO` is the one way an opcode your program
never sent can reach the firmware. It has happened twice at the bench, both
on 2026-09-10: a `$07` nobody sent ended one session, and in the next run
the live scene's active camera disappeared and every `LOOP_START` after it
answered `NO_CAMERA` for the rest of the run. Requiring a second, specific
byte means a single bad sample can no longer do either.

The key is **one-shot**. Every dispatch of every class reads `ARG15` and
immediately clears it, so it authorises exactly the command it was written
for; the next command starts with it at `$00` again. Write it as part of
staging the arguments, in the same place you write `ARG0`:

```asm
        lda #$a5
        sta $df65               ; ARG15 -- the key
        lda #$07                ; LOOP_STOP
        sta $df51               ; CMD_LO -- and it is spent here
```

`ARG15` is read by no opcode for anything else, which is what makes it the
right register: `ARG0` is not, because the per-frame node stream writes it
constantly and a phantom arriving mid-frame could find a coordinate byte
that happens to be `$A5` already sitting there. The key applies whether or
not the loop is running, so scene setup needs it too.

`GET_HEALTH` bytes 76-79 report what the gate caught: 76 counts refusals,
**77 is the opcode of the last one** — the name of the phantom — and 78/79
count the damaging events that did get through (active camera lost, scene
wiped while built).

**The check byte (optional).** Any command, of any class, can carry its
own check, and the firmware refuses it if its bytes did not arrive as sent.
Write `ARG15` = `$D0 | n` and `ARG14` = the XOR of:

- that `ARG15` byte,
- `CMD_HI`, `CMD_LO`, `ID_LO` and `ID_HI`,
- `ARG0` to `ARG(n-1)`.

`n` is how many argument bytes the command writes, `0` to `14`. It is part
of the key because arguments the command does not write still hold the
previous command's bytes, and your program cannot vouch for those. If the
sum does not match, the command is refused with `BAD_ARGS` before any class
sees it: nothing executes. A flipped `CMD_LO` or `CMD_HI` is refused too,
because both are in the sum.

```asm
        ; SET_POSITION ($30) on node $0001, 12 argument bytes in pos
        lda #$d0 | 12
        sta key
        eor #1                  ; CMD_HI
        eor #$30                ; CMD_LO
        eor #$01                ; ID_LO
        eor #$00                ; ID_HI
        ldx #11
-       eor pos,x
        dex
        bpl -
        sta $df64               ; ARG14 -- the check
        lda key
        sta $df65               ; ARG15 -- spent by this dispatch
        lda #$30
        sta $df51               ; CMD_LO
```

Why it exists: writing every byte twice defeats a write the bus loses, but
not one it delivers with a bit flipped, because the second copy is believed
just as well. On a retained scene a flipped position byte is a camera 128
units away for a frame, or a door in the wrong place until something re-sends
it. That is the see-through flicker the E1M1 game showed at the bench. A
command that is **absolute** (a position, an orientation, a node repair) can
simply be sent again on `BAD_ARGS`, so put the check on those and retry a few
times. `SCENE_COMMIT` has no `ID` or arguments, so write `ID_LO` = `ID_HI` =
`0` and its check is the constant `$D0 ^ 1 ^ $08` = `$D9`.

The check shares the one-shot `ARG15` with the destructive key, so a single
command carries one or the other, never both. A command without it runs
unchecked, exactly as before, and so does a checked command sent to firmware
that predates the check. What it cannot catch is the `ARG15` write itself
being lost or flipped out of the `$D0` range: the command then runs
unchecked.

**Strict mode: checking once means checking always.** The bus can also
deliver a `CMD_LO` write your program never made, and that phantom runs on
whatever the argument registers still hold from the previous command. At the
bench a phantom `SET_PERSPECTIVE` ran on a `SET_POSITION`'s coordinates:
the field of view jumped, and near a certain spot the near plane landed tens
of units out and whole frames went black. A phantom never carries a check, so
the firmware uses that. The first checked `SCENE_COMMIT` switches strict mode
on, and from then on a class-1 command **without** a check is refused with
`BAD_ARGS`, with these exceptions:

- commands carrying the `$A5` destructive key;
- `LOOP_START` ($06), `ARENA_STATUS` ($09), `CLIP_MOVE` ($15),
  `LEVEL_ENT` ($16) and `GET_TRANSFORM` ($36). These only read state or are
  retried by design, so a phantom one does no lasting harm.

`WORLD_TICK` ($6C) is **not** exempt, because it moves nodes. In strict mode,
send it with the check like any `SET_*`.

Strict mode also guards three class-0 opcodes. While the loop is running
and a `SCENE_COMMIT` succeeded in the last half second, `LOG_ENABLE` ($07),
`VBLANK_SYNC` ($09) and `FULL_RESET` ($0B) are refused with `BAD_ARGS`
unless they carry the `$A5` key or a check. A phantom `FULL_RESET` blanks
every page and the border and stops the loop, which looks like the monitor
switching off for half a second; the bench saw exactly that. A program that
takes over from a strict one is unaffected, because it sends its
`FULL_RESET` after commits have stopped.

A keyed `SCENE_RESET`, a `FULL_RESET` or a C64 reset switches strict mode
off again. A program that never checks its commits is unaffected. A program
that checks its commits must check every other class-1 command it sends.

**A stopped loop repairs itself.** If a `SCENE_COMMIT` arrives while the
loop is not running, it re-starts the loop — the same checks `LOOP_START`
makes, the same shadow reseed — and answers `OK`. It does not commit that
frame; poll `FRAME_READY` and commit again as usual. The loop is therefore
running for exactly as long as commits keep arriving, and a stop your
program did not ask for costs one frame instead of the session. A deliberate
teardown is unaffected, because a program that has sent `LOOP_STOP` sends no
more commits. `GET_HEALTH` bytes 80-83 count the repairs.

**Confirm that it landed.** `LOOP_STOP` succeeds *if it arrives with its
key*, and
the write that carries it is subject to the same sampling defect as every
other — measured at roughly one command in a few hundred, and once at one in
64. A lost `LOOP_STOP` leaves the loop running after your program has exited,
still rendering its scene over whatever runs next, and nothing reports it.
Send it with a sequence number and re-send until `SEQACK` confirms: unlike
`SCENE_COMMIT`, `LOOP_STOP` is idempotent, so a retry that follows one that
did land costs nothing — but **re-write `ARG15` inside the retry loop**: the
key is spent by the attempt that failed, as well as being as droppable as
any other write, so a retry that does not re-stage it reads back as
`BAD_ARGS`. Check both answers, too: `SEQACK` says the command was *seen*,
and `ERRCODE` says it was *obeyed*. `GET_HEALTH` byte 87 reads back `1` once
a `LOOP_STOP` opcode has actually executed, which is the independent check.

**No 2D overlay while the loop runs.** The loop owns the framebuffer, and
in handshake mode there is no moment between its frames at which the C64 may
draw into the page that is about to be shown: a class 0 framebuffer op
issued against a running loop is racing core 1's render of that same page.
So a HUD, a weapon sprite, a status bar — anything that used to be drawn
over a finished 3D view with class 0 — cannot be done this way. Put it in
the scene as a view-space node (see [View space](#view-space-the-gun-and-the-status-bar)),
which is how the Quake game draws its gun and status bar, or keep it on
the C64's own screen. This is a real limitation of the stage-16 loop,
not an oversight; immediate mode (`DRAW_MESH`/`DRAW_NODE` with the loop
stopped) has no such restriction and can still be overlaid freely.

The firmware enforces this: while the loop runs, every class 0 op that
touches the display (`RESET_STATE`, `VBLANK_ARM`/`ACK`, `SET_DRAW_PAGE`,
`PAGE_FLIP`, `SET_BORDER`, drawing, palette, blits and text mode) answers
`BUSY` and does nothing. `NOP`, `GET_INFO`, `LOG_ENABLE`, `VBLANK_SYNC`,
`GET_HEALTH`, `FULL_RESET`, `SET_DMA_WINDOW` and the matrix ops stay legal.

**What free-running mode (`LOOP_START ARG0=1`) will add**, once built: the
same loop driven by vblank instead of by `SCENE_COMMIT` — the C64 never
calls anything per frame at all, it just edits the shadow state whenever it
likes and the loop picks up whatever was last written each vsync. Not yet
implemented; `ARG0=1` is `UNSUPPORTED`.

## What retaining the scene actually costs

`games/quake/gpu64_demo_quake.a` and `games/quake/gpu64_demo_quake3d.a`
are the same room, the same monsters and the same controls, written twice:
once in class 2's immediate mode, once as a retained class 1 scene driven
by the handshake loop. The second is the port stage 17 asked for, and it
exists so that "retained is cheaper on the bus" is a measurement rather
than an argument.

The figures below come from `tools/prgsim`, which counts every dispatch and
every write into the `$DF50-$DF68` window. They are **marginal** per-frame
costs — a 400-frame run subtracted from an 800-frame one — so one-time
start-up traffic is out of them. That start-up is smaller than it sounds:
about 60 dispatches and 570 register writes for class 1 (against class 2's
45 and 450) to create every node and upload the mesh, ten textures and the
colormap, because bulk data moves by REU DMA — the C64 writes a blob
descriptor, never the bytes. "Moving" holds two keys down so the camera
changes on every frame; "standing still" touches nothing.

| Per frame | class 2, immediate | class 1, retained |
|---|---|---|
| moving — dispatches | 11 | 6 |
| moving — register writes | 102 | 70 |
| standing still — dispatches | 11 | 4 |
| standing still — register writes | 102 | 42 |

Class 2 pays the same on every frame because it redraws: camera, two
lights, `FILL_VIEW`, `DRAW_WORLD`, `DRAW_THINGS`, `STATS`, the weapon
sprite, the status bar and the flip go out whether or not anything moved.
Class 1's six are the camera's position and orientation, the orientation of
the monster that turns, the position and orientation of the monster that
walks, and `SCENE_COMMIT`. Everything else — thirty triangles, ten
textures, the directional light, the torch, and every transform that did
not change — is resident and is never sent again.

**Read the table honestly in two places.**

*It is not a like-for-like comparison of 3D work.* Part of class 2's
per-frame total is class 0: the status bar and HUD text it draws onto the
HDMI view, which class 1 cannot do at all while the loop owns the
framebuffer (see above). Split by class, class 2's frame is **7 dispatches
and 74 register writes of class 2 rendering, plus 4 and 28 of class 0
overlay**; class 1's is class 1 throughout. So against the rendering half
alone the retained scene is only slightly ahead while the player is moving
(6 / 70 against 7 / 74) and properly ahead when he is not (4 / 42) — the
headline 11 → 6 is in large part the HUD moving to the C64's own screen
rather than traffic that vanished. Say it that way round.

*The number that matters most is not in the table.* The C64 is DMA-halted
for the whole of every dispatch, and only for a dispatch: `waitReady`'s
poll of `STATUS` bit 4 is a plain read that costs nothing. So the dispatch
column is also the count of times per frame the C64 stops dead — and the
frame those six commands describe is rendered on core 1 while the C64 keeps
running, which is the part immediate mode cannot do at any price.

*The room predates view space.* Its class 2 HUD had no class 1 equivalent
when it was measured, so its class 1 port left the HUD out. A view-space
node now draws a HUD inside the scene; see
[Porting a class 2 program](#porting-a-class-2-program) for what a full
game costs.

## Porting a class 2 program

This section maps each class 2 idea to its class 1 equivalent and lists
the extra work a retained scene asks of your program. The worked example
is the Quake game (`games/quake/gpu64_demo_game.a`), written for class 1
from the start. It does everything the class 2 Quake demo did and much
more.

**Do not port column-cast programs.** `DRAW_COLUMNS`, `DRAW_WALLS` and
`DRAW_SECTORS` have no class 1 equivalent and will not get one. The
Doom-style demos stay on class 2, which is frozen but not removed.

### Construct by construct

| Class 2, every frame | Class 1, once, then only on change |
|---|---|
| `FILL_VIEW` to start the frame | Nothing. The loop clears the view and depth buffer itself. |
| `SET_CAMERA3D` | A camera node: `CREATE_CAMERA`, then `SET_POSITION` / `SET_ORIENTATION` when the eye moves. |
| `UPLOAD_POLYS` + `DRAW_WORLD` over the visible range | A mesh uploaded once and an object node per piece, or the whole level with `LOAD_LEVEL` / `LEVEL_STEP`. The Pi culls by Quake's PVS itself, so there is no visible range to compute. |
| `DRAW_THINGS` batch | One `SPRITE` node per thing (`CREATE_SPRITE`, `SET_SPRITE`). Directional things use flag bit 0. A thing that walks costs one `SET_POSITION`. |
| `SET_LIGHT` slot | A `LIGHT` node (`CREATE_LIGHT`, `SET_POINT_LIGHT`), switched with `SET_VISIBLE`. |
| Class 0 HUD drawn over the view | A view-space node (pose flag `$02`), unlit. Class 0 drawing is `BUSY` while the loop runs. |
| `PAGE_FLIP` + vblank wait | `SCENE_COMMIT`, paced by `STATUS` bit 4 (`FRAME_READY`). |
| Many movers, sent one by one | One `WORLD_TICK` block per frame (`$6C`): actors, movers, line-of-sight traces and monster steps together. |

### What a retained scene asks of you

Immediate mode repairs itself: a lost write spoils one frame, and the next
frame sends everything again. A retained scene keeps a lost write
**forever**, because nothing re-sends it. That shifts some work onto your
program.

- **Pace on `FRAME_READY`.** Commit once per ready frame, and poll
  `STATUS`, which is a plain read that does not halt the C64. If you commit
  flat out, about four commits in five are refused with `BUSY`.
- **Check `SEQACK` on every node update, and resend the whole command.**
  Do not retry `SCENE_COMMIT` itself; a commit that did land would answer
  `BUSY`.
- **Refresh from a shadow copy.** Keep your own copy of every retained
  value, and rewrite a few entries of it each frame, round-robin. That
  gives any lost write a bounded lifetime. See
  [state-refresh.md](state-refresh.md).
- **Key the destructive opcodes.** `SCENE_RESET`, `LOOP_STOP`,
  `FREE_RESOURCE`, `DESTROY_NODE` and `LOAD_LEVEL` need `ARG15` = `$A5`
  (see "The key byte"). A phantom one would otherwise wipe the scene.
- **Bound every answer you read back.** A dropped argument byte can make a
  well-formed reply mean the wrong thing. Range-check it before you act on
  it.

### What it costs

The measurement below is the Quake game on E1M1, from `tools/prgsim`. It
is marginal: a 200-frame run subtracted from a 400-frame one, at a fixed
32 ms frame.

| Per frame | Dispatches | Register writes |
|---|---|---|
| Class 1 | 7.9 | 90 |
| Class 0 (`GET_HEALTH`) | 0.14 | 0.9 |

The traffic is the same whether the player stands still or moves. That is
deliberate: the game spends its bus budget on robustness rather than
saving it.
- The camera goes out twice a frame, so one lost write cannot move the eye.
- Two refresh rings each rotate one entry a frame.
- The `WORLD_TICK` block carries every monster, door and trace.

Compare the class 2 room: 11 dispatches and 102 writes per frame, for one
room, two monsters and no doors, and every one of those frames halted the
C64 while it was drawn. The class 1 game draws a whole Quake level with
its monsters, doors, lights, gun and status bar on core 1, and the C64
keeps running while it does.

## Mesh format

The C64 never touches mesh bytes — it points a blob descriptor at data
already sitting in RAM/REU, built by an offline exporter. Two blobs, fitting
the standard two-descriptor `ARG` layout used by `UPLOAD_MESH`:

```
blob 0 — vertices, 6 bytes each, at most 256 per mesh
    x, y, z            signed 8.8, model space (model fits ±128 units)

blob 1 — faces, 12 bytes each, triangles only
    i0, i1, i2         1 byte each, index into blob 0
    u0,v0, u1,v1, u2,v2  1 byte each, per-corner texcoords
    texid              1 byte, low byte of a texture resource ID
    flags              1 byte: bit0 double-sided, bit1 flat-colour
                       (texid is a palette index instead), bit2 unlit
    pad                1 byte, unused; write 0
```

Vertex and face counts are implied by each blob's `len` (`len/6`, `len/12`);
a `len` not a multiple of the stride is `BAD_ARGS`.

Notes that affect how you author or export a mesh:

- **Triangles only** — a quad exporter must triangulate. Two triangles cost
  one extra index byte and one extra flags byte versus a quad primitive; the
  format trades that for a single rasteriser path.
- **UVs are per face-corner, not per vertex.** This is required for boxy
  geometry — a cube corner needs three different UVs for the three faces
  meeting there. Don't try to dedupe vertices across faces with different
  UVs; the format already expects one UV set per triangle corner.
- **Normals are computed GPU-side from winding order at upload time** — you
  do not supply them, and there is no smooth-shading mode. Every mesh is
  flat-shaded per face.
- **256 vertices is per mesh, not per scene.** An index byte is relative to
  that mesh's own vertex blob. Split large geometry across multiple meshes
  rather than trying to raise this limit — it also makes per-object culling
  (once frustum culling exists) actually useful, since a giant single mesh
  is all-or-nothing against the frustum.

| Limit | Value | Set by |
|---|---|---|
| Vertices per mesh | 256 | the 1-byte face index |
| Meshes and textures resident | thousands | flat 16-bit resource ID space, 512 MB resource RAM (32 MB in the phase-1 arena) |
| Object instances in the scene | 256 | the scene table |

## Texture format

Power-of-two dimensions, 8 to 256 px on each side, not necessarily square,
8bpp palette indices — the same palette as class 0 and class 2. Power-of-two
is what lets the rasteriser wrap `u`/`v` with a mask instead of a modulo.
256 is the ceiling the byte-sized face-record UVs already imply; any other
size is `BAD_ARGS`.

## Resource lifecycle

- **Re-upload to a live ID replaces it** — implicitly frees the old
  allocation first, so "reload this texture" needs no free-then-upload
  dance.
- **`FREE_RESOURCE` is explicit; nothing is auto-evicted.**
- **All resources are freed on a session reset** (RUN/STOP+RESTORE or
  equivalent) — don't assume an ID survives across program runs.
- **IDs are one flat 16-bit namespace** shared by every resource kind
  (textures, meshes). IDs are chosen by the C64 side; nothing detects two
  pieces of code picking the same ID.
- **Phase 1: the arena is bump-only.** `FREE_RESOURCE` and re-upload reclaim
  the resource table's slot but not the underlying bytes — the arena itself
  only comes back at session reset. A program that re-uploads the same
  resource in a loop (e.g. streaming texture updates) *will* run the 32 MB
  arena dry and get `OUT_OF_MEMORY`, which is a truthful error, not a bug to
  work around. Use `ARENA_STATUS` ($09) to watch the free space if your
  program uploads/re-uploads a lot in one session.

## Loading a level from the REU image

A level is far too large to upload through the register window — E1M1 is
620 KB of textures, meshes and world nodes — and it does not have to be.
Your game ships as a pair: the `.prg` and a `.reu` image holding a **level
pack** (one or more `.g64lev` files). The player selects the `.reu` as the
REU image in the RAD menu, then runs the `.prg`, and two opcodes turn a
level of that pack into a live scene. gpu64 reads levels and pictures from
nowhere else: there is nothing on the Pi's card for you to install.

```
        ; --- begin ---
        lda #<1000  : sta ARG+0     ; mesh resource IDs start here
        lda #>1000  : sta ARG+1
        lda #<100   : sta ARG+2     ; world node IDs start here
        lda #>100   : sta ARG+3
        lda #1      : sta ARG+4     ; create the camera as ID 1
        lda #0      : sta ARG+5
        lda #$a5    : sta ARG+15    ; LOAD_LEVEL is destructive
        cmd $13

        ; --- and then step it until it is done ---
step    cmd $14
        bne failed                  ; ERRCODE
        lda RESULT
        cmp #$ff
        bne step                    ; RESULT is percent complete: draw a bar
```

E1M1 takes **29 steps**. Each one spends a bounded budget of work and returns,
so the C64 is halted for a normal command's worth of time and not for the
whole multi-second build; `RESULT` is there so you can show a loading bar
while it happens.

Four things the split implies:

- **The pack is checked when the REU image is loaded, not at `LOAD_LEVEL`.**
  If the image is not a valid level pack, `LOAD_LEVEL` answers `BAD_ARGS` —
  and the Pi's log says why the pack was refused. If the player selected
  another game's image, the slots hold that game's levels: check that the
  image is yours (a `LEVEL_PICTURE` of a picture only your pack has, or your
  own marker in REU memory) before relying on it.
- **`LOAD_LEVEL` resets the arena, the resource table and the scene**, so a
  second level in one session does not leak the first one's 20-odd MB. It does
  **not** touch your viewport, field of view or clip planes: set those up
  before the load, because the view you want exists before the level does.
- **Texture IDs are not yours to choose.** They are forced to `0..ntex-1`,
  because a mesh face carries its texture as one byte. Mesh and node ID bases
  are free; pick them clear of `0..ntex-1` and of each other.
- **The camera comes from the level.** The file's header carries a start
  position and yaw (for Quake, the converter takes them from
  `info_player_start`), so gpu64 creates the camera there, at the header's
  eye height, facing the right way, and makes it active. Pass 0 for the
  camera ID if you would rather place your own; a level whose header has no
  start answers `BAD_ARGS` unless you do.
- **Neither opcode works while the loop is running.** Both answer `BUSY`.
  Load first, then `LOOP_START` — or `LOOP_STOP` before reloading.

### More than one level: slots and the level pack

`LOAD_LEVEL` loads **slot N** when `ARG6` = N and `ARG7` = N EOR `$5A`.
Without that pair, for example when `ARG6/7` still hold another command's
arguments, it loads slot 0. A slot with no level answers `BAD_ARGS`.

```
        lda #1      : sta ARG+6     ; slot 1
        lda #1^$5a  : sta ARG+7     ; ...confirmed
```

The slots come from the pack, up to 8 levels. Build the REU image with
`tools/pack_levels.py game.reu E1M1=e1m1.g64lev E1M2=e1m2.g64lev`; the
levels become slots 0, 1, … in the order you list them. RAD sizes the REU
from the file. The Pi uses each level directly from REU memory, so **your
program must not DMA into that part of the REU** — the rest of the REU,
past the pack, is yours.

If a pack fails its checksums, none of it is used and every slot is empty.
The Pi's log lists every slot, or says why the pack was refused.

A loaded level also culls itself. The file carries Quake's own
potentially-visible set, baked down to the level's world nodes, and every
frame gpu64 finds the leaf the active camera stands in and skips the world
nodes that cannot be seen from there. Nothing in the API changes and nothing
is asked of you. What it covers:

- **Only the level's own world nodes**, and only while each one still
  instances its own level mesh. Doors and other brush movers, your actors,
  sprites and anything you created are always drawn.
- **A camera outside the map culls nothing**: everything is drawn, as
  before.
- **A level file without the section**, or one whose world nodes were
  renumbered, loads and draws everything.

### Pictures

A level file can also carry 2D pictures: the art for a title screen, a menu
and an intermission. A 320x200 background is 64 KB, which is more than a C64
has to send it from, so the pictures travel in the REU image with the level,
and `LEVEL_PICTURE` ($1A) draws one by index onto the class 0 draw page.

```
        lda #PIC_CONFADE : sta ARG+0    ; which picture
        lda #0 : sta ARG+1 : sta ARG+2  ; x
        sta ARG+3 : sta ARG+4           ; y
        lda #2 : sta ARG+5              ; opaque, level palette first
        cmd $1a
        ...                             ; more pictures, keyed (ARG5 bit 0)
        cmd class0 PAGE_FLIP
```

- **The indices come from the converter.** `tools/gen_quakelevel.py --inc`
  writes them into your include as `PIC_*` symbols (`PIC_CONBACK`,
  `PIC_LOADING`, `PIC_NUM_0`…), and they are part of the catalogue hash, so a
  program built against another conversion refuses the file instead of
  drawing the wrong picture.
- **Pixels are palette indices.** Bit 1 of `ARG5` installs the level file's
  palette first, so a title that has been cycling colours gets Quake's back
  without a level loaded. Index 255 is the hole in Quake's own `.lmp` art, and
  a keyed draw (bit 0) leaves it untouched.
- **It is a title-screen command.** It answers `BUSY` while the loop runs,
  because the loop owns the pages. Draw your intermission after `LOOP_STOP`.
- **The format is versioned.** A firmware accepts one level format — v8,
  whose header also carries the player start and body sizes — and refuses
  any other, so a pack built for an older firmware has to be rebuilt.

Nothing above stops the camera leaving the map — placing it is your job, and
`CLIP_MOVE` below is how you ask whether a place is legal.

## Walking in a level

`CLIP_MOVE` ($15) is one question: *I am here, I would like to move by this
much, where do I actually end up?* gpu64 answers it against the same Quake
clip hulls the level file carries, and then forgets it — **the C64 owns the
player's position**. gpu64 never moves the camera on your behalf, which is
why you send an absolute position every time rather than a running delta: one
lost register write to a delta protocol is a permanent displacement nothing
later contradicts.

Everything travels in one 56-byte block rather than in the registers (plus 16
bytes per door you name — see "Doors that block" below). It does not fit in
them — a position and a displacement in 16.16 are 24 bytes by themselves — and
a block is one DMA burst with a checksum where two dozen register writes would
be two dozen independent chances to lose one.

```
        ; the block lives anywhere in C64 RAM (or the REU)
        lda #0      : sta ARG+0     ; space: 0 = C64, 1 = REU
        lda #<blk   : sta ARG+1
        lda #>blk   : sta ARG+2
        lda #0      : sta ARG+3
        lda #56     : sta ARG+4
        lda #0      : sta ARG+5
        cmd $15
        ; ERRCODE here is a diagnostic, not the verdict -- see below. If the
        ; block really was rejected the Pi wrote nothing, so the magic test
        ; on the output half catches it too, and that test cannot be fooled
        ; by a garbled register read.
```

| Bytes | Dir | Meaning |
|---|---|---|
| 0-11 | in | `start` x, y, z — s32 16.16 world units, gpu64 axes, y up |
| 12-23 | in | `delta` x, y, z — the displacement you want, same format |
| 24 | in | hull: 1 = player, 2 = the larger hull. Anything else is `BAD_ARGS` |
| 25 | in | mode, below |
| 26 | in | model: 0 = the world, or a brush model to move *instead of* the world |
| 27 | in | magic `$C5` |
| 28 | in | check: XOR of bytes 0-27, byte 29, and every mover byte |
| 29 | in | movers: how many 16-byte mover records follow, 0-16 |
| 30-31 | — | reserved, not read, not summed |
| 32-43 | out | `end` x, y, z — where the move really ended |
| 44 | out | flags, below. Also copied to `RESULT` |
| 45 | out | contents at the **feet** of `end` (the level header's feet depth below the origin — 23 units for a Quake level), from the level's point hull, so water, slime and lava are reported: 0 empty, 1 solid, 2 water, 3 slime, 4 lava, 5 sky, 255 unknown |
| 46 | out | fraction 0-255 of the *horizontal* displacement actually covered |
| 47 | out | bumps: slide iterations used, 0-4. Diagnostic |
| 48 | out | magic `$5C` |
| 49 | out | check: XOR of bytes 32-48 |
| 50 | out | water level, as Quake's `waterlevel`: 0 dry, 1 feet in a liquid, 2 waist deep (swim), 3 eyes under (drowning). The liquid is byte 45 |
| 51 | out | `$FF` minus byte 50. Bytes 50-51 are **outside** the checksum: take byte 50 only if byte 51 is its complement, and keep last frame's value otherwise. Firmware older than the water level answers 0/0, which fails the test |
| 52-55 | out | zero |

**Mode** (bitwise): `$01` slide along walls instead of stopping dead, `$02`
step up over obstacles up to the level header's step height (18 units for a Quake level), `$04` settle onto the floor and
report standing, `$08` the position you send and get back is the **eye**, not
the feet — gpu64 subtracts the header's eye height (22 units for a Quake level) on the way in and adds it
back on the way out, so you can hand it your camera's position unchanged.
`$07` (slide + step + floor) is ordinary walking; `$0f` is ordinary walking
with a camera-height position. A swimmer drops the floor settle (`$0b`):
the settle traces one step height down and would pin them to the bottom.

**Flags**: `$01` standing on ground, `$02` hit a wall, `$04` hit a ceiling,
`$08` the move needed a step up, `$10` the *start* was already inside solid
(you are stuck — the move is refused, not attempted), `$20` the whole trace
was in solid, `$40` the move was cut short by the iteration limit rather than
by geometry.

Three things worth doing, all cheap:

- **Check the output magic and checksum**, not `ERRCODE`. `ERRCODE` is a
  single register read, and a register read that fails returns whatever was
  last on the bus rather than an error. The block's own `$5C` plus XOR is the
  channel that cannot lie to you; if it fails, call `CLIP_MOVE` again — the
  input half is untouched, so a retry is just the six register writes of the
  descriptor and the dispatch. **Retry rather than skipping the frame.** Poison
  only the output half between attempts, so the movers and the request survive.
  Three or four attempts is the right budget: gpu64's bench run 37 had 61% of
  first attempts rejected once it started sending sixteen mover records a
  frame, and a frame the player does not move is far more expensive than
  asking again.
- **Set `SET_DMA_WINDOW`** ($0C, class 0) around the block. `CLIP_MOVE` is the
  one class-1 opcode that writes to C64 memory every frame, and the window is
  what guarantees a dropped address byte cannot put those 24 bytes somewhere
  else — see [state-refresh.md](state-refresh.md).
- **Bound the answer by the question.** The magic and the checksum are the
  channel; this is the meaning. A move cannot end further from where it
  started than the displacement you asked for, plus the level's step height (18
  units for Quake) that step-up mode `$02` is allowed to add — so compare the returned position
  against your own start and reject anything outside that, exactly as you
  reject a bad checksum. It costs a handful of compares on the integer halves
  and it is the only check that is *exact*: an 8-bit XOR over seventeen bytes
  passes roughly one corrupted block in 256, and unlike a dropped frame a
  corrupted **position** is permanent, because you are the one who is
  authoritative about it and nothing will ever contradict you. gpu64's own
  bench run 36 lost a single byte of the x coordinate — `$000F` arriving as
  `$FF0F` — and spent the rest of the session 222 world units outside the
  map.

### Doors that block

A closed door is **not** part of the world's collision. The BSP compiler put
the doorway's hole in the world hull and the door panel in a brush model of
its own, so a trace that names only the world walks straight through every
door on the map. Say which doors are in the way, and where they currently are,
and the same trace is blocked by them:

| Bytes | Meaning |
|---|---|
| +0 | model: the brush model index, from `LEVEL_ENT`. 0 is an empty slot, skipped |
| +1 | flags: reserved, write 0 |
| +2-3 | reserved, write 0 |
| +4-15 | `ofs` x, y, z — s32 16.16, how far you have already opened it |

The first record is at byte **56**, i.e. immediately after the output half, so
`len` must be `56 + 16 × movers` and a block with no movers is unchanged from
before. `ofs` is the same displacement you passed to `SET_POSITION` on that
model's nodes: zero for a closed door, its full `travel` for an open one, and
anything in between while it is moving.

The list is an **argument, not state**. gpu64 does not know which of your
doors are open — you do — so you say so on every call. That is what keeps
`CLIP_MOVE` a pure function of its block, and therefore answerable while the
render loop is running. Sixteen movers is the limit; a model the level does
not have is `BAD_ARGS`, because a door that quietly stops blocking is how a
player leaves the map.

Gravity, jumping, friction and acceleration are **not** here. `CLIP_MOVE`
answers a geometric question; how fast the player falls is a game's decision,
and one the 6502 can make in a few bytes of arithmetic. Fall by calling it a
second time with a downward delta and looking at `$01`.

## What is in a level

`CLIP_MOVE` tells you where you may walk; `LEVEL_ENT` ($16) tells you what is
there. A level's entity list is where its *game* is — E1M1 has 369 entities
naming the player start, fourteen doors and the buttons that open them, the
triggers, forty-two monsters and every item on the map — and it is already in
the Pi's memory, because the level file carries it.

It does not travel to the C64 wholesale: 16 KB of records plus a string pool.
So you ask for one record at a time, by index, and keep the handful your game
cares about. `entCount` comes back on **every** call, so the loop is "read 0,
learn how many there are, read the rest".

The record is pre-digested for a 6502. There are no strings to compare: a
classname arrives as a `kind` byte you can switch on, `target`/`targetname` as
opaque 16-bit ids that are equal exactly when the strings were (the pool is
deduped, so `door.targetname == button.target` is one `cmp`, not a `strcmp`),
a door's travel as a ready displacement instead of Quake's `lip`/`angle`
formula, and a brush model's geometry as a contiguous run of scene node ids.

```
        lda #0      : sta ARG+0     ; space: 0 = C64, 1 = REU
        lda #<blk   : sta ARG+1
        lda #>blk   : sta ARG+2
        lda #0      : sta ARG+3
        lda #96     : sta ARG+4
        lda #0      : sta ARG+5
        cmd $16
```

| Bytes | Dir | Meaning |
|---|---|---|
| 0-1 | in | index: which entity, `0 .. entCount-1` |
| 2 | in | magic `$C6` |
| 3 | in | check: XOR of bytes 0-2 |
| 4-15 | — | reserved, not read |
| 16-27 | out | `origin` x, y, z — s32 16.16 world units, gpu64 axes |
| 28-39 | out | `mins` — the brush model's box, zero if it is not a brush model |
| 40-51 | out | `maxs` |
| 52-63 | out | `travel` — the closed→open displacement, zero if it is not a mover; for a working `trigger_teleport`, the destination's eye position |
| 64-65 | out | yaw, u16, a full turn is 65536 |
| 66-67 | out | spawnflags |
| 68-69 | out | `param0` — s16; which Quake key this is depends on the kind |
| 70-71 | out | `param1` |
| 72-73 | out | `target` id, 0 = none |
| 74-75 | out | `targetname` id, 0 = none |
| 76-77 | out | `nodeFirst` — scene node id of this model's first chunk |
| 78-79 | out | `entCount` — how many entities this level has |
| 80 | out | `nodeCount` — chunks in the run; 0 if the model draws nothing |
| 81 | out | `kind`, below |
| 82 | out | model — brush model index, 0 = not a brush model |
| 83 | out | reserved, zero |
| 84 | out | magic `$6C` |
| 85 | out | check: XOR of bytes 16-84 |
| 86-95 | out | zero |

**Kind** is a range, not an enumeration, so a game can ignore a whole class of
thing with one comparison:

| Kind | What |
|---|---|
| 0 | no opinion — a classname the converter does not classify. Never an error |
| 1-9 | the world and its wiring: 1 `worldspawn`, 2 `info_player_start`, 3-4 deathmatch/coop starts, 6 `path_corner`, 7 teleport destination, 8 intermission |
| 10-19 | brush movers: 11 `func_door`, 12 `func_door_secret`, 13 `func_plat`, 14 `func_button`, 15 `func_wall`, 16 `func_train`, 17 `func_illusionary` |
| 20-29 | triggers (invisible volumes): 21 `_once`, 22 `_multiple`, 23 `_secret`, 24 `_teleport`, 25 `_changelevel`, 26 `_counter`, 27 `_hurt`, 28 `_push` |
| 30-49 | pickups: 31 health, 32-34 armour, 35-44 weapons, 45-48 ammo, 40-43 the four powerups |
| 50-79 | monsters: 51 grunt, 52 dog, 53 ogre, 54 knight, 55 zombie, 56 wizard, 57 demon, 58 shambler, 59 enforcer, 60 hell knight |
| 80-89 | scenery: 80-81 explosive boxes, 82 teleport train, 83 fireball |
| 90-99 | lights and sound: 90 `light`, 91-94 the named light entities, 95-96 ambient sounds |

Within a range, an unlisted classname falls back to the family's own number:
anything `monster_` is 50, `item_` 30, `weapon_` 35, `trigger_` 20, `func_` 10,
`light` 90. So "is this a monster?" is `kind >= 50 && kind < 80` and it stays
right for a level with monsters E1M1 does not have. The full table is
`ENT_KIND` in `tools/gen_quakelevel.py`.

Moving a door is then three things and no lookups: `travel × fraction` into
`SET_POSITION` on nodes `nodeFirst` through `nodeFirst + nodeCount - 1`, and
the same displacement into the door's mover record in your next `CLIP_MOVE`
block so the player stops walking through it. The wiring is the ids: a
`trigger_multiple` whose `target` equals a door's `targetname` is what opens
that door.

A `trigger_teleport` (kind 24) carries everything a jump needs, so the game
never has to look up kind 7: `param1` is 1 when the teleport works, `travel`
is then the absolute eye position at its destination (Quake's
`info_teleport_destination` plus 27 + 22 units) and `param0` is the yaw, s16,
to face there. `param1` is 0 for one with a `targetname` — Quake's monster
ambushes, live only for a moment after they are fired — and for one excluded
from every skill.

Both obligations are `CLIP_MOVE`'s, for the same reasons: check the output
magic **and** checksum rather than `ERRCODE`, and set `SET_DMA_WINDOW` around
the block.

## A world in one block

A Quake game has thirty-odd things moving at once: monsters, items, the gun,
the status bar. Posing each one with `SET_POSITION` and `SET_ORIENTATION`
would take about seventy commands a frame, and each one is a chance for the
bus to drop or flip it. `WORLD_TICK` ($6C) takes all of them in one block,
plus every geometric question the game's rules ask in that frame: *can this
monster see me*, *where did this shot land*, *where does this monster end up
if it walks*.

The descriptor is `CLIP_MOVE`'s: `ARG0` space, `ARG1-3` address, `ARG4-5`
length. The length must cover the input **and** the output, because gpu64
writes the answers into the same block, straight after your input records.

**Header, 16 bytes:**

| Byte | Meaning |
|---|---|
| 0 | magic `$D8` |
| 1 | actors, 0-32 |
| 2 | traces, 0-24 |
| 3 | moves, 0-24 |
| 4 | movers, 0-16 |
| 5 | frame: any value. It is echoed back, so you can recognise a stale answer |
| 6-7 | check: 16-bit sum of bytes 0-5, bytes 8-15 and every input record byte |
| 8-15 | reserved. Included in the sum, but not read |

The input records follow from byte 16, back to back, in this order: actors,
then traces, then moves, then movers.

**Actor, 20 bytes.** It poses one node that already exists. `WORLD_TICK`
never creates or destroys a node.

| Bytes | Meaning |
|---|---|
| +0-1 | node id |
| +2-3 | resource. An object takes a mesh id; a sprite takes a texture id. `$FFFF` leaves the resource unchanged. It must be `$FFFF` for a light or a camera |
| +4 | flags: `$01` visible, `$02` view space. Any other bit refuses the record |
| +5-7 | yaw, pitch, roll: the high byte of `SET_ORIENTATION`'s angles, so a full turn is 256 |
| +8-19 | position x, y, z, s32 16.16. In view space it is relative to the eye |

**Trace, 28 bytes.** One hitscan shot or line of sight.

| Bytes | Meaning |
|---|---|
| +0-11 | start, s32 16.16 |
| +12-23 | end |
| +24 | hull: 0 is a point (use it for shots and sight), 1 the player's box, 2 the large one |
| +25-27 | zero |

**Move, 28 bytes.** `CLIP_MOVE` for a monster: +0-11 start, +12-23 delta,
+24 hull (1 or 2), +25 mode (`CLIP_MOVE`'s mode bits), +26-27 zero.

**Mover, 16 bytes.** Exactly `CLIP_MOVE`'s door record. The doors you list
block every trace and every move in the block.

**Output.** gpu64 writes it from `OUT` = 16 + the bytes of all your input
records:

| Bytes | Meaning |
|---|---|
| OUT+0 | magic `$8D` |
| OUT+1 | frame, echoed |
| OUT+2 | actor records applied |
| OUT+3 | zero |
| OUT+4-7 | refused: bit *n* is set when actor record *n* was refused, little-endian |

Then one 16-byte answer per trace:

| Bytes | Meaning |
|---|---|
| +0-11 | where the trace stopped |
| +12-13 | fraction, u16. 65535 means it went the whole way |
| +14 | flags: `$01` clear (it reached the end untouched), `$02` it started in solid, `$04` it was solid all the way |
| +15 | contents of what it hit, using `CLIP_MOVE`'s contents codes. 0 means no hit |

Then one 16-byte answer per move. This is exactly `CLIP_MOVE`'s bytes 32-47:
end, flags, contents, fraction, bumps.

Then a 4-byte trailer: a 16-bit sum of every output byte before it, the
magic `$8D` again and the frame again. The magic is at both ends because a
block that only half arrived passes a check made only at the front.

Line of sight is a hull-0 trace from the monster's eye to yours: it can see
you when flag `$01` is set. A shot is a hull-0 trace from the muzzle to a
point far along the aim. Compare its fraction against each monster's box on
the C64; the level only knows walls and doors, not monsters.

### What gets refused

A malformed block is refused **whole**. gpu64 writes nothing, changes nothing
and answers `BAD_ARGS` when any of these is true:

- the magic is wrong;
- a count is over its maximum;
- the length does not cover the input and the output;
- the check fails;
- a trace hull is not 0-2, or a move hull is not 1-2;
- a mover names a model the level does not have;
- the block has traces, moves or movers and no level has finished loading.

A block with only actors needs no level.

Once the block has passed, a bad **actor record** is refused on its own and
the rest are applied. That happens when the node does not exist, when the
resource is the wrong kind for the node, or when a flag bit is undefined. The
refused bitmap tells you which records failed, and `RESULT` tells you how
many. Actor records are absolute and are applied only from a block that
passed, so a lost block costs one frame of stale pose, never a wrong one.

The obligations are `CLIP_MOVE`'s:

- Check the output magics, the sum and the frame echo, not `ERRCODE`.
- Poison the output half before each attempt.
- Keep the block inside `SET_DMA_WINDOW`.
- When it fails, send it again. The input half is untouched.
- Bound every move answer by its question, as for the player.

### View space: the gun and the status bar

A node posed with flag `$02` is drawn in **view space**. Its position and
angles are relative to the eye rather than to the world. gpu64 draws it after
the whole world, over a cleared depth buffer, so it never sinks into a wall
you are standing against. Use this for the gun and the HUD. The flag stays set
until a `WORLD_TICK` clears it; `CREATE_*` and `LEVEL_NODE` also clear it.
A camera never goes into view space: on a camera the flag is ignored and the rest of the record is applied.

Two practical points:

- **Place the gun forward of the eye.** A Quake view model is built around
  the eye, and at z = 0 most of it is behind the near plane. About 0.3-0.4
  world units forward, with a near plane of 0.1, puts it on screen.
- **Mark HUD sprites unlit** (`SET_SPRITE` flag bit 3). View-space nodes are
  lit, and the directional light is applied to them as if it turned with you.
  A HUD sprite faces straight back at the eye, so unless the light happens to
  shine along your view it is shaded dark.

## Lighting

One directional light (`SET_LIGHT`), shaded per face: `ambient + N·L`,
clamped to 0-15 and used as an index into a 16-level generated colormap
(`BUILD_COLORMAP`) rather than as an RGB multiply — the framebuffer stays
8bpp indexed all the way to scanout, same as everywhere else in gpu64.
Level 15 is full brightness (the identity mapping); level 0 is **1/15 of
the way up, not black** — a face turned fully away from the light still
reads as its own colour, not as a silhouette. Call `BUILD_COLORMAP` once
after any palette change; it costs milliseconds and must never be called
per frame.

Face `flags` bit2 (unlit) skips this entirely for that face and draws its
texture (or flat colour, if bit1 is also set) at full brightness — useful
for UI-ish geometry rendered through the 3D pipeline, e.g. a cockpit panel
that shouldn't dim when the scene does.

### Point lights

On top of the directional light, up to **eight** point lights, each a
`CREATE_LIGHT` node placed by the ordinary transform opcodes and given a
strength and a radius by `SET_POINT_LIGHT`. Their contribution is added to
the level the directional light produced, so a scene with a low `ambient`
and no directional light at all is lit only where the point lights reach.

- Meshes are lit **per pixel**: the view-space position is interpolated
  across the triangle affinely — unlike `u`/`v`, which are perspective
  correct. A wall crossing a light's radius gets a gradient, not one flat
  step; on a large face at a grazing angle that gradient's *centre* can sit
  a few pixels off where the light really is.
- Sprites are lit **per record**, once at the billboard's own position —
  a billboard has no interior geometry for a gradient to live on.
- The cap is eight *live* lights: a light node is skipped when it is hidden
  (`SET_VISIBLE 0`) or inert (strength or radius zero), so a scene may hold
  many more than eight as long as no more than eight are lit at once. Past
  eight, the **first eight in scene order** win — scene order is node-table
  slot order, which is the order the IDs were created in.
- The whole lit path sits behind one branch outside the pixel loop, so a
  frame with no live lights costs exactly what it did before lights existed.

Switching a light is therefore `SET_VISIBLE`, not create/destroy: a muzzle
flash is one node created at start-up, moved to the player each frame, and
made visible for as long as the shot lasts.

## Depth buffer

A 16-bit z-buffer sized to the viewport, storing `near/z` — **larger values
are nearer**, and an untouched pixel (value 0) loses every depth compare, so
it behaves correctly as "nothing drawn here yet" without a separate clear
value. `CLEAR_VIEWPORT` ($40) is what clears both the colour and the
z-buffer; nothing else does, so call it once per frame before your draws.

Depth precision is controlled entirely by `near` (from `SET_PERSPECTIVE`):
half the buffer's numeric range is spent between `near` and `2*near`,
wherever that is. Setting `near` unnecessarily small to avoid clipping
trades that away as z-fighting everywhere else in the scene — set `near` as
far out as the closest thing the camera will ever actually get to.

## Coordinate and rotation conventions

- **Axes are left-handed**: +x right, +y up, +z away from the camera.
- **Winding is clockwise as seen from outside the model.** A face is
  front-facing when its normal (from `cross(v1-v0, v2-v0)`) points back
  towards the camera. Get this backwards and every face of a closed mesh is
  culled — which looks exactly like a mesh that never uploaded. This is why
  `DRAW_MESH`'s `RESULT` is a triangle count: check it.
- **Positive pitch tips a node's own +z towards -y** — a camera with
  positive pitch looks *down*.
- **Yaw, pitch and roll compose in that order, each about the node's own
  axes** (`R = Ry * Rx * Rz`): yaw turns about the world's +y, then pitch
  tips about the node's already-yawed +x, then roll spins about its own +z.
  So a camera keeps pitching up and down whatever direction it faces, and its
  horizon stays level — pitch is not a rotation about world x. A model given
  both a yaw and a pitch carries its pitch with it as it turns, the way an
  aircraft's nose does.
- Angles are a 16-bit binary angle (65536 = 360°): add a turn rate and let
  it wrap, no clamp or degrees/radians conversion needed. Positions are
  signed 16.16 (±32768 units at 1/65536 — a whole level, not just one
  model); per-frame deltas (`MOVE_LOCAL`/`MOVE_WORLD`/`ROTATE_LOCAL`) are
  8.8, matching everything else's world-unit convention.
- Internally, orientation is composed in 1.15 fixed point, not 8.8 — that
  detail is invisible at the wire format, but it's why a model turns
  smoothly instead of visibly wobbling: an 8.8 rotation matrix quantizes to
  1/256, which at 320px puts a rotating box's edge a whole pixel off its
  true position, and the error is systematic rather than random, so it
  reads as wobble rather than noise.
- **`near`/`far` (`SET_PERSPECTIVE`) are 8.8, so the far plane cannot exceed
  127.99 units.** This is a *scale* convention, not a hard distance ceiling
  — author your scene so the far plane fits, exactly as a mesh is authored
  to fit its own ±128-unit vertex space. Nothing stops "one unit" from
  meaning a kilometre; the unit just has to be chosen once, for the whole
  scene, up front.

## Register additions

Class 1 adds one readable byte beyond the class 0 register set:

| Address | Name | Dir | Purpose |
|---|---|---|---|
| $DF66 | `RESULT` | R | Low byte of the last command's result — page number from `SCENE_COMMIT`, triangle count from `DRAW_MESH`/`DRAW_NODE`, face count from `UPLOAD_MESH`. Meaning is per-opcode; undefined for opcodes that define none. `CREATE_OBJECT`/`CREATE_CAMERA` don't allocate an ID and so don't set `RESULT` — node IDs are chosen by the C64 side, same as resource IDs. As of stage 15b, `DRAW_MESH`/`DRAW_NODE`'s `RESULT` is deferred — see "Deferred RESULT" above for when it actually becomes valid. |

and one `STATUS` bit:

| Bit | Meaning |
|---|---|
| 4 | frame-ready — the loop has finished a frame and is waiting for `SCENE_COMMIT` (handshake mode only). Polled at the top of every dispatch, so it goes high as soon as core 1 finishes, not only when `SCENE_COMMIT` itself runs. |

New `ERRCODE` values this class adds: `OUT_OF_MEMORY` (resource RAM
exhausted), `QUEUE_FULL` (the ring was full and the opcode was one of the
ones that still rejects outright rather than waiting — see "Deferred RESULT"
above), `BAD_ID` (no such resource or node), `NO_CAMERA` (a render was asked
for with no active camera), `WORKER_TIMEOUT` (core 0 gave up waiting on core
1 in a generous worst-case window — designed to be unreachable in normal
operation). As of stage 15b this has two distinct triggers, not one: a
`CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE` pushed against a full ring waiting
for space to open up, or any other class 1 opcode (including the drain
inside a session reset, i.e. RUN/STOP+RESTORE) waiting for a previously
queued render to finish before it mutates the state that render reads. Both
mean the same thing at the hardware level — core 1 did not drain the ring in
time — just reached from two different call sites. See
[error-codes.md](error-codes.md) for the shared table.

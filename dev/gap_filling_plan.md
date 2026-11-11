# Gap-filling plan: class 1 phase 2 — gpu64 as "OpenGL for the C64"

Staging document, not a status record — `progress_tracker.md` gets the
real entry, with the real milestone number, when each stage below actually
lands. Numbers here (`Stage 14` etc., continuing `progress_tracker.md`'s
`## N.` sequence from `## 13. The resident world`) are provisional and
exist only to give the stages a stable name to reference from other docs;
expect them to shift.

## Why this document exists

Decided 2026-08-28: gpu64's 3D layer is not "a Doom engine" or "a Quake
engine" — those were the illustrative targets used to pressure-test class
1 and class 2's design, not the scope. The target is a general 3D API for
the C64, closer in spirit to OpenGL than to either game engine: retained
GPU-side resources, a scene the C64 manipulates by reference, and a render
loop that runs to completion on the RPi side, decoupled from the C64's own
clock.

The good news, confirmed by re-reading
[milestone6_3d_design.md](milestone6_3d_design.md) in full while scoping
this document: **that architecture is already designed**, in detail, and
has been sitting in class 1's "design only" opcode ranges since
2026-08-23. Nothing here is a new design pass. It is the plan for building
what phase 1 (2026-08-24) deliberately left unbuilt to get a rasteriser on
hardware quickly:

- The retained scene graph (`CREATE_OBJECT`/`CREATE_CAMERA`/`DESTROY_NODE`/
  `SET_ACTIVE_CAMERA`/`SET_VISIBLE`, $20-$24) and per-node transforms
  (`SET_POSITION`/`SET_ORIENTATION`/`MOVE_LOCAL`/`MOVE_WORLD`/
  `ROTATE_LOCAL`/`SET_SCALE`/`GET_TRANSFORM`, $30-$36) — currently all
  `BAD_OPCODE`.
- The autonomous render loop (`LOOP_START`/`LOOP_STOP`/`SCENE_COMMIT`,
  $06-$08) — currently all `UNSUPPORTED`.
- The core-0/core-1 split that lets the loop run without ever stealing a
  C64 bus cycle — designed, ring buffer built and fed, but core 1
  executes nothing off it yet.

## Decisions this plan is built on

Four questions were resolved before drafting this plan; they are binding
on everything below, not just preferences:

1. **GPU hardware access stays inside what Circle 44.3 actually supports
   today — nothing aspirational.** See "What 'native graphics hardware'
   means here" below: full VideoCore/QPU acceleration is out of reach
   under gpu64's current build, and the plan does not pretend otherwise.
2. **Frame pipelining is single-buffered, poll-to-confirm.** At most one
   frame in flight: the C64 fires `SCENE_COMMIT`, resumes immediately, and
   later polls `STATUS` bit4 to know the next commit is accepted. No deep
   queue, no multi-frame lookahead.
3. **Class 1 supersedes class 2 as gpu64's 3D forward path.** Class 2 is
   deprecated (frozen, not removed — see `docs/api_design.md`'s class 2
   section) as of this document. Its own opcodes keep working and nothing
   here plans to touch them; new 3D capability goes into class 1.
4. **Per-frame dynamic state is per-object command writes, not a bulk
   delta blob.** Every changed object, camera or light gets its own small
   synchronous command targeting its resident `ID`, every frame — exactly
   the shape `SET_POSITION`/`MOVE_LOCAL`/`SET_LIGHT` etc. already have.
   This was the non-default choice (a single per-frame delta blob was the
   recommended alternative): it was picked because it reuses the existing
   per-node opcodes as-is, needs no new blob format, and keeps each write
   independently droppable-and-detectable under the bus's existing
   sequence-number/gap-detection discipline — a blob merges N updates into
   one DMA pull that either lands whole or doesn't, which is a worse
   failure mode against the bus's known drop rate than N independently
   sequenced small writes.

## What "native graphics hardware" means here

The user's instruction was to use the RPi's native graphics hardware where
it's safely available, and to let the RPi GPU's own API shape gpu64's
design — while explicitly **not** going beyond what Circle 44.3 supports.
Both were researched before this plan was drafted:

- **Full VideoCore/QPU acceleration (GLES, OpenVG, Dispmanx) is out of
  reach.** Circle's `addon/vc4/` (VCHIQ + a GLES1.1/2.0/OpenVG/Dispmanx
  port) exists in the checked-out Circle tree, but its own README says so
  plainly: *"The accelerated graphics support is still experimental... It
  does build with AARCH = 32 only and cannot be built on Raspbian."*
  gpu64 builds AArch64 by default (`tools/build.sh`, `Makefile`), and the
  restriction covers the whole `interface/` tree, not just full GLES — so
  even the simpler Dispmanx 2D-compositing path is blocked. Migrating the
  whole firmware to AArch32 to get it would put the cycle-exact polling
  loop's hand-tuned i-cache-window timing at risk for a feature that is
  itself labelled experimental — not a trade this plan proposes. Hand-
  rolling a register-level V3D/QPU driver is further still beyond "well
  supported" and is explicitly out of scope.
- **What is genuinely available: `CDMAChannel`.** Circle's core library
  (architecture-independent — not gated behind the AArch32-only vc4
  addon) wraps the BCM2835/2711's DMA controller
  (`include/circle/dmachannel.h`, `dmacommon.h`, `dma4channel.h`). This is
  real, dedicated silicon separate from the ARM cores, built for exactly
  memory-to-memory work: bulk copies, clears, composites, texture
  streaming — done without spending ARM cycles on the copy loop, which is
  the actual "less energy than the CPU doing it" argument for using it.
  gpu64 does not use it today. This plan's DMA-offload stage (18) is
  scoped to this driver.
- **RPi GPU as API inspiration, not as the runtime.** The retained-
  resource / scene-graph / command-queue shape this plan builds (resources
  uploaded once and referenced by ID; a command stream a producer feeds
  and a consumer drains asynchronously; a present/commit boundary that
  decouples producer from consumer) is deliberately the same shape
  VideoCore's own driver stack uses internally (a command-buffer GPU
  behind VCHIQ) and that GPU APIs in general use — that resemblance is
  where "the RPi GPU inspires the backend API" is satisfied. **The
  execution stays software rasterisation on the ARM cores** (already what
  `gpu64_3d_render`/`gpu64_3d_raster` do, and NEON-eligible if a stage
  ever needs the headroom), offloaded to `CDMAChannel` for the bulk-memory
  parts of the job. This should be stated to the C64 programmer plainly —
  "gpu64 is shaped like a GPU API, backed by ARM cores plus a DMA
  engine" — rather than implied to be doing hardware-accelerated 3D
  rasterisation it is not.

## Staging

Each stage is gated on the one before it working and bench-verified — per
CLAUDE.md, that means `tools/hostsim`/`tools/rastercheck`-equivalent
verification first, then several-hundred-frame VICE runs, then hardware,
never the reverse. None of these stages are estimated in time; "near
future" per the user's framing means "the next things to build," in this
order, not a schedule.

### Stage 14 — the scene graph and transforms, on core 0 (decided 2026-08-29)

**This is the agreed next stage.** It reorders the plan: build the retained
scene-graph API before touching multicore, not after. Rationale for taking
it out of turn ahead of the old Stage 14 (below, now Stage 15): it repeats
phase 1's own precedent almost exactly — `DRAW_MESH` was deliberately built
synchronous-on-core-0 first so the rasteriser pipeline could be brought up
"with the cross-core question still open" (milestone6_3d_design.md, "The
renderer runs on core 0, and immediate mode is why"). Doing the same for
the scene graph decouples two risks that don't need to be taken together:
whether the node/transform API is right, and whether core 1 can execute it
safely under the bus's timing constraints. The first is cheap to get wrong
and cheap to fix; the second cost eighteen ladder rounds last time.

Scope:

- Implement $20-$24 (`CREATE_OBJECT`, `CREATE_CAMERA`, `DESTROY_NODE`,
  `SET_ACTIVE_CAMERA`, `SET_VISIBLE`) and $30-$36 (`SET_POSITION`,
  `SET_ORIENTATION`, `MOVE_LOCAL`, `MOVE_WORLD`, `ROTATE_LOCAL`,
  `SET_SCALE`, `GET_TRANSFORM`) for real — all currently `BAD_OPCODE`.
- Wire up `DRAW_NODE` ($42), currently `BAD_OPCODE` because it depends on
  nodes existing. Executes synchronously on core 0, exactly like
  `DRAW_MESH` does today — same DMA-halt window, same single-owner
  framebuffer rule, no ring buffer involved yet.
- **No `LOOP_START`/`LOOP_STOP`/`SCENE_COMMIT` in this stage** — they stay
  `UNSUPPORTED`. A node's transform is live the instant the opcode that set
  it returns (no shadow-scene double-buffer yet, since there's no loop to
  race against); a program calls `DRAW_NODE` per object per frame itself,
  the same rhythm `DRAW_MESH` already has today. What changes versus today
  is that the C64 sends a small transform update to a resident node instead
  of the mesh's full pose on every `DRAW_MESH` call — real bus-traffic
  savings, measurable now, without needing the autonomous loop to get them.
- Decide, during this stage rather than before it (cheap to settle
  empirically once nodes exist, expensive to keep guessing at): the three
  open questions milestone6_3d_design.md left open —
  1. Whether the scene graph needs hierarchy (`SET_PARENT`) — build it
     only if a real scene wants a turret-on-a-tank; the design already
     treats it as a one-opcode addition.
  2. Clipping strategy — near-plane clip + 2D scissor (the doc's own
     leaning) vs. full Sutherland-Hodgman; measure against a real scene
     with geometry that actually straddles the near plane before
     deciding, rather than guessing.
  3. Bounding-sphere source for per-object frustum culling — computed at
     `UPLOAD_MESH` (no exporter changes, costs upload-time CPU) vs.
     supplied by the exporter (free at upload, one more field in the
     export pipeline). Lean: compute it at upload — it's a one-time cost
     against a format the exporter side doesn't have to touch, consistent
     with "the C64 never touches mesh bytes" already meaning the RPi side
     absorbs this kind of derived-data cost elsewhere (normals, most
     obviously).
- Verification: **no hardware ladder needed for this stage** — it's
  single-core, same execution model as existing `DRAW_MESH`. `tools/hostsim`
  for pixel correctness (extend its scene with a couple of `DRAW_NODE`-
  equivalent calls), then a several-hundred-frame VICE run, then a normal
  (non-ladder) hardware bench pass. Extend `gpu64_3d_arena_test.a`-style
  exhaustive testing to the scene table (256 objects, per the design's cap)
  the same way the arena test drives resource RAM to exhaustion on purpose
  — an untested error path is not a tested error path.
- Out of scope here, deliberately: this stage does **not** yet deliver "the
  scene renders independently of the C64 CPU" — that needs the loop
  (Stage 16). What it delivers is the retained-node API and the per-object
  command-write traffic pattern (decision 4), validated before the harder
  multicore work is built on top of it.

### Stage 15 — core 1 actually renders (bring-up, immediate mode only)

Get the render loop running on core 1 at all, now that there's a settled
node/transform API to run it against. Scope deliberately narrow.

**Completion signal, settled 2026-08-29 (Opus review, cross-checked against
the actual source — see progress_tracker.md's "## 14." regression writeup for
the review methodology this follows):** an earlier draft of this stage said
both "`DRAW_MESH`/`DRAW_NODE` still return when the draw is done" *and* "this
stage is what actually exercises [the store-burst rule] against a live C64" —
those two cannot both hold if core 0 waits for the draw with the bus held,
because a bus-held wait leaves nothing free-running for the store bursts to
contend against. The fix is not a new mailbox — CLAUDE.md's "core 0 must never
read a line core 1 writes" is milestone 6a's finding about
`reuUsingPolling()`'s per-C64-cycle hot loop specifically (its own instrument
only ever measured loop-pass elongation there), not an unconditional rule
about all core-0 code. Dispatch already runs with the C64 DMA-halted and no
per-cycle deadline, and shipped code already reads core-1-written state from
there (`logGpu64_3dStats()`, gpu64_api.cpp — reads `gpu64_3dWorkerStats` every
`LOG_ENABLE(1)`, hardware-verified since milestone 6). So: `gpu64_3dDispatch()`
reads `gpu64_3dRing.tail` (bus held) and waits for it to catch `head` — no new
signal, the worker already publishes `tail` *after* each command executes, so
`tail == head` is already an exact completion signal. See
`gpu64_3d_internals.h`'s `Gpu64_3dRing` comment for why the read itself is
cheap enough not to matter at dispatch time.

Split into two rounds so a core-1 rendering bug and a bus-timing bug fail in
different bench passes rather than the same one:

- **15a — plumbing, zero concurrency.** Move `CLEAR_VIEWPORT`/`DRAW_MESH`/
  `DRAW_NODE`'s existing logic from core-0-synchronous execution to core 1,
  driven by draining the ring buffer that phase 1 already feeds (today core 1
  counts entries but executes nothing). Stall on drain at the top of *every*
  dispatch, so the concurrency window is nil and only rendering correctness is
  under test. Two known fixes belong in this round, both found reading the
  real code, not hypothetical:
  - `cleanViewport()`'s call into `CGpu64FrameBuffer::CleanRows()` is one
    unchunked `CleanDataCacheRange` over the whole viewport (~1000 back-to-back
    cache ops, no yield) — free today because core 0 holds the bus for the
    whole draw; needs chunking to the same 256-byte/yield budget as the
    rasteriser before 15b puts it under contention. 6a's one data point on
    bulk cache-maintenance ops (`DC ZVA`) was "actively worse," not a hatch.
  - `gpu64_3dDispatch()` currently pushes onto the ring *before* validating/
    executing, so a `BAD_OPCODE`/`UNSUPPORTED` entry can already sit in the
    ring — harmless while core 1 only counts (its `execute()` skips a switch
    "because core 0 has already validated"), but that premise inverts the
    moment core 1 actually executes. Swap the order.
- **15b — narrow the stall, arm the positive control.** Stall only at
  observation points (page flip/vsync commit, class 0 framebuffer ops, ring
  full) rather than every dispatch, so a full frame of `DRAW_NODE` calls
  actually queues and overlaps while the C64 free-runs — the real contention
  regime. This is where the store-burst rule (milestone6_3d_design.md, "The
  real risk: store bursts, not core-count and not the L2") goes from a `DSB`
  protecting nothing to load-bearing for the first time; `gpu64_3d_span.h`'s
  chunking already exists and is already sized to 256 bytes (4 lines).
  Ring entries snapshot `gpu64Regs.arg` but not `s_State` (viewport/camera/
  light) or node transforms read at execute time, so a `SET_LIGHT` issued
  between push and deferred execute would change what a queued draw sees —
  sequence the state opcodes through the ring too, not just draws.
- Verification: the load ladder's positive control (a real REU round-trip
  running underneath core 1's render traffic) plus the existing hostsim
  reference PPMs as the pixel-correctness oracle — a core-1 rendering bug
  and a bus-timing bug must not be diagnosed as the same symptom. **No
  hardware ladder campaign needed** — the store-burst budget is already
  measured (6a); this stage exercises it, it doesn't re-derive it.
- Carries forward untouched: never poll MMIO from core 1 (the current
  worker already parks on `WFE` against the generic timer, satisfying this).
- `QUEUE_FULL` becomes reachable for the first time once draws take real
  time. Prefer waiting for a slot with the bus held over rejecting —
  `GPU64_ERR_QUEUE_FULL`'s rationale in gpu64_api.h is about the *polling
  loop's* predictability, which a dispatch-time wait doesn't touch — but keep
  a timeout backstop so a wedged core 1 reports rather than hangs.

### Stage 16 — the commit protocol

With core 1 proven under real render load:

- Implement `LOOP_START`/`LOOP_STOP`/`SCENE_COMMIT` ($06-$08) in handshake
  mode (mode 0) only first — this is the mode that matches decision 2
  above (single-buffered, poll-to-confirm) exactly: loop renders a frame,
  stops, sets `STATUS` bit4 + `RESULT` = page; C64 draws its HUD via class
  0, then fires `SCENE_COMMIT`. This is the "one fire-and-forget command
  which triggers the whole scene recompute" from the original request,
  and `STATUS` bit4 is the poll-to-confirm half.
- Add the shadow-scene double-buffer that Stage 14's nodes didn't need:
  once the loop is reading the scene on its own schedule, `SET_POSITION`/
  `MOVE_LOCAL`/`SET_VISIBLE`/etc. must write into a shadow copy, published
  atomically by `SCENE_COMMIT` — otherwise a frame can show one object
  moved and another not.
- Free-running mode (mode 1) — the loop never waiting on the C64 at all —
  is explicitly a later stage (18), not part of this one: handshake mode
  alone already delivers "the scene renders independently of the C64
  CPU," and mode 1 adds a second frame-lifecycle path worth verifying on
  its own once mode 0 is solid.
- Verification: same shape as Stage 15 — real bus traffic underneath,
  hostsim/VICE first for the frame-lifecycle logic itself (this doesn't
  need hardware to get wrong), then hardware for the handshake timing.

### Stage 17 — class 2 demo migration and deprecation follow-through

Once Stage 16's loop can run a populated scene end-to-end:

- Port the existing Quake-style demo (currently driving class 1's
  immediate-mode `DRAW_MESH` per object per frame — see
  `progress_tracker.md`'s "11. Directional things, and a quake demo you
  can walk around in" and "12. Dynamic point lights, and a muzzle flash")
  to the retained scene graph and the commit protocol — this is the real
  test that per-object command writes (decision 4) plus the autonomous
  loop are actually cheaper on the bus and actually decouple the C64 than
  what immediate mode does today, not just cheaper on paper.
- Do **not** port the Doom-style (`DRAW_COLUMNS`/`DRAW_WALLS`/
  `DRAW_SECTORS`) demos — per decision 3, that column-cast rendering model
  has no class-1 equivalent and isn't getting one; those demos stay on
  class 2 indefinitely, which is exactly what "frozen, not removed" means.
- **Done 2026-09-30:** the migration write-up is "Porting a class 2 program" in `docs/class1-3d-mesh-reference.md`, with the room's before/after and the game's per-frame figures. Originally: write up the migration as a short note in `docs/class1-3d-mesh-reference.md`
  once there's a real before/after to point at, rather than guessing at
  the numbers now.

### Stage 18 — free-running mode, and DMA offload

Two independent pieces of follow-through, staged together only because
both are "make the already-working thing better" rather than "make the
next thing work":

- **Free-running mode (mode 1) -- DROPPED 2026-09-30 by the user's
  decision; not to be built.** Kept below only as the record of what was
  proposed. The loop renders and flips on its own
  schedule, never waiting on the C64 — the "C64 goes off and does BASIC
  while the HDMI scene keeps rendering" case from milestone6_3d_design.md's
  Philosophy section. `SCENE_COMMIT` still publishes updates atomically;
  the difference from mode 0 is nothing waits for it. Needs its own
  hardware verification pass — mode 0's handshake gives the loop a natural
  synchronization point that mode 1 doesn't have, so this is not "the same
  thing with a flag."
- **`CDMAChannel` offload for the bulk-memory parts of a frame** —
  `CLEAR_VIEWPORT`'s fill, and inter-page composites once free-running
  mode makes those a real per-frame cost rather than a one-off. Per
  CLAUDE.md's "any bulk upload through REU DMA needs a checksum or length
  readback" rule (originally aimed at C64→gpu64 transfers, but the
  principle — this transport has known silent-failure modes, verify
  rather than trust — applies just as much to a same-chip DMA engine gpu64
  hasn't used in anger yet): verify each `CDMAChannel` transfer's
  completion status and, for anything correctness-sensitive, its length,
  before trusting the destination. Scope this stage to measuring whether
  it's worth it first (energy/cycle savings vs. ARM `memset`/copy loops,
  on real hardware) before wiring it into the hot path.

  **Closed as not worth building, 2026-09-30 (desk estimate, tracker 97).**
  With free-running mode dropped, the only bulk fill left is the loop's
  per-frame clear.
  - It stores 320x200 colour bytes plus 16-bit depth, 192 KB, in about 760
    span bursts of 256 bytes, each ended by a DSB.
  - On a 1.4 GHz A53 that is roughly 0.2-0.4 ms of a ~31 ms frame, so about
    1% of core 1's frame is all a DMA offload could save.
  - Against that:
    - **(a)** A DMA fill is one unbroken 192 KB write stream. Burst length
      is the one axis milestone 6a proved breaks core 0, and pacing the
      engine with TI.WAITS or 2D strides would need its own load ladder.
    - **(b)** The engine reports completion through its CS register, and
      polling any MMIO register from a second core is the other thing 6a
      proved breaks core 0. Completion would have to come by interrupt.
    - **(c)** The BCM DMA has no fill mode. A fill is a non-incrementing
      128-bit source, which is new and unverified code on the frame path.
  - Reopen only if a core-1 frame-time instrument shows the clear costs
    well over a millisecond.

## What this plan deliberately does not stage

- **No opcode additions beyond what milestone6_3d_design.md already
  specified.** "OpenGL for the C64" is a framing correction — this class
  was never meant to be Doom/Quake-scoped — not a mandate to invent new
  primitive types, material models, or render state beyond textured/
  affine-mapped/flat-lit triangles with one directional light plus class
  2's eight dynamic points (which stay class-2-only per decision 3, unless
  and until a real class-1 program asks for point lights). Generalizing
  further than the existing design is real future work, but it is
  speculative in exactly the way the "Multi-blob commands" section of
  milestone6_3d_design.md already warns against for its own topic ("no
  in-scope command needs it... speculative generality") — better decided
  against an actual program that wants it than guessed at now.
- **No date estimates.** Per CLAUDE.md, bench time is the scarce resource
  here, not calendar time; stages are ordered, not scheduled.

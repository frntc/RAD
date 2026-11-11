# Milestone 4 — 2D command API: design notes

Why the protocol in [api_design.md](api_design.md) looks the way it does.
That document is the developer-facing reference and stays free of
rationale; this one holds the reasoning, the alternatives, and the open
questions. Status, dates and hardware test results live in
[progress_tracker.md](progress_tracker.md).

Companions: [project_description.md](project_description.md) (IO address
space allocation, target hardware configs, the three operating modes),
[bus_access_design.md](bus_access_design.md) (why cycle-by-cycle sniffing
doesn't fit this hardware), [milestone6_3d_design.md](milestone6_3d_design.md)
(continuous render loop, multicore, persistent GPU-side resources).

## Scope

Milestone 4's 2D command set — clear/fill, pixel, line, rect, blit,
palette, page flip — plus, eventually, general-purpose math/matrix ops.
Everything executes synchronously in the existing single core-0 bus-watch
loop: immediate-mode dispatch, the same model `showTestPattern()` already
proves on hardware. No independent render loop and no second core is
required at this scope; see milestone6_3d_design.md for where that changes.

## Philosophy

- **IO2 carries commands only, never bulk payload.** A command that needs
  data (e.g. a blit's source bytes) passes a *reference* — where the data
  already lives (C64 RAM or REU) and how long it is — not the data itself
  byte-by-byte through a register. gpu64 pulls the referenced bytes via a
  DMA burst, the same technique milestone 3's mirror snapshot already
  proved on hardware, just parameterized instead of hardcoded to screen RAM.
- **A payload-carrying command causes one bounded DMA-held burst.** Per
  [project_description.md](project_description.md#operating-modes), the C64
  is never DMA-halted for anything but a brief burst, in any gpu64 mode.
  The burst's duration is proportional to the payload's `len` — a known,
  C64-computable cost, not an open-ended stall.
- **Errors are never silent.** An undefined opcode or an out-of-range blob
  reference sets `ERRCODE` and does nothing, rather than doing something
  approximately right. `ERRCODE` is written on success too, so a program
  never has to distinguish "no error" from "stale error from three commands
  ago".

## Display architecture

The framebuffer api_design.md specifies (320x200, 8bpp indexed, 256-entry
24-bit palette, double buffered) does not exist yet: today both
`showTestPattern()` and `showMirror()` write straight into `m_Screen`'s
negotiated COLOR16 HDMI framebuffer (1824x984 on the test rig) with no
intermediate surface at all. Building it is milestone 4's first code step,
before any opcode.

**The VideoCore does all of it natively** — verified in the pinned Circle
tree, not assumed:

- `CBcmFrameBuffer(nWidth, nHeight, nDepth, nVirtWidth, nVirtHeight,
  nDisplay, bDoubleBuffered)` takes a depth of 8 and its own virtual size.
- `SetPalette32(u8 index, u32 RGBA)` + `UpdatePalette()` program a 256-entry
  hardware palette at 8 bits per channel — a real 256-out-of-16.7M
  selection, not a 5-bit-per-channel approximation. (`SetPalette(index,
  RGB565)` is the lower-fidelity sibling; use the 32-bit one.)
- Double buffering is the standard virtual-height trick: allocate virtual
  height = 2x physical, then `SetVirtualOffset(0, 0 | height)` to swap.
  `WaitForVerticalSync()` exists for the deferred-flip case.
- The 320x200 surface is scaled to the HDMI mode by the VideoCore's own
  scaler, so no ARM-side upscale is needed.

So the palette and the back buffer are both free, hardware features — *if*
gpu64's surface is the display. That is the catch:

**Only one framebuffer owns the HDMI output.** Circle's `CScreenDevice`
allocates its own `CBcmFrameBuffer` at a compile-time `DEPTH` (16, project
wide) and drives everything currently on screen: the HDMI log column, the
milestone 3 mirror, the test pattern. An earlier attempt at a *second*
`CBcmFrameBuffer` at 320x200 alongside it produced nothing visible (noted
in rad_main.h) — consistent with the mailbox allocating one framebuffer per
display, not with a subtle bug worth re-chasing.

**Decided (2026-08-22): option A — gpu64's surface owns the display.**
One `CBcmFrameBuffer`, 320x200, depth 8, virtual height 400: hardware
palette, hardware scaler, hardware page flip, real vsync. Everything
api_design.md specifies then costs a mailbox call rather than ARM work
inside the bus-watch loop, and a flip is genuinely tear-free.

What that costs, and has to be dealt with as part of the work:

- **The HDMI log column doesn't fit.** Today it's a full-height text column
  beside a 700x460 box at 1824x984; at 320x200 the whole screen is 40x25
  characters. **Resolved:** the log becomes an overlay drawn straight into
  the 8bpp surface in one reserved palette entry (255), glyph pixels only,
  suppressible with `LOG_ENABLE` and on at reset. That matters because it is
  the only working debug channel in API mode — GPIO14/15 are the cartridge
  latches, so there is no serial fallback (see [hw_testing.md](hw_testing.md)).
- **`CScreenDevice`/`CHDMIConsole` have to be re-pointed or dropped.**
  `CScreenDevice` allocates its own framebuffer at a compile-time `DEPTH`
  of 16; gpu64 needs to own the allocation instead, and `CHDMIConsole`
  currently writes `COLOR16` values through `CScreenDevice::SetPixel()`.
- **The milestone 3 mirror has to render into the 8bpp surface.** That's
  arguably an improvement: its 16 C64 colours become palette entries 0–15
  (which is already the reset palette), and its 40x25 text at 8x8 glyphs is
  exactly 320x200 — a 1:1 fit with no scaling, replacing today's ARM-side
  2x upscale.

The alternatives, recorded for completeness:

- **B — keep `m_Screen`, present in software.** The 320x200x8 pages stay
  plain RAM; "present" is an ARM-side palette lookup plus 2x upscale into
  `GPU_OUTPUT_BOX`, like `showMirror()` already does. Log column and mirror
  survive untouched, but each flip is ~256000 framebuffer writes plus a
  ~512KB cache clean inside the bus-watch loop, the palette collapses to
  `COLOR16`'s 5 bits per channel, and "tear-free" is only as good as the
  copy.
- **Hybrid — switch the framebuffer when a program engages the API**, since
  [the modes are already mutually exclusive](project_description.md#operating-modes).
  Gets A's fidelity and keeps B's tooling in mirror/menu mode, at the cost
  of runtime framebuffer reallocation and losing the log exactly when API
  bring-up wants it.

## Decisions taken during implementation

- **Dispatch runs with the C64 DMA-halted** (forced by the first hardware
  test, see progress_tracker.md). The original model — command executes
  synchronously in the write handler while the C64 free-runs — cannot work,
  and the reason is worth keeping: the C64 goes on executing during the
  command, so every `STA` it issues into the register window while gpu64 is
  busy is simply never seen by the polling loop. A `CLEAR` is ~64000 byte
  writes plus a cache clean; the CPU gets through dozens of instructions in
  that window. The observed symptom was one command vanishing entirely and
  the next one drawn from half-stale arguments.
  Making commands *faster* cannot fix this — even a `SET_PIXEL` outlasts the
  ~1µs between two 6502 stores — and making the C64 poll `STATUS` first
  cannot either, since a read during a busy handler is not serviced any more
  than a write is. Holding the bus is the only option that keeps the
  protocol honest, and it is what REU transfers already do. The invariant in
  project_description.md still holds in spirit: the halt is bounded, by the
  command's own work rather than by payload length alone.

- **The DMA release is phase-aligned; the register window is reached without
  a call.** Both forced by the second hardware test, which showed the
  bus-hold above working but the program degrading the further it got: the
  first command or two always landed, later ones intermittently didn't, and
  `ERRCODE` read back as a value that isn't a defined error code. Two causes,
  both in the polling loop rather than in the API:

  1. Dispatch released `bDMA_OUT` wherever the command happened to finish.
     Every other release in RAD (`DMA_READBYTE_P3`, `DMA_WRITEBYTE_P2`, both
     via `DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER`) first syncs to the start
     of a VIC half-cycle. Restarting the 6502 mid-cycle is not immediately
     fatal, which is exactly why the damage looked cumulative.
  2. `gpu64_apiWriteReg()` / `gpu64_apiReadReg()` were ordinary cross-TU
     calls made from inside the cycle-critical window — uncached, unlike
     everything else the loop touches (`warmCache()` preloads
     `reuUsingPolling`, `gpu64_mirrorSnapshot`, and the blob helpers, but
     could not preload these). The read path has a hard deadline
     (`WAIT_CYCLE_READ2`), so a miss there hands the C64 whatever was on the
     bus; on the write path a miss means the loop is still busy when the next
     `STA` arrives and loses it, which is a command running on a
     half-written argument block.

  So the register file is now a plain global (`GPU64REGS gpu64Regs`) with the
  two accessors `static inline` in the header, and only `gpu64_apiDispatch()`
  — which runs DMA-halted and has no deadline — remains a real call.

- **The border is real framebuffer area, not a display-side trick.** The
  physical framebuffer is 384x272 with the 320x200 surface centred in it, so
  `SET_BORDER` is an ordinary fill and needs nothing from the VideoCore that
  page flipping doesn't already use. The alternative — asking the mailbox to
  paint the area outside a 320x200 framebuffer — doesn't exist: the
  VideoCore upscales the framebuffer to fill the mode, so a 320x200 surface
  has no outside.

  The whole cost is absorbed by `PageBuffer()` returning the top-left of the
  *surface* rather than of the page: every draw op keeps indexing
  `p[y * pitch + x]` in 320x200 coordinates, so no drawing code changed.
  `PageBase()` is there for the two things that legitimately want the whole
  page — `SetBorder()` and the cache clean.

  The border belongs to the display, not to a page, so `SetBorder()` paints
  both pages. That makes it too expensive to call per frame, which is fine:
  `$D020` on a C64 is set when it changes, and the raster-bar idiom that
  changes it mid-frame has no meaning here anyway (gpu64 has no raster the
  C64 can chase).

- **A DMA burst warms its own instruction cache first.** `warmCache()` runs
  once at REU start, which is not where the cache pressure is: the dispatch
  standing between it and the first blob transfer touches far more memory
  than the preload survives. So `gpu64_blobRead()`/`gpu64_blobWrite()` redo
  the preload themselves, right before their phase sync. The general rule
  this points at: for gpu64, "preloaded at start-up" is not a durable
  property of anything the API calls, because the API's own commands are the
  biggest cache consumers in the system. Anything cycle-critical that a
  command can reach has to warm itself at the point of use.

- **8.8 arithmetic rounds half away from zero and saturates.** Products
  accumulate at full width (s64 — a 255-term dot product of 16.16 values
  overflows s32), then come back to 8.8 rounded rather than truncated, and
  clamp at ±127.99 rather than wrapping. Saturation costs a compare per
  element but a clipped highlight looks like clipping, where a wrapped one
  looks like a hardware fault. C64 code can observe this, so it is written
  into api_design.md rather than left as an implementation detail.
- **Matrix inversion runs in double precision regardless of element
  format**, bounded to n ≤ 64. An 8.8 pivot divides away to nothing very
  quickly; doing the elimination in the operand's own format would lose the
  result long before the matrix is actually singular. The bound is what
  keeps the augmented working matrix a fixed 64KB static buffer rather than
  an allocation.
- **`gpu64ApiActive` is now set by a *successful dispatch*,** not by any
  write into the window. The milestone 2 trigger set it on any write to
  $DF0B, which an REU-detection utility scanning IO2 could trip by accident
  — see progress_tracker.md's milestone 3 debugging notes.
- **vblank is specified but not implemented,** and says so: `VBLANK_ARM(1)`
  and a deferred `PAGE_FLIP` return a new `UNSUPPORTED` error rather than
  silently behaving like their immediate equivalents. There is no vblank
  event source in the bus-watch loop yet, and blocking on
  `WaitForVerticalSync()` inside the write handler would stall the loop for
  up to a frame — exactly what the cycle-predictability rule forbids.

## Decisions worth recording

- **Coordinates clip, blob references don't.** Drawing geometry partly
  offscreen is normal (a sprite leaving the screen) and must never set
  `ERRCODE`; a blob `addr + len` past the end of its space is always a bug
  and always errors. Deliberately opposite rules, for deliberately
  different reasons — and specifically *not* REU's own legacy wraparound
  behaviour, which gpu64 has no compatibility reason to replicate.
- **`len = 0` and zero `w`/`h` are no-ops, not errors.** Loop-friendly:
  C64-side code computing a clipped width doesn't need a special case.
- **Sticky `CMD_HI`.** The common case never leaves class 0, so the class
  selector shouldn't cost a write per command.
- **Per-opcode byte counts are a hard requirement, not polish.** `ARG` is
  write-only with no implicit clear, so an opcode that reads more bytes
  than it declares silently inherits an earlier command's leftovers — the
  same failure shape as the `gpu64ApiActive` bug (see progress_tracker.md,
  milestone 3), one register layer up.
- **Math ops live in class 0**, not their own class: likely used every
  frame once milestone 6 exists, so they belong on the register C64 code
  writes most.
- **Results are written back to C64 memory, not read out of registers**
  (user's decision, 2026-08-22). A command that produces data takes a
  destination descriptor and DMA-writes the result there before the
  `CMD_LO` write returns — so a matrix multiply is "two operands and a
  result address", and the C64 reads its answer out of its own RAM with
  ordinary `LDA`. This kills the `RES0`–`RES15` register-window idea
  entirely, and it is strictly better: no 16-byte ceiling on a result, no
  second protocol for reading, results can land in REU as easily as RAM,
  and the read path stays exactly two registers wide (`STATUS`, `ERRCODE`)
  — which sidesteps the REU read-handler landmine below rather than having
  to work around it. The cost is a write-direction DMA burst, which REU's
  own Stash path already does, so it is not new machinery.
- **Two math opcode sets, not a format flag** (user's decision,
  2026-08-22): $80–$8F are 8.8 fixed point, $90–$9F are IEEE float32, same
  layouts, low nibble picks the operation. Fixed point is what a 6502
  actually wants to build and read; float is there when range or precision
  matters more than bus traffic. Encoding the choice in the opcode rather
  than an `ARG` byte keeps the dispatch a jump table and costs nothing —
  and it means the implied byte count follows from the opcode alone, which
  the bounds check needs anyway.
- **Compact descriptors for math operands.** A full blob descriptor is 6
  bytes; three of them plus dimensions would be 21, over the 16-byte `ARG`
  block. Dropping `len` — which dimensions and element size already imply
  — makes it 4, and `MAT_MUL`'s three operands plus m/k/n fit in 15. It
  also removes an entire error class (`len` disagreeing with the shape).
- **General m x k x n dimensions** (user's decision, 2026-08-22) rather
  than fixed 4x4: one opcode covers transforms, batched vertex arrays and
  anything else without new opcodes. Dimensions are single bytes, so a
  batch is capped at 255 per call.
- **Palette index 255 is reserved for the on-screen log** (user's
  decision, 2026-08-22), which is what makes architecture A survivable: the
  log overlay draws glyph pixels only, in one colour, leaving the space
  around each glyph untouched. Legibility over arbitrary content is
  therefore not guaranteed — a two-colour overlay with a background box
  would fix that but costs a second reserved entry. The overlay is
  suppressible with `LOG_ENABLE` ($07) and **on at reset**, so bring-up and
  crashes are visible by default and a finished program spends one write to
  get a clean screen. Nothing prevents a program using index 255 as a
  drawing colour or repointing it; that is the program's business.
- **Banking is implicit.** A fetch sees whatever `$01` is in effect at the
  moment gpu64 samples it — the convention `gpu64_mirrorSnapshot()`
  already relies on to reach `$D800`. Revisit only for a concrete use case
  that needs to read through a different banking configuration than the
  CPU's current one.
- **vblank IRQ stays armed and auto-rearms**, like a raster IRQ: the
  handler acknowledges on entry rather than re-arming every frame.
  `IRQ_OUT`/`bIRQ_OUT` is a real GPIO line into the CBTD3861 transceiver
  and REU's own emulation already drives it (`rad_reu.cpp:605,618`) for its
  completion interrupt — gpu64 reuses that mechanism, not a new one.

## Implementation notes

Widening the IO2 decode beyond REU's own:

- The *current* `reuUsingPolling()` dispatch masks `IO_ADDRESS & 0x1f` —
  that's REU's own internal decode (11 registers fit in 5 bits), not a
  hardware limit; the GPIO latch delivers the full 8-bit address. gpu64's
  own dispatch must decode the full byte, or the usable space silently
  shrinks to ~21 aliased addresses.
- **Existing landmine on the read path**: `rad_reu.cpp`'s REU register read
  handler indexes `((u8*)&reu.status)[addr]` with `addr` masked to 5 bits —
  a read from anywhere in the gpu64 range as currently masked walks off the
  end of `REUSTATE` and returns whatever's there. gpu64's registers need
  their own bounds-checked read path, not REU's.
- Widening the write-side `switch(addr)` to a full 8 bits puts a bigger
  jump table/compare chain inside the cycle-critical write path — keep it
  branch-cheap.

Cycle-predictability: core 0's write handler must never do anything with
*unbounded* or *contention-dependent* latency — no allocation, no blocking
locks, no unbounded cache work. A blob fetch's cost scales with `len`,
which is known and C64-computable, so it doesn't violate this. The
destination region a blit writes into needs the same explicit cache clean
`showMirror()` already does for its rows, rather than relying on eventual
eviction.

Memory model at this scope: the only destination is the framebuffer. There
is no persistent, ID-addressed GPU-side resource store — that arrives in
milestone6_3d_design.md alongside textures and meshes, which is also why
`ID` is specified but unused here.

## Open questions

1. **The deferred flip's mailbox cost.** `SetVirtualOffset` measured 71 µs
   best and ~900 µs typically over 256 flips on real hardware. It runs
   inside a DMA hold so it is safe, but that is 5% of the C64's cycles for a
   program that flips every frame. Writing the display offset directly
   instead of through the mailbox would remove nearly all of it.

## Resolved

- **Log overlay legibility.** A transparent glyph background made the log
  unreadable over busy content, and gave the overlay no way to erase itself
  — scrolled text ghosted. Rows that hold text are now painted opaque over
  palette index 0, and the overlay hides itself entirely once a program
  engages the API. No extra palette entry was reserved.
- **Vblank, and the deferred `PAGE_FLIP` / `STATUS.busy` contract.** Both
  rested on the premise that knowing where the raster is costs a blocking
  mailbox call. Too narrow: **you only have to ask the VideoCore once.** The
  frame period is measured at boot against the free-running ARM system
  timer, and the loop then finds the next boundary with one MMIO read and a
  compare — the same cost as the GPIO samples it already takes twice per
  pass. Both clocks derive from the same 19.2 MHz crystal, so the
  extrapolation holds; it drifts slowly, which is what `VBLANK_SYNC` is for.
  `STATUS.busy` now has a real user: set by a deferred flip, cleared when it
  lands, which is what makes the poll loop in
  [api_design.md](api_design.md#tear-free-animation) work.
- **Write-direction DMA and the class-0 dispatcher** are both proven end to
  end on a real C64 (see
  [progress_tracker.md](progress_tracker.md#hardware-bring-up-four-rounds-four-bugs)).

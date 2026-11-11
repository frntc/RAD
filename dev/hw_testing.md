# Building and testing the firmware on real hardware

## Building

```
tools/build.sh                        # build external/Circle/app/Firmware/kernel8.img
SDCARD=/path/to/mounted/sdcard tools/build.sh   # build + deploy (see "Deploying to the right kernel")
```

The script clones [Circle](https://github.com/rsta2/circle) (pinned to `Step44.3`,
the version this Rules.mk/sysconfig.h were written for) into `external/Circle`
on first run, overlays this repo's Circle config onto it, builds the Circle
libraries gpu64 links against, then builds `Source/Firmware`. Re-runs are
incremental (only rebuilds what changed) and normally take well under a
second. `external/` is gitignored -- it's fetched/rebuilt, not checked in.

**Toolchain**: you need a bare-metal `aarch64-none-elf-*` cross compiler,
*not* `aarch64-linux-gnu-*` -- the latter's headers (glibc) collide with
Circle's freestanding ones and produce a wall of "conflicting declaration"
errors. If your distro doesn't package `aarch64-none-elf-gcc`, the
[xPack AArch64 Embedded GCC](https://github.com/xpack-dev-tools/aarch64-none-elf-gcc-xpack/releases)
prebuilt tarball works out of the box; extract it and put its `bin/` on
`PATH`. `tools/build.sh` looks for `${PREFIX64}gcc` (default
`aarch64-none-elf-`) and fails fast with a clear message if it's missing.

### What had to be fixed to get a clean build

The vendored `Source/Firmware` didn't compile as-is against a standards-strict
modern compiler; these are now fixed in the tree (see git history for the
mechanical details):

- `reuUsingPolling()`'s declaration didn't match its definition/call sites
  (missing `int step` parameter).
- A handful of `strstr(...) > 0` pointer/int comparisons (dirscan.cpp,
  rad_iecdevice.cpp) -- always illegal in C++, apparently tolerated by
  whatever compiler this was last built with.
- No `toupper`/`strupr` declarations visible outside helpers.cpp (that file
  already defines its own freestanding versions -- this is a no-libc build,
  there's no `<ctype.h>` to fall back on).
- `rad_iecdevice.o` was missing from the Makefile's `OBJS` entirely.
- FatFs' `FF_FS_RPATH` was off, but `helpers.cpp` calls `f_chdir()` --
  `tools/build.sh` flips it on in the Circle checkout.
- Two missing library dependencies (`lib/usb`, `lib/input` -- pulled in by
  `rad_iecdevice.cpp`'s USB-serial IEC device support) added to the Makefile.
- `rad_iecdevice.cpp` `#include`s `Printer/drv-nl10.h`, the IECBuddy
  dot-matrix-printer emulation driver -- **not present in this repo or in
  upstream frntc/RAD**, and irrelevant to gpu64. It's now gated behind
  `RAD_IEC_PRINTER_SUPPORT` (undefined by default); printing is a no-op, all
  other IECBuddy functionality (file sync, disk swapping) is unaffected. If
  you get hold of that driver, define the macro and add the file back.

None of this touches gpu64-specific behavior -- it's exactly what milestone 1
asked for: get the *existing* RAD image to build.

## Testing the 3D renderer without hardware

Most of milestone 6 does not need the Pi. The maths, the rasteriser, the
colormap builder and the mesh parsers depend on nothing but
`<circle/types.h>`, and `tools/hostsim` compiles those firmware sources
unchanged with a native g++ against a stubbed header:

```
make -C tools/hostsim run          # writes tools/hostsim/out/*.ppm
```

It renders a turning box, a z-buffer interpenetration case, a near-plane
straddle, the flat-colour path, and `out/prgpreview.ppm` — the exact scene
`tests/gpu64_3d_cube.a` asks for, from the same code the firmware
runs. **That file is the expected result of the hardware test.** Hold the HDMI
output against it: any difference is the firmware's plumbing — the blob pull,
the palette, the page flip, the cache clean — and not the pipeline.

Every per-image checksum is printed, so a change that was meant to be
cosmetic and was not is visible without opening a viewer.

Do this before a bench trip, not after. The first run of the sim found four
transposed signs in the Euler matrix, a bug that presents as a perspective
artefact rather than as a maths error and would have cost a whole session to
chase on hardware.

## Testing the class 2 raster core without hardware

The same trick, differently aimed. `Source/Firmware/gpu64_raster_core.cpp`
is the whole of class 2's pixel work and depends on nothing but
`<circle/types.h>`, so `tools/rastercheck` compiles it natively and renders
randomised scenarios through it -- and through the independent reference
model in `tools/prgsim/gpu64model.py`, which was written from
[api_design.md](api_design.md) rather than from the firmware. Then it
compares all 64000 pixels and the batch counters.

```
make -C tools/rastercheck run            # 400 scenarios
python3 tools/rastercheck/check.py -n 5000 --seed 3
```

A disagreement is a finding either way round: either the firmware does not
do what the document says, or the document does not say what the firmware
does. Both are worth knowing, and both are far cheaper to find here.

The scenarios deliberately concentrate on the edges a PRG can only sample a
few of -- columns starting above the view (so `v` must be advanced across
the clipped rows), negative `dv`, `u` past the texture width, spans naming a
non-power-of-two texture, sprites clipped by `clipY0`/`clipY1`, masking and
lighting. `DRAW_WALLS` scenarios add a random camera and segments biased to
straddle the near plane, plus an illegal camera one time in ten. Do this
before a bench trip: the first run found `DRAW_SPRITE` not counting itself
in the stats block at all.

Wall scenarios matter here more than anywhere else in class 2, because
`DRAW_WALLS` is the only opcode whose output is arithmetic rather than a
copy. Two people writing a perspective projection from the same paragraph
will disagree about a rounding or a sign, and the disagreement will be one
wrong pixel on one column at one angle. That is not findable at a bench, and
it is findable here in a second -- which is why the model's divisions go
through an `idiv()` that truncates toward zero the way C does, rather than
Python's flooring `//`.

## The demonstration programs

`Source/Demos/gpu64_demo_*.a` is nine developer-facing programs -- `hello`,
`palette`, `sprites`, `bounce`, `rotate`, `matrix`, `raycast`, `walls` --
indexed in
[demos.md](demos.md). They assert nothing and reach no verdict; they are what
a developer copies from, and on the bench they are a fast visual smoke test
of the whole API surface. `hello` is the go/no-go: if it draws, the cartridge
is wired up and the menu is in REU mode.

```
tools/examples.sh              # assemble, run on the PC in both display cases
                            # and render Source/Demos/out/<name>.ppm
```

Those PPMs are the expected HDMI output. Compare against them before
concluding that hardware is wrong -- the same argument as `tools/hostsim` and
the suite below, and it paid the same way: the first six passed on hardware
in a single round. `raycast` has been on the bench and blinks -- see the
progress tracker, section 8b. `walls` is newer still and has never run on
hardware.

**Run a demo long before believing it.** `runsim.py` reports the STOP key
pressed after four polls, and a demo polls it once a frame, so the default
run is about five frames. That is not a check:

```
python3 tools/prgsim/runsim.py Source/Demos/gpu64_demo_walls.prg \
        --demo --stop-after=400 --frame-log --max=2000000000
```

`--frame-log` prints one line per page flip -- the frame number, the drawn
page's non-zero pixel count and whatever the demo has written on the C64's
status rows -- and `--ppm-frame=N:PATH` writes the page being flipped away at
frame N. This is what a demo that misbehaves only after a while looks like on
a PC, and `vice-validation-run-length` is the note about why four frames was
never going to find one.

**Run a demo after another demo, too.** The Pi is not reset when the C64
loads a `.prg`; only a C64 `/RESET` calls `gpu64_apiReset()`. So a demo
launched from the RAD menu inherits the previous one's palette, border,
display mode, framebuffer contents and draw/visible page pairing -- and a
demo that is correct on its own can be wrong purely because of what ran
before it. That is not hypothetical: it produced two bench reports on
2026-09-09, `bounce` in the wrong colours and `palette` recolouring
`bounce`'s leftover rectangles instead of drawing its own rings. `dmInit`
now issues `$0B FULL_RESET`, and the regression is:

```
tools/examples.sh --chain      # every class-0 demo rendered twice, once clean
                            # and once launched on top of another demo's
                            # leftover gpu64 state; the two must match
```

Underneath it is `runsim.py --chain=PRG`, which runs one program and then
another **on the same model** -- the thing the menu does and a single-program
desk run structurally cannot see. Off by default in `demos.sh` because it
doubles the run.

Deploy by copying the `.prg` files into `RAD_PRG/` on the card. RAD reads
those at launch time, so unlike the kernel image they never go stale --
but the card *accumulates*, and it has collected renamed and abandoned
copies across the campaign. `SDCARD=<mountpoint> tools/deploy_prg.sh` copies
everything this tree builds and removes any gpu64-named `.prg` on the card
that it does not, leaving the user's own C64 software alone; `--dry-run`
shows what it would do first.

## The API conformance suite

`tests/gpu64_test_*.a` is a thirteen-program suite that checks
class 0 and class 2 against [api_design.md](api_design.md) and prints its own
verdict. It exists
so that a bench session starts from a known-good baseline instead of from a
demo that looks about right.

| Program | Covers |
|---|---|
| `gpu64_test_system` | `NOP`, `GET_INFO` field by field, flag-byte validation, and **every error code class 0 can produce** |
| `gpu64_test_draw` | `CLEAR`, `SET_PIXEL`, `LINE`, `RECT`, `RECT_FILL` — verified by reading the framebuffer back, plus clipping at both corners |
| `gpu64_test_blit` | `BLIT`, `BLIT_KEYED`, `READ_RECT`, the blob descriptor rules, an REU-space source, and a 16000-byte round trip compared byte for byte |
| `gpu64_test_math` | `$80`–`$86`, 8.8 fixed point: products, rounding on both signs, saturation, inverse, and `SINGULAR` leaving the destination untouched |
| `gpu64_test_float` | `$90`–`$96`, IEEE 754: the same shapes, as the control for the fixed-point rounding cases |
| `gpu64_test_pages` | `SET_DRAW_PAGE`, both forms of `PAGE_FLIP`, `VBLANK_ARM`/`ACK`/`SYNC` — and it adapts to a display with no frame clock |
| `gpu64_test_raster` | Class 2 end to end: the view rectangle, texture upload rules, both batch kinds read back pixel by pixel, clipping with `v` advanced across the dropped rows, rejected records against `RASTER_STATS`, the batch checksum passing and failing, `DRAW_SPRITE` scaling and clipping, and the colormap |
| `gpu64_test_walls` | `SET_CAMERA`'s three refusals, and `DRAW_WALLS` against geometry worked out by hand: a flat-on wall at a known distance, each of its four edges asserted from both sides, a back face and a wall behind the camera counted as rejections, and `CAM_PAINT` filling the ceiling and floor of the columns a wall covers and no others |
| `gpu64_test_sectors` | `SET_SECTORS`' three refusals, and `DRAW_SECTORS` against geometry worked out by hand: a one-sided wall's four edges from both sides, a portal's upper band, window and lower band at their exact rows, and four assertions on the **per-pixel** depth buffer -- a far wall visible through the window, losing to the near wall's lower band in the rows they share, at its own width, and unchanged when the two records are sent in the other order. Those four are the ones a per-column depth buffer would fail |
| `gpu64_test_things` | `DRAW_THINGS` against the same hand-worked geometry: a billboard's four screen edges at a known distance, the five malformed records that are rejected and the well-formed one that clips away and is not, a thing behind a wall and a thing in front of one, two things in either order, `THING_NODEPTH`, and the four quadrants of a 2x2 texture plus `FLIPX` and the mask key. The wall assertions are the ones that matter: they only pass if `DRAW_SECTORS` and `DRAW_THINGS` really share one depth buffer |
| `gpu64_test_polys` | `DRAW_POLYS` against geometry worked out by hand — the arithmetic is `tools/rastercheck`'s job; this checks what only hardware shows, that the vertex pool, the texinfo table and the face records the C64 wrote are the ones the core was handed, and that the depth buffer really is the one `DRAW_SECTORS` and `DRAW_THINGS` use |
| `gpu64_test_world` | `UPLOAD_POLYS`/`DRAW_WORLD`, the resident world: that a record uploaded in one command is still that record several commands later, that a chunked upload lands where its index said, and that a range past the end of the pool is refused rather than read. Same geometry as `gpu64_test_polys` |
| `gpu64_test_reset` | `$0B FULL_RESET`: into 80x50 text mode, `GET_INFO` agrees, reset, and the framebuffer is demanded back — mode, page count, border geometry, all three pages black, the draw page drawable, and `PAGE_FLIP` legal again. The same firmware path a `/RESET` takes, reached through an opcode because a reset line cannot be desk-checked |

Each prints one line per assertion — a name and either `OK` or `F<hex>` — and
a verdict line at the bottom. **The fail code is the byte that was wrong**,
almost always the `ERRCODE` that came back, so a red line is usually readable
without opening the source. The codes that are not an `ERRCODE` are `$E1` (two
buffers differed), `$E2` (a region was not uniformly filled), `$E3` (a byte
was wrong and happened to be zero), `$E4` (a value was in range when it should
not have been, or a timed wait expired) and `$FE` — **a command returned OK
where an error was demanded**, which is the important one: an error path that
never fires is not a tested error path.

Results stay on screen until RUN/STOP, which also clears it so BASIC's
`READY.` cannot land on a result line.

### Desk-check before the bench

```
tools/testprg.sh                # assemble them all and desk-check them
tools/testprg.sh -v system      # ...and print the screen it produced
```

`tools/prgsim/` is a 6502 core plus a reference model of the class 0 API
written from `api_design.md`. `testprg.sh` runs each program against it and
requires `VERDICT PASS`, in both display modes — with a frame clock and
without.

The direction matters. The model is built from the same document the suite
asserts against, so **a failure on the PC means the TEST is wrong** and a
failure on hardware means the **firmware** is. Getting the first kind out of
the way costs a second here and a session at the bench, which is the same
argument as `tools/hostsim` above — and it paid the same way: the first run
caught two transposed row/column offsets in the blit clipping expectations,
which on hardware would have looked exactly like a clipping defect in the
firmware.

The model is not a second implementation to be trusted over the firmware. It
is a second opinion. Where the two disagree, one of them is wrong and the
disagreement is the finding.

### Hardware results (2026-08-24)

The suite has been run on real hardware. 174 assertions, and every failure it
found was a real defect:

| Program | Result |
|---|---|
| `gpu64_test_system` | 39 / 39 |
| `gpu64_test_draw` | 36 / 36 |
| `gpu64_test_float` | 26 / 26 |
| `gpu64_test_pages` | 24 / 24 |
| `gpu64_test_math` | 30 / 30 (3 red before the `fixWrite()` fix) |
| `gpu64_test_blit` | 22 / 24 — `BIG EXACT` and `REU EXACT`, both open |

`gpu64_test_math` is the case worth remembering. `runsim.py
--firmware-rounding` predicted, on a PC, that exactly `ROUND NEG`,
`MUL MINUS ONE` and `SCALE NEG` would go red and nothing else. Hardware
produced exactly those three. The cause was `fixWrite()` biasing the
accumulator by ±128 and then arithmetic-shifting right by 8: the shift
floors, so on a negative accumulator the bias landed twice and every
negative 8.8 product came back one LSB further from zero than it should
(`-1.0 * 1.0` returned `$FEFF`, −1.00391, instead of `$FF00`). Positive
products were unaffected, which is why it survived a hardware-verified
milestone — a one-LSB error on negatives only is invisible in a rendered
frame. Fixed by biasing and dividing toward zero rather than shifting;
confirmed green on hardware.

**Two failures are still open**, both in the bulk-transfer path and both
reporting `$E1`:

- `BIG EXACT` — 8000 bytes to the framebuffer and back. First mismatch at
  offset `$1CE1` (7393), expected `$06`, got `$5C`.
- `REU EXACT` — a tile stashed into REU space through the REU's own
  controller and blitted from `space = 1`. First mismatch at offset 0,
  expected `$10`, got `$A9`.

Both commands returned OK, so the dispatches were accepted and the data did
not survive. This is the path CLAUDE.md flags as carrying both known REU
defects. `kitCmp` now also reports the total number of differing bytes,
which is what separates a single dropped byte (this project has a known drop
floor) from a stream that shifted.

**Anything else red is a new finding.** And a bench run showing a single
otherwise inexplicable glitch should be weighed against the known dropped
register write — roughly 1 transfer in 35000, always at the start of a
transfer — before it is attributed to anything here.

### Reading a bulk-transfer failure

`BIG` and `REU` each report three numbers beyond the verdict:

```
xxx DIFF AT    offset of the first mismatching byte
xxx DIFF N     how many bytes differ in total
xxx WANT/GOT   the expected byte and the actual one
```

`DIFF N` is the one that decides what happened. `0001` is a single corrupted
byte. A count that runs to the end of the buffer is a shifted stream, which
means the transfer lost or gained a byte rather than corrupting one — a
different bug with a different fix. Without the count, both report `$E1`.

`BIG` runs the same 8000 bytes through the API three times, and the three
passes differ in exactly one thing each so that the offsets can be compared:

| pass | source | framebuffer rect | destination |
|------|--------|------------------|-------------|
| `BIG`  | `$4000` | (0,100) 320x25 | `$6000` |
| `BIG2` | *(no new upload — re-reads the same framebuffer)* | same | `$6001` |
| `BIG3` | `$4001` | same | `$6000` |

The framebuffer base is a multiple of 256 in all three, so buffer index *k*
sits at framebuffer page offset *k* throughout; only the C64 addresses move.

* `BIG2` failing at the same *k* as `BIG` — the **destination** write address
  does not select the damaged byte.
* `BIG3` failing at the same *k* — the **source** read address does not
  either, and since `BIG3` is a fresh upload, the damage is chosen anew every
  transfer.
* `BIG3` failing one lower — the source address does select it.

`BIG2` alone cannot separate much: it re-reads a framebuffer that was
uploaded once, so if the upload is what corrupted it, every readback repeats
the same bytes faithfully and the results match whatever the cause. That is
why `BIG3` uploads again.

### What the three passes found (2026-08-25) -- FIXED, hardware-confirmed

`BIG3` came back **clean on both runs** while `BIG` failed on both: a second,
warm upload of the same 8000 bytes is correct, the first, cold one is not.
And every mismatch ever recorded -- eleven of them across five runs -- sits
beyond offset **1024**, the lowest at 1057.

1024 was `nWarm` in `gpu64_blobRead()`. The burst warmed the first 1024 bytes
of its destination and nothing after, so past that window every 64th store
landed on a cold cache line, the core had to read-allocate it from an SDRAM
the VideoCore is also driving, and the stall outlasted the C64 cycle the
burst was riding. The byte sampled was then whatever the bus had moved on to.
`gpu64_warmBuffer()` now warms the whole length, with real loads rather than
an advisory `prfm` -- `prfm` may be dropped by the hardware, and this is the
one place where "mostly warmed" and "warmed" differ by a corrupted byte.
`BIG2` showed no damage beyond `BIG`'s for the same reason it could not: the
upload had just warmed that staging buffer for the readback to re-read.

Two readings died on the way here, both artefacts of the test data: "the
wrong byte is always `$5C`, the page's last byte", and then "failures cluster
at page offset `$E1`". `kitSeq`'s original add-and-xor step was a bijection
on the byte, so its period was exactly **256, the same as a page**: every
byte value lived at one fixed page offset, which made "the value is `$06`"
and "the page offset is `$E1`" the same statement. `kitSeq` is now a Galois
LFSR of period 255, coprime with 256, and the clustering did not survive.

Confirmed on hardware the same day: with `src:cf073b3a` all three passes
report `0000` across the board, and the other five programs stayed green --
`gpu64_blobWrite()` took the same change and every test uses it.

One failure stays outside this account, and it is **not** the drop floor.
`REU EXACT` -- and once `BLIT EXACT` -- has failed on three of seven runs, on
a **64-byte** transfer. 64 bytes is inside any warm window, so the cold-line
defect cannot reach it; and three in seven is orders of magnitude above the
~1/35000 register-write drop rate. `gpu64_probe_reu` was written to measure
it: see below.

### gpu64_probe_raster -- the class 2 frame loop

`tests/gpu64_probe_raster.a`. Written for the raycast blink and
useful for any class 2 batch defect that only shows up over frames. 300
frames in about six seconds, three phases differing in exactly one axis:

| Phase | Records | Flip | Isolates |
|---|---|---|---|
| `BIG` | 320 | yes | the demo's own frame shape |
| `NOFL` | 320 | no | whether the fault is in the page flip |
| `SML` | 40 | yes | whether the fault scales with the size of the pull |

Nothing in it is random and nothing in it moves: the same checksummed batch
is drawn every frame and the expected pixels are a constant, so a screen
that is right on frame 1 and wrong on frame 40 has no explanation other than
the frame loop. Each phase counts four things separately, and which one
moves is the answer:

- `ERR` -- a dispatch did not return OK. The batch carries a checksum, so a
  pull that arrived damaged appears here as `BAD ARGS` ($04) rather than as
  garbage. This separates "the records did not arrive" from everything else.
- `STAT` -- OK returned, but `RASTER_STATS` does not report the batch that
  was sent. The records arrived and the parse disagreed with what was
  written.
- `PIX` -- stats perfect and the pixels still are not there. A 32-pixel
  strip is read back with `READ_RECT` every frame and compared to the colour
  the records asked for.
- `1ST` -- the first frame of the phase on which anything went wrong, so a
  late onset is visible as a number rather than as a feeling.

All zero on all three phases is clean. `PIX 0064` on `BIG` with `ERR 0000`
and `STAT 0000` means every frame drew nothing while claiming to have drawn
everything -- which is what the raycast demo looks like.

### gpu64_probe_reu

`REU EXACT` does three things at once, and any of them could be at fault: it
stashes a tile into REU space using the REU's **own** controller (ten
consecutive writes to `$df01`-`$df08`, exactly the register-write pressure
the `$df00`-`$df0a` decode path is known to be fragile under), blits it out
of space 1, then reads the framebuffer back. The probe repeats each part 64
times and reports `FAILS` / `1ST` / `OFF` / `W/G` per phase:

* **RAW** — stash and fetch back with the REU controller alone, no gpu64
  command involved. Red here means the defect is in RAD's REU emulation and
  gpu64 is only the messenger.
* **API** — the whole `REU EXACT` path repeated, stash included.
* **ONCE** — one stash, then 64 blit-and-read cycles. Clean here but red in
  API pins it on the stash rather than on reading REU memory.

`1ST = 0000` with `FAILS = 0001` would be the notable case: only the very
first transfer after start-up affected, which is all a single-stash program
like `gpu64_test_blit` can ever see.

64 iterations is deliberately modest — each writes ten REU registers, and
heavy REU register traffic is the documented way to latch the emulation into
total transfer failure. `FAILS` jumping to a large number from some `1ST`
onward is that latch, not this defect, and needs a power cycle.

**First result (2026-08-25): all three phases clean, 0 failures in 192
iterations.** That is not a null result, it is a constraint. If `REU EXACT`
failed as an independent event at anything like the observed 3-in-7 rate, a
run of 192 would be all but certain to catch it, and it caught none. So the
failure is **not** a property of the REU-space path repeated in isolation —
not the controller stash, not the space-1 blit, not the readback. What
remains is either something about the state `gpu64_test_blit` is in when it
reaches that line, some twenty commands in, or an event rarer than three runs
in seven made it look.

Cheapest things to check next: whether the framebuffer rectangle `REU EXACT`
reads back at (180,100) overlaps anything an earlier check in the same
program painted — the probe used (0,0) and could not have seen that — and
whether the probe stays clean with those twenty-odd commands replayed ahead
of it.

### gpu64_probe_latch -- which stage 16 layer arms the latch

`tests/gpu64_probe_latch.a`. Written for the stage 16 derail (§ 16b
of the progress tracker), after the arm-B run that accepted 204 commits and
then lost 98.5% of every register write afterwards -- an abrupt latch, not
accumulating timing damage, and the signature this document and CLAUDE.md
both attribute to the `$df00`-`$df0a` REU decode path, which the gpu64
command window `$df0b`-`$df21` had never reproduced.

Stage 16 changed three things at once, and the bench data cannot separate
them because all three scale with the same number -- commits accepted. So
the probe adds them one at a time, in one launch, out of paths that are each
already hardware-verified on their own:

| Phase | Issues, flat out | Adds |
|---|---|---|
| `NOP` | class 0 `NOP` | command-window write pressure only |
| `FLI0` | class 0 `PAGE_FLIP` arg **0** | + the flip work, in the *dispatch's* hold |
| `FLIW` | class 0 `PAGE_FLIP` arg 1, then a `STATUS` poll until `BUSY` clears | + the same work in the *loop's* hold, with an **empty** armed window |
| `FLIP` | class 0 `PAGE_FLIP` arg 1, flat out | + dispatch traffic **inside** the armed window |
| `REND` | class 1 `SCENE_COMMIT` | + core 1 rendering a frame |

`FLIP` is the rung worth understanding. A deferred flip needs `calibrated`
but not `armed`, and returns `BUSY` while one is outstanding, so a program
that asks flat out self-paces to ~60 accepted flips a second at a ~21%
accept rate -- the same two numbers every healthy stage 16 run reports, with
core 1 doing nothing at all. `REND` renders an **empty** scene (a camera, no
objects), so core 1 still does the per-frame clear and the ~200-row cache
clean but not the mesh raster; a clean `REND` against a dying
`gpu64_loop_test` therefore narrows the answer rather than leaving it open.

Per phase it reports `ATT` / `ACC` / `MISS` / `DUR`, the last being jiffies,
so every count can be normalised per dispatch, per second and per accepted
flip. That is deliberate: normalising three ways is what turned `MISSED`
from noise into a fingerprint in round 4, and two counts had already been
misread off bench photos by then. `MISS` is reported and never judged --
the drop floor is pre-existing, so a nonzero count is the measurement, not
the failure.

Reading it:

- **All three rows complete, C64 alive** -- none of the three axes arms it
  in this combination. What is left between this and `gpu64_loop_test` is
  the mesh raster burst and `ROTATE_LOCAL`'s argument writes.
- **`NOP` dies, or its `MISS` sits far above the ~1/180000 floor** -- sheer
  write pressure on the command window arms it, and stage 16 is merely the
  first workload fast enough to get there. Core 1 and the flip are not
  involved.
- **`NOP` clean, `FLIP` dies (or `MISS`/`ACC` lands near 0.19)** -- the
  deferred commit-flip window is the mechanism and core 1 is exonerated. It
  would also explain why milestone 4b/4d was clean: nothing ran between the
  arm and the commit there.
- **`NOP` and `FLIP` clean, `REND` dies** -- core 1 rendering while core 0
  polls a free-running C64 is what arms it. That is the milestone 6a burst
  rule at 100% duty cycle, a regime 6a never measured because its own ladder
  paced the bursts apart.

The rungs run in one launch, in order, so a latch that needs total
accumulated exposure could be armed by `NOP` and charged to `FLIP`. `FIRST
ERR`'s attempt number and the `DUR` column are what tell those apart: an
onset at the same absolute dispatch count wherever it lands is cumulative;
an onset at a fixed count *within* a rung belongs to that rung.

**No firmware change is needed for any of it**, which is the point -- the
thing under test is a cycle-critical loop, and any instrumentation added to
it would be a change to the thing being measured.

Desk-checked against `tools/prgsim/runsim.py`, which models class 0 but not
class 1: the `NOP` and `FLIP` rungs run there for real (the model mirrors
`PAGE_FLIP`'s `BUSY` and its vblank-driven commit), and `SEQ`/`SEQACK` were
added to the model for it, so `MISS` reads `0000` on a perfect bus. Budgets
are `.weak`, so a desk run can shrink them:
`64tass --cbm-prg -D N_NOP=200 -D N_FLIP=400 -o out.prg gpu64_probe_latch.a`.
The bench always builds the defaults.


`tools/prgsim/runsim.py --alias-drop=OFF[,OFF...]` injects a byte-level fault
at chosen offsets, so the suite's diagnostic lines can be read against a
known answer on a PC before a bench run is spent on them.

### gpu64_probe_bus -- was the address mis-sampled, or the access dropped?

`tests/gpu64_probe_bus.a`. Written after the 62291-frame Quake run
of 2026-09-08, which was clean on everything its screen could report except
135 frames whose commit answered `ERRCODE` **$FF**.

`$FF` is not an errcode -- `GPU64_ERR_*` stops at $0C -- and it has exactly
two producers that that screen could not tell apart:

- **(a) mis-sampled address.** The loop serviced the read, but `IO_ADDRESS`
  came out wrong and `gpu64_apiReadReg()` fell through its four cases to
  `return 0xFF`. `IO_ADDRESS` takes A0-A3 from GPIO 10-13, which are
  dedicated, and A4-A7 from GPIO 0-3, which the LVC257 multiplexes with NMI,
  ROMH, IO1 and BUTTON. Every gpu64 register lives at `$DF0B`-`$DF23`, so
  A4-A7 read 0 when sampled correctly and a mis-sample can only ever *set*
  bits: `$DF0E` degrades to `$1E`, `$2E`, `$4E` or `$8E`, all still inside
  the window and all returning `$FF`.
- **(b) dropped access.** The loop never sampled the read at all -- the
  ~1-in-180000 floor -- and the C64 latched a floating bus reading `$FF`.

The responses differ, which is why separating them matters. Under (a) the
firmware's state is intact and only the answer was wrong, so a C64-side
re-read repairs it and the API can say so. Under (b) a *write* in the same
position would have vanished silently, which no re-read repairs and which
only a sequence number can even detect.

**This one does need a firmware change**, unlike `gpu64_probe_latch`, and
that is the whole point: until now the drop rate had a numerator
(SEQ/SEQACK mismatches) and no denominator, so "1 in 70 commit dispatches"
and "1 in 180000 writes" were never comparable numbers.
`Source/Firmware/gpu64_busstats.h` counts, on core 0 and deliberately *after*
`WAIT_CYCLE_READ2`/`SET_GPIO` so it is off every deadline path, how many
gpu64-window reads and writes the polling loop actually serviced and how many
of those carried an address no gpu64 register occupies. They come out through
`GET_HEALTH` bytes 48-63 (see
[../docs/class0-2d-reference.md](../docs/class0-2d-reference.md)). Nothing
was added to the `$df00`-`$df0a` REU decode path, per CLAUDE.md.
`reuUsingPolling()` grew 0x2558 -> 0x2684 against the 0x2800 IPL window.

Four columns per rung:

| Column | Meaning |
|---|---|
| `ISSD` | what the C64 issued, counted at every single read site |
| `SAW` | what the loop serviced, minus the fixed cost of the `GET_HEALTH` round trip itself (the `CAL` row) |
| `BAD` | of those, how many carried an impossible address |
| `FF` | how many answers came back `$FF` |

and four rungs, all class 0 -- no viewport, no camera, no core 1, because a
rung that cannot fail for an unrelated reason is worth more here than a
realistic one:

| Rung | Issues | Isolates |
|---|---|---|
| `READ` | `lda ERRCODE` flat out, 8 per iteration | maximum read pressure on an otherwise quiet bus |
| `DISP` | `sta CMD_LO` (NOP) then `lda ERRCODE` | the shape every command helper in this tree emits: the read lands on the instruction right after a DMA hold released |
| `FLIP` | the same read, but only while a deferred `PAGE_FLIP` is in flight | races the loop-fired commit hold -- the closest class-0 analogue of the Quake frame |
| `WRIT` | `sta ARG+0` flat out | the write-side twin; `FF` is meaningless here and prints `----` |

`FLIP` exists because the Quake rate works out at about one event per 460
commits, two orders of magnitude above the documented write-drop floor. If
the commit window is what concentrates them, this is the rung that says so.

Reading it:

- `SAW < ISSD` -- **(b)**, and by exactly how much.
- `BAD > 0` -- **(a)**, and the `BADAD` row names the bits. Expect the low
  nibble to match the register the rung was reading (`$0E` for `ERRCODE`)
  and the damage to be in the high nibble; anything else refutes (a).
- `FF` well above `BAD + (ISSD - SAW)` -- a **third** mechanism, in the data
  half of the bus rather than the address half. That would be a new finding,
  and it is the one verdict path never exercised on a PC.

The `CAL` row is the load-bearing part of the arithmetic. It is two
back-to-back `GET_HEALTH` calls whose delta is the per-round-trip overhead --
*measured*, not counted, because the firmware counts the triggering `CMD_LO`
write only after dispatch. It prints `RR WW EE`; `EE` must be `00` or
`healthBuf` was never filled and every number below it is a difference
between two copies of nothing. Desk runs give `CAL 01 07 00`, which is what
was predicted by hand, and that agreement is what keeps a systematic
accounting error from reaching the bench disguised as `DROPPED ACCESS`.

Desk-checked against `tools/prgsim/runsim.py`, which grew a fault injector
for it: `--bus-fault=drop:N`, `addr:N` or `both:N` corrupts every Nth access
in the gpu64 window, so the verdict tree can be shown to fire before a bench
run is spent on it. Confirmed on a PC: `drop:401` shortens `SAW` by exactly
the `FF` count and prints `DROPPED ACCESS`; `addr:401` leaves `SAW == ISSD`
with `BAD == FF` and prints `MIS-SAMPLED ADDRESS`; `both:401` prints
`MIS-SAMPLED AND DROPPED`; an uninjected run prints `CLEAN - NO EVENT`.
Budgets are `.weak` (`N_READ`, `N_DISP`, `N_FLIP`, `N_WRIT`, `N_BURST`), so
a desk run can shrink them; the bench builds the defaults. Desk runs need
`--stop-after=100000 --demo`, because the default `--stop-after=4` aborts any
probe whose iteration loop polls `$91`.

A clean run is not a pass. The defect is rare; a clean run bounds its rate
from above and nothing more.

### Bench run-sheet -- round of 2026-09-09, RUN, and what it said

Ran on build `743dfa5c-dirty src:29172d5e`. **`VERDICT CLEAN - NO EVENT`**,
`PHASE CRDFWV`, every column zero across ~111000 gpu64-window accesses, `CAL`
matching the desk value to the byte. The counters work on hardware and the
polling-loop change is safe; `FLIP`, the leading suspect, produced nothing in
27439 reads. What it does *not* do is disprove anything -- against a
~1-in-180000 floor a run this size predicts well under one event, exactly as
step 4 below warned. Full reading in
[progress_tracker.md](progress_tracker.md) § 19.

**The next move is not a bigger `probe_bus` run.** It is to read the same
health-block counters from **`gpu64_demo_quake3d`**, the class-1 demo, which
is the only workload known to produce the events -- its `SCENE_COMMIT` is
what answered `$FF`, and it is sent without the retry the demo's node updates
get. `gpu64_demo_quake` is the class-2 raster port and is not the subject.
The sheet below is kept as the procedure.

Everything below was built and desk-verified before the round. In priority
order, because the first item is the one the round exists for.

1. **Deploy.** `SDCARD=<mountpoint> tools/build.sh`, then copy
   `tests/gpu64_probe_bus.prg` to the card. Check the boot log's
   build id -- a stale-firmware deploy has silently wasted whole rounds. The
   id moves off `src:a3c2943e`.
2. **RAD menu: press T until it reads REU.** Without it `reuUsingPolling()`
   never runs and none of gpu64 executes.
3. **Run `gpu64_probe_bus`.** Power-cycle first; a re-launch from the RAD
   menu inherits firmware state from its predecessor. It is a single screen
   and a short run. Photograph the whole screen -- `CAL`, all four rungs,
   `BADAD`, `VERDICT`, `PHASE`. `PHASE` should read `CRDFWV`; if it stops
   short, that rung is where the machine died and the photograph is still
   the finding.
4. **If `VERDICT` is `CLEAN - NO EVENT`**, run it two or three more times
   rather than concluding anything -- the Quake rate would put roughly one
   event in a run of this size, so a single clean screen is expected under
   *either* hypothesis. If it stays clean across repeats, re-run the Quake
   demo and read `ERRCODE $FF` frames against the same health-block counters,
   which now exist there too.
5. **Then re-run the Stage 16 loop test and the Quake demo** as the regression
   check on the firmware change itself: the bus-stats counters sit in the
   polling loop, and the loop staying inside its IPL window on a PC is not
   the same as the C64 staying alive.

What the answer buys, so the round is not spent twice:

- **MIS-SAMPLED ADDRESS** -- the fix is C64-side and cheap. Re-read a
  register that answers `$FF`; document it as the contract (already written
  up in [../docs/error-codes.md](../docs/error-codes.md)) and add the re-read
  to `Source/Demos/gpu64_demo_quake3d.a`. That mitigation is deliberately
  *not* in the tree yet -- it is speculative until this measurement lands.
- **DROPPED ACCESS** -- no re-read helps a dropped write, so the answer is
  the sequence number, and the useful output is the measured rate with a
  denominator: the first number that makes the read and write drop floors
  comparable.
- **UNEXPLAINED FF** -- stop and redesign. The fault is in the data half of
  the bus and nothing in the tree currently measures that.

### Bench run-sheet -- attributing MISSED: RUN, and what it said

Run 2026-09-09. **Verdict: some of each, and mostly neither.** 3895 frames,
quit with RUN/STOP. `DISP` 17147 firmware vs 17184 C64 -- short by 39 against
a +2 baseline, so 39 `CMD_LO` writes were lost. `SEQREP` 85 against a
baseline of 2, so 83 `SEQ` writes were lost. Together 122 of `MISSED`'s 198.
`MISSRD` 561 -- three times `MISSED` -- so the dominant failure is a
**`SEQACK` read that is wrong once and correct on an immediate re-read**, and
`lastSeqAck` `$FE` (a stale but plausible value, not `$FF`) says those are
not all mis-sampled addresses. `CMTMISS` fell to 1 in 85, so the 1-in-6
`SCENE_COMMIT` concentration that motivated this run is not reproduced.

`readsBad` 50492 of 1171878 was an instrument bug, now fixed: `sta ARG,y`
makes the 6502 dummy-read the write-only register it is about to write, and
every one scored as a mis-sample. Those now go to `argDummyReads`, GET_HEALTH
bytes 72-75, shown as a third column on `BUS RW`. `orBadAddr` `$FF` survives
the correction -- the `ARG` range only reaches `$3F`, so bits 6 and 7 came
from real high-nibble mis-samples, and next round's `readsBad` counts exactly
those. Full write-up: [progress_tracker.md](progress_tracker.md) section 21.

**Run 2 (corrected instrument, 4926 frames): the SEQACK read is the defect.**
`readsBad` 50492 -> 965 with `argDummyReads` 69675 taking the difference, so
the fix landed. But `MISSRD` 805 of 22140 sequenced dispatches is **1 in 27**
for that one read, and those 805 are 83% of all 965 bad reads while SEQACK
reads are 1.5% of all reads -- a 55x concentration on a single instruction,
which a bus floor cannot produce. Write side reproduced worse: 1133 bad
writes in 241368 (1 in 213), `DISP` short by 75, `SEQREP` up by 100.
`lastBadAddr` and `lastBadWrite` both `$F9` = ARG8 with bits 5-7 set: high
nibble only, the first check of the LVC257 fingerprint against a real
address. Section 22 of [progress_tracker.md](progress_tracker.md).

**Next round adds two fields and asks one question:** is that read getting a
floating `$FF` (mis-sampled address) or a stale-but-valid value (the hold
gate deferred the dispatch past it)? `SEQREP` row now carries how many of the
`MISSRD` first-reads were `$FF`, and the `DISP` row carries
`dispatchDeferred`/`dispatchFired` from GET_HEALTH bytes 44-47. All-`$FF`
with a zero deferred pair = bus fault; anything else = a race in the gate.
**Firmware is unchanged from run 2 -- only the PRG needs redeploying.**

The procedure below is unchanged and is the one to repeat.

### Bench run-sheet -- next round: attributing MISSED

Built 2026-09-09, after the quake3d run showed 1284 SEQACK mismatches in
7703 `SCENE_COMMIT`s -- one commit in six -- against a bus `probe_bus` had
just measured as clean over 111000 accesses. Those cannot both be about the
same defect, and until now nothing on the screen could say which access a
mismatch was even about. See [progress_tracker.md](progress_tracker.md)
section 20 for the argument; this is the procedure.

**One run, one screen, five minutes.**

1. **Deploy.** `SDCARD=<mountpoint> tools/build.sh`, then
   `tools/deploy_prg.sh`. The build id moves off `src:29172d5e` -- check the
   boot log, a stale-firmware deploy has silently wasted whole rounds. The
   firmware change is the 80-byte `GET_HEALTH` rung and two counters written
   inside `gpu64_apiDispatch()`; nothing was added to the polling loop.
2. **RAD menu: press T until it reads REU.**
3. **Run `gpu64_demo_quake3d`.** Power-cycle first. Walk around for a minute
   or two -- the events scale with frames, and the commit rate above would
   put ~1000 of them in that time. Then **quit with RUN/STOP**: the four
   bottom rows are filled in by `finish` and a run that is switched off
   instead of quit produces nothing.
4. **Photograph the whole screen.** The four new rows are `BUS RW`,
   `BUS BAD`, `DISP`, `SEQREP` on rows 19-22, and they are differences across
   the run.

Reading it, in this order:

| what the screen says | what it means |
|---|---|
| `DISP` firmware **below** `DISP` C64 | the `CMD_LO` writes are being lost -- the real drop defect, now with a denominator in `BUS RW` |
| `DISP` firmware = C64 + 2, `SEQREP` **above 2** | the commands all ran; the `SEQ` writes are the casualty. Harmless, and the retry the demo sends is unnecessary work |
| `DISP` firmware = C64 + 2, `SEQREP` = 2 | nothing was lost on the write side at all, and `MISSED` has never been the drop defect |
| `BUS BAD` non-zero | an access *was* serviced at a mis-sampled address; the OR/AND bytes name the bits, expect them in the high nibble |
| `MISSRD` (row 22, second field) non-zero | reads of `SEQACK` that were wrong once and right immediately after -- the `$FF` producer, and the C64-side re-read already repairs it |

The +2 on `DISP` and the 2 on `SEQREP` are the baseline, not slack:
`LOOP_STOP` and the closing `GET_HEALTH` are both dispatches and neither
carries a sequence number.

All three branches were executed on a PC first, with
`runsim.py --bus-fault=drop:997 / addr:997 / drop:53` -- the table is in
section 20. A clean run is still not a pass: it says the write side is
innocent, which is a finding, not an absence of one.

## Testing on hardware

The friction the progress tracker calls out -- "physically move the SD card
to the RPi" -- has two different answers depending on which stage of
development you're at, because **GPIO14/15 (the only UART pins broken out on
the 40-pin header) are physically wired into the cartridge's bus latches**
(`OE_Dx` = GPIO14, `LATCH_A0` = GPIO15, see
[gpio_defs.h](../Source/Firmware/gpio_defs.h)). That rules out Circle's
built-in serial/USB bootloader (`doc/bootloader.txt`, the "Flashy" tool) for
any build that's actually plugged into a C64 -- it needs those exact two pins
for a USB-serial adapter.


**Result (2026-09-06, three power-cycled runs): outcome 3.** `NOP` was
20000/20000 with `MISS 0000` every run; `FLIP` derailed every run after
43/50/69 accepted flips with `MISS 0000` and a `TRAP 01` (BRK) at a wild
PC; `REND` was never reached. The deferred commit-flip is the fault, on
its own, with no dropped writes and no core 1. Full write-up in
[progress_tracker.md](progress_tracker.md) section 16b.

Two things to know before re-running it. The unfinished rung's `DUR` field
is **stale**, not zero: `dur` is only written by `finishPhase`, so a rung
that dies mid-way still shows its predecessor's duration — derive its
elapsed time from accepted flips at 60 Hz instead. And the accepted
*ratio* on `FLIP` is ~2.6%, not the ~21% the class-1 runs show, because a
class-0 `PAGE_FLIP` dispatch is roughly ten times cheaper; 60 accepted
flips a second is the invariant, not the percentage.

**`FLI0` and `FLIW` were added after that run** to split what was left.
They are what makes the ladder decisive rather than merely suggestive,
because the three rungs now differ by exactly one thing each:

| | fires the commit from | armed window contains |
|---|---|---|
| `FLI0` | *(no deferred commit -- the flip happens inside the dispatch)* | -- |
| `FLIW` | the polling loop | nothing |
| `FLIP` | the polling loop | ~40 refused dispatches |

- **`FLI0` dies too** -- the flip *work* is the fault, immediate path
  included. Milestone 4b has carried this all along and nothing ever
  issued enough flips to find it.
- **`FLI0` clean, `FLIW` dies** -- the loop-fired DMA hold itself, or
  something inside `gpu64_commitFlip()` that stalls unpredictably. Re-run
  with `LOG_ENABLE` on and read `gpu64FlipStats`: `slowCount > 0` means
  the mailbox fast path fell back to Circle's ~900 us blocking
  `SetVirtualOffset()` from inside the hold ([gpu64_fb.cpp:257](../Source/Firmware/gpu64_fb.cpp#L257)).
- **`FLI0` and `FLIW` clean, `FLIP` dies** -- the *armed window* is the
  mechanism, and the only thing that changes across that line is
  instruction-cache residency of the commit path. `gpu64_apiWarmPollingLoop()`
  re-warms it while a flip is armed ([rad_reu.cpp:668](../Source/Firmware/rad_reu.cpp#L668))
  but is called only from the class-1 and class-2 dispatch paths -- **there
  is no class-0 call site**. So class-0 flip traffic gets one warm, at arm
  time, and then a frame's worth of dispatches with nothing to renew it.
  The fix is a one-liner and the comment at
  [rad_reu.cpp:643](../Source/Firmware/rad_reu.cpp#L643) already predicts
  the failure.

Don't enable `LOG_ENABLE` for the discriminating run itself -- the overlay
is drawn inside `PrepareFlip()`, so it adds i-cache pressure to the exact
window under test. It's the follow-up instrument, not part of the ladder.

**Result, 2026-09-06 -- outcome 3, twice.** Two power-cycled runs, five
rungs each, identical to the digit:

| rung | ATT | ACC | MISS | DUR | reading |
|---|---|---|---|---|---|
| `NOP` | 4E20 | 4E20 | 0000 | 01E7 | 20000/20000, both runs |
| `FLI0` | 07D0 | 07D0 | 0000 | 0032 | 2000/2000, both runs |
| `FLIW` | 012C | 012C | 0000 | 012C | 300/300, both runs |
| `FLIP` | 1300 / 05A0 | 007F / 0025 | 0002 | *(stale)* | derailed, both runs |
| `REND` | -- | -- | -- | -- | never reached |

`FLIW`'s `DUR` is 300 jiffies for 300 deferred flips: exactly 60.0/s, the
frame clock, sustained for five seconds with not one lost write. That one
row acquits the loop-fired DMA hold *and* the mailbox/`slowCount`
candidate at the same time, because both are present in `FLIW` in exactly
the form they take in `FLIP`. `FLI0` acquits the flip work itself.

`FLIW` and `FLIP` run the same firmware path at the same rate with the
same holds and differ **only** in whether dispatches are issued inside the
armed window. So the armed window is where the mechanism lives. That much
still stands.

**What did not stand: the cause.** The missing class-0 re-warm was fixed at
[gpu64_api.cpp:1109](../Source/Firmware/gpu64_api.cpp#L1109) and the next
run derailed exactly as before -- 29 accepted flips, squarely inside the
25-127 spread the unfixed builds gave. The re-warm is left in place (it is
correct by symmetry with classes 1 and 2, and removing it now would be a
second uncontrolled variable), but it is **not** the cause and the ladder's
conclusion above was published a run too early.

Two incidental corrections the run forced:

- **An immediate flip is cheap.** `FLI0` ran 2000 dispatches in 0x32
  jiffies = 0.83 s, ~2400/s, the same rate as `NOP`. The mailbox round
  trip is asynchronous (`gpu64_flipPost`), so the "~900 us per flip"
  figure in the old `N_FLIP0` budget comment was simply wrong.
- **Time-to-derail on `FLIP` varies about 3.4x** between runs (127 vs 37
  accepted flips, ~2.1 s vs ~0.6 s; earlier runs gave 43/50/69). It is
  probabilistic, and it is why a short run is not evidence of a fix. Give
  a candidate fix the full 6000-iteration budget.

### The subject was wrong: it is the Pi that dies (2026-09-06)

Three further runs, and between them they demolish the framing every
earlier round used. The C64 is not derailing and dragging the Pi down; the
Pi goes away and the C64 is what is left holding a dead bus.

| run | `FLIP` reading | `FIRST ERR` | what the Pi did |
|---|---|---|---|
| C | `0440 / 001D / 0000 / 012C` | `05 @ 03 0239` | solid green LED, HDMI black |
| D | -- | -- | **fresh Circle boot log on HDMI**, from `00:00:00` |
| E | `0660 / 002B / 0001 / 012C` | `01 @ 03 0459` | HDMI black, C64 `TRAP 01 4003` |

Three independent reasons the Pi is the subject:

1. **`FIRST ERR 05` is not an error code.** `$05` is not a legal
   `PAGE_FLIP` failure (`BAD_ARGS` is 04, `UNSUPPORTED` 06, `BUSY` 07) --
   it is `OP_PAGE_FLIP` itself, the last byte the 6510 drove onto the bus
   before `lda ERRCODE` in `cmd0`. Reading an IO2 nobody is driving returns
   the last bus value. So at `FLIP` iteration 569 the Pi stopped answering,
   and the C64 went on looping ~500 more times reading its own last write
   back with `ACC` frozen at 29. The C64 was *fine* at that instant.
2. **Run D booted.** The HDMI showed a complete Circle start-up log
   beginning at `00:00:00` with the right build id and ending normally at
   `detectSID`, with the RAD menu logo. **No exception dump.** Circle
   installs handlers and prints one on a fault, so this is a *hard reset*,
   not a panic: watchdog or supply.
3. **Run E's `TRAP 01 4003`.** A BRK at `$4003` -- the probe occupies
   `$0801-$0ff9`, so the 6510 had run off into blank RAM and executed
   zeros. That is what a 6510 does when the cartridge stops driving DMA
   cleanly mid-instruction, and it retro-explains the blank `TRAP`/`VERDICT`
   rows of runs A and B rather than needing an instrument bug.

Note the failure mode *varies* run to run -- Pi hangs, Pi resets, C64
derails first. A single deterministic software bug does not usually
present three ways.

**Two concrete defects found while reading the path** (both fixed
2026-09-06, both in the `FLIP` rung's path and nowhere else in the ladder):

- `gpu64_flipPost()` spun on `MAILBOX1_STATUS & FULL` **with no bound**,
  on core 0, inside `gpu64_vsyncCommitFlip()`'s DMA hold. "It cannot
  happen" was the only thing between a wedged VideoCore and a Pi that
  never samples the bus again -- which is exactly run C. Now bounded by
  `GPU64_FLIP_POST_FULL_TIMEOUT_US` and counted.
- `gpu64_flipDrain()`'s timeout was **50 ms**, and `gpu64_flip.h` claimed
  it ran "outside the hold". It does not: `gpu64_apiDispatch()` is entered
  with `CLR_GPIO(bDMA_OUT)` already asserted, so that was a licence to
  halt the C64 for 50000 cycles inside one command. Lowered to 5 ms,
  counted, and the header comment corrected.

`FLIP` is also the only rung that dispatches while a flip is in flight, so
it is the only rung that can reach either of those waits at all. That is a
second reading of "the armed window", and one that does not depend on the
instruction cache.

**The power hypothesis has never actually been tested.** `readHealth()` in
`gpu64_api.cpp` has exactly **one** call site -- the `GET_HEALTH` opcode --
despite its own comment describing a once-a-second poll. There is no poll.
So the sticky-under-voltage red border it can raise could not have fired on
any run in this campaign, and a hard Pi reset with no exception dump is
precisely what a sagging 5V rail looks like. `gpu64_probe_latch` now issues
`GET_HEALTH` after setup and after every rung and prints the result on a
`HEALTH` row -- see the row legend in the probe source.

**Unexplained, and worth watching:** on both runs the `TRAP` (row 16) and
`VERDICT` (row 18) rows were *blank*, while `PHASE` showed `NIWFV` -- the
`V` breadcrumb is written by `allDone`, which then unconditionally calls
`verdict`, which unconditionally prints one of three non-empty strings at
row 18. `ALIVE` showed `STAGE 11`, not the `30` `finishPhase` writes, so
`FLIP` did not exit through `finishPhase`; BASIC's `READY.` landed at row
20, so nothing scrolled and the rows really are empty. Both direct
absolute stores (`phase`) worked while both `printAt` calls produced
nothing. No reading of the three clean rungs depends on this -- their
numbers come from rows the loop refreshes live -- but if it recurs, that
is a corrupted-6510 fingerprint worth capturing rather than an instrument
bug: the strings are present and the host model prints them.

### Tier 1 -- bare RPi on the bench (no C64 needed)

For pure firmware bring-up (does it boot, does the menu render, does REU
emulation work against nothing) you don't need the cartridge at all: a bare
RPi 3A+/Zero 2, HDMI monitor, and a 3.3V USB-serial adapter wired to GPIO14/15
gives you Circle's serial bootloader for real edit-build-flash-see cycles
with **no SD card handling per iteration**:

1. One-time: build Circle's bootloader kernel (`external/Circle/boot`, `make
   all`, follow `doc/bootloader.txt`) onto an SD card that stays in the Pi
   permanently.
2. Each iteration: `tools/build.sh` builds `kernel8.img`, then Circle's flash
   tool pushes it over the serial link and the Pi runs it immediately -- no
   SD access, no physical trip.
3. `CSerialDevice` logger output comes back over the same link, so you get a
   real console, not just on-screen text.

This is the right loop for early gpu64 milestones (2 and 3 in particular)
before bus-sniffing/C64 interaction needs to be verified for real.

### Tier 2 -- seated in a live C64 (GPIO14/15 unavailable)

Once you're testing against the real bus, use:

- **A USB microSD card reader with a built-in cable** (5–6 foot cable is ideal)
  plugged into your PC's USB port permanently, with the female socket dangling
  on your desk. When you want to redeploy, power off the RPi, mount the card
  from the reader on your PC, run `SDCARD=/mount/point tools/build.sh`, then
  power the RPi back on. This is the actual fix for "physically move the card"
  — the card never moves, you just toggle the RPi's power and re-mount from
  your desk. `SDCARD=/mount/point tools/build.sh` makes the whole
  rebuild+redeploy a single command.
  
  (Why a reader with cable instead of an extension on the RPi side? The RPi's
  microSD slot is female, and USB readers are also female, so you can't daisy-chain
  them. A long-cable reader is simpler and avoids the signal-integrity issues that
  plague flimsy ribbon extensions between the two devices.)

- **Power-cycle to reboot**: Circle doesn't support hot-reloading a new kernel;
  after deploying the new firmware, power off/on the RPi (or the C64, if the
  RAD is powered from the expansion port) to boot into it.

- **On-screen logging as the console**: with UART unavailable, the debug
  channel is `logger->Write()`, tee'd to HDMI via
  [`Source/Firmware/tee_device.h`](../Source/Firmware/tee_device.h) --
  `CTeeDevice` fans every log line out to both `m_Serial` (works on Tier 1,
  a no-op on Tier 2) and `CHDMIConsole` (works on both). **Important:**
  neither plain `CSerialDevice::Write()` nor `CScreenDevice::Write()`
  (Circle's own text console) can be used for this once RAD calls
  `DisableIRQs()`, which happens early in `CRAD::Run()` and stays disabled
  for essentially the rest of the program's life (needed for cycle-precise
  C64 bus timing). With `REALTIME` defined (`Source/Firmware/Circle/sysconfig.h`),
  both of those classes check `CurrentExecutionLevel()` and silently drop
  the write whenever the IRQ mask bit is set -- Circle's DAIF-based check
  can't distinguish "really inside an interrupt handler" from "IRQs are
  just globally masked", so on-screen logging via the normal console
  appears to work at boot and then silently goes dark the moment
  `DisableIRQs()` runs, with no hang and no error. `CHDMIConsole` sidesteps
  this entirely by blitting text directly via `SetPixel()` (using Circle's
  `CCharGenerator` font data), which has no such guard -- same mechanism
  `showTestPattern()` already relies on. It only flushes the cache range for
  the screen row(s) it actually touched (not the whole framebuffer) --
  doing a full-framebuffer flush per call was cheap the first time (boot)
  but expensive enough to blow the per-rasterline timing budget on any
  later call during live operation, which showed up as bus corruption
  (garbled characters) in testing before this was narrowed down.
  Layout: a reserved box top-left (`GPU_OUTPUT_BOX_W` x `GPU_OUTPUT_BOX_H`)
  for the eventual upscaled C64-passthrough image, log text in the column to
  its right using the full screen height.

### Two hardware bring-up gotchas that cost real debugging time

**1. `KERNEL_MAX_SIZE` too small for RAD's real `.bss` -- total silent hang,
zero diagnostic output.** RAD's static buffers (`printOutputFile`, `filesAll`,
`vsf`, `mempool`, `sort`, `previewImage`, etc.) add up to ~98MB, well past
Circle's stock 64MB `KERNEL_MAX_SIZE` default. Circle places the kernel
stack, exception stacks, and MMU translation tables at fixed offsets
computed from `MEM_KERNEL_START + KERNEL_MAX_SIZE` (`memorymap.h`) -- with
the old 64MB cap those landed *inside* RAD's real `.bss` range, so crt0's
`.bss`-zeroing at boot silently corrupted the stack/page tables before a
single instruction of application code (not even the earliest possible
serial log line) could run. Fixed by raising `KERNEL_MAX_SIZE` to 160MB in
[`Source/Firmware/Circle/sysconfig.h`](../Source/Firmware/Circle/sysconfig.h).
This required a full clean rebuild of Circle's base `lib/` (not just the
app) -- the offsets are baked into `libcircle.a` at compile time, and
`tools/build.sh`'s dependency tracking doesn't catch a `sysconfig.h` change
for that base build, so bumping this again would need a manual
`rm -f lib/*.o lib/*.d lib/*.a` + rebuild.

**2. `armstub=rad-prefetch.bin` must be uncommented in `config.txt`.**
[`Source/Firmware/ARM STUB/rad-prefetch.S`](<../Source/Firmware/ARM STUB/rad-prefetch.S>)
is a custom armstub (runs at EL3, before Circle's kernel even loads) that
disables L1 data prefetching, tunes L2 cache read/write latency, and
disables cache-coherency broadcast (`SMPEN`) -- all things that otherwise
introduce unpredictable timing jitter into RAD's cycle-counted bus-hijack
loop (`RESTART_CYCLE_COUNTER`/`WAIT_UP_TO_CYCLE`, raw `PMCCNTR_EL0` reads).
With the stock RPi armstub (i.e. this line commented out, which is how this
particular SD card's `config.txt` was found), the picture and RAD menu are
unstable/garbled even though the hardware itself is fine -- confirmed by
reproducing perfectly stable behavior with the *stock, non-gpu64* firmware
once this line was commented out, and stable behavior returning the moment
it was uncommented again. This looked exactly like a hardware bus-timing
problem (bad solder joint, undervoltage, bus contention) and cost real time
to rule out as one -- check this line first next time.

### Deploying to the right kernel

The Pi's bootloader loads whatever the SD card's `config.txt` names in its
`kernel=` line. RAD's cards set `kernel=kernel_rad.img`, **not** the
`kernel8.img` that Circle's build produces. `tools/build.sh` originally copied
to `kernel8.img`, so every deploy wrote a file the Pi never read and the
hardware silently kept booting the previous firmware.

This cost three consecutive hardware test rounds during milestone 3: a bug fix
and two sets of newly added diagnostics all "produced no output", because none
of them were ever on the machine. Test PRGs on the card *did* update normally
(RAD reads those at launch time), which is what made it look like a firmware
bug rather than a deployment one. The decisive tell was a log containing two
lines that bracket a third, newly added line that never appeared -- no single
build can do that, so the running image had to be older than the source.

Two guards exist now:

- `tools/build.sh` parses `config.txt`'s `kernel=` line, deploys under that
  name, and `cmp`-verifies the copy afterwards.
- Every boot logs `Run: bc0 build <git-describe> src:<digest>` as its first
  line, and `build.sh` prints the same id after deploying. If the two do not
  match, nothing else in the log says anything about the current source.

The build id is a git description plus a digest of the firmware sources,
deliberately not a wall-clock timestamp -- a timestamp would differ on every
invocation and force a rebuild and re-flash even when nothing changed.

### Reading the on-screen log

`CHDMIConsole` (tee_device.h) is a **ring**, not a scrolling console: when it
reaches the bottom of the column it continues from the top, overwriting the
oldest lines. A blank one-row gap is cleared ahead of the write head so the
wrap point is visible -- without it, a full column reads exactly like a frozen
log, which is how it was first reported from hardware.

Keep logging out of hot paths. Each line does glyph rendering via `SetPixel`
plus a cache clean, and anything logged from inside `reuUsingPolling()` runs
with the C64 free-running and no bus access being serviced. The mirror logs
once, on its first snapshot, for exactly this reason.

### Configuring what RAD starts in

`RAD/rad.cfg`'s `STARTUP` line decides what happens at power-on:
`MENU` opens RAD's file browser; `REU128K`/`REU1M`/... boot straight into REU
emulation at that size, with the C64 arriving at READY and gpu64's mirror
already polling, no menu navigation needed. The menu is then reachable only
via the RAD button. There is no startup option that also loads a `.reu` image
file -- the startup path initialises a blank REU.

Note the menu's `T` key cycles a separate `meType` state (REU / GeoRAM /
None) that is independent of the REU size setting and of whether an image is
mounted. Only REU reaches `reuUsingPolling()`, so neither gpu64's trigger nor
its mirror can fire in the other two.

### Open item

**RAD's Mahoney-technique digi music (SID `$D418` writes once per raster
line, see `rad_hijack.cpp`) is silent on real hardware, even though
`music.wav` loads and converts correctly** -- confirmed via the on-screen
log: `readFile()` returns the full 3219278-byte file, the raw bytes start
with the `RIFF` header as expected, and the post-`convertWAV2RAW_inplace()`
samples are real PCM data centered near 0x80, not blank/silent. The
equalizer animates and the C64's audio-out is confirmed connected and
otherwise working (original, non-gpu64 firmware plays music fine on this
same hardware). So the write path has good data -- the open question is
whether the SID register writes are reaching the chip correctly, or
whether `SIDType`/`supportDAC` auto-detection (`detectSID()`,
`rad_hijack.cpp` ~line 2585) is picking the wrong playback branch.

**Update:** `detectSID()` now logs the detected `SIDType`/`hasSIDKick`/
`supportDAC` values on-screen the first time it runs -- not yet run on
hardware to see the actual numbers, that's still the next step. But reading
the surrounding code while adding that log turned up a likely root cause:
`SIDType` (a file-static `u8` in `rad_hijack.cpp`, zero-initialized) is
**never assigned anywhere** in this vendored firmware -- it's only ever
compared against, never set. That means `mahoneyLUT` (assigned from
`SIDType == (6581 & 255) ? lookup6581 : lookup8580` right after the
detection block) always resolves to `lookup8580`, regardless of which chip
is actually on the board. Mahoney's technique needs the chip-correct
nonlinearity table, so if this hardware has a 6581, silently getting the
8580 table would plausibly produce exactly this symptom (SID writes with
good data, audio-out confirmed working, still silent/wrong) -- on top of,
or instead of, the already-understood `SIDType == 0 && !supportDAC`
fallback path being taken whenever no SIDKick is present (that part is
working as designed). Next step once on hardware again: check the new log
line's `SIDType` value (expected: always 0, confirming this), then check
whether RAD's actual upstream/original (non-gpu64) firmware has real 6581
vs. 8580 detection logic that just isn't present in this vendored copy --
if so, port it in; if this vendored copy never had it, that's a
pre-existing upstream gap unrelated to gpu64's changes.

[project_description.md](project_description.md#io-address-space-allocation)
already flags that the Ultimate's (and any real REU's) IO2 decode needs
confirming on hardware -- that's the first thing to check once milestone 2's
trigger PRG exists, using whichever of the two tiers above matches what's
plugged in at the time.

### The rail sagged, and the ladder completed once (2026-09-06, runs F and G)

First runs of the build that added the `HEALTH` row to `gpu64_probe_latch`
(`GET_HEALTH` after setup and after every rung) and bounded the two mailbox
waits in `gpu64_flip.cpp`.

Run F, in full:

| rung | ATT | ACC | MISS | DUR |
|---|---|---|---|---|
| `NOP`  | 4E20 | 4E1F | 0000 | 01E6 |
| `FLI0` | 07D0 | 07D0 | 0000 | 0033 |
| `FLIW` | 012C | 012C | 0000 | 012B |
| `FLIP` | 1770 | 009E | 0001 | 009D |
| `REND` | 0BB8 | 09D4 | 0B7A | 004C |

`FLIP` completed its whole 6000-iteration budget and `REND` ran -- both for
the first time in the campaign. No `TRAP`, `ALIVE STAGE 30`, C64 alive at the
end. The `FAIL` is one NOP in 20000 answering `$FF` (`FIRST ERR FF @ 00 2020`),
which is the documented drop floor against a rung whose rule is zero
tolerance.

Run G, a separate boot minutes later, derailed the familiar way: `FLIP` dead at
iteration 160, `TRAP 01 5D57` (BRK far outside the probe's `$0801-$0FF9`). Its
`SETUP 00 00 00 00 00 FF` is not a failure -- `setupErr` is `.fill 6, $ff` and
slot 5 is `LOOP_START`, which the trap kept it from ever reaching.

**Run F's HEALTH row: `00 00 05 77 01 00 00 00`.** `ss = 05` is
`GET_THROTTLED >> 16` = bit 0 (under-voltage **has occurred**) + bit 2
(throttling has occurred), at 37.5 C -- so not thermal. The 5V rail sagged
during that session. This bit has been unreadable for the whole campaign
because `readHealth()` has exactly one call site, the `GET_HEALTH` opcode, in
spite of its comment describing a once-a-second poll.

**`dd = pp = cc = 00` in both runs.** Neither bounded mailbox wait ever
expired. The unbounded `MAILBOX1_STATUS & FULL` spin and the 50 ms drain
timeout were real defects and the bounds stay in, but they are not the
mechanism.

Two boots of identical firmware, minutes apart, one completing 32000 dispatches
and one dying at 160, with a brownout on the record for one of them: that
variance is not one deterministic software bug. **Fix the supply and re-run
before spending more on firmware theory.** Two consecutive clean `FLIP`
completions with `ss = 00` would settle it.

#### Open, and new: REND loses 98% of its SEQs

`REND MISS = 0B7A` -- 2938 SEQ/SEQACK mismatches in 3000 dispatches, in 1.27 s,
against 1-in-6000 for `FLIP` and 0-in-20000 for `NOP`. But 84% of those same
dispatches answered `OK`, so they executed.

Ruled out already: `sSeqAck = sSeq` is published unconditionally at the top of
`gpu64_apiDispatch()` before any early return (`gpu64_api.cpp:1047`) and is
class-independent, so this is not class 1 skipping the publish; and `$22`/`$23`
are inside the decode window (`addr >= GPU64_REG_CMD_HI`, `rad_reu.cpp:874`),
so it is not a boundary miss.

That leaves either a detector that misreads class 1, or class-1
`SCENE_COMMIT` traffic hitting the bus defect some 400000x harder than class 0.
Unexplained either way, and worth a rung of its own.

### The async hold was the killer, and FLIS is what proved it (2026-09-06, runs H-N and the fix)

Runs H-N ran the `gpu64_probe_latch` ladder on a corrected supply. `ss = 00`
throughout, 41-50 C, no throttling, both bounded mailbox waits still at zero.
**Power is exonerated.** The derail continued regardless.

| run | reached | outcome |
|---|---|---|
| F | `FLIP` 1770 (full budget) | completed, `ss = 05` |
| G-K | `FLIP` 00A0 .. 0B60 | derailed |
| L | `FLIN` 0280, MISS 0000, `TRAP 01 0003` | derailed before `FLIP` |
| M | `FLIS` ATT 1340, ACC 133F, MISS 0000, **DUR 0072** | froze, `STAGE 60` |
| N | `FLIS` ATT 0AC0, ACC 0ABF, MISS 0000, **DUR 0048** | froze, `STAGE 60` |

Two instrument fixes made those last two rows readable. `DUR` is now
recomputed in `showPhaseRow` instead of being read back from what
`finishPhase` left behind -- run L had aborted mid-rung, never reached
`finishPhase`, and printed the *previous* rung's `DUR` as if it were a
measurement. And the new `FLIS` rung isolates the one variable nothing else
had.

#### The discriminator

Both rungs arm one deferred `PAGE_FLIP` per frame and then do work inside the
armed window. They differ in one thing:

- **`FLIW`** spins on `lda $DF0D / and #$01` inside the window -- **reads
  only**. It survived 300 armed windows across four runs, every time.
- **`FLIS`** writes `$DF22`/`$DF0B`/`$DF11` inside the window and never
  `CMD_LO`, so it dispatches nothing and takes no hold -- **writes**. It died
  after 114 and 72 armed windows (`DUR` in jiffies is the window count).

Same dispatch, same deferred flip, same hold, same everything else. The
arming dispatch is therefore **not** the killer and `PAGE_FLIP` is acquitted.
Writes inside the armed window are the poison.

#### The mechanism

`gpu64_vsyncCommitFlip()` and `gpu64_mirrorSnapshot()` were the only two DMA
holds in the system opened from the *top* of a loop pass, before the loop had
sampled anything about the current cycle. Every other hold -- the CMD_LO
dispatch, RAD's own `$DF01` transfer -- is opened only *after* the loop
sampled a C64 access.

That difference is the bug. Asserting `DMA_OUT` halts the 6510 through RDY,
which stops it only on **read** cycles; a write cycle completes regardless,
and AEC has already tri-stated the address bus. A write in flight when the
hold opens therefore lands at a floating address. That is the wiped stack and
the `BRK` at `$0001` in run L, and it explains `ACC = ATT - 1` in runs M and N
as the same corruption caught on a `jsr tally` return-address push, in a
place survivable enough to keep counting.

A read-cycle gate on the sampled `g2` does **not** fix this. The C64's bus is
multiplexed: the 6510 only drives R/W during the PHI2-high half, by which time
it is too late to halt that cycle, so the assert always lands on cycle N+1 and
`sta abs` is three reads followed by a write. Cycle N tells you nothing about
cycle N+1.

#### The fix

Use the program-structure guarantee the dispatch hold has relied on since
milestone 4 instead. **An IO2 access is always the last cycle of its
instruction** -- `lda abs`, `sta abs`, `sta (zp),y` all spend their final
cycle on the operand access -- so the cycle after any sampled `$DFxx` access
is an opcode fetch, and if an interrupt is pending the sequence's two dummy
reads come before its three pushes. A read either way.

So the frame boundary now only *arms* the commit (`gpu64Vsync.commitDue`) and
a new gate just before `noREUAccess:` in `reuUsingPolling()` fires it once the
loop has serviced an IO2 access.

**Result: `VERDICT PASS`, twice, on separate boots** -- the whole ladder,
`NOP` through `REND`, first clean completion in the campaign. `FLIS` went from
dying in ~100 armed windows to running its full budget.

#### Residual, documented rather than fixed

- ~~**A read-modify-write on a gpu64 register** (`inc $DF0D`) reads in its
  fourth cycle and writes in its fifth and sixth, breaking the guarantee. The
  CMD_LO dispatch hold has carried the same exposure since milestone 4.~~
  Fixed 2026-09-07. The audit that found it also found a second shape with
  the same defect and a wider blast radius: `sta $DFFF,X` performs an
  *unconditional* dummy read at the un-fixed address -- in IO2 -- and writes
  on the next cycle, so a hold armed on the read opens into the write. Both
  are closed by arming on the access and confirming on the following cycle
  before opening; see rule 7 in [CLAUDE.md](../CLAUDE.md) and
  `Source/Firmware/gpu64_holdgate.h`. The instrument is
  `tests/gpu64_probe_rmw.a`, which drives four shapes into an armed
  commit window and puts the two dispatch shapes on CMD_LO itself.
- ~~**`gpu64_mirrorSnapshot()` is still an async hold** with exactly this
  defect, firing 4x/s.~~ Fixed 2026-09-06. The IO2 gate could not be applied
  to it -- the mirror runs only when `!gpu64ApiActive`, i.e. at a BASIC prompt
  that touches IO2 never -- so it got its own gate of the same kind: the
  *read* of $FFFF that fetches the IRQ/BRK vector high byte, whose next cycle
  is the handler's opcode fetch. That is also the jiffy clock the mirror wants.
  See *Mirror lifecycle and the $FFFF gate* in
  [progress_tracker.md](progress_tracker.md).
- **A deferred `PAGE_FLIP` now lands on the program's next `$DFxx` access**
  rather than exactly at the boundary. Every handshake-mode program polls
  STATUS while BUSY is set, so in practice that is the next instruction;
  `gpu64Vsync.commitLateMax` records the worst case seen.

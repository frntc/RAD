# gpu64 — working agreement for implementation agents

gpu64 is a Raspberry Pi 3A+ bare-metal cartridge (RAD + Circle 44.3) that gives
a Commodore 64 a second, independent HDMI screen and a command API to draw on
it. `reuUsingPolling()` in `Source/Firmware/rad_reu.cpp` is the cycle-critical
loop that samples the C64 bus on core 0; almost every hard-won rule below
exists because something in or near that loop was violated.

Read [dev/progress_tracker.md](dev/progress_tracker.md) for status and
[docs/README.md](docs/README.md) for the API surface.

## The bus is not reliable — design for it

**This is the constraint most likely to bite work in progress.** A register
write from the C64 is occasionally not sampled by the polling loop: roughly
**1 in 35000 REU transfers** and **1 in 180000 gpu64 command writes**. There is
no retry and no error — the write simply never happened. It is pre-existing, it
reproduces with all 3D code compiled out, and it is not going to be fixed
before the API grows.

Consequences that are binding on new work:

1. **Never assume a register write landed.** Any multi-write protocol needs to
   be able to detect that it didn't.
2. **Commands must carry a sequence number** with a gap detector on the
   firmware side, so a dropped argument write becomes a reported error instead
   of a command silently executing on a half-written argument block.
3. **Any bulk upload that goes through REU DMA needs a checksum or a length
   readback.** That path carries both known defects.
4. **When a bench run shows one otherwise-inexplicable glitch, suspect this
   floor first** — not your code.

There is a second, much larger defect: under extra *REU* register write
pressure the emulation latches into 100% transfer failure. It lives in the
`$df00-$df0a` decode path, which resets `reu.pl`/`reu.pl2` and calls
`reuPrefetch()` on **every** write. The gpu64 register window `$df50-$df68`
does neither, which is why command traffic has never reproduced it. **Do not
add per-write work to either decode path.**

Full detail and the continuation handle (`io2-sampling`) are in the memory
note `gpu64-io2-sampling-reliability`.

## Rules for anything reachable from the polling loop

Every milestone-4 hardware bug was one of these, and none was visible in a
build or in code reading.

1. **The C64 free-runs through the loop.** Anything the loop is too busy to
   sample is lost outright. Long work must hold the bus (`CLR_GPIO(bDMA_OUT)`),
   which is why a gpu64 command halts the C64.
2. **Release DMA on a cycle boundary**, never wherever the work finished. Get
   this wrong and damage accumulates — the program survives its first commands
   and derails later.
3. **`WAIT_FOR_VIC_HALFCYCLE` alone is not a sync.** Precede it with
   `WAIT_FOR_CPU_HALFCYCLE`. It falls straight through when the C64 is
   *already* in the VIC half, and then the whole entry sequence degenerates:
   `RESTART_CYCLE_COUNTER` anchors mid-half-cycle, `TIMING_TRIGGER_DMA` has
   already elapsed, and the bus is taken at an undefined phase. So before
   opening a hold, check which half-cycle the **call site** is in — the
   polling loop calls into its helpers from inside the VIC half. This has now
   been the bug twice: milestone 4 in the blob helpers, 2026-08-27 in
   `gpu64_vsyncCommitFlip()`, which cost days of chasing a phantom power
   fault.
4. **"Preloaded at start-up" is not durable.** A single `CLEAR` evicts it.
   Cycle-critical code reachable from a command must warm its own i-cache at
   the point of use. Cold-fails / warm-passes is the signature.
5. **Exception to 4: warm from upstream when the point of use is unprotected.**
   A 2KB preload running in the loop *before* the bus is held is the delay it
   was meant to prevent. `gpu64_apiWarmPollingLoop()` and `gpu64_vsyncWarmCommit`
   are the two worked examples.
6. **Work added ahead of the bus sampling is exposed**, and how much depends on
   which mode it runs in. Judge loop additions by that, not by their cost.
7. **A DMA hold must be armed on one cycle and opened on the next, once
   that next cycle has confirmed itself.** Asserting `bDMA_OUT` halts the
   6510 through RDY, which stops it only on **read** cycles; a write
   completes regardless, with AEC already tri-stating the address bus, so it
   lands at a floating address and corrupts the C64. This killed the C64 for
   the whole Stage 16 campaign.

   The original rule was "open immediately after a sampled IO2 access",
   resting on an IO2 access being the *last* cycle of its instruction. That
   holds for absolute loads and stores — all the API itself emits — and
   fails for `inc $DF11` (reads, then writes twice) and for `sta $DFFF,X`
   (an unconditional dummy read in IO2, then the write). Both were live
   exposures, recorded as an API constraint rather than fixed, until
   2026-09-07.

   The rule that replaced it: let N be the cycle the previous pass sampled
   and N+1 the cycle this pass just sampled. A hold may open during N+1 iff
   **(1)** N was a sampled IO2 access or the $FFFF vector read, **(2)** the
   previous pass really was the previous C64 cycle — `PMCCNTR_EL0` stamps,
   compared against `gpu64HoldGap.armPerC64`, because a pass that held the
   bus is not adjacent to anything, **(3)** N+1 is a read, and **(4)** N+1's
   low address byte differs from N's. Then N+1 is an opcode fetch and N+2 is
   provably a read, because no 6502 instruction and no interrupt sequence
   writes before its third cycle. Term 4 is what closes the indexed case:
   indexed addressing fixes up only the *high* byte, so the dummy and real
   addresses share a low byte and term 3 alone would be satisfied by the
   real read. The arming cycle is deliberately not one of the terms — an arm
   is only a request for the bus, and simply waits for the first qualifying
   pair. See `Source/Firmware/gpu64_holdgate.h`.

   The screen mirror never sees an IO2 access — it runs only when
   `!gpu64ApiActive`, at a BASIC prompt — so it arms on the other decode the
   loop can see: a **read of $FFFF**, the IRQ/BRK vector high-byte fetch.
   You cannot substitute a read/write test on the sampled `g2` for term 1:
   the C64's multiplexed bus only shows R/W in the PHI2-high half, too late
   to halt that cycle. The one remaining hold that still fires on the old
   unconfirmed rule is `gpu64_mirrorIdleLoop()`'s, which runs before
   `armPerC64` is calibrated and with only KERNAL/BASIC alive.

Also: **RAD's low-level macros do not parenthesise their arguments.** Never
pass an expression containing `?:`, `+` or `%`.

## Multicore

The milestone 6a load ladder settled this in eighteen rounds; do not re-derive
it.

- **Core 1 may write at most 7 consecutive cache lines (448 bytes) before
  yielding.** Design to **256 bytes (4 lines)** for margin — one 320-byte
  scanline per yield is inside the limit.
- Burst length is the *only* axis. Write bandwidth (free to 64 MB/s),
  working-set size, L2 capacity and duty cycle are all non-constraints.
- **Never poll an MMIO register from another core** — any MMIO spin breaks core
  0's bus timing. Use `CNTVCT_EL0`, not the BCM system timer.
- **Inside `reuUsingPolling()` and anything it reaches with the bus still
  free-running, core 0 must never read a cache line another core writes.**
  Separate cache lines are not sufficient; removing the read is what produced
  a zero noise floor. This is a statement about that loop's per-C64-cycle
  deadline, not a general memory-model rule — 6a's own instrument only ever
  measured loop-pass elongation there. Code that runs with the C64 DMA-halted
  (a command dispatch, the log path) has no such deadline and already reads
  core-1-written state safely in shipped, hardware-verified form
  (`logGpu64_3dStats()` in `gpu64_api.cpp`, the ring's own cached-tail
  fallback). Conflating the two cost a full design round on Stage 15 — see
  `dev/gap_filling_plan.md`'s Stage 15 section.
- `STNP` and `DC ZVA` were both tried as escapes and both fail.

## Testing

- **Bench time is the user's scarce resource.** Optimise instruments for
  *events per minute*, not statistical power. A test that needs two hours needs
  redesigning — the accelerated bench found the same rate 16x faster.
- **Verify on the host or in VICE before asking for a hardware run.**
  `tools/hostsim` compiles the renderer natively and renders to PPM;
  `tools/hostsim/out/prgpreview.ppm` is the expected HDMI output.
- **VICE runs must be several hundred frames.** A 96-frame desk check missed a
  defect whose onset was frame 98.
- **Poison destination buffers so a miss has a fingerprint**, and log per
  event — a sum and a maximum cannot recover a distribution.
- **A clean timing metric is necessary, not sufficient.** The ladder's `L=`
  counter read `0/0` on rungs that were killing the C64. The C64 staying alive
  is the only ground truth.
- Deploying: `SDCARD=<mountpoint> tools/build.sh`. The card boots
  **`kernel_rad.img`** per `config.txt`, not `kernel8.img` — check the boot
  log's build id, because stale-firmware deploys have silently wasted whole
  test rounds.
- In the RAD menu, press **T until it reads REU**. Without it
  `reuUsingPolling()` never runs and none of gpu64 executes at all.
- 64tass: **never use `--nostart`** — it strips the `$0801` header silently and
  the program simply never runs.

## Docs

`docs/` is for the API's users; `dev/` is for whoever works on gpu64.

- `docs/README.md` and the files it indexes — only what a developer needs
  to *use* the API, split one file per class/topic.
- `dev/milestone*_design.md` — rationale and as-built decisions.
- `dev/progress_tracker.md` — status and campaign history.

Keep them separated that way.

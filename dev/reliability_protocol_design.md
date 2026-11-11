# Command-reliability protocol — fixing plan

Status: **revised after Opus 5 peer review, 2026-08-30; detector-only build
implemented the same day, not yet run on hardware.**
The original two-layer draft (DISPCNT differential + macro-folded checksum)
had several defects a reviewer found blocking — see "What the review found"
below. This revision replaces the mechanism; the motivation, register
budget, and scope sections below are updated to match. The full retry
machinery ("Corrected design" below) is still not implemented — only the
"Recommended next step" detector is built. See that section for what
shipped and two deliberate deviations from its literal wording.

Written 2026-08-30 in response to an explicit user directive after a
hardware run of `gpu64_loop_test.prg` showed run-to-run-inconsistent results
(sometimes a clean `SCENE 07 07`, sometimes a premature READY, sometimes the
HDMI frozen on a static frame while the C64's own `FRAME` counter kept
incrementing).

## What the review found

Full review on file; the load-bearing points:

- **The retry was not idempotent, and the commands it would retry are not
  either.** `gpu64_loop_test.a`'s `ROTATE_LOCAL` is a *relative* rotation;
  blindly retrying `CMD_LO` on a false "dropped" reading would double-rotate
  and double-flip — worse than the bug it fixes, and invisible to every
  existing assertion.
- **The `DISPCNT` differential detector assumed reads are reliable**, which
  begs the question — a garbled read can misreport in either direction with
  no way to tell.
- **The checksum domain didn't match.** `ARG` bytes are not cleared between
  commands and every opcode stages only a subset, so the C64's accumulator
  and firmware's live `gpu64Regs.arg[]` disagree by construction for any
  command that doesn't stage all 16 bytes. As specified, `CHK` could
  essentially never validate.
- **"Zero changes to ~15 existing test/demo programs" was false.** A grep
  found hundreds of raw `sta ARG+n` sites outside the four macros (loops,
  variable-sourced data) that the macro-folding approach silently misses —
  each one would checksum-fail every time under strict mode.
- **The firmware placement was wrong.** "At the top of the `CMD_LO` branch,
  bus already held" doesn't hold — the actual bus-held region starts several
  lines later in `rad_reu.cpp`; the branch's own top-of-function comment says
  that earlier region has no cycle budget at all. New logic belongs in
  `gpu64_apiDispatch()` (`gpu64_api.cpp`), which is unconditionally bus-held
  with no polling-loop exposure, not in `rad_reu.cpp`.
- The causal chain from "one dropped write" to "HDMI frozen while the C64
  keeps counting" was asserted, not shown — a single dropped `SCENE_COMMIT`
  should cost one missed frame, not a persistent hang. The reviewer's
  read: something downstream latches on a missed commit, and a naive retry
  would mask that latch rather than fix it.

## Corrected design

Replaces the DISPCNT-differential + macro-folded-checksum mechanism with a
single firmware-acknowledged sequence number plus a RAM-shadowed checksum:

- **`SEQACK`** (new read-only register): the `SEQ` of the last command
  firmware actually *executed* (not a free-running counter). The C64 writes
  a chosen `SEQ` (cycled `$01..$FE` — never `$00` or `$FF`, so an open-bus
  or unserviced readback can never be mistaken for a real ack) and compares
  `SEQACK` against it, not against another read of itself. One readback,
  one control decision — no more differential counter, no more separate
  `ERRCODE == SEQ_GAP` branch.
- **Exactly-once dispatch, firmware side.** Firmware tracks
  `lastExecutedSeq`. If an incoming `SEQ` equals it, the command is *not*
  re-executed — firmware just re-publishes the cached `err`/`result`/
  `status` from the original execution. This makes every retry safe by
  construction, regardless of what actually happened on the wire, which is
  what makes retrying a non-idempotent opcode like `ROTATE_LOCAL` safe.
- **A 20-byte C64-side RAM shadow of the register image**, not an
  incrementally-folded XOR accumulator. Staging macros write the shadow;
  one `cmdsend` routine blits shadow → registers, computes the checksum
  over the *whole* shadow in the same pass, and owns the retry loop.
  Firmware checksums its own live 20-byte image the same way. This is what
  makes the checksum domains actually agree, and what makes "restage and
  retry" implementable in shared code instead of only in the caller.
- **Strict mode is a firmware mode latch, not a per-command `CMD_HI` bit.**
  Set by an explicit command, confirmed by a readable `STATUS` bit — so a
  dropped *enable* is detectable by readback instead of silently degrading
  to unchecked dispatch (the `CMD_HI`-bit design's edge case the original
  draft could only wave at with a coding convention).
- **Give-up returns a reserved sentinel** in `$80-$FF` (firmware `ERRCODE`s
  are `$00-$0D` and will never reach that range) — never a bare stale
  `ERRCODE` a caller could mistake for success.
- **Retry limit: 3**, not 8 — beyond 2 attempts the drop is systematic, not
  statistical, and a large limit just turns a real fault into a slow hang.
- **Register placement: sequence after the UCI remap, not before it.** The
  remap ([[gpu64-uci-register-overlap]]) is a pure address shift with zero
  logic risk; landing it first means `SEQ`/`CHK`/`SEQACK` go straight into
  the new `$DF37+` growth space instead of into space that has to move a
  second time. Also: the *current* unmapped layout already overlaps UCI at
  `$DF1B-$DF1F` (= today's `ARG10..ARG14`) — enabling strict mode on a UCI
  Ultimate before the remap would fail confusingly for an unrelated reason.
- **Opt-in is per-program, not "the whole suite for free."** Given the
  hundreds of raw-store sites the review found, start strict mode on the
  programs where an undetected drop actually compounds — the per-frame
  animation loops (`gpu64_loop_test.a`, the demos) — and leave the one-shot
  conformance tests alone, where a drop already surfaces as a single
  re-runnable failure a human is reading directly.

## Recommended next step (before building retry machinery at all)

Ship the detector alone first: `SEQACK` plus an on-screen "missed dispatch"
counter in `gpu64_loop_test.a`, no retry logic yet. Then deliberately inject
a skipped `SCENE_COMMIT` in hostsim/VICE and confirm it actually reproduces
the persistent-freeze symptom, not just a one-frame skip. This is a single
cheap bench-equivalent instrument (matches CLAUDE.md's "optimise for events
per minute" testing rule) that either confirms the whole causal chain this
plan rests on, or redirects the effort at whatever the real latch is —
before spending implementation time on retry machinery for the wrong bug.

**Built 2026-08-30, with two deviations from the paragraph above, both
deliberate and both worth flagging rather than silently deciding:**

1. **Register addresses.** `SEQ`=`$DF22`, `SEQACK`=`$DF23` — the
   currently-reserved tail right after `RESULT` (`$DF21`), in the
   *un-remapped* layout, not the post-UCI-remap `$DF37+` space the
   "Corrected design" section reserves for the shipped protocol. This is
   acceptable only because this pair is throwaway diagnostic
   instrumentation: it will be deleted or renumbered once the UCI remap
   lands and the real protocol is built, not carried forward as committed
   API surface. Implementation: `gpu64_api.h`/`gpu64_api.cpp`, register
   file gains `seq`/`seqAck`, `gpu64_apiDispatch()` sets
   `gpu64Regs.seqAck = gpu64Regs.seq` unconditionally as its first line
   (verified against `rad_reu.cpp`'s decode path by inspection — both the
   write and read branches gate only on `addr >= GPU64_REG_CMD_HI`, so
   `$DF22`/`$DF23` reach the accessor functions exactly like every other
   gpu64 register, with no separate bounds check to trip on).
2. **Verification path: direct-to-hardware, not hostsim/VICE fault
   injection.** The literal instruction above isn't executable as written:
   `tools/hostsim` doesn't link `gpu64_3d_class1.cpp`/`gpu64_3d_core1.cpp`
   (Stage 16's class-1 render-loop state machine has no PC-side simulator
   coverage at all — see [[gpu64-milestone16-commit-protocol]]), and VICE
   emulates the C64, not the RPi cartridge firmware or bus-sampling code —
   there is no fault to inject into on either. The practical substitute:
   `gpu64_loop_test.a`'s `animLoop` now writes an incrementing `SEQ` and
   checks `SEQACK` around both `ROTATE_LOCAL` and `SCENE_COMMIT` (the two
   per-frame opcodes), counts mismatches in a new `missed` word, and shows
   it on HDMI as a new `MISSED` row. This turns the next hardware bench run
   itself into the fault-detection instrument — a nonzero `MISSED` count
   correlated against the freeze/inconsistency symptom is the empirical
   answer to the causal question, in place of a synthetic injection this
   codebase has no harness for.

Not yet run on hardware. What a run would tell us:

- `MISSED` stays `0000` through a run that still shows the freeze/HDMI-stall
  symptom → the causal hypothesis is wrong, or the drop is happening
  somewhere this detector doesn't cover (e.g. inside a blob transfer, not a
  `SEQ`/`CMD_LO`-adjacent write) — go looking elsewhere before building
  retry machinery.
- `MISSED` increments and the freeze/inconsistency reproduces in the same
  run → confirms the bus-drop defect is the mechanism, and specifically
  that it can hit `SCENE_COMMIT`/`ROTATE_LOCAL`, which is exactly what the
  full retry design (exactly-once dispatch, keyed on `SEQ`) is built to
  survive. Proceed to the "Scope / sequencing" section below.

## Why now, and why this isn't the vsync bug

The vsync-calibration retry fix (`gpu64_vsync.cpp`, see
[[gpu64-milestone16-commit-protocol]]) was verified working — HDMI now
renders. The NEW inconsistency reported afterward is a different, older, and
already-documented defect: CLAUDE.md's "the bus is not reliable" section and
memory [[gpu64-io2-sampling-reliability]] — a C64 register write is
occasionally never sampled by `reuUsingPolling()` (~1/180000 for a gpu64
command write, no retry, no error). `gpu64_loop_test.a` fires `ROTATE_LOCAL`
+ `SCENE_COMMIT` unthrottled every pass — roughly 80,000 raw register writes
for a `FRAME` count of `9AC0`, an order of magnitude more command traffic
than any prior milestone test generated, which is why this is the first test
to expose it at visible scale. A dropped `CMD_LO` write means the command
silently never dispatches; stale `ERRCODE`/`RESULT`/`STATUS` from the
previous command can look like success while nothing actually ran — exactly
the "HDMI hung, C64 still counting" symptom.

The user's directive: *"Let's not defer it anymore. The further feature
increments has to be stable else we just roll new problems."* This plan
implements CLAUDE.md's rule 2 ("commands must carry a sequence number with a
gap detector") for real, instead of continuing to treat it as a documented,
deferred limitation.

## What CLAUDE.md already requires

1. Never assume a register write landed — detect that it didn't.
2. A sequence number with a gap detector, so a dropped argument write becomes
   a reported error instead of a command executing on a half-written block.
3. (Bulk blob DMA needs its own checksum/length readback — separate,
   already-tracked concern, not addressed by this plan.)

Two distinct failure modes still need covering, but (per the review) with
one mechanism instead of two independent detectors:

- **The `CMD_LO` write itself never lands.** Nothing runs at all; `ERRCODE`
  is whatever the previous command left there. Nothing about the argument
  block is wrong — the trigger just didn't fire.
- **`CMD_LO` lands, but an earlier write in the same block (an `ARG`, `ID`,
  or `CMD_HI` byte) didn't.** The command DOES dispatch, on a block that is
  part new, part stale.

`SEQACK` + exactly-once dispatch (below) covers the first. The RAM-shadow
checksum covers the second. Both are described in full under "Corrected
design" above; this section is kept only for the CLAUDE.md cross-reference.

## Register addresses (revised)

The block currently lives at `$DF0B-$DF21` with `$DF22-$DFFF` reserved.
Per the review, sequence the pending, already-decided UCI remap
([[gpu64-uci-register-overlap]], `dev/uci_register_remap_design.md`,
shifting the whole block to `$DF20-$DF36`) **first** — it's a pure address
shift with no logic risk, and the current unmapped layout already overlaps
UCI at `$DF1B-$DF1F`, so enabling strict mode before the remap would fail
confusingly on Ultimate hardware for an unrelated reason. Land the remap,
then place the new registers in the new `$DF37+` growth space:

    GPU64_REG_SEQ      = $DF37   (write-only, C64-chosen, cycles $01..$FE)
    GPU64_REG_CHK      = $DF38   (write-only, checksum over the 20-byte shadow)
    GPU64_REG_SEQACK   = $DF39   (read-only, SEQ of the last command actually
                                   executed — replaces the old DISPCNT design)

`GPU64_ERR_SEQ_GAP` is no longer needed as a distinct `ERRCODE` — a checksum
mismatch is reported by `SEQACK` simply never reaching the value the C64
wrote, which the retry loop already handles; give-up uses the `$80-$FF`
sentinel range described above instead of a dedicated error code.

## C64-side retry/give-up policy (revised)

    stage into the 20-byte RAM shadow (ARG/ID/CMD_HI as before)
    seq = next value in $01..$FE, cycling
    shadow.seq = seq
    chk = checksum(shadow)               ; whole 20 bytes, one pass
    attempt = 0
    retry:
      blit shadow -> registers (ARG/ID/CMD_HI/SEQ/CHK), then write CMD_LO
      read SEQACK
      if SEQACK == seq: return ERRCODE   ; done -- landed and (re-)executed safely
      attempt += 1
      if attempt >= 3: return sentinel in $80-$FF, do not touch ERRCODE
      goto retry

Firmware side owns exactly-once: if the incoming `SEQ` equals
`lastExecutedSeq`, don't re-run the opcode — just re-publish the cached
`err`/`result`/`status` and set `SEQACK`. This is what makes blind retry
safe even for non-idempotent opcodes like `ROTATE_LOCAL`. `LIMIT = 3`
(review's recommendation: beyond 2 attempts the drop is systematic, not
statistical, and a larger limit just turns a real fault into a slow hang).

## Scope / sequencing of this change (revised)

0. **Land the UCI register remap first** (separate, already-decided,
   zero-logic-risk change) — see "Register addresses" above.
1. **Validate the causal hypothesis before building retry machinery** — see
   "Recommended next step" above. A `SEQACK`-only instrument plus a
   deliberate fault injection in hostsim/VICE, confirming a dropped
   `SCENE_COMMIT` actually produces the persistent-freeze symptom and not
   just a one-frame skip.
2. Firmware: `gpu64_api.h` (new register defines, `GPU64REGS` struct gains
   `seq`/`chk`/`seqAck`/`lastExecutedSeq`/cached `err`/`result`/`status`
   fields, extend `gpu64_apiWriteReg`/`gpu64_apiReadReg` for the new
   addresses — keep the new comparison early in the read-path chain, per
   the review's note on `WAIT_CYCLE_READ2`'s deadline).
3. Firmware: **all new dispatch logic goes in `gpu64_apiDispatch()`**
   (`gpu64_api.cpp`), not in `rad_reu.cpp` — it is unconditionally bus-held
   with no polling-loop exposure, unlike the region the original draft
   pointed at. `reuUsingPolling()` itself gains zero instructions.
4. C64 side: one shared shadow-staging + `cmdsend` implementation, landed in
   `gpu64_testkit.inc`/`gpu64_testkit_rt.inc` but **opted in per program**,
   not suite-wide (the review found hundreds of raw `sta ARG+n` sites
   outside the four macros that would checksum-fail every time under a
   suite-wide flip). Start with the per-frame animation loops
   (`gpu64_loop_test.a`, the demos), where an undetected drop compounds;
   leave the one-shot conformance tests alone for now.
5. Verify on host/VICE first per standing bench-time-scarce guidance — a
   fault-injection harness that drops a write at each byte position of a
   staged command *and* a garbled read, run for several hundred simulated
   frames, before requesting hardware bench time.
6. `docs/api_design.md` gets the new registers and the strict-mode-latch
   command documented (developer-facing surface); this design doc and
   `dev/progress_tracker.md` get the rationale/status entries
   (CLAUDE.md's doc-separation rule).

## Resolved design points (from the review)

- **Strict mode is a firmware mode latch set by an explicit command and
  confirmed by a readable `STATUS` bit** — not a per-command `CMD_HI` bit —
  so a dropped *enable* is itself detectable by readback instead of
  silently degrading to unchecked dispatch.
- **Checksum**: whole 20-byte shadow, one pass, on both sides — not an
  incrementally-folded per-write accumulator (which had no way to agree
  with firmware's `gpu64Regs.arg[]`, since `ARG` retains stale bytes between
  commands and every opcode stages only a subset).
- **Retry limit**: 3.
- **Register sequencing**: UCI remap first.
- **Two drops in one command**: reviewed and found negligible (~2e-9 per
  command, ~1.6e-4 per run at this test's volume) — the single-drop
  assumption elsewhere in this design is sound.

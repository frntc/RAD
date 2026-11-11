# Register remap design: getting gpu64 off the Ultimate's UCI block

Coexisting with a C64 Ultimate running its Ultimate Command Interface (UCI)
with the interface enabled is a hard requirement, not a nice-to-have. This
records why gpu64's current register window collides with it, the address
map gpu64 is moving to, and the checklist for when that move is implemented.
Implementation is deferred; this doc exists so the decision doesn't get
re-derived later.

## As built (2026-09-27) — read this first

Implemented, but **not** as planned below. Three things the plan missed:

1. **$DF20 is Ultimate Audio.** The Ultimate can map its sampler at
   $DF20–$DFFF ("Map Ultimate Audio" in its cartridge settings). With the UCI
   at $DF1B–$DF1F and REU at $DF00–$DF0A only 16 bytes stay free, and gpu64
   needs 25, so gpu64 cannot coexist with *both*. Decided with the user:
   coexist with the UCI, require Ultimate Audio **off** (and the Ultimate's
   own REU off, which was always true). Documented in docs/api_design.md and
   docs/getting-started.md.
2. **The mux repair is keyed on the high nibble.** rad_reu.cpp re-reads the
   address when the sampled A4–A7 nibble cannot be real. A half-switched
   LVC257 can only *add* one-bits (it is leaving the $C–$F signal nibble), so
   the valid nibbles must not be supersets of each other. $2/$3 fail that
   ($3 ⊃ $2): a block at $DF20–$DF38 would have made some mis-samples
   undetectable and put ARG10+/RESULT/SEQ/SEQACK on the re-read path.
   **The block is at $DF50–$DF68** (nibbles $5/$6, plus REU's $0), and the
   test is a mask, `GPU64_MUX_BAD_NIBBLES` in gpu64_api.h.
3. **A constants-only change was unsafe.** The read path drove the data bus
   for every IO2 read ≥ CMD_HI and indexed `reu.status` for the rest; the
   write path sent everything below CMD_HI into REU's switch, whose tail
   resets prefetch state on every write (the REU write-pressure latch). Both
   now ignore $DF0B–$DF4F outright: no drive, no REU side effects. The added
   compare sits only on the REU/foreign branch, never on gpu64's.

SEQ/SEQACK (added after this doc) moved with the block: $DF67/$DF68.
Final map: `CMD_HI` $DF50, `CMD_LO` $DF51, `STATUS` $DF52, `ERRCODE` $DF53,
`ID` $DF54–55, `ARG0`–`ARG15` $DF56–$DF65, `RESULT` $DF66, `SEQ` $DF67,
`SEQACK` $DF68. The prgsim model's address-fault injector now sets A7
instead of A6, because A6 is already set in every new register.

Everything below is the original plan, kept for the reasoning.

## The collision

gpu64 currently owns $DF0B–$DFFF in IO2 (REU keeps $DF00–$DF0A), with actual
registers laid out at $DF0B–$DF21 as documented in
[docs/api_design.md](../docs/api_design.md):

| Register | Address |
|---|---|
| `CMD_HI` | $DF0B |
| `CMD_LO` | $DF0C |
| `STATUS` | $DF0D |
| `ERRCODE` | $DF0E |
| `ID_LO`/`ID_HI` | $DF0F–$DF10 |
| `ARG0`–`ARG15` | $DF11–$DF20 |
| `RESULT` | $DF21 |

The Ultimate's UCI, when enabled in its "Command Interface" config menu, maps
a 5-byte block at **$DF1B–$DF1F**: SoftwareIEC Bus ID ($DF1B), Control/Status
($DF1C), Command Data/Identification ($DF1D), Response Data ($DF1E), Status
Data ($DF1F) — see the
[Command Interface docs](https://1541u-documentation.readthedocs.io/en/latest/command%20interface.html)
and [Core UCI Architecture](https://1541u-documentation.readthedocs.io/en/latest/uci/core_uci_architecture.html).

That block lands exactly on gpu64's `ARG10`–`ARG14`. With UCI enabled, both
gpu64 and the Ultimate's own logic decode $DF1C–$DF1F simultaneously: any
gpu64 command that uses `ARG11`–`ARG14` also writes into the Ultimate's
Control/Status and Command Data registers, which can trigger unintended UCI
state transitions or get gpu64's staged argument bytes corrupted by whatever
the Ultimate drives back. See memory note `gpu64-uci-register-overlap` for
the byte-by-byte mapping.

## Decision: shift the whole window past the UCI block, keep it contiguous

Move the entire 23-register block from $DF0B–$DF21 to **$DF20–$DF36** — the
first free byte after UCI's $DF1F. The block stays exactly as wide and in
the same internal order; only its base address changes.

| Register | Old | New |
|---|---|---|
| `CMD_HI` | $DF0B | $DF20 |
| `CMD_LO` | $DF0C | $DF21 |
| `STATUS` | $DF0D | $DF22 |
| `ERRCODE` | $DF0E | $DF23 |
| `ID_LO`/`ID_HI` | $DF0F–$DF10 | $DF24–$DF25 |
| `ARG0`–`ARG15` | $DF11–$DF20 | $DF26–$DF35 |
| `RESULT` | $DF21 | $DF36 |

$DF0B–$DF1A (16 bytes) becomes free/reserved, $DF1B–$DF1F stays the
Ultimate's, and $DF37–$DFFF remains reserved for future gpu64 growth.

**Why a contiguous shift instead of splitting the block around the UCI
hole** (e.g. leaving `CMD_HI`..`ARG9` where they are and only relocating
`ARG10`–`ARG15`/`RESULT`): a split needs a second address-range check with a
different offset into `gpu64Regs.arg[]`, and that decode lives inside
`reuUsingPolling()`'s per-C64-cycle path — exactly the code the project's
timing rules single out as fragile (see CLAUDE.md, "Rules for anything
reachable from the polling loop"). A straight base-address shift changes only
the `GPU64_REG_*` constant values; the comparison logic in
`gpu64_api.h`/`rad_reu.cpp` (`addr >= GPU64_REG_CMD_HI`,
`gpu64Regs.arg[addr - GPU64_REG_ARG0]`, etc.) is untouched. Preserving the
old byte values had no real benefit to weigh against that — nothing outside
this repo's own tooling consumes them.

This doesn't touch the separate, still-open question in
[project_description.md](project_description.md#io-address-space-allocation)
about whether a real REU (or the Ultimate's own REU emulation) only decodes 5
address bits and would alias part of gpu64's window back onto its own
registers. That risk is a property of the $DF0B–$DFFF space in general, not
of which sub-range gpu64 picks inside it, and stays a hardware-verify item
for whenever Ultimate bench testing happens (see memory note
`gpu64-ultimate-testing-deferred`).

## Migration checklist (for whenever this is implemented)

Purely mechanical — no logic changes, only the constant values below.

- [ ] `Source/Firmware/gpu64_api.h` — the six `GPU64_REG_*` `#define`s
      (`CMD_HI` through `RESULT`)
- [ ] `tests/gpu64_testkit.inc` — `CMD_HI`..`RESULT` constants
- [ ] `Source/Demos/gpu64.inc` — `CMD_HI`..`ARG` constants
- [ ] `tools/prgsim/gpu64model.py` — `REG_CMD_HI`..`REG_RESULT` constants
- [ ] `tools/prgsim/runsim.py:190` — hardcoded `0xDF0C` flip-detect check
      (worth referencing `gpu64model.REG_CMD_LO` here instead of a literal,
      so this can't drift from the model again)
- [ ] `docs/api_design.md` — register table (§ around line 84–95), the
      `CMD_LO`/`ARG0` example addresses (~line 1006–1007), and the "gpu64
      owns $DF0B–$DFFF" line
- [ ] `docs/class1-3d-mesh-reference.md` — `ARG0` ($DF11) and `RESULT`
      ($DF21) address mentions
- [ ] Comments in `Source/Firmware/rad_reu.cpp` that cite the old range by
      address (the "$DF0B–$DFFF is gpu64's own register window" comments) —
      update to the new range, not just the constants
- [ ] `dev/project_description.md` / `dev/progress_tracker.md` —
      these are historical record and don't need rewriting, but the current
      IO-allocation section in `project_description.md` should get a
      pointer forward to this doc so a future reader doesn't take the old
      addresses as current

## Verification plan

Entirely host-side, no hardware bench time needed for this step (per
"verify on host or in VICE before asking for a hardware run"):

1. `tools/testprg.sh` (the class 0 conformance suite) against the new
   addresses.
2. `tools/hostsim` render of anything in `Source/Demos` that exercises
   `ARG10`–`ARG15` (the bytes that used to sit in the UCI hole) — these are
   the ones most likely to have been silently wrong under the old layout if
   ever run on real Ultimate hardware with UCI on, and are the ones worth
   double-checking land correctly post-remap.
3. `tools/prgsim/rastercheck` differential run, since `gpu64model.py`'s
   register constants are part of what it cross-checks against the firmware.

A hardware run to actually confirm UCI-enabled coexistence stays deferred
per `gpu64-ultimate-testing-deferred` — that's a separate, later bench trip,
not a gate on landing this remap.

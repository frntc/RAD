# Demo hardware status

What a developer needs to *use* the demos is
[docs/getting-started.md](../docs/getting-started.md). This is the bench
history: which of the ten programs in `Source/Demos/` have actually run on
real hardware, and when.

- `hello`, `palette`, `sprites`, `bounce`, `rotate`, `matrix` — verified on
  hardware 2026-08-25, first bench round, no defects found.
- `raycast` — ran in the bench round that closed tracker section 8b; the
  blink it was originally named for did not reproduce there.
- `sectors` — verified on hardware 2026-08-25, in the round that added
  `DRAW_THINGS`.
- `walls` — rendered on a PC and not separately checked at the bench.
- `quake` (`gpu64_demo_quake`) — hardware-verified as part of the
  ten-green `tools/examples.sh` round that closed the intermittent-hang
  investigation: left running unattended on hardware, it no longer hangs.
  See progress tracker section "11. Directional things, and a quake demo
  you can walk around in" and the hang investigation that follows it for
  the full history.

Full campaign detail, including every round these results came out of, is
in [progress_tracker.md](progress_tracker.md).

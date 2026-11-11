# gpu64 documentation

gpu64 turns a Raspberry Pi 3A+ cartridge into a second, independent HDMI
screen for the Commodore 64 — not a VIC-II overlay, but a separate display
(think C128's 80-column screen) that the 6502 drives through a small
command API mapped into IO2 (`$DF50`-`$DF68`). A program writes arguments
into `ARG0`-`ARG15`, then writes an opcode to `CMD_LO`; that write halts the
C64 until gpu64 finishes and writes `ERRCODE`. Everything under `docs/`
documents that API from the point of view of someone writing a C64 program
against it — a game, a demo, a CAD-style tool. Nothing here covers how
gpu64 itself is built, tested, or debugged; that material lives in
[`dev/`](../dev/) instead.

Opcodes are grouped into **classes**, selected by `CMD_HI`:

- **Class 0** — 2D drawing, palette, blit, system/info, and the matrix
  coprocessor. Always compiled in; every program uses it.
- **Class 1** — the retained 3D scene graph (meshes, nodes, render ops on
  core 1). gpu64's forward path for 3D as of the 2026-08-28 decision.
- **Class 2** — the earlier first-person raster layer (walls, sectors,
  things, per-frame polygon batches). Frozen but still working; superseded
  by class 1 for new work.

## The v1 surface

As of 2026-09-11 the API below is **frozen for v1**: opcodes, argument
layouts and error codes will not change meaning, and anything added later
takes a new opcode number rather than a new argument on an existing one.
Three things are deliberately outside that freeze, and a program should not
be written as if they were settled:

- **Free-running mode** (`LOOP_START` with `ARG0` = 1) answers `UNSUPPORTED`
  and is staged. Handshake mode is the whole of v1's loop. `LOOP_START`
  itself is optional in handshake mode — `SCENE_COMMIT` starts a stopped
  loop — so it survives v1 only as the place you will eventually ask for
  free-running.
- **Class 2** is frozen, not removed. It still works; it gets no new
  opcodes.
- **Visibility** is not part of the v1 API. A level loaded with
  `LOAD_LEVEL` culls its own world nodes by the level file's PVS, which is
  invisible to your program. Everything else is drawn as given; see
  [known-gaps.md](api/known-gaps.md) and
  [level-scale-visibility.md](api/level-scale-visibility.md).

Two v1 rules are not optional for a program that runs for more than a few
seconds, because the IO2 bus drops roughly one register write in tens of
thousands and a retained scene remembers every one of them: refresh your
node state on a rotation ([state-refresh.md](api/state-refresh.md)), and fence
every readback with `SET_DMA_WINDOW` before you use one.

## Where to start

**Guide** — [`guide/`](guide/)

1. [getting-started.md](guide/getting-started.md) — register map, what a
   command costs the C64, issuing a command, blob descriptors, building and
   running the examples. Start here.
2. [snippets.md](guide/snippets.md) — three short complete programs: draw,
   blit with vsync, matrix coprocessor.
3. [vibe-coding.md](guide/vibe-coding.md) — setting up Claude Code to write
   gpu64 programs for you.

Then the programs themselves: [`examples/`](../examples/README.md), in
reading order.

**Reference** — [`api/`](api/)

- [overview.md](api/overview.md) — the whole API in one place.
- [class0-2d-reference.md](api/class0-2d-reference.md) — system, surface,
  primitives, palette, blit, 80x50 text mode, info/health, matrix and
  vector ops.
- [class1-3d-mesh-reference.md](api/class1-3d-mesh-reference.md) — the
  retained scene graph: meshes, nodes, render ops.
- [class2-raster-reference.md](api/class2-raster-reference.md) — the frozen
  raster layer: walls, sectors, things, polygon batches.
- [vblank-and-animation.md](api/vblank-and-animation.md) — syncing to the
  frame; tear-free double buffering.
- [error-codes.md](api/error-codes.md) — the shared `ERRCODE` table.
- [state-refresh.md](api/state-refresh.md) — keeping a retained scene
  correct despite dropped writes. Read before shipping a game.
- [known-gaps.md](api/known-gaps.md) — what is missing, what is easy to get
  wrong, what to size a level around.
- [level-scale-visibility.md](api/level-scale-visibility.md) — a historical
  requirements note on culling for level-scale worlds.

**AI agents** — [`ai/AGENTS.md`](ai/AGENTS.md).

Each reference file closes with a "See also" line pointing at its `dev/`
counterpart, where the rationale and bring-up history live.

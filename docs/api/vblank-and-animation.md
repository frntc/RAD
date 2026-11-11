# vblank and tear-free animation

## Two ways to sync to the frame

- **Polling**: call `VBLANK_SYNC` ($09). It blocks — halts the C64, like
  any other command — until the next vblank, then returns. Simple, and
  fine for a program whose main loop has nothing else to do while it
  waits.
- **Interrupt**: call `VBLANK_ARM` ($02) once, then let the C64 continue
  running; gpu64 raises an IRQ line at the next vblank. `VBLANK_ACK` ($03)
  must follow every arm that fires, to release the line before arming
  again.

Both `VBLANK_SYNC` and `VBLANK_ARM` (while arming) can halt the C64 for up
to a full frame — don't call either from inside a per-frame budget that
assumes sub-millisecond commands.

## What the vblank signal actually is

gpu64 does not query the VideoCore for its raster position — there is no
such API available to it. Instead, the frame period is **measured once at
boot** against a free-running microsecond timer, and every subsequent
vblank prediction is extrapolated from that one measurement plus elapsed
time.

This has two consequences:

- A **constant offset** — the boot-time mailbox round-trip latency — is
  baked into the very first prediction and never fully removed.
- A slow **drift** — on the order of 1ms per several minutes — accumulates
  between the extrapolated clock and the real display. `VBLANK_SYNC`
  re-pins the extrapolation to a fresh measurement every time it's called,
  and `VBLANK_ARM` does this automatically on every arm. A program that
  syncs or arms regularly never accumulates enough drift to notice; one
  that free-runs for a long time between syncs will.

If the boot-time measurement itself failed, `GET_INFO`'s frame period
field reads 0, and both `VBLANK_SYNC` and `VBLANK_ARM` return
`UNSUPPORTED` for the rest of the session — as does `PAGE_FLIP` with
`ARG0 = 1`, since a deferred flip has no boundary to land on. Immediate
`PAGE_FLIP` (`ARG0 = 0`) is unaffected: it never waited on vsync to begin
with, so a program that checks the frame period and falls back to unsynced
flips still runs, just with visible tearing possible.

## Tear-free animation

The idiom every animated demo uses (see `gpu64_demo_bounce` for a worked
example):

1. Draw the next frame into the page that **isn't** currently visible —
   `SET_DRAW_PAGE` to the back page.
2. `VBLANK_ARM`.
3. Do any other per-frame work (input, game logic) while the C64 keeps
   running.
4. When ready to present, `PAGE_FLIP` — flips are immediate and cheap
   (about 1us), so doing this slightly before the vblank arrives is fine.
5. Spin on `STATUS` bit0 (busy) — or just re-check `STATUS` bit2
   (vblank-pending) — until the armed vblank actually lands, then
   `VBLANK_ACK`. This step is a plain register-read spin from the C64
   side; it does **not** halt the C64 the way a command dispatch does.

Repeat with the pages swapped. The two draw pages plus this idiom is what
keeps a moving picture free of visible tearing without ever blocking the
C64 for a full frame on every single frame — only the initial arm can
cost that much, and only if it lands unluckily close to the vblank it's
arming for.

## The deferred flip, and when the draw page moves

`PAGE_FLIP` with `ARG0 = 1` is the shorter version of the same idiom: it
arms the swap for the next vblank instead of doing it now, sets `STATUS`
bit0 (busy) until it lands, and saves you the arm/ack pair. A second
deferred flip while one is still pending returns `BUSY` and changes
nothing.

The one thing worth knowing about it: **the draw page moves when the flip
is armed, not when it lands.** As soon as `PAGE_FLIP` returns, drawing ops
— and `READ_RECT` — are addressing a fresh page, not the one you just
queued for display. So the sequence is simply

1. Draw the frame.
2. `PAGE_FLIP` with `ARG0 = 1`.
3. Draw the *next* frame immediately — no `SET_DRAW_PAGE`, no waiting for
   the flip to land. There are three pages precisely so that this is safe:
   the one being scanned out, the one queued to replace it, and the one
   you are drawing into.

Waiting for busy to clear before drawing again is still allowed, and is
what paces a program to the frame rate; it just isn't needed for
correctness of the page you draw into. This guarantee is also what class
1's `SCENE_COMMIT` relies on to keep an autonomous render frame off the
page that is about to be displayed.

See also: [getting-started.md](../guide/getting-started.md) for register addresses
and [error-codes.md](error-codes.md) for what `UNSUPPORTED` means here.

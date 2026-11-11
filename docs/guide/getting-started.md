# Getting started

gpu64 gives a Commodore 64 a second, independent HDMI screen and a command
API to draw on it. This page is everything you need to issue your first
command and read your first demo; the opcode tables live in their own
per-class reference files — see [README.md](../README.md) for the map.

## Register map

gpu64's registers are `$DF50`–`$DF68` in IO2 (REU keeps `$DF00`–`$DF0A`;
`$DF0B`–`$DF4F` is left to other hardware; IO1, `$DE00`–`$DEFF`, is
untouched).

**On a C64 Ultimate**, enable only the external cartridge on the
cartridge bus; with other bus options on, gpu64 sees none of the C64's
register accesses. **On any Ultimate** (C64 Ultimate, Ultimate 64,
1541 Ultimate) gpu64 coexists with the
Ultimate's Command Interface (`$DF1B`–`$DF1F`, fine to leave enabled), but two
other Ultimate features must be **off**: its own REU emulation (gpu64 provides
the REU at `$DF00`–`$DF0A`) and *Map Ultimate Audio `$DF20`-`$DFFF`*, which
covers gpu64's registers. If the HDMI boot log is still showing after your
program has started, no command has reached gpu64. Run
`gpu64_probe_ultimate.prg`: its VERDICT line says whether the machine's I/O2
routing or the bus timing is at fault.

**If gpu64 hangs on your machine** while 3D rendering runs, add
`GPU64_BURST_BYTES 128` to `SD:RAD/rad.cfg` (range 64–448, default 256). It
limits how much the Pi's rendering core writes between pauses: smaller is
slower to render but gives the C64 bus more margin. The picture is identical at
every setting.

| Register | Address | Purpose |
|---|---|---|
| `CMD_HI` | $DF50 | Selects the opcode class. Sticky — set once, then fire many opcodes in that class. |
| `CMD_LO` | $DF51 | Write an opcode number here to dispatch it. This is the write that halts the C64 until the command completes. |
| `STATUS` | $DF52 | bit0 busy, bit1 error, bit2 vblank-pending, bit3 vblank-IRQ-armed |
| `ERRCODE` | $DF53 | Written by every dispatch, success or failure — see [error-codes.md](../api/error-codes.md) |
| `ID` | $DF54–$DF55 | 16-bit resource id, for opcodes that create or reference one (textures, meshes, nodes...) |
| `ARG0`–`ARG15` | $DF56–$DF65 | 16-byte argument block for the opcode about to fire |
| `RESULT` | $DF66 | Low byte of the last command's result, per opcode |
| `SEQ` | $DF67 | Sequence number you write before `CMD_LO`, $01–$FE |
| `SEQACK` | $DF68 | The `SEQ` of the last command that actually dispatched |

## What a command costs the C64

Writing `CMD_LO` halts the 6502 — like an REU transfer — until gpu64
finishes the command and writes `ERRCODE`. Most opcodes return in well
under a millisecond. Two are different and must not be called in a
per-frame loop: `VBLANK_SYNC` and `VBLANK_ARM` (when arming) can each halt
for up to a full frame, because they wait on a real vertical sync.

## Issuing a command

1. Set `CMD_HI` if you're not already in the right class (it's sticky, so
   skip this if the last command you sent was in the same class).
2. Stage the opcode's `ARG` bytes. Staging order is free — write them in
   whatever order is convenient. `ARG` registers are write-only and are
   **not** cleared between commands, so an opcode with fewer arguments than
   the last one you sent will still see old bytes in the ones it doesn't
   use — harmless, since it only reads what it's documented to read, but
   don't rely on `ARG` registers reading back as zero.
3. Write the opcode number to `CMD_LO`. The C64 halts here.
4. When it resumes, `ERRCODE` holds the result. `STATUS` bit1 is just
   `ERRCODE != OK`, for a one-instruction check.

## Blob descriptor

Two shapes, both little-endian, both naming a source or destination outside
gpu64's own address space:

**Full descriptor — 6 bytes.** Used wherever a transfer's length isn't
implied by other arguments (`BLIT`, `UPLOAD_TEXTURE`, `GET_INFO`, ...).

| Offset | Size | Contents |
|---|---|---|
| 0 | 1 | `space`: 0 = C64 RAM, 1 = REU |
| 1 | 3 | `addr`, 24-bit |
| 4 | 2 | `len`, 16-bit |

**Compact descriptor — 4 bytes.** `space` + `addr` only, no `len` — used
where the transfer size is implied by other operands (the matrix ops'
dimensions, for instance).

Both shapes double as a *destination*: for `GET_INFO`, `READ_RECT`, and the
matrix/vector ops, gpu64 writes its result to the memory the descriptor
points at, before the `CMD_LO` write that started the command returns.

Rules that hold for every descriptor, source or destination:

- `len = 0` (full descriptor) is a no-op — nothing is read or written.
- `addr + len` running past the end of its space is `OUT_OF_RANGE`; nothing
  is clamped.
- `space` outside 0/1 is also `OUT_OF_RANGE`.

## The example programs

[`examples/`](../../examples/README.md) holds eleven small programs in a
reading order, from `hello` (a rectangle) to `cube` (a lit 3D object) and
`sectors` (a Doom-style level). [`games/`](../../games/) holds three whole
games built on the same API. None of them asserts anything — they are what
you copy from. The conformance suite that does assert is in
[`tests/`](../../tests/), run by `tools/testprg.sh`.

### Building and checking them

```
tools/examples.sh              # assemble, run on the PC, render everything
tools/examples.sh -v rotate    # just this one, and print the C64 screen
```

Each demo is run twice under [`tools/prgsim`](../../tools/prgsim/): once
modelling a display gpu64 measured a frame period from, and once modelling
one it could not, where every vblank feature answers `UNSUPPORTED`. A demo
has to survive both, which is why the runtime's `dmVbWait` / `dmFlip` fall
back to free-running and immediate flips instead of refusing to run.

What the run leaves in `out/examples/<name>.ppm` is what the HDMI
output was showing when the program returned — the visible page, through
the current palette, inside the border. **Look at those before deploying to
hardware.** Bench time is the scarce resource, and a demo that renders a
black rectangle on a PC will render a black rectangle on the bench.

### Running them on the C64

Copy the `.prg` files onto whatever the machine loads from, then

```
LOAD"GPU64-HELLO",8,1
RUN
```

In the RAD menu, press **T until it reads REU**. Everything gpu64 does
lives inside the REU polling loop; without that, nothing runs at all and
the HDMI screen keeps showing the text mirror.

### Resetting

The cartridge does not need to be power-cycled with the C64. Pressing the
C64's reset button, or power-cycling the C64 with the Pi left running, puts
gpu64 back to exactly the state it has just after the Pi boots — graphics
mode, the C64 palette, border 0, page 0, black pages, health alarm cleared —
and re-arms the screen mirror, so the READY prompt reappears on HDMI without
waiting for the Pi to boot again. A program can ask for the same thing
mid-session with `FULL_RESET` ($0B); see
[class0-2d-reference.md](../api/class0-2d-reference.md).

With the C64 switched off there is no bus clock at all, and HDMI shows a
holding screen until it comes back.

### The two include files

- `gpu64.inc` — the register window, the opcode numbers, the error
  codes, four macros for staging an argument block (`#argb`, `#argw`,
  `#argblob`, `#argcd`) plus `#cmd` to fire one, and — for class 2 — three
  more for staging batch records (`#r2col`, `#r2span`, `#r2batch`) and
  `#cmd2` / `#cmd0` to switch class and fire in one go. Copy this into your
  own project; it is the part of a gpu64 program that is the same in every
  gpu64 program.
- `gpu64_rt.inc` — housekeeping the demos share and the API knows
  nothing about: text and hex on the C64 screen, the STOP key, and the two
  vblank helpers with their no-frame-clock fallback. Included **last**,
  because it is code and data rather than macros.

A demo's skeleton is then:

```asm
	.include "gpu64.inc"
	#basicStub			; "10 SYS 2064", entry at $0810

start
	jsr dmInit			; FULL_RESET, class 0, read GET_INFO
	#argb 0, BLUE
	#cmd OP_CLEAR
	jmp dmHold
	.include "gpu64_rt.inc"
```

## Start your program with `FULL_RESET`

**Everything gpu64 holds survives your program loading.** The Pi is not
reset when the C64 loads a `.prg` — only a C64 `/RESET` re-baselines it — so
a program launched from a menu, or after another gpu64 program, inherits
whatever that program left behind: the palette, the border, the display
mode, the framebuffer contents, and which page is being drawn into versus
which page is on screen.

That last one is the least obvious and the most confusing to debug. If the
previous program ended with the draw page away from the visible page, your
`CLEAR` and everything you draw after it land on a page nobody is looking
at, while the screen keeps showing the previous program's last frame —
recoloured by *your* palette if you loaded one. It looks like your drawing
commands are being ignored and someone else's are working.

So begin with `FULL_RESET` ($0B), which puts the display back to its
post-boot state: graphics mode, the C64 palette, border 0, page 0 both drawn
and visible, every page black.

```asm
	lda #0
	sta CMD_HI			; class 0
	#cmd OP_FULL_RESET
```

It is a **setup-time** command — leaving text mode reprograms the VideoCore
and the C64 is halted for all of it, up to ~20 ms — so issue it once at
start-up and never in a frame loop. `dmInit` in `gpu64_rt.inc` does
exactly this, which is why the demos are safe to launch in any order.

One thing it does *not* do: class 1 and class 2 resources (meshes, nodes,
textures) are not freed. Those are released on a C64 `/RESET`. A program
that uploads resources and might be relaunched without one should keep
that in mind.

See also: [dev/demos_status.md](../../dev/demos_status.md) for which
of these have actually run on real hardware, and when.

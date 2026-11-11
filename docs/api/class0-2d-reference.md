# Class 0 — 2D and system reference

This is what a byte means for `CMD_HI = 0`: framebuffer drawing, palette,
blitting, and the system/info/matrix opcodes every gpu64 program touches
regardless of which higher class it also uses. Class 0 is always compiled
in.

See [getting-started.md](../guide/getting-started.md) for the register map and how a
command is issued, and [error-codes.md](error-codes.md) for the codes every
opcode below can return.

## System — $00–$0F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $00 | `NOP` | none | Dispatches and returns `OK`. Useful as a liveness check. |
| $01 | `RESET_STATE` | none | Resets the draw and visible page to 0, the text planes, and the vblank arm state. Does **not** touch the palette, the border, the display mode, or the framebuffer contents — for those use `FULL_RESET` ($0B). |
| $02 | `VBLANK_ARM` | none | Arms the vblank IRQ line. May halt up to a frame if a vblank is due imminently — see [vblank-and-animation.md](vblank-and-animation.md). |
| $03 | `VBLANK_ACK` | none | Releases the armed IRQ line. Must follow every `VBLANK_ARM` that fired. |
| $04 | `SET_DRAW_PAGE` | 1: page index | Selects which framebuffer page subsequent draw opcodes write to. `OUT_OF_RANGE` past `GPU64_FB_PAGES` (see the info block). |
| $05 | `PAGE_FLIP` | 1: 0 = now, 1 = at next vblank | Makes the draw page visible and hands you a fresh page to draw into. With `ARG0 = 0` the swap is immediate. With `ARG0 = 1` it lands at the next vblank, `STATUS` bit0 (busy) stays set until it does, and a second deferred flip while one is pending returns `BUSY` and changes nothing. **Either way the draw page moves as soon as the command returns** — see [vblank-and-animation.md](vblank-and-animation.md). `UNSUPPORTED` for `ARG0 = 1` if the boot-time frame period measurement failed, and `UNSUPPORTED` in either form while text mode is up (there is only one page there). |
| $06 | `GET_INFO` | 0-5: dest descriptor | Writes the 16-byte info block (below) to the given destination. |
| $07 | `LOG_ENABLE` | 1: 0/1 | Turns firmware-side debug logging on or off. Has no effect on drawing. Refused unkeyed while a strict class 1 program is committing, like `FULL_RESET`. |
| $08 | `SET_BORDER` | 1: colour | Sets the HDMI border colour. Overridden automatically by the sticky under-voltage indicator — see the health block below. |
| $09 | `VBLANK_SYNC` | none | Blocks until the next vblank. May halt up to a full frame. `UNSUPPORTED` if the boot-time frame period measurement failed. Refused unkeyed while a strict class 1 program is committing, like `FULL_RESET`. |
| $0A | `GET_HEALTH` | 0-5: dest descriptor | Writes the health block (below) to the given destination — 12 bytes, up to 128 with the diagnostic counters, or 132 with a magic and two check bytes that let you verify the block arrived whole. |
| $0B | `FULL_RESET` | none | Puts the whole display back to the state it has just after the Pi boots: graphics mode, the C64 palette, border 0, page 0 both drawn and visible, every page black, text planes cleared, the health alarm re-baselined, and a running class 1 loop stopped (the next `SCENE_COMMIT` restarts it). A **setup-time** command — leaving text mode reprograms the VideoCore and the C64 is halted for all of it (up to ~20 ms), so never issue it per frame. While a strict class 1 program is committing frames it is refused unkeyed — see strict mode in the class 1 reference. |
| $0C | `SET_DMA_WINDOW` | 0-1: base, 2-3: length | Confines every C64-space **readback** (`GET_INFO`, `GET_HEALTH`, `READ_RECT`, `GET_TRANSFORM`, the class-2 info blocks) to `base..base+length-1`. A destination outside it is refused with `OUT_OF_RANGE` and nothing is written. Length 0 means unrestricted, which is the reset default. `RESULT` echoes `ARG0^ARG1^ARG2^ARG3^$A5` so you can confirm the window landed — see below. |

## Whole surface — $10–$1F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $10 | `CLEAR` | 1: colour | Fills the current draw page with one colour. |
| $14 | `UPLOAD_POLYS` | 10: 0-5 blob descriptor, 6-7 count, 8-9 first | Loads `count` 16-byte polygon records — the same record format `DRAW_POLYS` takes — into the **resident world** table starting at slot `first`, so a level bigger than one transfer arrives as several commands. Max 2048 slots; `len` must equal `count * 16`, and `first + count` past 2048 is `BAD_ARGS`. `count = 0` with `first = 0` drops the pool; `count = 0` with any other `first` does nothing. Slots may be rewritten in place; the pool survives everything except `RASTER_RESET`. This is a class 0 opcode, but the table it fills is only drawn by class 2's `DRAW_WORLD` ($27) — see [class2-raster-reference.md](class2-raster-reference.md). |

## Primitives — $20–$2F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $20 | `SET_PIXEL` | 4: x(2), y(2), colour(1) — 5 total | Sets one pixel. Off-page coordinates are silently clipped, not an error. |
| $21 | `LINE` | x0,y0,x1,y1,colour | Bresenham line. Clipped to the page. |
| $22 | `RECT` | x,y,w,h,colour | Outline only. Clipped. |
| $23 | `RECT_FILL` | x,y,w,h,colour | Filled. Clipped. |

## Palette — $30–$3F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $30 | `PAL_SET` | 1: index, 3: r,g,b | Sets one palette entry. |
| $31 | `PAL_LOAD` | 0-5: source descriptor | Loads up to 256 entries (768 bytes, 3 per entry) from RAM or REU. |

## Blit — $40–$4F

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $40 | `BLIT` | dest x,y + 0-5 source descriptor + w,h | Opaque rectangular copy from C64 RAM or REU into the draw page. |
| $41 | `BLIT_KEYED` | as `BLIT`, +1: key colour | As `BLIT`, but pixels equal to the key colour are skipped — source stays transparent there. |
| $42 | `READ_RECT` | source x,y,w,h + 0-5 dest descriptor | Copies a rectangle *out of* the draw page into C64 RAM or REU. Pairs with `BLIT`/`BLIT_KEYED` for a save-under-sprite/restore idiom. |

## Text mode — $50–$5F

A second display **mode**, not a layer over the graphics one. `TEXT_MODE 1`
re-programs the display to 640×400 and turns it into 80 columns by 50 rows
of 8×8 glyphs taken from the Commodore 64's own character ROM; `TEXT_MODE 0`
puts the 320×200 paged framebuffer back. Only one of the two exists at a
time:

- While text mode is up there is no framebuffer page. Every drawing opcode
  above still returns `OK`, but writes nowhere, `PAGE_FLIP` returns
  `UNSUPPORTED`, and **every class 1 and class 2 command returns
  `UNSUPPORTED`** before it runs.
- `GET_INFO` answers for the live mode: in text mode it reports 640×400,
  `GPU64_FB_PAGES` = 1, and a 64×72 border. Reading `pages = 1` is how a
  program learns `PAGE_FLIP` is unavailable without trying it.
- `SET_BORDER`, `GET_HEALTH`, `VBLANK_ARM`/`ACK`/`SYNC` and the matrix ops
  all work normally.

The model is the C64's: three parallel 4000-byte planes — **screen codes**,
**foreground colour**, **background colour** — indexed row-major, so cell
`row * 80 + col`. Screen code **bit 7 is reverse video**, exactly as at
`$0400`. What the C64 does not have, and this does, is a per-cell
*background* colour, with all 256 palette entries available in both colour
planes.

| Op | Name | ARG bytes | Does |
|---|---|---|---|
| $50 | `TEXT_MODE` | 1: 0 = graphics, 1 = text | Switches the display and repaints from the planes. Asking for the mode you are already in returns `OK` and costs nothing. `BAD_ARGS` above 1; `BUSY` if a deferred `PAGE_FLIP` is still pending; `UNSUPPORTED` if the display could not be re-programmed. Drains any queued class 1 render first — this is a Stage 15b observation point. |
| $51 | `TEXT_CLEAR` | 1: fg, 1: bg | All 4000 cells to a space in those two colours. |
| $52 | `TEXT_PUT` | col, row, code, fg, bg | One cell. Off-grid is clipped, not an error — the same contract `SET_PIXEL` has. |
| $53 | `TEXT_WRITE` | 0-5: source descriptor, 6: col, 7: row, 8: fg, 9: bg, 10: flags | One line of text. `len` is clamped to 80 and the line is truncated at the right-hand edge, never wrapped. A `row`/`col` off the grid draws nothing and is not an error. Flags bit 0 = the payload is ASCII and gets translated to screen codes for the character set currently up; clear it to pass screen codes straight through, which is the only way to reach the graphics half of the ROM or to set bit 7. |
| $54 | `TEXT_UPLOAD` | 0-5: source descriptor, 6: plane, 7-8: first cell (u16) | Copies `len` bytes into one plane starting at cell `first`, leaving the other two planes alone. Plane 0 = screen codes, 1 = foreground, 2 = background. `BAD_ARGS` for a plane above 2; `OUT_OF_RANGE` if `first ≥ 4000` or `first + len > 4000` — checked before anything is written, so a caller that gets the arithmetic wrong is told rather than left with a partial upload. This is the cheap way to paint: a whole plane is one command. |
| $55 | `TEXT_CHARSET` | 1: 0 or 1 | Picks the half of the character ROM the whole screen is drawn with: 0 = upper case and graphics, 1 = lower case and upper case. Repaints; the planes do not change. `BAD_ARGS` above 1. |
| $56 | `TEXT_SCROLL` | 1: rows, 2: fg, bg | Scrolls the **whole screen** up by `rows`, filling the rows that appear at the bottom with spaces in the given colours. |
| $57 | `TEXT_FILL` | col (s8), row (s8), w, h, code, fg, bg | A rectangle of cells, all three planes. `col` and `row` are signed, so a rectangle may start off the left or top edge; the result is clipped. |
| $58 | `TEXT_REFRESH` | none | Repaints all 50 rows from the planes. Only needed after something outside the API has disturbed the display. |

`$59`–`$5F` are undefined and return `BAD_OPCODE`.

**The plane commands work with the graphics screen still up.** They update
the planes either way and only repaint when text mode is the live mode, so a
program can compose an entire text screen and have it appear complete at the
`TEXT_MODE 1` that follows. `RESET_STATE` ($01) resets the planes too — every
cell a space, light blue on blue, charset 1.

**Text mode is sticky within a session, but no longer across a C64 reset.**
A program that leaves text mode up still hands the *next command* a display
its graphics drawing cannot reach, so a program that switches modes should
still put the mode back where it found it. What has changed is what happens
between programs: a C64 reset or power-cycle now runs `FULL_RESET` in the
firmware, so the machine that comes back up at the READY prompt always has
the graphics display and the mirrored screen. The unconditional `TEXT_MODE 0`
the demo runtime's `dmInit` used to need for this is no longer load-bearing;
`FULL_RESET` ($0B) is the direct way to ask for the same thing mid-session.

## Info block

Written by `GET_INFO` ($06), 16 bytes:

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | `"$G64$"` magic (4 ASCII bytes) |
| 4 | 1 | API version |
| 5 | 2 | Framebuffer width (640 in text mode) |
| 7 | 2 | Framebuffer height (400 in text mode) |
| 9 | 1 | Bits per pixel |
| 10 | 1 | `GPU64_FB_PAGES` — number of framebuffer pages available for `SET_DRAW_PAGE`; 1 in text mode, where `PAGE_FLIP` is `UNSUPPORTED` |
| 11 | 1 | Implemented-classes bitmap (bit N set = class N is compiled into this build) |
| 12 | 2 | Border width, height (64×72 in text mode) |
| 14 | 2 | Frame period, in the same units `VBLANK_SYNC` extrapolates from; 0 if the boot-time measurement failed |

## Health block

Written by `GET_HEALTH` ($0A). The Pi reports its own throttle/temperature
state; a sticky under-voltage condition survives even a hang, because it's
read out of hardware state, not firmware state.

Ask for 12 bytes and you get the block below. Ask for 20, 36, 48, 64, 80, 96,
112 or 128 and you additionally get the diagnostic counters described under
"Diagnostic extension"; any other `len` ≥ 12 is rounded *down* to the next of
those sizes, so a program written against the 12-byte block keeps working
unchanged.

**Ask for 132 and the block verifies itself.** The four extra bytes are a
trailer over everything before them:

| Offset | Size | Contents |
|---|---|---|
| 128 | 1 | `'G'` |
| 129 | 1 | `'6'` |
| 130 | 1 | XOR of bytes 0-129 |
| 131 | 1 | Sum of bytes 0-129, modulo 256 |

Check all four. The magic says a block arrived at all; the two check bytes say
it arrived *whole* — XOR catches any single wrong byte, the sum catches a
transposition XOR is blind to. This matters because the readback travels over
the same DMA path as every bulk transfer, and a block that arrives only in part
is the one failure a "did my marker pattern get overwritten?" test cannot see:
the marker is gone from the bytes that landed and still present in the ones
that did not, so the test passes on corrupt data. If the trailer does not
check out, dispatch `GET_HEALTH` again — the counters are cumulative, so a
repeated read costs nothing but the dispatch.

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | Throttle word, current and sticky bits — see bit table below |
| 4 | 4 | Throttle word as latched **this session** — every word read since the Pi booted or since the last reset, OR'd together. A C64 reset or `FULL_RESET` ($0B) clears it, so it answers "has anything gone wrong since this program started". Offset 0's own bits 16-19 come straight from the VideoCore and latch until the **Pi** reboots; a reset cannot clear those. |
| 8 | 2 | Core temperature, tenths of a degree C |
| 10 | 2 | Highest core temperature seen this session, tenths of a degree C |

Throttle word bits:

| Bit | Meaning |
|---|---|
| 0 | Under-voltage detected, now |
| 1 | ARM frequency capped, now |
| 2 | Currently throttled |
| 3 | Soft temperature limit active, now |
| 16 | Under-voltage has occurred since boot (**sticky**) |
| 17 | ARM frequency capping has occurred since boot (sticky) |
| 18 | Throttling has occurred since boot (sticky) |
| 19 | Soft temperature limit has occurred since boot (sticky) |

**Bit 16 set is a verdict, not a hint.** If the sticky under-voltage bit is
set, something in the power delivery to that Pi was marginal at some point
— HDMI corruption, an intermittent hang, or a raster glitch reported around
that time should be re-examined as a power problem before anything else.
gpu64 automatically forces the HDMI border red the first time bit 16 latches,
independent of whatever `SET_BORDER` last asked for — this is deliberate and
is not a bug in `SET_BORDER`.

The red border is **session-scoped**: a C64 reset or `FULL_RESET` ($0B) takes
a fresh baseline of the VideoCore's already-latched bits and clears the alarm,
so the border goes back to what `SET_BORDER` asks for and only a brownout
that happens *after* that point reddens it again. Offset 0 bit 16 still reads
set — the hardware latch is untouched — so a program that wants "has this Pi
*ever* browned out" should read offset 0, and one that wants "has it browned
out since I started" should read offset 4.

### Diagnostic extension

Everything past byte 12 is an instrument, not a feature: 16-bit saturating
counters that exist so a bench program can read the firmware's own view of
itself without turning the log on and changing what it is measuring. They
are stable enough to write a test against and are not part of the drawing
contract.

| Offset | Size | Contents |
|---|---|---|
| 12 | 8 | Flip path: drain timeouts, post-full timeouts, slow flips, flips posted |
| 20 | 6 | DMA hold spacing: smallest gap, smallest dispatch→commit gap, ARM cycles per C64 cycle — all in raw ARM cycles |
| 26 | 10 | Hold-gap histogram buckets 0-2, dispatch→commit asserts under 4 C64 cycles, holds opened |
| 36 | 4 | Hold gate: gates armed, gates fired. Equal is the healthy steady state |
| 40 | 2 | Hold gate: worst wait from arm to hold, in C64 cycles |
| 42 | 2 | Hold gate: passes an armed gate could not fire on |
| 44 | 4 | Hold gate: `CMD_LO` writes deferred as read-modify-write, and deferred dispatches that ran |
| 48 | 4 | Bus sampling: gpu64-window **reads** the polling loop serviced, full 32-bit little-endian and **not** saturating |
| 52 | 4 | Bus sampling: gpu64-window **writes** it serviced, `CMD_LO` dispatches included; same format |
| 56 | 4 | Bus sampling: reads at no readable register, writes at no writable register (2 bytes each, saturating) |
| 60 | 4 | Bus sampling: last bad read address, OR of every bad read address, AND of every bad read address, last bad write address |
| 64 | 4 | Commands **dispatched**, full 32-bit little-endian and **not** saturating |
| 68 | 4 | Dispatches that arrived with `SEQ` unchanged since the previous one; same format |
| 72 | 4 | Reads of an `ARG` register: the dummy read `sta ARG,y` emits, not a fault. Same format |
| 76 | 1 | Destructive class 1 opcodes refused for want of the one-shot key in `ARG15` (saturating) |
| 77 | 1 | `CMD_LO` of the **last** such refusal — the opcode a phantom command arrived as |
| 78 | 1 | Times the live scene's active camera went away (saturating) |
| 79 | 1 | `SCENE_RESET`s that ran against a scene that had an active camera (saturating) |
| 80 | 2 | `SCENE_COMMIT`s that found the class 1 loop stopped and **re-started** it (saturating) |
| 82 | 2 | Class 1/2 commands refused because the display was not in graphics mode (saturating) |
| 84 | 2 | `SCENE_COMMIT`s refused because the frame clock was not calibrated (saturating) |
| 86 | 1 | Flag byte sampled at the **last** refusal — see the bit list below |
| 87 | 1 | What stopped the class 1 loop last: 0 nothing, 1 the `LOOP_STOP` opcode, 2 `SCENE_RESET`, 3 a session teardown (C64 reset), 4 boot, 5 the `FULL_RESET` opcode |
| 88 | 2 | Times the loop went running → stopped (saturating) |
| 90 | 2 | `CMD_LO` and `CMD_HI` of the last refusal |
| 92 | 1 | The same flag byte as 86, sampled **now** |
| 93 | 1 | `SEQ` the dispatch that stopped the class 1 loop was acting on |
| 94 | 1 | `SEQ` the dispatch *before* that one was acting on |
| 95 | 1 | `CMD_LO` of that previous dispatch |

Bytes 44-47 are 0 for every program that writes `CMD_LO` with a plain `sta`,
which is every program in this tree. A non-zero value there means something
is doing `inc $DF51` or an indexed store whose dummy read lands in `$DFxx` —
safe, and handled, but worth knowing about.

Bytes 48-63 count what the polling loop actually saw on the bus, which is
what makes a lost access measurable from the C64 side. Count your own
accesses to `$DF50-$DF68` between two `GET_HEALTH` calls and compare:

- **Fewer seen than issued** — an access was never sampled at all. The C64
  read a floating bus, which answers `$FF`.
- **Bad addresses non-zero** — an access *was* serviced, at the wrong
  address. A4-A7 are multiplexed on the cartridge port, so a mis-sample can
  only ever *set* those bits; bytes 61 and 62 say which ones moved.

Bytes 76-79 are the one-shot key's ledger — see "The key byte" in
[class1-3d-mesh-reference.md](class1-3d-mesh-reference.md). Byte 77 is the
reading that matters: it names the opcode of the last destructive command
refused for want of the key, which is how a mis-sampled `CMD_LO` gets
identified rather than merely survived. Bytes 78 and 79 count the damage
that was *not* prevented, so a run can tell "the gate caught it" from "it
never happened".

Bytes 72-75 are the exception to "bad reads are faults", and are counted
apart from byte 56 for that reason. `sta ARG,y` — the ordinary way to stage
an argument block — makes the 6502 spend a dead cycle *reading* the address
it is about to write. `ARG` registers are write-only, so that read answers
`$FF`, the 6502 throws the byte away, and nothing is wrong. Before these
bytes existed the idiom read as a 4.3% mis-sample rate.

Because bytes 48-55 are the denominators for a rate, they are the one part
of this block that does not saturate. They wrap at 2^32 and callers are
expected to use *differences* between two reads, never the absolute value.
`gpu64_probe_bus` in `tests` does exactly this and prints a verdict.

Bytes 64-71 do the same job one level up, for programs that use the
sequence-number protocol. If your `SEQACK` read does not match the `SEQ` you
wrote, three different things could have gone wrong and they have different
fixes; these two counters, differenced across the run, separate them:

- **Dispatches short of the commands you sent** — the `CMD_LO` writes were
  lost, and those commands never ran at all.
- **Dispatches match, byte 68 up by your mismatch count** — every command
  ran; it was the `SEQ` writes that were lost, so the commands executed
  against a stale sequence number and the arguments were fine.
- **Dispatches match, byte 68 unchanged** — nothing was lost on the write
  side. Read `SEQACK` a second time: a re-read that agrees means the first
  *read* was the failure, which is the `$FF` case in
  [error-codes.md](error-codes.md).

Byte 68 counts any dispatch that arrives with `SEQ` unchanged, so a program
that mixes sequenced and unsequenced commands sees one count per unsequenced
command as its baseline. Bytes 64-71 also count the `GET_HEALTH` that builds
the block, so two calls differ by at least one dispatch.

Bytes 80-92 answer a different question: **what is class 1 doing that you
did not ask for?** `GPU64_ERR_UNSUPPORTED` ($06) is produced in several
unrelated places. Difference bytes 80-85 across the run:

- **Byte 80 up** — something stopped the loop, and a later `SCENE_COMMIT`
  restarted it. This is a repair count, not a failure count: as of
  2026-09-10 a stopped loop is no longer permanent, and each of these cost
  one frame. It is still worth reading, because it should be **zero** —
  every one of them is a stop your program did not ask for. Byte 87 names
  what stopped it; `1` means a `LOOP_STOP` opcode executed, so if your
  program did not issue one, the command stream was corrupted.
- **Byte 82 up** — the display left graphics mode, which refuses *every*
  class 1 and class 2 command, not just the commit. Only `TEXT_MODE`
  ($50-$58) does that.
- **Byte 84 up** — the frame clock lost calibration.

Bytes 93-95 follow byte 87 up. When 87 reads `1` — a `LOOP_STOP` opcode
executed — they say where that opcode came from, and the reading is the
difference `93 - 94`: how far the sequence number moved for the dispatch that
stopped the loop.

- **1** (or **3** if your counter skips values, as ours skips `0` and `$FF`)
  — a real command arrived. Its `CMD_LO` byte was what was wrong, so the
  write was sampled but its *data* was not what you sent. A sequence number
  cannot detect that.
- **0** — no command arrived at all: `SEQ` did not move, so nothing wrote it.
  Something put a `$07` in `CMD_LO` that your program did not send.
- **anything else** — commands were lost either side of this one as well.

Byte 95 names the opcode that ran immediately before, which says where in
your own command stream the intruder landed. All three are only evidence
while byte 87 reads `1`; otherwise they hold whatever the last stop left.

Byte 86 is the state at the moment of the last refusal, which is the state
the run was in when it went wrong; byte 92 is the same flags now, which after a
`LOOP_STOP` legitimately reads differently. Bits, in both:

| Bit | Meaning |
|---|---|
| 0 | The class 1 loop is running |
| 1 | The frame clock is calibrated |
| 2 | The display is in graphics mode |
| 3 | A page flip is pending |
| 4 | `STATUS` bit 4, `FRAME_READY` |
| 5 | `STATUS` bit 0, `BUSY` |
| 6 | The framebuffer exists and is initialised |

## Matrix and vector ops — $80–$9F

A small linear-algebra coprocessor, useful for anything transforming
vertices before handing them to class 1/2, or for a game doing its own math
gpu64 can do faster. Two parallel op sets, same opcode offsets within each
half:

- **Fixed** — $80–$8F. Elements are 8.8 fixed point, 2 bytes each.
- **Float** — $90–$9F. Elements are IEEE754 single precision, 4 bytes each.

| Offset | Name | ARG bytes | Does |
|---|---|---|---|
| +$00 | `MAT_MUL` | dims + 2 compact descriptors (operands) + 1 compact descriptor (dest) | `dest = a * b`. Matrix dimensions from ARG bytes; `a`'s columns must equal `b`'s rows. |
| +$01 | `MAT_ADD` | dims + 2 operand descriptors + 1 dest descriptor | Elementwise. Operand dimensions must match. |
| +$02 | `MAT_SUB` | as `MAT_ADD` | Elementwise. |
| +$03 | `MAT_SCALE` | dims + 1 operand descriptor + scalar + 1 dest descriptor | Elementwise multiply by a scalar. |
| +$04 | `MAT_TRANSPOSE` | dims + 1 operand descriptor + 1 dest descriptor | |
| +$05 | `MAT_IDENTITY` | dim (square) + 1 dest descriptor | Writes an n×n identity matrix. |
| +$06 | `MAT_INVERSE` | dim (square, n≤64) + 1 operand descriptor + 1 dest descriptor | Gaussian elimination with partial pivoting (double precision internally, regardless of fixed/float mode, for numerical stability). `SINGULAR` if the matrix has no inverse. `OUT_OF_RANGE` for n>64. |

Matrices are row-major. A matrix operand is addressed with a **compact**
descriptor (space + addr, no length — the op's dimension arguments imply
the byte count). Any dimension of 0 is `BAD_ARGS`. An operand or
destination whose implied extent runs past its space is `OUT_OF_RANGE`. All
results are written back to their destination before the `CMD_LO` write
that started the command returns.

A vector is a 1×N (or N×1) matrix — there is no separate vector opcode set.
`MAT_MUL` with a 1-row or 1-column operand is how a vertex or a batch of
vertices gets transformed; a single call is limited to 255 vertices.

`$A0`–`$FF` in class 0 are undefined and return `BAD_OPCODE`.

See also: [class2-raster-reference.md](class2-raster-reference.md) for
`UPLOAD_POLYS`'s polygon record format and `DRAW_WORLD`, and
[dev/milestone4_2d_api_design.md](../../dev/milestone4_2d_api_design.md)
for class 0's original design rationale.

## The readback fence (`SET_DMA_WINDOW`, $0C)

Every opcode that hands data back to the C64 takes its destination address in
`ARG` bytes, and no `ARG` byte is covered by `SEQ`/`SEQACK`. One dropped store
therefore does not produce a failed readback — it produces a **successful**
readback to an address you never named, written by the Pi, into a C64 that is
halted while it happens. If that address is inside your program, your program
is gone, and no amount of retrying or re-sending repairs it. This is the only
direction in which a lost register write can damage the C64 rather than the
scene.

`SET_DMA_WINDOW` is the fence. Declare the buffer you read into, once, before
the first readback:

```asm
	lda #0
	sta CMD_HI
	#argw 0, hbuf			; base
	#argw 2, 128			; length
	lda #OP_SET_DMA_WINDOW
	sta CMD_LO
```

After that a readback aimed anywhere else answers `OUT_OF_RANGE` ($03) and
writes nothing. A dropped `ARG` byte becomes an error code you can see instead
of damage you cannot.

**Confirm it landed.** `ARG` registers are not readable — the register file
answers `$FF` to everything except `STATUS`, `ERRCODE`, `RESULT` and `SEQACK` —
so `RESULT` carries the echo: `ARG0 ^ ARG1 ^ ARG2 ^ ARG3 ^ $A5`. Compare it,
and send the command again if it does not match. Do **not** refuse to start if
it never matches: a program with no window is exactly as safe as every program
written before this opcode existed, and giving up is the more expensive
mistake (see [error-codes.md](error-codes.md)).

**Re-assert it.** The window is state you set once and never change, which by
the rule in [state-refresh.md](state-refresh.md) means nothing else will ever
correct it — and a misaddressed command that moves it aims your next readback
back into open memory. Give it a slot in your refresh ring.

**Fence the *first* readback, not the second.** The window covers one
contiguous region, so a program that reads into two different buffers has to
either declare a window that spans both or move the window before each
readback. Getting this wrong is easy in the direction that leaves a readback
outside the fence rather than the one that fails loudly: gpu64's own demo
runtime called `GET_INFO` in its start-up routine and only then declared a
window over its health buffer, so the very first writeback every demo made was
the one writeback nothing protected. It went unnoticed for the whole project
and showed up as a hung C64 (`dev/progress_tracker.md` section 56). Count
your readbacks, and fence from the first one.

**Check the data arrived, not just that the command returned.** A fence turns a
misaddressed readback into `OUT_OF_RANGE`, but a readback whose dispatch was
lost returns nothing at all and leaves your buffer untouched — and a buffer
full of plausible-looking zeros is indistinguishable from a real answer. Fill
the buffer with a known pattern, dispatch, and verify. Where the block carries
a constant — `GET_INFO`'s bytes 0-2 are always `'G'`, `'6'`, `'4'` — check
*that*, because a magic needs no judgement about what plausible data looks
like, and a pattern check can pass on a block that arrived only in part.

The window is cleared back to unrestricted by a C64 reset, because the memory
map it describes belongs to the program that declared it.


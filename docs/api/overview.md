# gpu64 API reference

The C64→gpu64 command protocol over IO2: everything a C64 programmer needs
to drive gpu64's framebuffer.

Everything on this page is implemented. Vblank included — it shipped after
milestone 4, and the one thing that can still take it away is a display
gpu64 could not measure a frame period from at boot; read the frame period
out of `GET_INFO` to find out (see [vblank](#vblank)). See
[progress_tracker.md](../../dev/progress_tracker.md) for hardware test status.

Why the protocol looks the way it does is in
[milestone4_2d_api_design.md](../../dev/milestone4_2d_api_design.md); status and
hardware test results are in [progress_tracker.md](../../dev/progress_tracker.md).
Class 1 (3D) is specified in
[class1-3d-mesh-reference.md](class1-3d-mesh-reference.md); the retained
scene graph, autonomous render loop and per-frame commit it targets — the
thing this whole API is converging on being "OpenGL for the C64" — is
[milestone6_3d_design.md](../../dev/milestone6_3d_design.md)'s subject,
staged in [gap_filling_plan.md](../../dev/gap_filling_plan.md).

Class 2 (below) is **deprecated in favour of class 1's retained scene
graph** — it still works and its own opcode table is unchanged, but new
work should target class 1. See the note at the top of that section.

---

## Framebuffer

| Property | Value |
|---|---|
| Resolution | 320 x 200 |
| Format | 8 bits per pixel — one byte per pixel, a palette index |
| Page size | 64000 bytes |
| Pages | 2 (front / back), hardware-swapped |
| Palette | 256 entries, 24-bit RGB — any 256 colours out of 16.7M. Index 255 is reserved for the on-screen log. |
| Origin | top-left, +y down |
| Border | 32 px left/right, 36 px top/bottom, around the 320x200 surface |

Draw ops write to the **draw page**; the **visible page** is what the HDMI
output shows. Both are page 0 at reset, so a program that ignores paging
draws straight to the visible screen. `SET_DRAW_PAGE` and `PAGE_FLIP` give
you tear-free buffering: draw the whole frame into the back page, then flip.

There are **three** pages, not two. A deferred `PAGE_FLIP` hands the new
offset to the display hardware and returns without waiting for it, so the
page that was visible can stay on screen for up to one more display frame.
The third page is what the flip gives you to draw into meanwhile; you never
have to think about which one it is, because `PAGE_FLIP` picks it.

### Border

Around the drawing surface is a **border**, exactly as on the C64: a frame
32 pixels wide at the sides and 36 tall at top and bottom, which no draw op
can reach and which `SET_BORDER` ($08) paints in one palette index. It is
black at reset.

The border is a property of the display, not of a page, so `PAGE_FLIP` never
changes it — set it once and it stays. That also means `SET_BORDER` is
comparatively expensive: it repaints the frame on every page. Set it when it
changes, not once per frame.

Coordinates are **signed 16-bit**. Drawing ops **clip** to the 320x200
page — geometry partly or wholly offscreen is normal and never sets an
error. `w`/`h` fields are unsigned 16-bit; a zero `w` or `h` is a no-op.

At reset, palette entries 0–15 hold the standard C64 colours (indices match
the C64's own colour numbering), 16–254 are black, and 255 is white. The
framebuffer contents at reset are undefined — `CLEAR` first.

### Index 255 and the on-screen log

gpu64 draws its own diagnostic log over your framebuffer in **palette index
255**, one colour, glyph pixels only — the space around each glyph is left
untouched, so whatever you drew shows through. The log is **on at reset**;
hidden automatically the moment your first command succeeds; turn it back on with `LOG_ENABLE` ($07) when you want it.

Nothing stops you using index 255 as an ordinary drawing colour, or
repointing it with `PAL_SET` — the log just draws in whatever colour that
entry holds, and becomes invisible if you set it to match your background.

## Register map

gpu64's registers are **$DF50–$DF68** in IO2. REU keeps $DF00–$DF0A;
$DF0B–$DF4F is left alone for other hardware (the firmware neither answers
reads nor acts on writes there); IO1 ($DE00–$DEFF) is untouched.

**On a C64 Ultimate / Ultimate 64 / 1541 Ultimate** gpu64 coexists with the
Ultimate's Command Interface (`$DF1B`–`$DF1F`, fine to leave enabled), but two
other Ultimate features must be **off**: its own REU emulation (gpu64 provides
the REU at `$DF00`–`$DF0A`) and *Map Ultimate Audio `$DF20`-`$DFFF`*, which
covers gpu64's registers.

| Address | Name | Dir | Purpose |
|---|---|---|---|
| $DF50 | `CMD_HI` | W | Class selector. **Sticky** — persists until changed, 0 at reset. Writing it alone triggers nothing. |
| $DF51 | `CMD_LO` | W | Opcode within the current class. **Writing it fires the command**, using whatever `ID`/`ARG` are staged. |
| $DF52 | `STATUS` | R | bit0 busy, bit1 error, bit2 vblank-pending, bit3 vblank-IRQ-armed |
| $DF53 | `ERRCODE` | R | Result of the last dispatch (see [Error codes](#error-codes)) |
| $DF54–$DF55 | `ID` | W | Resource ID, 16-bit — unused in class 0; class 2 names a texture with it |
| $DF56–$DF65 | `ARG0`–`ARG15` | W | 16-byte argument block, layout defined per opcode |
| $DF66 | `RESULT` | R | Low byte of the last command's result; meaning is per opcode (class 1) |
| $DF67 | `SEQ` | W | Sequence number, $01–$FE, written before `CMD_LO` |
| $DF68 | `SEQACK` | R | The `SEQ` of the most recently dispatched command |
| $DF69–$DFFF | — | — | Reserved |

### What a command costs the C64

**The write to `CMD_LO` halts the C64 until the command finishes.** gpu64
holds the bus for the whole dispatch, exactly the way an REU transfer does,
and the CPU resumes at the next instruction with nothing missed. You do not
need to poll, wait, or space your writes out — staging `ARG` bytes and
firing `CMD_LO` back to back always works.

The halt is bounded by the command's own work: a `SET_PIXEL` is trivial, a
`CLEAR` writes 64000 bytes, a blit also moves its payload across the bus.
Nothing is unbounded, and nothing else in the API halts the CPU for longer
than a normal register access.

If you have raster-timed code running, treat a gpu64 command the way you
would treat an REU transfer: it steals the cycles it needs.

### Issuing a command

1. Write `CMD_HI` **only when changing class**. Staying in class 0 (the
   normal case) means never writing it after the first time.
2. Stage `ARG` bytes in any order, any number of writes.
3. Write `CMD_LO` — this dispatches.
4. Optionally read `ERRCODE`.

`ARG` registers are write-only and are **not** cleared between commands.
Every opcode reads exactly the byte count listed in its table row and never
looks past it, so leftovers from a previous command cannot leak in — but
that also means you must stage every byte an opcode reads, every time.

Class 0 commands complete before the `CMD_LO` write returns, except a
vblank-deferred `PAGE_FLIP`. Poll `STATUS` bit0 (busy) if you used one.

Two commands are slower than their work suggests, because they wait for the
display rather than for gpu64: `VBLANK_SYNC`, and `VBLANK_ARM` when arming.
Both can halt the C64 for up to one frame. They are setup and housekeeping
commands — do not put either in a per-frame loop.

### Blob descriptor

Bulk data never crosses the register window byte-by-byte. A command that
needs data passes a 6-byte **blob descriptor** — where the bytes already
live — and gpu64 fetches them itself:

| Bytes | Field | Purpose |
|---|---|---|
| 1 | `space` | 0 = C64 RAM, 1 = REU |
| 3 | `addr` | 24-bit offset in that space (C64 RAM uses the low 16 bits) |
| 2 | `len` | byte count, max 65535 |

Multi-byte fields are little-endian (low byte first), as a 6502 expects.

The same descriptor is also used as a **destination**: commands that produce
data (`GET_INFO`, `READ_RECT`, every math op) don't return it through a
register window — they write it back into the memory you point them at, and
the write lands before the `CMD_LO` write returns. So a matrix multiply is
"here are the two operands, here is where the result goes", and you read the
result straight out of your own RAM.

- `len = 0` is a no-op, not an error.
- `addr + len` past the end of the space → `OUT_OF_RANGE`. No clamping, no
  wraparound.
- The fetch sees whatever memory banking (`$01`) is in effect at that
  moment. There is no bank-select field.
- Cost: the transfer's time is proportional to `len` — a 64000-byte blit
  costs more than a 256-byte one, and a command with both a source and a
  destination pays for both. See "What a command costs the C64" below.

#### Compact descriptor

Math opcodes use a 4-byte **compact descriptor** instead — `space` (1) plus
24-bit `addr` (3), with no `len`, because the byte count is already implied
by the operand's dimensions and element size. Bounds are checked against
that computed count exactly as if you had written it out.

## Class 0 opcodes — 2D, system, math

Set `CMD_HI = 0` (the reset default). `ARG` offsets below are relative to
`ARG0` ($DF56). "Bytes" is exactly how many `ARG` bytes the opcode reads.

### System — $00–$0F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $00 | `NOP` | 0 | — | Nothing. Liveness probe: sets `ERRCODE = OK`. |
| $01 | `RESET_STATE` | 0 | — | `ERRCODE = OK`, vblank IRQ disarmed, vblank-pending cleared, draw page = visible page = 0. Leaves framebuffer contents and palette alone. |
| $02 | `VBLANK_ARM` | 1 | `ARG0`: 0 = disarm, 1 = arm | Arms the auto-rearming vblank IRQ. Reflected in `STATUS` bit3. Arming also re-syncs the frame clock, so it costs up to one frame of halt — see [vblank](#vblank). |
| $03 | `VBLANK_ACK` | 0 | — | Clears `STATUS` bit2 (vblank-pending). Poll-loop programs write this after seeing the bit; IRQ handlers write it on entry. |
| $04 | `SET_DRAW_PAGE` | 1 | `ARG0`: 0, 1 or 2 | Selects the page subsequent draw ops write to. |
| $05 | `PAGE_FLIP` | 1 | `ARG0`: 0 = now, 1 = at next vblank | Makes the draw page visible and the old visible page the draw page. With `ARG0 = 1` the swap happens at the next vblank and `STATUS` bit0 (busy) stays set until it lands; asking for a second deferred flip while one is still pending returns `BUSY` and changes nothing. |
| $06 | `GET_INFO` | 6 | `ARG0-5` destination descriptor | Writes the 16-byte info block (see below) to your memory. `len` must be ≥ 16. |
| $07 | `LOG_ENABLE` | 1 | `ARG0`: 0 = off, 1 = on | Shows or hides gpu64's on-screen log overlay. On at reset, but **the first successful command of a session hides it automatically** so firmware text does not land on your output; `LOG_ENABLE` itself is exempt, so call it whenever you actually want the log. Turning it on also prints two `FLIP` lines giving what this session's page flips cost, in microseconds. |
| $08 | `SET_BORDER` | 1 | `ARG0`: palette index | Paints the border around the drawing surface. Black at reset. See [Border](#border). |
| $09 | `VBLANK_SYNC` | 0 | — | Re-syncs the frame clock against a real vertical sync. Costs up to one frame of halt; occasional housekeeping for a long-running program, not a per-frame call. See [vblank](#vblank). |
| $0A | `GET_HEALTH` | 6 | `ARG0-5` destination descriptor | Writes the 12-byte health block (see below) to your memory: the Pi's own power and temperature state. `len` must be ≥ 12; ask for 132 and the block carries a magic plus XOR and sum check bytes over everything before them, so you can tell a block that arrived whole from one that arrived in part (see class0-2d-reference.md). Costs a mailbox round trip to the VideoCore — about a millisecond of halt — so poll it once a second at most, never per frame. |
| $0C | `SET_DMA_WINDOW` | 4 | `ARG0-1` base, `ARG2-3` length (0 = unrestricted) | Fences every writeback opcode into one region of C64 memory, so a dropped destination-address byte cannot make gpu64 DMA over your program. `RESULT` echoes `ARG0^ARG1^ARG2^ARG3^$A5` so you can confirm the window landed — the only confirmation available, since `ARG` is not readable. See the readback fence in [class0-2d-reference.md](class0-2d-reference.md#the-readback-fence-set_dma_window-0c). |

### Whole surface — $10–$1F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $10 | `CLEAR` | 1 | `ARG0`: colour index | Fills the entire draw page. |

### Primitives — $20–$2F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $14 | `UPLOAD_POLYS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8-9` first | Loads `count` 16-byte polygon records — the same records `DRAW_POLYS` takes — into the **resident world** starting at slot `first`, so a level bigger than one transfer arrives as several commands. Max 2048 slots; `len` must equal `count * 16`, and `first + count` past 2048 is `BAD_ARGS`. `count = 0` with `first = 0` drops the pool; `count = 0` with any other `first` does nothing. Slots may be rewritten in place. The pool is dropped by `RASTER_RESET` and survives everything else, including `UPLOAD_TEX`. |
| $20 | `SET_PIXEL` | 5 | `ARG0-1` x, `ARG2-3` y, `ARG4` colour | One pixel. |
| $21 | `LINE` | 9 | `ARG0-1` x0, `ARG2-3` y0, `ARG4-5` x1, `ARG6-7` y1, `ARG8` colour | Line, both endpoints inclusive. |
| $22 | `RECT` | 9 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` w, `ARG6-7` h, `ARG8` colour | 1-pixel outline; `x,y` is the top-left corner, `w`/`h` include the outline. |
| $23 | `RECT_FILL` | 9 | as `$22` | Filled rectangle. |

### Palette — $30–$3F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $30 | `PAL_SET` | 4 | `ARG0` index, `ARG1` r, `ARG2` g, `ARG3` b | Sets one entry from 24-bit RGB. |
| $31 | `PAL_LOAD` | 8 | `ARG0-5` blob descriptor, `ARG6` first index, `ARG7` count | Fetches `count` RGB triples into entries `first .. first+count-1`. `count = 0` is a no-op. `len` must equal `count * 3` and `first + count` must be ≤ 256, else `BAD_ARGS`. |

### Blit — $40–$4F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $40 | `BLIT` | 14 | `ARG0-5` blob descriptor, `ARG6-7` dstX, `ARG8-9` dstY, `ARG10-11` w, `ARG12-13` h | Fetches `w * h` bytes of 8bpp palette indices, row-major with stride `w`, into the draw page at `dstX,dstY`. Clipped like any drawing op. `len` must equal `w * h`, else `BAD_ARGS`. |
| $41 | `BLIT_KEYED` | 15 | as `$40`, plus `ARG14` key index | Same, but source pixels equal to `key` leave the destination untouched — the sprite case. |
| $42 | `READ_RECT` | 14 | `ARG0-5` destination descriptor, `ARG6-7` srcX, `ARG8-9` srcY, `ARG10-11` w, `ARG12-13` h | The reverse of `BLIT`: copies a `w * h` rectangle of the **draw page** back into your memory, row-major with stride `w`. The rectangle must lie entirely within the page (this one does **not** clip) else `BAD_ARGS`; `len` must equal `w * h`. |

### Info block

`GET_INFO` writes 16 bytes:

| Offset | Size | Contents |
|---|---|---|
| 0 | 3 | `"G64"` |
| 3 | 1 | API version, major |
| 4 | 1 | API version, minor |
| 5 | 2 | framebuffer width (320) |
| 7 | 2 | framebuffer height (200) |
| 9 | 1 | bits per pixel (8) |
| 10 | 1 | number of pages (3) |
| 11 | 1 | bitmap of implemented classes: bit0 = class 0, bit1 = class 1 (3D), bit2 = class 2 (raster) |
| 12 | 1 | border width, each side (32) |
| 13 | 1 | border height, each side (36) |
| 14 | 2 | measured frame period, microseconds — **0 means the frame clock never calibrated**, and every vblank feature will answer `UNSUPPORTED` |

### Health block

`GET_HEALTH` writes 12 bytes. The Pi is powered separately from the C64, and
a supply that sags under load takes gpu64 down in ways that look like
anything but a power fault. This is the Pi saying so itself.

| Offset | Size | Contents |
|---|---|---|
| 0 | 4 | last raw throttle word read from the VideoCore |
| 4 | 4 | every throttle word ever read, OR-ed together |
| 8 | 2 | core temperature, tenths of a degree Celsius |
| 10 | 2 | highest core temperature seen this session |

Bits in both throttle words, low byte first:

| Bit | Meaning |
|---|---|
| 0 | under-voltage **now** |
| 1 | ARM frequency capped now |
| 2 | throttled now |
| 3 | soft temperature limit active now |
| 16 | under-voltage **has occurred** since boot |
| 17 | ARM frequency has been capped since boot |
| 18 | has been throttled since boot |
| 19 | soft temperature limit has been active since boot |

Bits 16-19 are sticky in the hardware, so a once-a-second poll cannot miss
an event however brief — which is the point: a brownout long enough to
corrupt a transfer is far shorter than a frame.

**Bit 16 set is a verdict, not a hint.** It means the supply is not
delivering 5V under load — a power supply or cable problem, not a gpu64
defect — and every other symptom should be read in that light until it is
fixed.

gpu64 also acts on it without being asked: the first time a `GET_HEALTH`
sees under-voltage, the firmware paints the border **red** and leaves it
red. That indicator is written to all three pages, so it survives a page
flip — and it survives the C64 hanging, which is exactly the case where no
program is left running to report anything.

## Matrix and vector ops — $80–$9F

Two parallel sets of the same operations, differing only in element format:

- **$80–$8F — fixed point.** Elements are signed 16-bit **8.8** fixed point,
  little-endian: `$0100` = 1.0, `$FF00` = −1.0, `$0080` = 0.5. Range
  ±127.99, resolution 1/256. **2 bytes per element.** Products accumulate at
  full width and come back to 8.8 **rounded half away from zero and
  saturated** — a value past the range clamps to ±127.99 rather than
  wrapping.
- **$90–$9F — floating point.** Elements are 32-bit IEEE 754 single
  precision, little-endian. **4 bytes per element.**

Layouts are identical between the two sets; only the implied byte counts
differ. The low nibble picks the operation, so `$80`/`$90` are both
`MAT_MUL`, `$81`/`$91` both `MAT_ADD`, and so on.

Matrices are **row-major**, dimensions are given as bytes 1–255, and
operands are addressed with the 4-byte [compact descriptor](#compact-descriptor)
— no `len`, since dimensions and element size imply it. A dimension of 0 is
`BAD_ARGS`; an operand or result running past the end of its space, or a
result larger than 65535 bytes, is `OUT_OF_RANGE`.

Results are written back to the memory the destination descriptor points at,
before the `CMD_LO` write returns. Source and destination may be in
different spaces (compute from REU, write back to C64 RAM). Overlapping
source and destination is undefined.

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $80 / $90 | `MAT_MUL` | 15 | `ARG0` m, `ARG1` k, `ARG2` n, `ARG3-6` A, `ARG7-10` B, `ARG11-14` C | C(m x n) = A(m x k) · B(k x n) |
| $81 / $91 | `MAT_ADD` | 14 | `ARG0` m, `ARG1` n, `ARG2-5` A, `ARG6-9` B, `ARG10-13` C | C = A + B, elementwise, all m x n |
| $82 / $92 | `MAT_SUB` | 14 | as `$81` | C = A − B |
| $83 / $93 | `MAT_SCALE` | 12 fixed / 14 float | `ARG0` m, `ARG1` n, `ARG2-5` A, `ARG6-9` C, then the scalar inline: `ARG10-11` (8.8) or `ARG10-13` (float) | C = A · scalar |
| $84 / $94 | `MAT_TRANSPOSE` | 10 | `ARG0` m, `ARG1` n, `ARG2-5` A, `ARG6-9` C | C(n x m) = A(m x n) transposed |
| $85 / $95 | `MAT_IDENTITY` | 5 | `ARG0` n, `ARG1-4` C | C(n x n) = identity |
| $86 / $96 | `MAT_INVERSE` | 9 | `ARG0` n, `ARG1-4` A, `ARG5-8` C | C = A⁻¹ (n x n), **n ≤ 64** (larger is `BAD_ARGS`). A singular matrix sets `SINGULAR` and writes nothing. The elimination runs in double precision whatever the element format is, so an 8.8 operand keeps its precision through the inversion. |

A vector is just a matrix with one row or one column: transforming a
4-vector by a 4x4 matrix is `MAT_MUL` with m=4, k=4, n=1, and transforming
a batch of vertices is the same call with n = however many. Every dimension
is a single byte, so a batch is at most 255 vertices per call — split larger
sets across calls, advancing the operand and result addresses.

Everything not listed above — including $A0–$FF — is undefined: writing it
sets `ERRCODE = BAD_OPCODE` and does nothing else.

## Class 2 opcodes — the raster layer

**Deprecated (2026-08-28), frozen, not removed.** Class 2 was gpu64's
first 3D-shaped layer — a per-frame, C64-driven batch of column/span/wall/
polygon records, rebuilt and re-sent every frame. Class 1's retained scene
graph (see [class1-3d-mesh-reference.md](class1-3d-mesh-reference.md) and
its [gap-filling plan](../../dev/gap_filling_plan.md)) is the forward
path: resources live GPU-side, the scene persists across frames, and the
C64's per-frame job shrinks to one fire-and-forget commit plus whichever
few objects actually moved. Class 2 is not going away — the opcodes below
keep working, the demos that use them keep working, and it stays the right
choice for genuinely 2.5D column-cast rendering (`DRAW_COLUMNS`/
`DRAW_WALLS`/`DRAW_SECTORS` have no class-1 equivalent and none is
planned). But it gets no new capability: the "Level-scale visibility"
wishlist below is superseded by class 1's gap-filling plan, not by
anything in this class.

Set `CMD_HI = 2`. This is the column/span/sprite layer a first-person
renderer needs: instead of one command per shape, you fill an array of
**16-byte records** in your own RAM and hand gpu64 the whole array in a
single dispatch. A frame of several hundred primitives costs one command
and one bulk fetch, not several thousand register writes. Why it is a class
of its own, and what it is aimed at, is in
[milestone8_raster_design.md](../../dev/milestone8_raster_design.md).

Every class 2 primitive is clipped to the **view rectangle**, which starts
as the whole surface. Records draw into the current draw page, so class 0's
`SET_DRAW_PAGE` and `PAGE_FLIP` work exactly as they do for 2D drawing, and
class 0 and class 2 can draw into the same page in the same frame.

### System — $00–$0F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $00 | `RASTER_RESET` | 0 | — | Frees every texture, view back to the whole surface, colormap back to identity, counters cleared. Also happens on a session reset, so a new program never inherits another one's texture ids. |
| $01 | `SET_VIEW` | 8 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` w, `ARG6-7` h | The clip rectangle every class 2 primitive obeys. `w` or `h` of 0, or a rectangle running off the surface, is `BAD_ARGS`. |
| $02 | `SET_COLORMAP` | 7 | `ARG0-5` blob descriptor, `ARG6` levels | Loads a `levels × 256` lighting table: a pixel of palette index `c` at light level `L` is drawn as `map[L * 256 + c]`. `levels = 0` removes it and returns to drawing raw indices. Max 64 levels; `len` must equal `levels * 256`. |
| $03 | `RASTER_STATS` | 6 | `ARG0-5` destination descriptor | Writes the 16-byte stats block (below) — what became of the **last** batch. `len` must be ≥ 16. |
| $04 | `FILL_VIEW` | 1 | `ARG0` palette index | Fills the view rectangle with one raw index. No colormap, no clipping beyond the view. **Also empties the class 2 depth buffer over the same rectangle** — a pixel just painted background has nothing in it, so this is the op a frame starts with. |
| $05 | `SET_CAMERA` | 15 | `ARG0-1` x, `ARG2-3` y, `ARG4` angle, `ARG5` flags, `ARG6-7` eye height, `ARG8-9` ceiling height, `ARG10-11` projection, `ARG12` floor colour, `ARG13` ceiling colour, `ARG14` horizon | The camera `DRAW_WALLS` projects through. Position and heights are signed 8.8 world units; angle is binary, 256 to the circle, 0 looking along +x. Projection is the distance to the projection plane in pixels as 8.8 — 160.0 (`$A000`) across a 320-wide view is a 90° field of view. Horizon is a signed pixel offset from the view's centre row. Flags bit 0 (`CAM_PAINT`) has each wall column also fill its own ceiling above and floor below in the two colours. A projection of 0, an eye height at or below 0, or a ceiling at or below the eye is `BAD_ARGS`. For `DRAW_SECTORS` the eye height is an **absolute** world height rather than a height above the floor, the ceiling height is not read at all (send the level's tallest, so the validation passes), and the two flat colours are not read either — they come from the sector. |
| $06 | `SET_SECTORS` | 8 | `ARG0-5` blob descriptor, `ARG6-7` count | Loads the sector table `DRAW_SECTORS` reads heights and flat colours from — see the sector record below. `count = 0` drops the table. Max 128 sectors; `len` must equal `count * 8`. A sector whose ceiling is at or below its own floor rejects the **whole** upload with `BAD_ARGS` and leaves the table that was already there untouched. |
| $07 | `SET_CAMERA3D` | 11 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` z, `ARG6` yaw, `ARG7` pitch, `ARG8-9` projection, `ARG10` flags | The camera `DRAW_POLYS` projects through — a free camera in three dimensions, with a real pitch rather than `SET_CAMERA`'s horizon shear. Position is signed 8.8 world units, `z` upwards. Yaw is binary, 256 to the circle, 0 looking along +x, and turns the same way `SET_CAMERA`'s angle does. Pitch is **signed** binary, $00 level, positive looking **up**; $40 is straight up and $C0 straight down, though a Quake game normally clamps well inside that. Projection is as `SET_CAMERA`: distance to the projection plane in pixels, 8.8, so 160.0 (`$A000`) across a 320-wide view is 90°. A projection of 0 is `BAD_ARGS`; there is no other validation, because there is no floor to be under. `ARG10` is reserved, write 0. This camera is entirely separate from `SET_CAMERA`'s: both can be live at once and a frame may use both. |
| $08 | `SET_LIGHT` | 10 | `ARG0` slot, `ARG1-2` x, `ARG3-4` y, `ARG5-6` z, `ARG7-8` radius, `ARG9` strength | Sets one of **8** dynamic point lights, or clears it. Position is signed 8.8 world units on the same axes as `SET_CAMERA3D`; radius is **unsigned** 8.8 and is how far the light reaches; strength is how many colormap levels it lifts a pixel at its own centre. A slot of 8 or more is `BAD_ARGS`. A radius of 0 or a strength of 0 clears the slot, and a slot is cleared by `RASTER_RESET`. Arguments are **inline** — no blob, so a light can be moved or raised on the frame it is wanted. Lights apply to `DRAW_POLYS` and `DRAW_THINGS`; see below. |

### Resources — $10–$1F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $10 | `UPLOAD_TEXTURE` | 11 | `ARG0-5` blob descriptor, `ARG6-7` w, `ARG8-9` h, `ARG10` flags; **`ID`** = texture id | Copies `w * h` bytes into gpu64's texture arena under id `1..255`. **`h` must be a power of two** (it is masked per pixel); `w` need not be, unless the texture is used by `DRAW_SPANS`, which masks both. Either dimension 0 or over 1024 is `BAD_ARGS`, as is a `w * h` over 65536; `len` must equal `w * h`. `ID = 0` is `BAD_ID`, and so is an id past 255. Re-uploading a live id replaces it. Arena exhausted is `OUT_OF_MEMORY`. |
| $11 | `FREE_TEXTURE` | 0 | **`ID`** = texture id | Releases the id. An id that is not live is `BAD_ID`. |
| $12 | `UPLOAD_VERTS` | 8 | `ARG0-5` blob descriptor, `ARG6-7` count | Loads the vertex pool `DRAW_POLYS` indexes into — see the vertex record below. `count = 0` drops the pool. Max 4096 vertices; `len` must equal `count * 8`. |
| $13 | `UPLOAD_TEXINFO` | 8 | `ARG0-5` blob descriptor, `ARG6-7` count | Loads the texture-axis table a textured polygon names — see the texinfo record below. `count = 0` drops the table. Max 255 entries; `len` must equal `count * 16`. |

The texture arena is a bump allocator: freeing an id, or re-uploading over a
live one, releases the **id** but not the bytes. Load a level's textures
once rather than swapping them per frame, and use `RASTER_RESET` — or a
session reset — to get the space back.

Textures are stored **column-major**: `texel(u, v)` is byte `u * h + v`. That
is the order a wall column reads them in, which is what makes a column one
sequential walk instead of `h` scattered reads. `ARG10` bit 0 set says your
source is row-major and asks gpu64 to transpose it on the way in — pay it
once at upload rather than per pixel.

### Batches — $20–$2F

| Op | Name | Bytes | Arguments | Effect |
|---|---|---|---|---|
| $20 | `DRAW_COLUMNS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` column records. |
| $21 | `DRAW_SPANS` | 9 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags | Draws `count` span records. |
| $23 | `DRAW_WALLS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` wall records — see below. |
| $24 | `DRAW_SECTORS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` **32-byte** sector-wall records — see below. `BAD_ARGS` if `SET_SECTORS` has not been sent. |
| $25 | `DRAW_THINGS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` 16-byte thing records — billboards in world space, depth-tested against the buffer `DRAW_SECTORS` or `DRAW_POLYS` filled. `ARG8` bit 1 (`BATCH_CAM3D`) projects the batch through `SET_CAMERA3D` instead of `SET_CAMERA`; with that bit set it is `BAD_ARGS` if `SET_CAMERA3D` has not been sent. See below. |
| $26 | `DRAW_POLYS` | 10 | `ARG0-5` blob descriptor, `ARG6-7` count, `ARG8` flags, `ARG9` key | Draws `count` 16-byte polygon records — arbitrary convex polygons in world space, through `SET_CAMERA3D`, depth-tested per pixel. See below. `BAD_ARGS` if `SET_CAMERA3D` or `UPLOAD_VERTS` has not been sent. |
| $27 | `DRAW_WORLD` | 4 | `ARG0-1` first, `ARG2-3` count, `ARG9` key | Draws polygons `first .. first+count-1` of the resident world uploaded by `UPLOAD_POLYS`. Pixel for pixel this is `DRAW_POLYS` over the same records — same clipping, texturing, lighting and depth test — but **no blob is transferred**: a frame is ten register writes however big the level is. `BAD_ARGS` if the range runs past what has been uploaded, or if `SET_CAMERA3D` or `UPLOAD_VERTS` has not been sent. `count = 0` is a no-op, not an error, so a room with nothing visible costs a command and nothing else. |
| $22 | `DRAW_SPRITE` | 15 | `ARG0-1` x, `ARG2-3` y, `ARG4-5` w, `ARG6-7` h, `ARG8` light, `ARG9` key, `ARG10-11` clipY0, `ARG12-13` clipY1, `ARG14` flags; **`ID`** = texture id | Scales one texture into the screen rectangle `x,y,w,h`, clipped to the view **and** to the inclusive row range `clipY0..clipY1`. Flags: bit0 mask on `key`, bit1 flip horizontally. `w` or `h` of 0 draws nothing (and counts as one rejected primitive); an unknown id is `BAD_ID`. |

`count = 0` is a no-op, not an error. Otherwise `len` must be exactly
`count * 16` — `count * 32` for `DRAW_SECTORS` — plus 2 more if the checksum flag is set, and no more than
65536 bytes in total — anything else is `BAD_ARGS`.

**`ARG8` bit 0 — the batch checksum.** With it set, the two bytes following
the records are a plain 16-bit little-endian sum of the record bytes. gpu64
recomputes it and answers `BAD_ARGS`, drawing nothing, if it disagrees. A
6502 cannot afford to sum thousands of bytes every frame, so use it where it
is free: on a batch that is built once and re-sent unchanged, the sum is
computed once too. For a batch rebuilt every frame, the `rejected` counter
in the stats block is the cheaper watch — see
[the note on the bus](#what-a-command-costs-the-c64).

**`ARG8` bit 1 — the 3D camera (`DRAW_THINGS` only).** With it set, the
batch projects through `SET_CAMERA3D` rather than `SET_CAMERA`, which is
what puts monsters into a level drawn with `DRAW_POLYS`. It is a property of
the batch and not of a record because a frame's things all belong to one
world: a game that has moved to the 3D camera has moved every one of them. A
frame may still send both kinds — one batch flagged and one not — and both
land in the same depth buffer. Every other opcode ignores this bit.

#### Wall record — 16 bytes

A wall segment in **world** coordinates. gpu64 does the projection, the
perspective texture mapping, the distance lighting and the depth sorting, so
the C64 sends the same unchanged bytes every frame and spends its own time
on the game. This is the one class 2 opcode that is geometry rather than
pixels, and it is where the per-column divide a 1 MHz 6502 cannot afford
went.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x1 | first endpoint, signed 8.8 world units |
| 2-3 | y1 | |
| 4-5 | x2 | second endpoint |
| 6-7 | y2 | |
| 8 | texid | texture id, or 0 for a solid colour taken from the low byte of `u1` |
| 9 | light | base light level, before distance is added |
| 10-11 | u1 | texture u at the first endpoint, signed 8.8 texels |
| 12-13 | u2 | texture u at the second endpoint |
| 14 | flags | bit0 mask on `key`; bit1 use `light` unchanged, with no darkening by distance |
| 15 | — | reserved, write 0 |

**Winding decides which side is the front.** A wall is drawn only from the
side that projects it left to right; seen from the other side it is one
rejected record and no pixels. Walls are one-sided, exactly as Doom's are,
and this is what makes a closed room cost nothing to look at from outside.

**u1 and u2 rather than a length and a scale**, because a length needs a
square root. Give the texture coordinate at each end and the interpolation
is perspective-correct with no root anywhere. Since `u` is signed 8.8 texels
it reaches 127.99, so tile a long wall as several segments — which is what a
cell-based level gives you for free, one record per cell face.

**Depth is sorted for you.** Every column is tested against a 1/z buffer that
`DRAW_WALLS` clears at the start of each batch, so records may arrive in any
order. Doom needed a BSP tree to avoid that sort; here it is a comparison.

**What is not here yet:** walls run floor to ceiling at one height for the
whole level, the height `SET_CAMERA` gives. Varying floor and ceiling
heights, two-sided walls and windows are `DRAW_SECTORS`, below; this opcode
is kept as it is because it is simpler to drive and its per-column depth
buffer is cheaper.

Distance darkening adds `z / 2` light levels to the record's `light`, `z`
being the column's distance in world units, unless bit 1 says otherwise. The
result is clamped to the colormap's last level, so a level that runs out of
levels goes as dark as it can rather than failing.

Rejections a wall record can earn, none of which fail the batch: both
endpoints behind the near plane (0.25 world units), an unknown `texid`, or a
back face. A camera that cannot be projected rejects every record in the
batch.

#### Sector record — 8 bytes

The vertical shape of a level. `DRAW_SECTORS`' wall records name two of
these by one-byte id instead of carrying four heights each, which is what
keeps the wall record down to 32 bytes.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | floorH | floor height, signed 8.8 **absolute** world units |
| 2-3 | ceilH | ceiling height; must be above the floor |
| 4 | floorCol | palette index the floor is painted in |
| 5 | ceilCol | palette index the ceiling is painted in |
| 6 | light | base light level for this sector's floor and ceiling |
| 7 | flags | bit0 (`SEC_SKY`) — the ceiling is not a surface |

**`SEC_SKY`** means the ceiling is neither painted nor given depth, and a
wall whose front sector has it draws no upper band. So a courtyard's wall
stops at the sky instead of growing a lintel across it, and whatever the
page already held shows through — put a `FILL_VIEW`, or a sky texture, there.

#### Sector-wall record — 32 bytes

`DRAW_SECTORS`' record. Everything the wall record does, plus the two things
a level with steps in it needs: floor and ceiling heights that come from the
sector table, and a **two-sided** wall that draws a band above the far
ceiling and a band below the far floor and leaves the middle see-through.

Thirty-two bytes rather than sixteen because a two-sided wall names two
sectors and three textures and none of that fits beside the geometry. It
costs nothing per frame: a level is uploaded once and its checksum computed
once.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x1 | first endpoint, signed 8.8 world units |
| 2-3 | y1 | |
| 4-5 | x2 | second endpoint |
| 6-7 | y2 | |
| 8-9 | u1 | texture u at the first endpoint, signed 8.8 texels |
| 10-11 | u2 | texture u at the second endpoint |
| 12 | frontSec | sector id on the side this record is seen from |
| 13 | backSec | sector id on the far side, or `$FF` for a solid wall |
| 14 | light | base light level for all three bands |
| 15 | flags | bit0 mask on `key`; bit1 use `light` unchanged; bit2 (`WALL_NOFLATS`) this wall's columns paint no floor and no ceiling whatever the camera says |
| 16 | texMid | texture for a solid wall, floor to ceiling; 0 for a flat colour |
| 17 | texUpper | texture for the band from this ceiling down to the far one |
| 18 | texLower | texture for the band from the far floor down to this one |
| 19 | colMid | palette index used when `texMid` is 0 |
| 20 | colUpper | likewise for `texUpper` |
| 21 | colLower | likewise for `texLower` |
| 22-31 | — | reserved, write 0 |

Winding, `u1`/`u2`, distance darkening, the near plane and the rejection
rules are all exactly as for the wall record above; a bad sector id rejects
a record the same way a bad texture id does. `v` walks each band's texture
over that band's own full height on screen, so an upper band carries the
whole texture squeezed into it.

**A two-sided wall is still one-sided per record.** The level carries the
other side as a second record with the endpoints swapped and the sectors
exchanged — which is also how the far side's bands get their own textures
without this record knowing anything about them.

**Depth is per PIXEL here, not per column.** It has to be: a window means a
column is no longer owned by one wall — the near wall owns the band above
the window and the band below it, and something further away owns the
middle. The buffer is cleared at the start of each batch, so records may
still arrive in any order, and Doom's BSP tree, which exists to produce that
order, is still not needed.

**Floors and ceilings are drawn per column, by the walls.** With
`CAM_PAINT` set, each wall column fills from the view's top down to where
its front sector's ceiling cuts the column, and from where its floor cuts it
down to the view's bottom, in that sector's colours. Every such row has its
own distance, so a floor depth-tests correctly against a wall standing on
it, and a two-sided wall also paints the far sector's flats inside its
window — nothing else would, because a corridor's own side walls seen
end-on cover almost no columns.

One correction is applied to that: **the depth a flat writes is never nearer
than the wall whose column painted it.** A plane is infinite and a sector's
floor is not, so without the clamp a low ceiling two rooms away wins the
rows above the wall that hides it. It is the cheap stand-in for Doom's
visplane clipping, which needs a front-to-back order this design does not
have. Lighting still uses the row's true distance, so the clamp is
invisible. One consequence: it only reaches one sector through a portal — a
sector two portals away can leave a hole.

**What is not here yet:** flat (floor and ceiling) textures, sloped floors,
and flats seen through two portals in a row. Sprites that depth-test against
this buffer are `DRAW_THINGS`, below.

#### Thing record — 16 bytes

A billboard at a **world** position, projected by the batch's camera —
`SET_CAMERA` by default, or `SET_CAMERA3D` if `ARG8` bit 1 is set — and
depth-tested per pixel against the buffer `DRAW_SECTORS` or `DRAW_POLYS`
filled, which is what lets a monster stand behind a wall.

`DRAW_SPRITE` cannot do this. It takes a screen rectangle, so the C64 has to
do the projection, and it has no argument room left for a depth. Keep
`DRAW_SPRITE` for what is genuinely at a screen position and must never be
occluded: the weapon in the player's hands, the status bar, a full-screen
flash.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x | world position, signed 8.8 |
| 2-3 | y | |
| 4-5 | base | **absolute** height of the bottom of the sprite, signed 8.8 — the same measure the sector table's floor heights use, so a thing standing on the floor of sector *n* carries that sector's floor height |
| 6-7 | h | height in world units, unsigned 8.8 |
| 8-9 | w | width in world units, unsigned 8.8 |
| 10 | texture id | must name a live texture; 0 is a rejected record. With `THING_DIRECTIONAL`, the first of **eight** consecutive ids |
| 11 | light | base light level, darkened by distance unless `THING_FLATLIT` |
| 12 | flags | bit0 mask on the batch `key`; bit1 flip horizontally; bit2 (`THING_NODEPTH`) ignore the depth buffer entirely; bit3 (`THING_FLATLIT`) use `light` unchanged; bit4 (`THING_DIRECTIONAL`) pick one of eight views from byte 13 |
| 13 | facing | which way the thing itself is pointing, in the camera's 256-to-the-circle units. Read only with `THING_DIRECTIONAL`; write 0 otherwise |
| 14-15 | — | reserved, write 0 |

**`THING_DIRECTIONAL` — eight views.** A monster that looks the same from
behind as it does from the front is the one thing that gives a billboard
away. Set bit 4 and byte 10 becomes the first of eight consecutive texture
ids: the thing seen from the front, then every 45° round it the way the
angle units run. Byte 13 says which way the thing is facing, in the same
units as `SET_CAMERA`'s `ang` and `SET_CAMERA3D`'s `yaw`, and gpu64 works
out from where the camera is standing which of the eight you are looking at.
A thing facing straight at the camera draws id + 0; one walking away draws
id + 4.

The 6502 sends the facing it already keeps for the monster's AI and nothing
else — no arctangent, no view index, no per-frame table. The camera used is
the batch's: under `BATCH_CAM3D` it is `SET_CAMERA3D`'s position that
decides the view. **All eight ids must be live**; a view whose texture is
missing is a rejected record, so upload the whole set together. Turning the
camera on the spot does not change the view — only moving does, which is
correct, and is what makes a strafing player see a monster's flank swing
round.

The card always faces the camera, so `w` is measured straight across the
view and there is no rotation to send. A thing is `w` wide and `h` tall in
world units at any distance; the screen rectangle is whatever the projection
makes of that, centred on the column the world position projects to.

Under the 3D camera the card stays **upright on the screen** — it does not
tilt with pitch, which is what a billboard is for — but its two ends are
projected separately, so looking up at a tall thing standing close makes it
taller on screen and pushes it down the view, as the walls beside it do.
`SET_CAMERA`'s horizon offset is not applied to a `BATCH_CAM3D` batch:
`SET_CAMERA3D`'s pitch is the whole of the vertical aim. Nothing else in the
record changes meaning — `base` is still the absolute world height of the
thing's feet, now measured on `SET_CAMERA3D`'s `z` axis.

**Depth is written as well as tested**, at drawn pixels only — so a masked
texel is a hole in the depth too, and a batch of things is order-independent
exactly as a batch of walls is. `THING_NODEPTH` opts out of both, which is
how a muzzle flash gets painted over the world; things drawn that way *do*
depend on the order they arrive in.

**Send this batch after the level's geometry for the same frame** —
`DRAW_SECTORS`, or `DRAW_POLYS` for a `BATCH_CAM3D` batch. The depth
buffer is shared and persists: it is emptied by `FILL_VIEW`, by
`DRAW_SECTORS` (over the view, at the start of each batch) and by
`RASTER_RESET`, and by nothing else. Things sent before the level's walls
would be occluded by whatever the buffer still held.

A record with `w` or `h` of 0, an unknown or zero texture id, or a position
behind the near plane is **rejected**; one that is well formed and clips
away entirely, or is too far off to cover a pixel, is **accepted** and draws
nothing. That distinction is the whole value of the counters: rejected means
the record was wrong, not that it missed.

#### Vertex record — 8 bytes

The pool `UPLOAD_VERTS` loads and polygon records index into. A vertex is
shared by every face that touches it, so a level is uploaded once and the
per-frame batch is nothing but indices.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | x | world position, signed 8.8 |
| 2-3 | y | |
| 4-5 | z | upwards |
| 6-7 | — | reserved, write 0 |

The two pad bytes are there so a vertex is 8 bytes and the 6502 reaches
index *n* with three shifts instead of a multiply.

Coordinates are 8.8, so the world is ±128 units across and no vertex may be
more than 127.99 units from the origin — the same far plane the rest of the
3D pipeline works in. Pick a scale where a room is a few units wide rather
than a few hundred.

#### Texinfo record — 16 bytes

Where a texture's texels land in the world, as a pair of axes — the same
idea Quake's `texinfo` lump carries. A face names one entry and gpu64
derives its own texture coordinates from the vertex positions, so a wall and
the floor it meets stay aligned however the face is split.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | sx | s axis, signed 8.8 |
| 2-3 | sy | |
| 4-5 | sz | |
| 6-7 | sOff | s offset in texels, signed 8.8 |
| 8-9 | tx | t axis, signed 8.8 |
| 10-11 | ty | |
| 12-13 | tz | |
| 14-15 | tOff | t offset in texels, signed 8.8 |

For a vertex at world position `P`:

    s = ((P · sAxis) >> 8) + sOff
    t = ((P · tAxis) >> 8) + tOff

both in 8.8 texels, and both wrap by masking, so an axis of length 1.0
(`$0100`) gives one texel per world unit and `$0400` gives four. The axes
need not be perpendicular and need not lie in the face's plane — only the
values at the vertices matter, and everything between them is interpolated
in perspective.

Entries are numbered **from 1**: a polygon record's `texinfo` byte of 1 is
the first record you uploaded. 0 means "none", which is only legal on an
untextured face.

#### Polygon record — 16 bytes

A convex polygon in world space, drawn through `SET_CAMERA3D` and
depth-tested per pixel. This is the Quake-shaped layer: arbitrary planes at
arbitrary angles, rather than `DRAW_SECTORS`' vertical walls and horizontal
flats.

| Bytes | Field | Meaning |
|---|---|---|
| 0-1 | first | index of the polygon's first vertex in the pool |
| 2 | nVerts | 3 to 16, taken consecutively from `first` |
| 3 | texinfo | 1-based texinfo index; ignored when `tex` is 0 |
| 4 | tex | texture id, or **0 for a flat-shaded face** drawn in `col` |
| 5 | col | palette index used when `tex` is 0 |
| 6 | light | base light level, darkened by distance unless `POLY_FLATLIT` |
| 7 | flags | bit0 (`POLY_MASKED`) skip texels equal to the batch `key`; bit1 (`POLY_FLATLIT`) use `light` unchanged; bit2 (`POLY_TWOSIDED`) no backface cull |
| 8-15 | — | reserved, write 0 |

The vertices must be **convex** and listed so that the face reads
**clockwise on screen** when you are looking at its front — the same
convention `DRAW_WALLS` states as "drawn only from the side that projects it
left to right". A face seen from behind is culled and counts as rejected,
which is what makes a sealed room cost only the polygons facing you; set
`POLY_TWOSIDED` for a face that has no back, such as a grate or a banner.

A textured face needs **both dimensions a power of two** — polys mask `u`
and `v` alike, so a texture that is legal for `DRAW_COLUMNS` may still be
rejected here.

**Depth is written as well as tested**, at drawn pixels only, into the same
buffer `DRAW_SECTORS` and `DRAW_THINGS` use. That means a batch of polygons
is order-independent, a masked texel is a hole in the depth too, and a Quake
level and a Doom level can be drawn into one frame if you want them to be.

**`DRAW_POLYS` does not clear the depth buffer.** `DRAW_SECTORS` clears it
because a Doom frame is one batch; a Quake frame is usually several — the
world, then the moving brushes, then the sprites — so the clear belongs to
`FILL_VIEW`, which is the op that starts the frame. Send `FILL_VIEW` first
or you are depth-testing against last frame.

A record is **rejected** if `nVerts` is outside 3..16, if `first + nVerts`
runs past the uploaded pool, if a non-zero `tex` names a dead texture or one
whose width is not a power of two, if a non-zero `tex` carries a `texinfo`
of 0 or past the uploaded table, if it is culled as backfacing, or if it
lies entirely behind the near plane. It is **accepted** if it is well formed
and merely clips away off the sides of the view, or is edge-on and covers no
pixels. Rejected means the record was wrong; accepted-and-invisible means it
missed.

#### Dynamic lights

`SET_LIGHT` gives the class 2 layer eight point lights on top of the
colormap. They do not replace a record's `light` field — they *brighten*
what it and distance darkening already produced:

```
level = litLevel( record light, distance )        as before
for each live light within its radius:
    d2   = squared distance from the pixel to the light, world units
    sub += strength * ( radius² - d2 ) / radius²  truncated per light
level = max( 0, level - sub )
colour = colormap[ level * 256 + texel ]
```

Falloff is linear in the *square* of the distance, which costs no square
root and gives a soft edge: a light is at full strength only at its exact
centre and reaches 0 exactly at its radius, so a light going out of range
never pops. Each light truncates to a whole level **before** it is added, so
two strength-4 lights at one point brighten by 8 levels at the centre but
slightly less than one strength-8 light elsewhere. Lower level is brighter —
lights subtract.

Where they are evaluated differs by opcode, and this is the cost/quality
trade:

- **`DRAW_POLYS`** evaluates every live light **per pixel**, at that pixel's
  true world position recovered from the perspective interpolation. A light
  on a wall is a round pool that follows the geometry, and it works on a
  `POLY_FLATLIT` face too.
- **`DRAW_THINGS`** evaluates them **once per record**, at the billboard's
  centre. A sprite is a flat cut-out, so a gradient across it would be wrong
  anyway; what this gives is a monster that lights up as it walks into a
  torch.
- **The Doom layer — `DRAW_SECTORS`, `DRAW_WALLS`, `DRAW_COLUMNS` and
  `DRAW_SPANS` — ignores dynamic lights entirely.** Those opcodes take their
  light per column or per span by design and their cost model depends on it.
  Use the record's `light` field there.

Eight slots are always scanned when any light is live, and a whole frame
with no light live costs one test. There is no per-light distance culling:
if you have more lights in a level than slots, pick the ones near the camera
yourself and re-send the slots each frame — that is ten register writes per
light and no upload, which is what makes it affordable.

#### Column record — 16 bytes

Doom's `R_DrawColumn`: one textured vertical strip.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | x — screen column, unsigned |
| 2 | 2 | y0 — first row, **signed** |
| 4 | 2 | y1 — last row, inclusive, **signed** |
| 6 | 1 | texture id, or 0 for solid colour |
| 7 | 1 | light level |
| 8 | 2 | u — texture column (wrapped modulo `w` once per record); with texid 0, the low byte is the palette index |
| 10 | 2 | v — texture row at `y0`, **8.8 signed** |
| 12 | 2 | dv — v step per screen row, **8.8 signed** |
| 14 | 1 | flags: bit0 = skip source texels equal to the batch `key` (the masked/see-through column) |
| 15 | 1 | reserved, write 0 |

`y0` and `y1` are signed and are *meant* to run off the view: a wall close
enough is thousands of pixels tall. gpu64 clips them **and advances `v`
across the rows it discarded**, so a texture does not slide as you walk into
a wall. A record with `y1 < y0`, an `x` outside the view, or an unknown
texture id is **rejected** — counted, not fatal, and the rest of the batch
still draws.

#### Span record — 16 bytes

Doom's `R_DrawSpan`: one horizontal strip, with `u` and `v` both stepping.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | y — screen row, **signed** |
| 2 | 2 | x0 — first column, **signed** |
| 4 | 2 | x1 — last column, inclusive, **signed** |
| 6 | 1 | texture id, or 0 for solid colour |
| 7 | 1 | light level |
| 8 | 2 | u — **8.8 signed** |
| 10 | 2 | v — **8.8 signed** |
| 12 | 2 | du — u step per screen column, **8.8 signed** |
| 14 | 2 | dv — v step per screen column, **8.8 signed** |

Both coordinates are masked every pixel, so a **textured** span needs a
texture whose width *and* height are powers of two; one whose width is not
rejects the record. Solid-colour spans (texid 0) have no such restriction
and are the cheap way to lay down a lit floor and ceiling gradient — one
record per row.

#### Stats block

`RASTER_STATS` writes 16 bytes describing the **last** batch dispatched:

| Offset | Size | Contents |
|---|---|---|
| 0 | 2 | `"R2"` |
| 2 | 2 | accepted — records that were drawn or clipped away |
| 4 | 2 | rejected — records that were malformed or named an unknown texture |
| 6 | 2 | requested — the `count` the command asked for |
| 8 | 4 | pixels written |
| 12 | 2 | live textures |
| 14 | 2 | free arena space, KB |

`requested` against `accepted + rejected` is the length readback the bus
asks for, and `rejected` is the counter that moves if a batch arrives
damaged. Reading it back costs one dispatch and a 16-byte write, which is
cheap enough to do every frame.

### Level-scale visibility — superseded by class 1

**This subsection is frozen, not actionable.** It records the gap between
class 2's per-frame batch model and what a level-scale engine needs, as it
stood before the 2026-08-28 decision to make class 1's retained scene graph
gpu64's forward path (see the deprecation note at the top of this class,
and [dev/gap_filling_plan.md](../../dev/gap_filling_plan.md)). A
persistent world, GPU-side visibility determination and a per-frame cost
that doesn't scale with level size are exactly what class 1's scene graph
and autonomous render loop are designed to give every class-1 program by
construction, not as a class-2-specific extension — so none of the items
below are planned to be built here. Kept for the historical record and
because `UPLOAD_POLYS`/`DRAW_WORLD` (marked `[IMPLEMENTED]` below) remain
live, working class-2 opcodes.

[REQUIREMENT NEEDS DETAIL DESIGN] All per-frame visibility determination —
deciding which polygons/faces are worth drawing for the current camera —
must run in gpu64 firmware on the RPi side, never on the 6502; the C64's
per-frame job is limited to updating the camera and game state, not walking
any spatial structure.

[IMPLEMENTED] A persistent, whole-level polygon table — `UPLOAD_POLYS`
($14) alongside `UPLOAD_VERTS`/`UPLOAD_TEXINFO`, drawn by range with
`DRAW_WORLD` ($27). The level's faces are uploaded once and a frame names
a range of them, so no face record crosses the bus after start-up. The
range is still chosen by the C64, which is what the requirement below
remains open about.

[REQUIREMENT NEEDS DETAIL DESIGN] A precomputed, offline-baked visibility
structure (portal graph and/or per-cell PVS bitset, built at asset-conversion
time on the PC, the same place a BSP-to-gpu64 converter would run) needs an
upload opcode of its own, so gpu64 has something cheap to walk instead of
testing every face in the level against the frustum every frame.

[REQUIREMENT NEEDS DETAIL DESIGN] gpu64 firmware needs to determine which
cell/leaf the camera currently occupies from `SET_CAMERA3D`'s position each
frame, so the C64 never has to track or send its own location within the
level.

[REQUIREMENT NEEDS DETAIL DESIGN] Full frustum culling (not just today's
per-face backface and near-plane rejection) needs to run inside gpu64 against
the persistent polygon table, scoped by the PVS/portal set for the camera's
current cell.

[REQUIREMENT NEEDS DETAIL DESIGN] A new batch-less draw opcode (no blob, no
per-frame record array) is needed so a level-scale frame costs the C64 one
fixed-size command — camera already set via `SET_CAMERA3D` — rather than a
record array sized by how much of the level is visible.

[REQUIREMENT NEEDS DETAIL DESIGN] A memory and frame-time budget for the
persistent polygon table and visibility structure on the RPi side needs to be
worked out against the 3A+'s actual headroom before any of the above is
implemented.

### Class 2 error codes

Class 2 adds three codes to the table below: `$08 OUT_OF_MEMORY` (the
texture arena is full), `$0A BAD_ID` (a texture id of 0, past 255, or not
live), and — from a batch op on a build without the class compiled in —
`$02 BAD_CLASS`.

## vblank

Two ways to sync to the display, both driven by the same signal:

- **Polling**: read `STATUS` bit2. Set at each vblank, cleared by
  `VBLANK_ACK`. No IRQ handler needed — usable from BASIC.
- **Interrupt**: `VBLANK_ARM` with `ARG0 = 1` raises the cartridge IRQ line
  once per vblank. It stays armed and re-arms itself, like a raster IRQ —
  acknowledge with `VBLANK_ACK` on handler entry. `VBLANK_ARM` with
  `ARG0 = 0` returns to polling.

`VBLANK_ACK` is also what releases the IRQ line, so a handler that forgets
it gets exactly one interrupt.

### What the vblank signal actually is

gpu64 does not ask the display where the raster is — doing that means a
mailbox call to the VideoCore, which blocks for up to a frame and would
stall the bus-watch loop. Instead the frame period is **measured once at
boot** and extrapolated from a free-running microsecond timer. Both clocks
come off the same crystal, so the extrapolation holds, but it is not exact:

- Every vblank lands a small **constant offset** after the display's true
  one, because the boot measurement carries the mailbox's own latency. It is
  consistent frame to frame, which is what tear-free flipping needs.
- It **drifts** over long runs — on the order of a millisecond per several
  minutes. A program that animates for a long time should call
  `VBLANK_SYNC` occasionally (once a minute is plenty) to re-pin it.
  `VBLANK_ARM` does this for you when you arm, so a program that arms at
  start-up begins accurate.

If the boot measurement failed, there is no signal at all: `GET_INFO`
reports a frame period of 0, `STATUS` bit2 never sets, and `VBLANK_ARM(1)`,
`PAGE_FLIP(1)` and `VBLANK_SYNC` all return `UNSUPPORTED`. Immediate
`PAGE_FLIP` still works, so a program can fall back to flipping untimed.

### Tear-free animation

The intended shape, and what
[`gpu64_vblank_demo.a`](../../tests/gpu64_vblank_demo.a) does:

1. `SET_DRAW_PAGE` 1 once, at the start. Every `PAGE_FLIP` after that
   rotates the pages for you, so the draw page is always one the display
   hardware is provably not scanning out.
2. Wait for `STATUS` bit2, then `VBLANK_ACK`.
3. Draw the frame.
4. `PAGE_FLIP` with `ARG0 = 1`.
5. Wait for `STATUS` bit0 to clear — the flip has landed.

Step 5 is the only place gpu64 makes the C64 wait on something other than
its own work, and it is a spin on a register read, not a halt: the C64 is
free to do anything else instead.

## Error codes

`ERRCODE` is written by **every** dispatch, including successful ones, so a
stale value can never be mistaken for a fresh failure. `STATUS` bit1 is
simply `ERRCODE != OK`.

| Value | Name | Meaning |
|---|---|---|
| $00 | `OK` | Success. |
| $01 | `BAD_OPCODE` | No such opcode in the current class. |
| $02 | `BAD_CLASS` | `CMD_HI` selects a class this firmware doesn't implement. |
| $03 | `OUT_OF_RANGE` | A blob descriptor's `addr + len` runs past the end of its space, or `space` isn't 0 or 1. |
| $04 | `BAD_ARGS` | Arguments inconsistent for an otherwise valid opcode: `len` not matching `w * h` or `count * 3`, a page or flag byte outside its defined values, a palette range past 256, a matrix dimension of 0, a `READ_RECT` rectangle outside the page. |
| $05 | `SINGULAR` | `MAT_INVERSE` on a matrix with no inverse. Nothing was written to the destination. |
| $06 | `UNSUPPORTED` | The feature is not available on this hardware right now. In practice this means the frame clock did not calibrate at boot, which takes every vblank feature with it. |
| $07 | `BUSY` | A vblank-deferred `PAGE_FLIP` is still waiting for its frame boundary. The pending flip is untouched; poll `STATUS` bit0 and retry. |
| $08 | `OUT_OF_MEMORY` | A resource upload had nowhere to go — in class 2, the texture arena is full. Free something and retry. |
| $0A | `BAD_ID` | A resource id that is 0, past the end of its table, or not currently live. |
| $0C | `WORKER_TIMEOUT` | Core 0 gave up waiting on class 1's second core in a generous worst-case window — either a render op (`CLEAR_VIEWPORT`/`DRAW_MESH`/`DRAW_NODE`) queued against a full ring, or any other class 1 opcode waiting for a previously queued render to finish first. Designed to be unreachable in normal operation — see [class1-3d-mesh-reference.md](class1-3d-mesh-reference.md#deferred-result). |

A failed dispatch does nothing: no drawing, no data fetch, no state change.

## Examples

Eight complete, commented programs are in [`examples/`](../../examples/) — one per area of the API, buildable and viewable on a PC with `tools/examples.sh`. See [examples/README.md](../../examples/README.md) for what each one shows. The fragments below are the same idioms in isolation.

Clear the screen to blue (index 6), then draw a filled white box:

```asm
CMD_LO  = $df51
ARG0    = $df56

        lda #6
        sta ARG0
        lda #$10        ; CLEAR
        sta CMD_LO

        lda #<100
        sta ARG0
        lda #>100
        sta ARG0+1      ; x = 100
        lda #<50
        sta ARG0+2
        lda #>50
        sta ARG0+3      ; y = 50
        lda #<80
        sta ARG0+4
        lda #>80
        sta ARG0+5      ; w = 80
        lda #<40
        sta ARG0+6
        lda #>40
        sta ARG0+7      ; h = 40
        lda #1
        sta ARG0+8      ; colour = white
        lda #$23        ; RECT_FILL
        sta CMD_LO
```

Blit a 32x32 sprite from C64 RAM at $C000 into the back page, then flip on
vblank:

```asm
        lda #0
        sta ARG0        ; space = C64 RAM
        lda #$00
        sta ARG0+1
        lda #$c0
        sta ARG0+2
        lda #$00
        sta ARG0+3      ; addr = $00c000
        lda #<1024
        sta ARG0+4
        lda #>1024
        sta ARG0+5      ; len = 32*32
        ; dstX, dstY, w, h in ARG6..ARG13 ...
        lda #$40        ; BLIT
        sta CMD_LO

        lda #1
        sta ARG0
        lda #$05        ; PAGE_FLIP at next vblank
        sta CMD_LO

wait    lda STATUS      ; bit0 clears when the flip lands
        and #$01
        bne wait
```

Multiply a 4x4 matrix at $2000 by a 4x1 vector at $2040, both 8.8 fixed
point, leaving the transformed vector at $2080 — all in C64 RAM:

```asm
        lda #4
        sta ARG0        ; m = 4
        sta ARG0+1      ; k = 4
        lda #1
        sta ARG0+2      ; n = 1

        lda #0
        sta ARG0+3      ; A: space = C64 RAM
        lda #$00
        sta ARG0+4
        lda #$20
        sta ARG0+5
        lda #$00
        sta ARG0+6      ; A: addr = $002000

        lda #0
        sta ARG0+7      ; B: space = C64 RAM
        lda #$40
        sta ARG0+8
        lda #$20
        sta ARG0+9
        lda #$00
        sta ARG0+10     ; B: addr = $002040

        lda #0
        sta ARG0+11     ; C: space = C64 RAM
        lda #$80
        sta ARG0+12
        lda #$20
        sta ARG0+13
        lda #$00
        sta ARG0+14     ; C: addr = $002080

        lda #$80        ; MAT_MUL, 8.8 fixed point
        sta CMD_LO      ; result is in $2080..$2087 when this returns
```

The float32 version is the same code with `#$90` in the last `lda` — and
16 bytes of result instead of 8.

# Error codes

`ERRCODE` (`$DF53`) is written by **every** dispatch, success or failure —
there is no code path that leaves it stale. `STATUS` bit1 is exactly
`ERRCODE != OK`, so a caller that only wants pass/fail can check one bit
instead of decoding the value.

| Code | Name | Meaning |
|---|---|---|
| $00 | `OK` | Command completed normally. |
| $01 | `BAD_OPCODE` | `CMD_LO` named an opcode not defined for the current `CMD_HI` class. |
| $02 | `BAD_CLASS` | `CMD_HI` named a class not compiled into this firmware build (see the info block's implemented-classes bitmap), or — for a batch op — a class-2 record referenced a batch kind the build doesn't support. |
| $03 | `OUT_OF_RANGE` | An address, length, index, or dimension argument would read or write outside the space it names. Since `SET_DMA_WINDOW` ($0C), this also covers a writeback whose destination falls outside the C64 window you declared. |
| $04 | `BAD_ARGS` | An argument combination is structurally invalid independent of range — e.g. a zero matrix dimension. |
| $05 | `SINGULAR` | `MAT_INVERSE` was asked to invert a matrix with no inverse. |
| $06 | `UNSUPPORTED` | The opcode is defined but not available right now — e.g. a vblank opcode when the boot-time frame-period measurement failed, or an argument selecting a mode this build does not implement (`LOOP_START` with `ARG0` = 1, free-running). A class-1 program stuck answering `$06` forever is almost always `SCENE_COMMIT` refusing because the flip subsystem never calibrated. |
| $07 | `BUSY` | A previous asynchronous operation on the same resource hasn't finished. |
| $08 | `OUT_OF_MEMORY` | A resource table (textures, meshes, nodes, ...) is full. |
| $09 | `QUEUE_FULL` | The core-1 render ring had no room and the opcode had no useful way to wait. The three render ops wait rather than reject, so in practice this reaches the other class-1 opcodes only. |
| $0A | `BAD_ID` | A resource id argument doesn't name a live resource. |
| $0B | `NO_CAMERA` | Class-1-specific: the operation needs an active camera and there isn't one. `LOOP_START` checks this once, at the call; `SET_ACTIVE_CAMERA` is how you fix it. Destroying the active camera node clears it, so this can appear long after setup. |
| $0C | `WORKER_TIMEOUT` | Class-1-specific: core 0 gave up waiting on core 1's render worker after a generous worst-case window. Designed to be unreachable in normal operation — if you see it, something is genuinely stuck, not just slow. |

## A value above `$0C` is not an error code

The table above stops at `$0C`. If a read of `ERRCODE` — or of `STATUS`,
`RESULT` or `SEQACK` — answers anything **above `$0C`**, no command failed and
the firmware did not write that value. It is what the C64 sees when its read
of a gpu64 register was not serviced:

- the polling loop never sampled the access, so nothing drove the data bus
  and the C64 latched whatever was there; or
- the loop sampled the access at a *wrong* address (A4-A7 are multiplexed on
  the cartridge port, so a mis-sample can only set those bits — `$DF53`
  degrades to `$73` or `$D3`, both still at or above the gpu64 window), found no readable register there, and answered `$FF` deliberately;
  or
- the firmware drove the data late enough that the C64 latched the previous
  cycle's value instead.

**Test the range, not the value `$FF`.** An unserviced read returns the last
value on the bus, which is *usually* all-ones and frequently is not: real runs
have reported `$20`, `$3C`, `$A3`, `$D1` and `$D3`. Code that special-cases
`$FF` passes every one of those through as though it were an answer.

### The three rules

**1. Any value `>= $0D` means "ask again".** Never map it onto an error name.
A program that reports `$D3` as an unknown error is reporting a dropped read.

**2. Retry more than once, and bound it.** `ERRCODE` is a plain latch: the
command finished long before your read returned, the value is settled, and
there is no deadline on reading it. Eight attempts is a sensible bound and
costs a few microseconds on a path taken roughly once in ten thousand
commands. One retry is **not** enough — two consecutive failed reads of
`LOOP_START`'s answer is a thing that has actually happened on the bench.

**3. If the retries run out, that is still not a failure.** This is the rule
that matters most, and the one whose absence cost a whole test session.
*"I could not read the answer"* is not *"the command failed"*: the command was
dispatched, `SEQACK` confirms that independently, and all that was lost is the
reply. So default to `OK`, keep a counter so the event stays visible, and
carry on. In particular:

- never let an unreadable answer abort start-up or end your main loop;
- re-send the state rather than reasoning about it — every transform and
  state opcode is idempotent, so a periodic refresh repairs anything that
  really did go wrong (see below);
- reserve hard failure for codes you actually read in range.

The same range rule applies to `STATUS`: an unserviced read has every bit set,
including busy and error, so a poll loop that does not filter it can hang or
declare a spurious failure. Filter first, then test the bit you wanted.

### Bounded-lifetime state

Because a dropped `ARG` or `ID` write is **not** covered by `SEQACK` — that
check covers `SEQ` and `CMD_LO` only — a lost argument can leave a retained
node holding a wrong transform indefinitely. The defence is not detection but
expiry: re-send each node's state on a rotating schedule, so any single lost
write is corrected within a bounded number of frames instead of lasting the
session. One node's position and orientation per frame is about twenty
register writes and heals an N-node scene in N frames.

Firmware-side counters for this live in the health block's bytes 48-63 —
see [class0-2d-reference.md](class0-2d-reference.md).

See also: [class2-raster-reference.md](class2-raster-reference.md) for the
handful of class-2-specific codes and situations layered on top of this
shared table.

# Bus access design: why not full sniffing, and the thin-client decision

This records the reasoning behind how gpu64 watches the C64 bus for
milestone 3 ("display sniffed current C64 screen buffer") and beyond, since
the obvious approach (continuously sniff every bus cycle) turned out not to
fit this hardware, and a deliberate scope decision was made instead of
guessing at a cycle-timing-critical fix with no hardware available to verify
it against.

## Why "just read the full address every cycle" doesn't work here

RAD's cartridge PCB does not expose all 16 C64 address lines to dedicated
RPi GPIOs. The RPi doesn't have enough usable GPIO pins for 16 address +
8 data + the various control lines simultaneously, so the board multiplexes
the address bus onto the *same physical pins* as the 8-bit data bus, gated
by three control lines (`LATCH_A0`, `LATCH_A8`, `LATCH_A_OE` in
[gpio_defs.h](../Source/Firmware/gpio_defs.h)). Reading a full 16-bit
address costs a sequence of GPIO writes and reads (select low byte, read,
select high byte, read, disable), not a single GPIO read.

Everywhere this repo currently reads a full address, it does so while the
C64 CPU is already halted via `DMA_OUT` (see the various macros in
[lowlevel_dma.h](../Source/Firmware/lowlevel_dma.h)) -- i.e. RAD is single-
stepping a frozen CPU on its own schedule, so the multi-step latch read has
as much wall-clock time as it needs without missing anything. The one thing
that *is* read cheaply on every cycle, IO2/IO1 decode (`IO2_ACCESS`,
`ADDRESS_FFxx`), works because that decode is done by hardware external to
the RPi (a mux/latch already wired to present just an 8-bit offset,
`IO_ADDRESS` in lowlevel_dma.h), not because RAD reads a full address
cheaply.

Consequence: catching every write to arbitrary RAM (e.g. screen RAM at
$0400, color RAM at $D800) while the C64 CPU runs at its own, un-halted
pace would need the full 16-bit address on every single cycle -- and the
latch-read sequence for that is very unlikely to fit inside a ~1us (1MHz)
or ~0.5us (2MHz, C128 fast mode) cycle window on an RPi 3A+.

**This is a hardware/PCB constraint, not a RAD software inefficiency.**
Dropping RAD's code and writing an independent gpu64-only firmware from
scratch would not remove it -- the same multiplexed latch chips and pin
count would still be there. Software-only, two paths existed:

1. Extend RAD's existing DMA-held single-step technique (already proven for
   milestone 2 and for REU/GeoRAM emulation) to run for the C64's *entire*
   session instead of brief windows, paying the slow latch-read cost on
   RAD's own paced schedule rather than the CPU's. This works, but means
   gpu64 becoming a permanent bus arbiter sitting between the CPU and every
   single memory access, for as long as the C64 is powered -- effectively a
   full software 6502 bus passthrough.
2. Change the PCB: add combinational address-range decode (extra
   latch/comparator logic, or a GAL/CPLD if the board has a spare one) so a
   hit on $0400-$07E7 or $D800-$DBE7 shows up as a single ready-to-read GPIO
   flag, the same way IO2 decode already does for the cartridge's own I/O
   window. This would let a free-running (non-DMA-held) sniff loop work,
   but it's a hardware change, not firmware.

## Decision: thin client, C64 always in charge

Option 1 above was rejected: **gpu64 must not intercept the C64 bus
completely.** The C64 stays in charge at all times; gpu64 does not become a
permanent bus arbiter sitting in the critical path of every memory access.
Option 2 (a PCB change) is not being pursued right now either -- no reason
to redesign hardware before the software-only approach below is proven
insufficient.

Instead, replicating full VIC-II behavior (every graphics mode, sprites,
raster effects, timing-perfect scrolling) is explicitly **not** the goal for
the default/no-API case. The bar is much lower: enough to show something
useful on the HDMI output (e.g. so a directory listing or other basic
C64-side activity is visible while looking at the HDMI screen instead of the
C64's own video out), not to run arbitrary existing demos faithfully. That
lower bar changes what's needed from "sniff every cycle, forever" to
"occasionally take a snapshot":

- **Default state (no gpu64 API in use):** gpu64 periodically does a brief,
  self-contained DMA-held read -- grab the bus for a short burst (same
  technique already used for menu injection and REU/GeoRAM emulation),
  read the current VIC-II register block (~$D000-$D02E, to know the
  current display mode and bank) plus whatever screen RAM / color RAM /
  character set buffers are currently in effect, release DMA, and update
  the HDMI framebuffer from that snapshot. This is a poll, not continuous
  sniffing -- the C64 briefly pauses for each snapshot (no different in
  kind from the pauses REU DMA transfers already cause) and otherwise runs
  completely on its own. Good enough for basic legibility (e.g. reading a
  directory listing on the HDMI screen), explicitly not aiming for
  cycle-perfect VIC-II replication or demo compatibility.
- **gpu64 API mode (the 320x200x256 framebuffer, once a C64 program writes
  to the trigger/command registers in $DF0B-$DFFF):** all snapshot polling
  stops. The C64 program is explicitly driving gpu64 through the command
  API at that point, so there's nothing to sniff -- the two modes are
  mutually exclusive at any given moment, matching the existing
  framebuffer-mode/VIC-replication-mode split already described in
  [project_description.md](project_description.md).

This supersedes the "extend the DMA hold to the whole session" framing that
was floated during design discussion -- that approach is rejected per the
above. Milestone 3's implementation should be scoped as periodic snapshot
polling, not a permanent bus takeover.

## Status: implemented in milestone 3

This design was implemented and confirmed on real hardware -- see milestone 3
in [progress_tracker.md](progress_tracker.md) for the implementation and its
scope decisions. The open questions this section originally listed resolved as
follows:

- **Polling rate:** a fixed iteration count in `reuUsingPolling()`'s per-cycle
  loop, tuned to roughly four snapshots per second. Responsive enough to
  follow typing and directory listings; the adaptive
  "only poll when nothing else is contending" idea remains parked.
- **One burst or several:** one burst is enough for text mode -- 1000 bytes of
  screen RAM, 1000 bytes of colour RAM and two VIC registers, read and
  released in a single DMA hold. Bitmap and multicolour modes may change this.
- **VIC bank / charset detection:** not implemented. The mirror assumes the
  power-on defaults ($0400 screen, $D800 colour). A program that relocates its
  screen before engaging gpu64's API renders wrong. This is the main
  correctness gap left in the default-state mirror.

One practical lesson worth keeping with the design: work done between grabbing
and releasing the bus is not the only cost. Rendering and logging after the
burst also run with the C64 free-running and `reuUsingPolling()` not servicing
any bus access, so they belong in the same budget. Cleaning the whole
framebuffer per snapshot (~3.6MB at 1824x984) was measurably wasteful and is
now narrowed to the rows actually drawn.
## Open questions for whoever implements milestone 3 under this design

- How often to poll in the default state -- frequent enough to feel
  responsive (e.g. scrolling a directory listing) without visibly stalling
  the C64 or spending too much of each DMA burst's budget on the latch-read
  overhead described above.
- Whether one DMA burst can read enough (VIC state + up to 1000 bytes
  screen + 1000 bytes color, potentially more for extended/bitmap modes) in
  one go, or whether it needs to be split across multiple bursts.
- Detecting the VIC bank (via CIA2 $DD00) and screen/charset pointers (VIC
  $D018) as part of each snapshot, rather than assuming the power-on
  defaults, so the fallback view stays correct if a program changes them
  before gpu64's API is ever engaged.

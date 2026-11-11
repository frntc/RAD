// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: how close together do two DMA holds get?

 Built 2026-09-06 as a *measurement*, not a fix. The Stage 16 derail has now
 survived three confident fixes (the class-0 re-warm, the mailbox bounds, and
 three separate power supplies), so this time the instrument goes first --
 see dev/hw_testing.md.

 What it is for. The gpu64_probe_latch ladder has eliminated every hold type
 individually:

   NOP  20000 dispatch holds, no flips            -- clean
   FLI0  2000 flips *inside* the dispatch hold    -- clean
   FLIW   300 loop-fired commit holds, C64 parked -- clean
   FLIP   the same holds, C64 doing real work     -- dies, every run

 The one structural difference left in FLIP is that a loop-fired commit hold
 can open only a few C64 cycles after a dispatch hold released. In FLIW the
 C64 sits in a STATUS poll with ~16 ms between the commit and any dispatch;
 in FLIP a dispatch releases the bus every 0.4-1 ms while the commit fires at
 60 Hz, so the two collide constantly. A 6510 that is restarted and re-halted
 before it completes a cycle is exactly how you corrupt a program counter
 without the Pi noticing anything at all -- which is what run H showed: not
 one bad errcode in 5472 dispatches, and a TRAP at $D6D0.

 So: stamp the ARM cycle counter at every bus release and every bus assert,
 and report the distribution of the gap between them. If the two holds never
 get close, this hypothesis dies without a firmware change.

 Cost. Nothing is added to either hold's critical path. Both the assert and
 the release sites already execute RESTART_CYCLE_COUNTER (an MRS of
 PMCCNTR_EL0) one instruction before they touch bDMA_OUT, so armCycleCounter
 is already an accurate stamp and this module only does arithmetic on it. The
 arithmetic at an assert runs with the bus held, where there is no deadline.
 The arithmetic at a release runs after SET_GPIO, where it costs the loop's
 next sample a handful of ARM cycles out of a ~600-cycle half-cycle -- and
 gpu64HoldGap is one 64-byte line, preloaded by both warm paths so that store
 cannot take a cold miss (rule 4).

 Units are raw ARM cycles. armPerC64 is measured, not assumed: the bus-watch
 loop's start-up block already spins 20000 WAIT_FOR_CPU/WAIT_FOR_VIC pairs
 with the bus held, and each pair is exactly one C64 cycle, so the counter
 delta across that block is the conversion factor for free. Expect ~1200.
*/
#ifndef _gpu64_holdgap_h
#define _gpu64_holdgap_h

#include <circle/types.h>

// Gap buckets, in whole C64 cycles: <1, <2, <4, <8, <16, >=16. The last one
// is also where every sample lands if armPerC64 never got measured.
#define GPU64_HOLDGAP_BUCKETS	6

// What opened or closed the hold. Recorded so the one pair that matters --
// a dispatch releasing and then the *loop-fired commit* taking the bus back
// -- can be separated from dispatch-to-dispatch, which NOP already proved
// harmless 20000 times over.
#define GPU64_HOLD_KIND_NONE		0
#define GPU64_HOLD_KIND_STARTUP		1
#define GPU64_HOLD_KIND_DISPATCH	2
#define GPU64_HOLD_KIND_COMMIT		3

// "Close" for the dispatch->commit counter, in C64 cycles. Four is a guess
// with a reason: the 6510 needs the DMA line released long enough to finish
// the cycle it was in and start another, and RAD's own DMA_READBYTE steals
// on that scale.
#define GPU64_HOLDGAP_CLOSE_CYCLES	4

// One 64-byte cache line, written only by core 0. Deliberately no section()
// attribute -- see the milestone-14 linker-orphan regression in
// dev/progress_tracker.md.
typedef struct
{
	u64	lastRelease;			// armCycleCounter at the last SET_GPIO( bDMA_OUT )
	u32	armPerC64;				// measured conversion factor, 0 = not measured
	u32	minGap;					// smallest release->assert gap seen, any pair
	u32	minGapD2C;				// smallest for dispatch-release -> commit-assert
	u32	bucket[ GPU64_HOLDGAP_BUCKETS ];
	u32	d2cClose;				// dispatch->commit asserts under CLOSE_CYCLES
	u32	asserts;				// denominator: every hold opened
	u8	lastKind;
	u8	pad[ 3 ];
} GPU64HOLDGAP;

extern GPU64HOLDGAP gpu64HoldGap;

#endif

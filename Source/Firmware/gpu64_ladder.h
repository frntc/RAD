// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__
         {_________         {______________		Expansion Unit

 gpu64 milestone 6 -- the multicore memory-traffic load ladder.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

*/
#ifndef _gpu64_ladder_h
#define _gpu64_ladder_h

#include <circle/types.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>

// gpu64: what this is, and why it is not the 2026-08-22 stress spike.
//
// The spike (gpu64_multicore.h) answered one binary question with a
// deliberately worst-case workload: three cores streaming 2MB buffers flat
// out corrupt the C64's own VIC-II output, while multicore *bring-up* alone
// does not. That leaves milestone 6's actual question open -- the render
// loop does not need flat-out bandwidth, it needs *some*, and nobody knows
// how much is safe. See dev/milestone6_3d_design.md: "find the actual
// contention threshold (a load ladder ... working up rather than starting
// from a deliberately worst-case synthetic stress)".
//
// So: one worker core (not three -- the design says start with core 0 plus
// one worker and expand only once that is measured), stepping through a
// ladder of *throttled* bandwidths, while core 0 measures its own bus-watch
// loop and the C64 runs a real REU + gpu64 workload. Each rung lasts a
// fixed wall-clock window; the whole ladder is one hardware run, and the
// result is a table printed by LOG_ENABLE(1).
//
// The pass/fail number is per-rung `L=`: bus cycles core 0 missed. The
// previous rounds' pass/fail signal was "does the RAD menu look like
// garbage", which cannot rank rungs. This one can.
//
// LEFT ON deliberately, unlike gpu64_multicore.h's toggles: this exists for
// one bench round and the image that round needs is the one tools/build.sh
// produces by default. **Comment it back out once the ladder result is
// recorded in dev/progress_tracker.md.** It forces multicore bring-up on
// (rad_main.h) whatever gpu64_multicore.h's own toggles say, it widens
// reuUsingPolling()'s instruction-cache preload window (below), and it puts
// measurement code inside that loop -- a diagnostic build, never a shipping
// one.
//
// With it commented out, every byte of this file compiles away: verified by
// building both ways and diffing the symbol table -- reuUsingPolling() comes
// out at the same address and the same 0x1b54 bytes as before the ladder
// existed.
// gpu64: milestone 6a is finished (2026-08-23, round 18) -- OFF for shipping.
// Uncomment only to re-run the ladder on the bench; it forces multicore
// bring-up on, widens the i-cache preload window to 0x2000, and puts
// measurement code inside reuUsingPolling().
//#define GPU64_LADDER_ENABLED

// Widen the polling loop's rolling i-cache preload window *without* dragging
// in the rest of the ladder. This exists to test one hypothesis and nothing
// else: the shipping window is 0x1a00 measured from reuEmulationMainLoop,
// and the loop runs past it (see the docs), so a tail of real bus-handling
// code -- the PMCCNTR waits, the GPIO reads, INP_GPIO_RW -- never enters the
// rolling preload. That is a candidate mechanism for the rare dropped IO2
// write the phase-0 bench found: an i-cache miss in the tail makes the loop
// too busy to sample a C64 write, which is lost outright.
//
// The ladder builds already ran at 0x2000 without trouble, so the widened
// window is known not to break anything by itself. Cost is a staler preload
// -- 128 passes to walk the window instead of 104 -- and about 1.3KB of
// overshoot past the end of the function, which merely preloads neighbouring
// code and is harmless.
//
// 2026-08-24: tested and OFF. The two-hour run at 0x2000 saw 14 events in
// 360000 frames -- 1 per 25700, against 1 per 23250 from the narrow-window
// runs. No effect whatsoever, so the tail falling outside the preload is not
// the mechanism and the shipping window stays at 0x1a00. The toggle is kept
// because it is a clean one-line way to take this variable off the table
// again after any change to the loop's size.
//#define GPU64_POLL_IPL_WIDE

// The isolation control the first bench round turned out to need. With this
// commented out, the ladder build keeps *everything* that touches core 0 --
// the per-pass measurement, the widened preload window, multicore bring-up
// -- but core 1 never runs the worker at all and parks in halt()/WFI, which
// is exactly the configuration that tested clean on 2026-08-22. A run in
// that state that still misbehaves indicts the instrument; a clean one
// exonerates it and puts the fault back on core 1. Leave it ON for a normal
// ladder run.
//
// Round 3 (2026-08-23) ran with this OFF and came back **perfectly clean**:
// 2048/2048 frames, zero REU verify errors, ERRCODE 0, a worst-case pass of
// 1716 ARM cycles (1.23us -- never even close to two C64 cycles) across
// 32.14M passes, and a visibly flawless sweep. That exonerates the
// instrument, the widened preload window and multicore bring-up, and leaves
// core 1's *activity* as the only remaining suspect for round 2's
// corruption. It is also the control the ladder now has and did not before.
//
// Back ON since round 4, which answered that question and produced the
// finding the current rung table is built around (see below).
#define GPU64_LADDER_WORKER_ENABLED

// Rungs, in order. Round 17 confirms the answer at the **renderer's real
// shape**, because round 16 answered the milestone's question.
//
// Round 16, all on an L1-resident 16 KB set:
//
//   b5 1M ... b5 32M   burst 5, 1/4/8/16/32 MB/s   L=0/0  x=1542-1544, 2M passes
//   b8  320us          burst 8,  1.6 MB/s          L=0/0  x=1542, 2M passes
//   b12 320us          burst 12, 2.4 MB/s          L=2/0  x=1610, 2M passes
//   b16 320us          burst 16, 3.2 MB/s          L=22/0 x=1757, dead at 471k
//
// **There is no rate ceiling.** 32 MB/s at burst 5 is indistinguishable from
// 1 MB/s at burst 5 -- same worst pass, zero losses -- and RATEK confirmed the
// throttle actually delivered 32000 KB/s. Ten times what a 320x200 50 fps
// framebuffer needs, for free.
//
// **The burst threshold is 8 clean / 12 marginal / 16 fatal**, measured at a
// fixed 320us slot so bursts-per-second was constant and only burst length
// moved.
//
// So milestone 6's rule is: *the render core may write up to 8 consecutive
// cache lines -- 512 bytes -- before yielding, and at that chunk size its
// throughput is unconstrained to at least 32 MB/s.*
//
// Two gaps keep that from being settled, and this round closes them:
//
//   b8 12M / b8 25M -- burst 8 has only been tested at 1.6 MB/s. Everything
//               fast was burst 5. If burst 8 holds at 12.8 and 25.6 MB/s, the
//               rule is a rule and not a coincidence of the two axes.
//   b5 64M   -- one more octave, to establish that "no rate ceiling" is not
//               just "no ceiling below 32".
//   b8 2M lo / b8 2M hi -- the renderer's real shape. Everything since round 13
//               has run on a 16 KB L1-resident set, which generates no bus
//               traffic at all. A 2 MB set at burst 8 misses L2 on every line,
//               at 1.6 MB/s and then at 12.8 MB/s. Rounds 11-13 found footprint
//               irrelevant *at burst 16*, where everything died regardless;
//               this checks it where it matters, at a burst that works.
//   b10 / b12 / b16 -- refines 8-to-12 and re-runs the two known rungs as
//               controls. Fatal one last.
#define GPU64_LADDER_RUNGS			9

// What core 1 does in a rung. Cumulative up to WORK; ZVA is WORK with the
// stores swapped for cache-line zeroing (unused since round 12, kept for the
// cold path). A rung with MODE_ACC and a short slot is the spin control.
#define GPU64_LADDER_MODE_PARK		0	// wake, read the rung, sleep again
#define GPU64_LADDER_MODE_TICK		1	// + read CNTVCT_EL0, pace the slot
#define GPU64_LADDER_MODE_ACC		2	// + private accumulator bookkeeping
#define GPU64_LADDER_MODE_WORK		3	// + write the working set
#define GPU64_LADDER_MODE_ZVA		4	// + write it with DC ZVA instead

// Working-set sizes. 16 KB is L1-resident and 2 MB cannot be cached at all;
// the L1 D is 32 KB and the shared L2 is 512 KB.
#define GPU64_LADDER_SET_16K		( 16 * 1024 )
#define GPU64_LADDER_SET_64K		( 64 * 1024 )
#define GPU64_LADDER_SET_512K		( 512 * 1024 )
#define GPU64_LADDER_SET_DRAM		( 2 * 1024 * 1024 )

// Consecutive 64-byte cache lines written at the top of each slot. This is the
// axis: 8 measured clean, 12 marginal, 16 fatal.
#define GPU64_LADDER_BURST_1		1
#define GPU64_LADDER_BURST_4		4
#define GPU64_LADDER_BURST_5		5
#define GPU64_LADDER_BURST_6		6
#define GPU64_LADDER_BURST_7		7
#define GPU64_LADDER_BURST_8		8
#define GPU64_LADDER_BURST_10		10
#define GPU64_LADDER_BURST_12		12
#define GPU64_LADDER_BURST_16		16
#define GPU64_LADDER_BURST_32		32
#define GPU64_LADDER_BURST_64		64

// Slot periods, microseconds. Rate is burst x 64 bytes / slot. Everything below
// ~853us is paced by spinning on CNTVCT_EL0 rather than the event stream --
// validated clean by round 14's `spin64` and every sub-millisecond rung since.
#define GPU64_LADDER_SLOT_5US		5
#define GPU64_LADDER_SLOT_10US		10
#define GPU64_LADDER_SLOT_20US		20
#define GPU64_LADDER_SLOT_32US		32
#define GPU64_LADDER_SLOT_40US		40
#define GPU64_LADDER_SLOT_64US		64
#define GPU64_LADDER_SLOT_80US		80
#define GPU64_LADDER_SLOT_160US		160
#define GPU64_LADDER_SLOT_320US		320
#define GPU64_LADDER_SLOT_1MS		1000
#define GPU64_LADDER_SLOT_3200US	3200
#define GPU64_LADDER_SLOT_6400US	6400
#define GPU64_LADDER_SLOT_12800US	12800

#define GPU64_LADDER_RUNG_PASSES	2000000

// The buffer core 1 writes into, sized for the largest arm. Rungs take a
// prefix of it (GPU64_LADDER_SET_*), so the whole ladder shares one
// allocation and one alignment.
#define GPU64_LADDER_BUFFER_BYTES	GPU64_LADDER_SET_DRAM


// Samples core 0 spends measuring the undisturbed loop before it fixes the
// missed-cycle threshold. ~100ms at rung 0 (park), i.e. the first 5% of rung 0
// is calibration and is not counted in that rung's L=.
//
// The threshold comes off that window's **mean**, not its minimum. Round 3
// (2026-08-23) was a visibly perfect run that the table scored as 31,973,007
// missed cycles out of 32,140,000 passes, because the minimum is not "one C64
// cycle" -- it is the *fastest* pass. The loop is PHI-locked but the lock has
// slop both ways: when the previous pass overran into the next VIC half-cycle,
// this pass's WAIT_FOR_VIC_HALFCYCLE returns immediately and the delta comes
// out short. Round 3 measured min 865 / max 1716 against a nominal 1421, so
// 1.5x the minimum sat *below* the typical healthy pass and tripped on nearly
// all of them.
//
// **Two thresholds, and round 12 is why the first one moved.** `lost2` stays at
// 4/3 of baseline -- 1893, the line that says a pass overran a whole VIC
// half-cycle -- so every number from rounds 10 to 12 stays comparable. `lost`
// is now 9/8, i.e. 1597: not a missed cycle, just a pass measurably longer than
// a healthy one. Round 12's fatal rung scored **L=0/0 with a worst pass of
// 1832** while the C64 went down, because the only counter that existed was the
// coarse one. Clean rungs across rounds 10-12 top out at 1495-1570, so 1597
// sits just above the observed healthy ceiling and turns that 1832 into a
// count instead of a single anecdote in the x= column.
//
// The 4/3 multiplier for `lost2`, rather than 3/2, also cost a round. Passes
// quantise to VIC half-cycles: 1420 healthy, ~2130 for one missed half-cycle,
// ~2840 for a whole missed cycle. A 3/2 threshold is 2130 -- exactly the first
// failure step, which it then fails to exceed. Round 6's first NT rung scored
// `L=0 x=2101`: a missed half-cycle, recorded as clean.
//
// Neither counter sees a stall that happens *mid-pass*, after
// WAIT_FOR_VIC_HALFCYCLE has resynchronised the loop -- that is round 12's
// finding and this file's standing caveat. A rung can corrupt the bus and score
// zero on both.
#define GPU64_LADDER_CALIB_PASSES	100000

// reuUsingPolling()'s instruction-cache preload window, and the size the
// callers preload the whole function at. The ladder's per-pass measurement
// adds ~500 bytes of hot loop code, which would otherwise fall outside the
// stock 0x1a00 window and leave code that runs *every pass* unpreloaded --
// timing jitter attributed to core 1 that was really the instrument's own.
// Widened only in the ladder build; a Cortex-A53's 32KB L1 I-cache has room
// to spare, and the rolling 64-bytes-per-pass preload just takes
// proportionally longer to walk the window.
//
// Keep this at or above the function's actual size, and check after any
// change to the instrumentation -- 0x1e00 was 68 bytes clear of it by round
// 5, which is not a margin:
//   aarch64-none-elf-nm -S external/Circle/app/Firmware/kernel8.elf \
//     | grep reuUsingPolling
#if defined( GPU64_LADDER_ENABLED ) || defined( GPU64_POLL_IPL_WIDE )
	#define GPU64_POLL_IPL_WINDOW	0x2c00
	#define GPU64_POLL_PRELOAD_SIZE	( 1024 * 11 )
#else
	// 0x1a00 measured 120-176B under the function's actual size across
	// several rounds (see [[gpu64-poll-loop-exceeds-ipl-window]]) -- recorded
	// as real but benign, since widening it previously changed nothing on
	// hardware. Bumped to 0x1c00 for margin while touching this file for the
	// Stage 16 loop-freeze fix (unrelated bug).
	//
	// Bumped again to 0x2800 on 2026-09-07 for the hold gate
	// (gpu64_holdgate.h), which took reuUsingPolling() from 0x1d9c to 0x24d4.
	// Most of that is not the gate's own code -- removing the gate's
	// duplicated dispatch hold and rebuilding made the function *larger*, at
	// 0x2608 -- it is the compiler rearranging a loop body that now carries
	// five more live registers. Either way the whole function is inside the
	// window again, which it had not been since the ladder rounds.
	//
	// 2026-09-08, the C64-reset state restore: 0x24d4 -> 0x2558, still ~680
	// bytes clear. The bounded no-clock wait costs a subs+b.eq per spin
	// iteration and a cbnz per pass, with no new stack spills in the hot
	// path -- the timeout became a countdown register and `noClock` folded
	// away entirely.
	//
	// 2026-09-09, the bus-sampling instrument (gpu64_busstats.h): 0x2558 ->
	// 0x2684, still ~380 bytes clear. Two inlined counter helpers at four
	// call sites, all of them off the deadline path.
	// Re-measure with the nm -S command above after any further change.
	#define GPU64_POLL_IPL_WINDOW	0x2800
	#define GPU64_POLL_PRELOAD_SIZE	( 1024 * 10 )
#endif

// Core 0's per-pass bookkeeping -- and **everything core 0 touches on the hot
// path**, deliberately gathered into one 64-byte line that no other core ever
// writes.
//
// This is the fix for round 8, and the bug it fixes had been in the ladder
// since round 1. The state used to be loose globals, and the linker put
// `gpu64LadderThreshold` (read by core 0 on every pass) at 0x690c364 and
// `gpu64LadderRung` (written by core 1 on every millisecond slot) at
// 0x690c370 -- **the same cache line**. So core 1 invalidated core 0's
// hottest line a thousand times a second, in every rung including `idle`,
// where core 1 writes no data at all.
//
// That explains the whole shape of rounds 7 and 8: a dirty idle rung with
// core 1 asleep; 16 KB scoring 128 missed cycles in round 8 after scoring
// zero at 450 MB/s in round 5; the numbers shifting when unrelated code
// changed; and the improvement from 30 to 15 when round 8 padded the stats
// array, which moved these addresses without meaning to. **Rounds 4, 5 and 6
// were clean by link-layout luck, not by design.**
//
// A coherence miss is only ~100 cycles, but the loop is PHI-locked: it must
// finish before the next VIC half-cycle, and when it does not it waits out a
// whole one. So a 100-cycle hiccup quantises into a 710-cycle miss, which is
// exactly the 2130-cycle passes the table has been reporting.
//
// It is also a lesson for milestone 6 proper, not just for the instrument:
// any variable the render core writes that shares a line with something core
// 0 reads per pass will do this, and it will look like a mysterious,
// build-dependent timing fault.
//
// The struct is deliberately a struct rather than the four loop locals it
// started as: as locals they added register pressure to a loop already at the
// limit, and the compiler paid for them by spilling other hot values -- ~500
// bytes of extra code for a ten-instruction measurement.
struct Gpu64LadderPass
{
	u64	prev;
	u32	skip;
	u32	left;			// passes remaining in this rung
	u32	rung;			// core 0's own copy, refreshed rarely from core 1's
	u32	threshold;		// ARM cycles, 9/8 baseline: measurably elongated
	u32	threshold2;		// 4/3 baseline: a missed VIC half-cycle
	u32	baseline;		// ARM cycles in one C64 cycle: the mean
	u32	dumped;			// so the table prints once, not twice
	u32	pad[ 7 ];
} __attribute__(( aligned( 64 ) ));

struct Gpu64LadderStats
{
	u32	passes;
	u32	lost;			// passes past 9/8: measurably longer than healthy
	u32	lost2;			// passes past 4/3: a missed VIC half-cycle or worse
	u32	minDelta;		// ARM cycles between consecutive loop passes
	u32	maxDelta;
	u32	pad;
	u64	totalDelta;
};

#ifdef GPU64_LADDER_ENABLED

// The one cross-core mailbox, alone on its own cache line. Core 0 writes it
// -- at a rung boundary, so roughly twice a second -- and core 1 reads it.
// Nothing goes the other way any more: core 0's hot path touches only its own
// lines, which is the property that makes its noise floor its own.
//
// `gen` is bumped on every arming and is what makes core 1 restart. Without it
// core 1 latched its start state exactly once, and a second arming (which any
// C64 reset causes, since resetREU() clears gpu64ApiActive and the next
// GET_INFO re-arms) left core 1 running the first run's timeline -- round 6
// hit exactly that.
struct Gpu64LadderToWorker
{
	volatile u32 gen;
	volatile u32 start;
	volatile u32 rung;
	u32 pad[ 13 ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64LadderToWorker gpu64LadderToWorker;

// Index GPU64_LADDER_RUNGS is the "ladder finished, core 1 idle again"
// bucket -- core 1 stops generating traffic there, so the machine is quiet
// while the log is read.
extern Gpu64LadderStats gpu64Ladder[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));
extern Gpu64LadderPass gpu64LadderPass;

// Core 1's entry point, called from CGpu64MultiCore::Run().
void gpu64_ladderWorker( void );

// Called from core 0 on that transition: clears the table and releases
// core 1.
void gpu64_ladderArm( void );

// Formats the table for the on-screen log. Returns the end pointer.
char *gpu64_ladderReport( char *p );

// Forces the log on and prints the table (gpu64_api.cpp). Declared here as
// well as in gpu64_api.h because GPU64_LADDER_SAMPLE now calls it, and
// rad_reu.cpp includes this header, not that one.
void gpu64_ladderDumpNow( void );

// --- core 0's per-pass instrumentation ---------------------------------
//
// Declared as macros rather than a function because it has to live inside
// reuUsingPolling()'s loop and keep its state in registers; a call would
// cost more than the thing it measures.
//
// GPU64_LADDER_SAMPLE goes at the *top* of the loop, right after the pass's
// WAIT_FOR_VIC_HALFCYCLE, and reads PMCCNTR_EL0 itself rather than reusing
// the loop's armCycleCounter. The tempting spot -- the mid-loop
// RESTART_CYCLE_COUNTER, where a fresh value is already in hand -- sits
// immediately before WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS ), a hard deadline
// measured in nanoseconds; inserting anything there would push the bus
// sample late and the instrument would be creating the fault it is looking
// for. The top of the loop is the slack region the mirror snapshot already
// lives in, and it is PHI-locked just the same, so the delta means the same
// thing.

#define GPU64_LADDER_LOCALS		/* state lives in gpu64LadderPass */

// The loop is PHI-locked: every pass syncs to the same edge, so consecutive
// samples are one C64 cycle apart unless a cycle was missed, in which case
// the delta jumps to a multiple of it. That is what makes a single
// subtraction a *missed bus sample* counter rather than a vague jitter
// metric.
#define GPU64_LADDER_SAMPLE { \
	u64 ladderNow; \
	asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (ladderNow) ); \
	u32 d = (u32)( ladderNow - gpu64LadderPass.prev ); \
	gpu64LadderPass.prev = ladderNow; \
	if ( gpu64LadderPass.skip ) \
		gpu64LadderPass.skip = 0; \
	else { \
		Gpu64LadderStats *s = &gpu64Ladder[ gpu64LadderPass.rung ]; \
		s->passes++; \
		s->totalDelta += d; \
		if ( d > s->maxDelta ) s->maxDelta = d; \
		if ( d < s->minDelta ) s->minDelta = d; \
		if ( gpu64LadderPass.threshold ) { \
			if ( d > gpu64LadderPass.threshold ) { \
				s->lost++; \
				if ( d > gpu64LadderPass.threshold2 ) s->lost2++; \
			} \
		} else \
		if ( s->passes >= GPU64_LADDER_CALIB_PASSES ) { \
			gpu64LadderPass.baseline = (u32)( s->totalDelta / s->passes ); \
			gpu64LadderPass.threshold2 = gpu64LadderPass.baseline + gpu64LadderPass.baseline / 3; \
			gpu64LadderPass.threshold = gpu64LadderPass.baseline + gpu64LadderPass.baseline / 8; \
		} \
	} \
	if ( --gpu64LadderPass.left == 0 ) { \
		gpu64LadderPass.left = GPU64_LADDER_RUNG_PASSES; \
		if ( gpu64LadderPass.rung < GPU64_LADDER_RUNGS ) { \
			gpu64LadderPass.rung++; \
			gpu64LadderToWorker.rung = gpu64LadderPass.rung; \
			if ( gpu64LadderPass.rung >= GPU64_LADDER_RUNGS ) GPU64_LADDER_DUMP \
		} \
	} }

// A DMA hold (REU transfer, gpu64 command, mirror snapshot, deferred flip
// commit) deliberately stops the loop sampling the bus, so the pass it
// lands in is not a missed cycle and must not be counted as one. Every
// hold site raises this; the next sample is discarded.
#define GPU64_LADDER_SKIP	gpu64LadderPass.skip = 1;

// One-shot, and reached three ways: the two crash/exit paths, and -- since
// round 5 -- core 0 itself, the moment the published rung index reaches the
// `done` bucket. That last one is why the ladder no longer depends on the C64
// surviving to the end of the run: round 4 lost seven rungs of data because a
// corrupted gpu64 command dropped the machine into the RAD menu 1.2s into
// rung 1, and a table nobody can read is the same as no measurement.
//
// It stalls the loop for as long as it takes to render ~15 log lines, which
// is many C64 cycles. That is deliberate and safe *here*: it fires only once
// the ladder is over, with core 1 back at idle, and the test PRG is by then
// doing nothing a stalled bus can corrupt. The pass it lands in is discarded
// via the skip flag on the way out.
#define GPU64_LADDER_DUMP { \
	if ( !gpu64LadderPass.dumped ) \
		{ gpu64LadderPass.dumped = 1; gpu64_ladderDumpNow(); GPU64_LADDER_SKIP } }

#else

#define GPU64_LADDER_LOCALS
#define GPU64_LADDER_SAMPLE
#define GPU64_LADDER_SKIP
#define GPU64_LADDER_DUMP

#endif

#endif

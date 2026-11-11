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
#include "gpu64_ladder.h"

#ifdef GPU64_LADDER_ENABLED

Gpu64LadderToWorker   gpu64LadderToWorker;

Gpu64LadderStats gpu64Ladder[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));
Gpu64LadderPass  gpu64LadderPass;

// One rung per row. `setBytes` is the working-set footprint (16 KB throughout
// this round -- L1-resident, so the memory hierarchy is not a variable),
// `burstLines` is how many consecutive 64-byte lines core 1 writes at the top
// of a slot, and `slotUs` is how long the slot is. Rate is burstLines x 64 /
// slotUs, so the last two together are what finally make burst and rate
// independent -- see gpu64_ladder.h for why that matters and what the 2x2 is
// meant to decide.
//
// Order matters: a derail ends the run and zeros every rung above it. The spin
// control comes first because it can void half the table, then the known-clean
// warm rung, then the burst arm at low rate, then the rate arm at low burst,
// and the rungs already known to be fatal last.
static const struct
{
	const char *pLabel;			// 7 characters, padded
	u32	setBytes;
	u32	burstLines;				// consecutive 64-byte lines per slot
	u32	slotUs;					// slot period, microseconds
	u8	mode;					// GPU64_LADDER_MODE_*
}
s_Rung[ GPU64_LADDER_RUNGS + 1 ] =
{
	{ "park   ",	0,						0,						0,							GPU64_LADDER_MODE_PARK },

	// Round 17 killed burst 8 on a cold 2 MB set at 1.6 MB/s while burst 8 on
	// an L1-resident set was clean at 25 MB/s. A cold store must allocate its
	// line before it retires, so the same burst is a much longer stall. Every
	// rung below is cold, and the edge is bracketed between 5 (clean, rounds
	// 11-12) and 8 (fatal).
	{ "c b4 lo",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_4,	GPU64_LADDER_SLOT_320US,	GPU64_LADDER_MODE_WORK },
	{ "c b4 hi",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_4,	GPU64_LADDER_SLOT_40US,		GPU64_LADDER_MODE_WORK },
	{ "c b5 hi",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_5,	GPU64_LADDER_SLOT_40US,		GPU64_LADDER_MODE_WORK },
	{ "c b6 hi",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_6,	GPU64_LADDER_SLOT_40US,		GPU64_LADDER_MODE_WORK },
	{ "c b7 hi",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_7,	GPU64_LADDER_SLOT_40US,		GPU64_LADDER_MODE_WORK },

	// The mitigation. DC ZVA writes a whole line with no read-for-ownership,
	// which is exactly the extra cost a cold store pays. Clean here against a
	// fatal plain b8 means the cold penalty can be engineered away.
	{ "zva b8 ",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_8,	GPU64_LADDER_SLOT_40US,		GPU64_LADDER_MODE_ZVA },
	{ "zva b16",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_16,	GPU64_LADDER_SLOT_40US,		GPU64_LADDER_MODE_ZVA },

	// Round 17's killer, reproduced last so a derail costs nothing above it.
	{ "c b8 lo",	GPU64_LADDER_SET_DRAM,	GPU64_LADDER_BURST_8,	GPU64_LADDER_SLOT_320US,	GPU64_LADDER_MODE_WORK },

	{ "done   ",	0,						0,						0,							GPU64_LADDER_MODE_PARK }
};

// Core 1's working set, sized for the largest arm. Static rather than
// heap-allocated for the same reason the spike's was: this is a diagnostic
// build and the allocator is not part of the question. Cache-line aligned so
// the L1 arm's 16KB really is 16KB of whole lines and not 16KB plus two
// straddling neighbours.
static u64 s_Buffer[ GPU64_LADDER_BUFFER_BYTES / 8 ] __attribute__(( aligned( 64 ) ));

// Bytes core 1 actually managed to write in each rung, and the microseconds
// it spent there. Written only by core 1, read only after the ladder is
// over -- the achieved rate is what the report prints, because a throttle
// that could not keep up would otherwise silently mislabel a rung.
static volatile u64 s_Written[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));
static volatile u64 s_ElapsedTicks[ GPU64_LADDER_RUNGS + 1 ] __attribute__(( aligned( 64 ) ));

void gpu64_ladderArm( void )
{
	for ( unsigned i = 0; i <= GPU64_LADDER_RUNGS; i++ )
	{
		gpu64Ladder[ i ].passes     = 0;
		gpu64Ladder[ i ].lost       = 0;
		gpu64Ladder[ i ].lost2      = 0;
		gpu64Ladder[ i ].minDelta   = 0xFFFFFFFF;
		gpu64Ladder[ i ].maxDelta   = 0;
		gpu64Ladder[ i ].totalDelta = 0;
		s_Written[ i ]      = 0;
		s_ElapsedTicks[ i ] = 0;
	}

	gpu64LadderPass.prev       = 0;
	gpu64LadderPass.skip       = 1;
	gpu64LadderPass.left       = GPU64_LADDER_RUNG_PASSES;
	gpu64LadderPass.rung       = 0;
	gpu64LadderPass.threshold  = 0;
	gpu64LadderPass.threshold2 = 0;
	gpu64LadderPass.baseline   = 0;
	gpu64LadderPass.dumped     = 0;

	gpu64LadderToWorker.rung = 0;

	// Bump last, and after everything else is cleared: this is what core 1
	// watches, and seeing it move is what makes core 1 restart its own
	// timeline. Before round 7 there was no generation counter and core 1
	// latched its start time exactly once, so a second arming -- which
	// happens on every C64 reset, since resetREU() clears gpu64ApiActive and
	// the next GET_INFO re-arms -- left core 1 running the *original*
	// timeline. Round 6 hit this: after a transient derail at 2s the ladder
	// re-armed, core 1's clock was already past the last rung, and it
	// published `done` for the remaining 51 seconds. The table came back with
	// every rung at zero and `done p=49016k`.
	gpu64LadderToWorker.start = 1;
	gpu64LadderToWorker.gen++;
}

// The ARM generic timer, read as a *system register*. Not
// read32( ARM_SYSTIMER_CLO ).
//
// This distinction cost the first bench round (2026-08-23). The BCM system
// timer is MMIO on the VideoCore peripheral bus -- the same bus core 0 reads
// ARM_GPIO_GPLEV0 from, in a tight spin, as its entire notion of where the
// C64's clock is. Core 1 polling that register to run its throttle put a
// second maximum-rate reader on that bus and pushed core 0's GPIO reads
// late. CNTVCT_EL0 is core-local silicon: no bus cycle, no contention, and
// no way for the instrument to disturb the thing it is measuring.
static inline u64 ladderTicks( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTVCT_EL0" : "=r" (v) );
	return v;
}

// `DC ZVA` zeroes a whole cache line without reading it first, so a cold
// write becomes one DRAM transaction instead of a read-for-ownership plus a
// later writeback. DCZID_EL0 says whether it is usable: bit 4 (DZP) set means
// prohibited, and the low four bits are log2 of the block size in words. A
// Cortex-A53 reports 4, i.e. 16 words = 64 bytes = one line. Anything else and
// the ZVA rung falls back to plain stores, which the report makes visible as a
// rung whose numbers match its plain-store neighbour exactly.
static inline bool ladderZvaIs64( void )
{
	u64 v;
	asm volatile( "MRS %0, DCZID_EL0" : "=r" (v) );
	return ( v & ( 1 << 4 ) ) == 0 && ( v & 0xF ) == 4;
}

static inline u64 ladderFreq( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTFRQ_EL0" : "=r" (v) );
	return v;
}

// Put core 1 genuinely to sleep between slots, instead of spinning.
//
// Round 2 (2026-08-23) corrupted core 0 with core 1 writing zero bytes and
// touching no MMIO -- a `yield` spin was enough, and `yield` on a
// Cortex-A53 is a hint that does nothing. Round 3, with core 1 parked in
// WFI, was flawless. So the spin itself is a disturbance, and leaving it in
// would make it a confound in *every* rung: each rung would measure its
// write traffic plus a constant spin, and the ladder is supposed to vary
// one thing.
//
// The generic timer's event stream is the way to wait without spinning and
// without interrupts: CNTKCTL_EL1.EVNTEN makes a chosen counter bit
// generate events, and WFE sleeps until one arrives. EVNTI = 14 gives an
// event every 2^14 ticks = 853us at 19.2MHz -- fine against a 1ms slot,
// since the loops re-check the real deadline on every wake and simply sleep
// again. No interrupt is involved at any point, which matters because RAD
// runs with IRQs disabled and a secondary core taking an unexpected
// exception has nowhere to report it.
//
// Failure mode if the event stream does not work: core 1 sleeps forever,
// the rung index never advances, and the report says so plainly -- every
// rung past idle reads p=0 and RATE reads all zeros. Visible, and harmless
// to the C64.
#define GPU64_LADDER_EVNTI	14

static inline void ladderEnableEventStream( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTKCTL_EL1" : "=r" (v) );
	v &= ~( (u64)0xF << 4 );			// EVNTI
	v |=  ( (u64)GPU64_LADDER_EVNTI << 4 );
	v &= ~( (u64)1 << 3 );				// EVNTDIR = 0, count up
	v |=  ( (u64)1 << 2 );				// EVNTEN
	asm volatile( "MSR CNTKCTL_EL1, %0" :: "r" (v) );
	asm volatile( "ISB" );
}

void gpu64_ladderWorker( void )
{
	// volatile so the stores are real traffic and not something the optimiser
	// can prove nobody reads.
	volatile u64 *pBuf = s_Buffer;

	u32 idx = 0;
	u64 v   = 0;

	const bool bZva = ladderZvaIs64();

	ladderEnableEventStream();

	u32 nGen = gpu64LadderToWorker.gen;
	while ( nGen == 0 )
		{ asm volatile( "WFE" ); nGen = gpu64LadderToWorker.gen; }

	// The armstub's EL3 prologue programs CNTFRQ_EL0 to OSC_FREQ on every core
	// (Source/Firmware/ARM STUB/rad-prefetch.S), so this is 19.2MHz in
	// practice -- but a zero here would be a divide-by-zero hang on a core
	// nothing can log from, so it falls back rather than trusting that.
	u64 nFreq = ladderFreq();
	if ( nFreq == 0 )
		nFreq = 19200000;

	// Recomputed whenever a rung changes the slot period. The event stream
	// fires once per 2^EVNTI ticks, so a slot shorter than that cannot be
	// waited on with WFE and is paced by spinning on CNTVCT_EL0 instead --
	// see the wait at the bottom of the loop, and `spin64`, its control rung.
	const u64 nEvtTicks = (u64)1 << GPU64_LADDER_EVNTI;

	u32 nLastSlotUs = 0;
	u64 nSlotTicks  = nFreq / 1000;

	u64 tSlot = ladderTicks();
	u64 tPrev = tSlot;

	u32 nLastSet = 0;
	u64 nAccBytes = 0;
	u64 nAccTicks = 0;
	u32 nAccSlots = 0;
	u32 nAccRung  = 0;

	while ( 1 )
	{
		// The only shared read on this path, and it is one word from a line
		// core 0 writes about twice a second.
		const u32 rung = gpu64LadderToWorker.rung;
		const u8  mode = s_Rung[ rung ].mode;

		// PARK: wake on the event stream, read that one word, sleep again.
		// Nothing else at all -- no timer read, no store, no arithmetic. This
		// is the control rung, and it has to stay this bare to be worth
		// anything.
		if ( mode == GPU64_LADDER_MODE_PARK )
		{
			asm volatile( "WFE" );
			continue;
		}

		if ( gpu64LadderToWorker.gen != nGen )
		{
			nGen = gpu64LadderToWorker.gen;
			tSlot = tPrev = ladderTicks();
			nLastSlotUs = 0;
			nLastSet = 0;
			idx = 0;
			nAccBytes = nAccTicks = 0;
			nAccSlots = 0;
			nAccRung  = 0;
		}

		// TICK and above: paced off the generic timer, in slots this rung's
		// own length. Resynchronise on a change rather than carrying the old
		// deadline forward, which would make the first slot of a longer rung
		// fire immediately.
		if ( s_Rung[ rung ].slotUs != nLastSlotUs )
		{
			nLastSlotUs = s_Rung[ rung ].slotUs;
			nSlotTicks  = nFreq * nLastSlotUs / 1000000;
			tSlot = ladderTicks();
		}

		u64 now = ladderTicks();

		if ( mode >= GPU64_LADDER_MODE_WORK )
		{
			// 8 u64 per 64-byte line, written in address order, so a burst of
			// N lines really is N contiguous lines and not N scattered ones --
			// the whole point of the rung is the length of an *unbroken* run.
			const u32 nLines = s_Rung[ rung ].burstLines;
			const u32 nBytes = nLines * 64;
			const u32 nWords = s_Rung[ rung ].setBytes / 8;

			// Restart at the top of the buffer whenever the footprint
			// changes, so a rung never inherits a cursor pointing past its
			// own working set. Not on a rung change alone: the warm rungs
			// share a set on purpose and must inherit each other's residency.
			if ( s_Rung[ rung ].setBytes != nLastSet )
				{ nLastSet = s_Rung[ rung ].setBytes; idx = 0; }

			if ( mode == GPU64_LADDER_MODE_ZVA && bZva )
			{
				// idx stays a multiple of 8 because every step is a whole
				// line and every set size is a whole number of lines, so the
				// address handed to DC ZVA is always 64-byte aligned.
				u8 *pLine = (u8 *)s_Buffer + (size_t)idx * 8;

				for ( u32 i = 0; i < nLines; i++ )
				{
					asm volatile( "DC ZVA, %0" :: "r" (pLine) : "memory" );
					idx += 8;
					if ( idx >= nWords ) { idx = 0; pLine = (u8 *)s_Buffer; }
					else pLine += 64;
				}
			} else
			{
				for ( u32 i = 0; i < nLines * 8; i++ )
				{
					pBuf[ idx ] = v++;
					if ( ++idx >= nWords ) idx = 0;
				}
			}

			nAccBytes += nBytes;
		}

		if ( mode >= GPU64_LADDER_MODE_ACC )
		{
			// Wall time is attributed a whole slot at a time, throttle wait
			// included -- the achieved rate has to be the rung's *average*
			// rate, which is what the throttle sets, not the rate of the
			// burst inside each millisecond.
			//
			// Accumulated in registers and published rarely. These arrays are
			// core-1-private and on their own cache lines, but 1000
			// shared-memory writes a second from the core whose entire job is
			// to not disturb core 0 is not a thing to leave in on the grounds
			// that it is probably harmless.
			nAccTicks += now - tPrev;

			if ( rung != nAccRung || ++nAccSlots >= 64 )
			{
				s_Written[ nAccRung ]      += nAccBytes;
				s_ElapsedTicks[ nAccRung ] += nAccTicks;
				nAccBytes = nAccTicks = 0;
				nAccSlots = 0;
				nAccRung  = rung;
			}
		}

		tPrev = now;

		// If the rung's budget did not fit in its millisecond, do not let the
		// throttle turn into a catch-up burst -- resync and let the
		// achieved-rate column report the shortfall instead.
		tSlot += nSlotTicks;
		if ( (s64)( ladderTicks() - tSlot ) > 0 )
			{ tSlot = ladderTicks(); continue; }

		// Sleep while there is more than one event-stream period to wait, then
		// spin out the remainder. For a slot shorter than 853us that is a pure
		// CNTVCT_EL0 spin -- core-local, no bus cycle, and controlled for by
		// the `spin64` rung.
		while ( (s64)( ladderTicks() - tSlot ) < 0 )
		{
			if ( (s64)( tSlot - ladderTicks() ) > (s64)nEvtTicks )
				asm volatile( "WFE" );
		}
	}
}

static char *ladDec( char *p, u32 v )
{
	char tmp[ 10 ];
	unsigned n = 0;
	do { tmp[ n++ ] = (char)( '0' + v % 10 ); v /= 10; } while ( v != 0 );
	while ( n-- ) *p++ = tmp[ n ];
	return p;
}

static char *ladStr( char *p, const char *q )
{
	while ( *q ) *p++ = *q++;
	return p;
}

char *gpu64_ladderReport( char *p )
{
	// Header: the calibration. `c64=` is the *mean* ARM cycles between two
	// consecutive undisturbed loop passes -- one C64 cycle, and a sanity
	// check in its own right: 1421 at the 1400MHz config.txt pins. `thr=`
	// is 1.5x that, the line above which a pass took longer than one C64
	// cycle. See the note on GPU64_LADDER_CALIB_PASSES for why this is the
	// mean and not the minimum; round 3 was scored 99.5% failing by a
	// minimum-based threshold on a flawless run.
	p = ladStr( p, "LADDER c64=" );
	p = ladDec( p, gpu64LadderPass.baseline );
	p = ladStr( p, " thr=" );
	p = ladDec( p, gpu64LadderPass.threshold );
	*p++ = '\n';

	for ( unsigned i = 0; i <= GPU64_LADDER_RUNGS; i++ )
	{
		const Gpu64LadderStats *s = &gpu64Ladder[ i ];

		p = ladStr( p, s_Rung[ i ].pLabel );
		*p++ = ' ';

		p = ladStr( p, "p=" );
		p = ladDec( p, s->passes / 1000 );
		*p++ = 'k';

		// Two severities: past 4/3 of a C64 cycle, and past two of them. See
		// the note on the threshold in gpu64_ladder.h -- the pair is what
		// keeps this round comparable with the ones scored at 3/2.
		p = ladStr( p, " L=" );
		p = ladDec( p, s->lost );
		*p++ = '/';
		p = ladDec( p, s->lost2 );

		// Per-rung mean and worst case. The mean is here because round 3
		// showed it is the number that says whether a rung was healthy at
		// all: a rung whose mean has drifted off the calibrated c64= figure
		// is one where core 0 is systematically late, which is a different
		// and worse thing than a few long passes.
		p = ladStr( p, " m=" );
		p = ladDec( p, s->passes ? (u32)( s->totalDelta / s->passes ) : 0 );

		p = ladStr( p, " x=" );
		p = ladDec( p, s->maxDelta );

		*p++ = '\n';
	}

	// Achieved rates, KB/s, one field per rung in ladder order. On its own
	// line because the per-rung lines are already at the log's 40 columns,
	// and because what these are for is validating the throttle rather than
	// reading alongside the timing: a rung whose achieved rate falls short
	// of its label was really running at this number, not at its name.
	//
	// Bytes per microsecond is megabytes per second, so bytes x 1000 per
	// microsecond is kilobytes per second. Generic-timer ticks
	// are converted here rather than per slot, so the worker never does a
	// 64-bit division in its inner loop.
	p = ladStr( p, "RATEK" );
	for ( unsigned i = 0; i <= GPU64_LADDER_RUNGS; i++ )
	{
		u64 nHz = ladderFreq();
		u64 nUs = nHz ? s_ElapsedTicks[ i ] * 1000000 / nHz : 0;

		*p++ = ( i == 0 ) ? ' ' : '/';
		// Kilobytes per second, not megabytes: round 11's whole ladder ran
		// below 1 MB/s and every field came back a rounded-down zero.
		p = ladDec( p, nUs ? (u32)( s_Written[ i ] * 1000 / nUs ) : 0 );
	}
	*p++ = '\n';

	return p;
}

#endif

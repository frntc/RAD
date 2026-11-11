// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__
         {_________         {______________		Expansion Unit

 gpu64 milestone 6 -- multicore feasibility spike.

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

*/
#ifndef _gpu64_multicore_h
#define _gpu64_multicore_h

#include <circle/multicore.h>
#include <circle/memory.h>
#include <circle/types.h>

// gpu64: multicore feasibility test matrix (2026-08-22, see
// dev/milestone6_3d_design.md and dev/progress_tracker.md). Two
// independent toggles so each hardware round isolates one variable:
//
//   GPU64_MULTICORE_ENABLED         -- calls CMultiCoreSupport::Initialize()
//                                       at all (rad_main.h). Off = secondary
//                                       cores stay parked in the armstub's
//                                       own spin table, Circle's multicore
//                                       machinery (CSpinLock::Enable(),
//                                       interrupt-routing rewrite, each
//                                       secondary's EnableMMU()/EnableIRQs())
//                                       never runs at all.
//   GPU64_MULTICORE_STRESS_ENABLED  -- within Run(), whether secondary
//                                       cores actually stream the stress
//                                       buffers, vs. returning immediately
//                                       (-> halt()/WFI, not a busy-spin).
//                                       Only meaningful if the above is on.
//
// Rounds so far: both off (clean menu, baseline) -- both on (VIC-II garbage
// before the menu is even reachable) -- ENABLED on/STRESS off (clean menu:
// bring-up alone is not the cause, see milestone6_3d_design.md). Both left
// off by default below: milestone 4 doesn't need multicore, so the resting
// state stays the plain, fully-proven single-core build. Toggle by hand for
// whichever round milestone 6 picks this back up with; not wired into
// tools/build.sh, deliberately, since these are throwaway diagnostic
// builds, not a shipping configuration.
// #define GPU64_MULTICORE_ENABLED
// #define GPU64_MULTICORE_STRESS_ENABLED

// gpu64: multicore feasibility spike for milestone 6 (see
// dev/milestone6_3d_design.md's "Architecture: a second core for the
// render loop" section) -- NOT the real render loop.
//
// Core 0 keeps running the existing, untouched, cycle-accurate
// reuUsingPolling() bus watch (started separately from CRAD::Run(), the
// same way it always has been -- this class does not touch core 0 at all).
//
// Cores 1-3 each continuously stream writes through a multi-megabyte
// buffer -- a render-loop-shaped memory access pattern, deliberately large
// enough to blow past the Cortex-A53's shared L2 (a plain register-resident
// counter, tried first, proves nothing about cache contention and was
// replaced after design review flagged it as testing the wrong thing: the
// real risk here is L2 *capacity* contention evicting core 0's hand-tuned
// preload schedule, not core-liveness or cache-coherency correctness).
//
// The pass/fail signal for this spike is NOT anything logged by this class
// -- it's whether milestone 3's existing hardware checks (mirror
// legibility, the $DF0B trigger flip, REU transfers staying byte-exact)
// still hold while this stress workload runs concurrently on another core.
// See progress_tracker.md for the test procedure and results.
class CGpu64MultiCore : public CMultiCoreSupport
{
public:
	// Per-core stress buffer size. Deliberately several times the RPi 3A+'s
	// shared L2 (Cortex-A53, on the order of 512KB-1MB) so each pass evicts
	// the whole cache, not just adds pressure to it. Comfortably inside
	// KERNEL_MAX_SIZE's 160MB budget even across three stress cores.
	static const unsigned STRESS_BUFFER_BYTES = 2 * 1024 * 1024;

	CGpu64MultiCore( CMemorySystem *pMemorySystem )
		: CMultiCoreSupport( pMemorySystem )
	{
	}

	~CGpu64MultiCore( void )
	{
	}

	// Called by Circle once per core (0-3) after Initialize() starts the
	// secondary cores. Core 0's branch here is intentionally empty --
	// core 0's real work is CRAD::Run(), called explicitly from main() the
	// same way it always has been, not through this dispatch.
	void Run( unsigned nCore );

	// Heartbeat: number of full passes each stress core has completed.
	// Read (never written) from core 0 for a one-shot before/after liveness
	// check -- confirms the cores are alive and running, but is not itself
	// the thing this spike is testing (see the class comment above).
	static volatile u32 s_PassCounter[ 4 ];

private:
	static void StressLoop( unsigned nCore );
};

#endif

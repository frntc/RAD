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
#include "gpu64_multicore.h"
#include "gpu64_ladder.h"
#include "gpu64_3d.h"

volatile u32 CGpu64MultiCore::s_PassCounter[ 4 ] = { 0, 0, 0, 0 };

// gpu64: one stress buffer per secondary core, static rather than
// heap-allocated -- simplest thing that works for a spike, and avoids
// pulling the memory allocator into a question this spike isn't about.
// 3 * STRESS_BUFFER_BYTES = 6MB, well inside KERNEL_MAX_SIZE's 160MB.
static u8 s_StressBuffer[ 3 ][ CGpu64MultiCore::STRESS_BUFFER_BYTES ];

void CGpu64MultiCore::Run( unsigned nCore )
{
	if ( nCore == 0 )
		return;	// core 0's real work is CRAD::Run(), called separately

	// gpu64: milestone 6 proper. One worker core, as the design says -- start
	// with core 0 plus one and expand only once that is measured. Cores 2 and
	// 3 return, i.e. go to halt()/WFI, so core 1's traffic is the only memory
	// traffic in the machine besides core 0's own. That matters: milestone 6a
	// showed a second core spinning on the wrong thing derails core 0 while
	// writing zero bytes, so every core that is not needed stays asleep.
	#ifdef GPU64_3D_ENABLED
	if ( nCore == 1 )
		gpu64_3dWorker();		// never returns
	return;
	#endif

	// gpu64: the milestone 6 load ladder (gpu64_ladder.h) supersedes the
	// stress spike below, and uses exactly one worker core -- the design
	// says start with core 0 plus one worker and expand only once that is
	// measured. Cores 2 and 3 return here, i.e. go to halt()/WFI, so the
	// ladder's rungs are the only memory traffic in the machine besides
	// core 0's own.
	#ifdef GPU64_LADDER_ENABLED
	#ifdef GPU64_LADDER_WORKER_ENABLED
	if ( nCore == 1 )
		gpu64_ladderWorker();
	#endif
	return;
	#endif

	// gpu64: GPU64_MULTICORE_STRESS_ENABLED toggles only this branch, never
	// whether the stress buffers are linked/sized (see s_StressBuffer
	// above, unconditional) -- per design review, changing the linked BSS
	// layout between test variants would itself be a confound, since RAD's
	// bus timing is layout-sensitive. This build variant (bring-up-only
	// control, see rad_main.h's GPU64_MULTICORE_ENABLED comment) leaves
	// secondary cores parked here -- per Circle's multicore.cpp, returning
	// from Run() takes the core to halt() (WFI), not a busy-spin.
	#ifdef GPU64_MULTICORE_STRESS_ENABLED
	StressLoop( nCore );
	#endif
}

void CGpu64MultiCore::StressLoop( unsigned nCore )
{
	u8 *pBuffer = s_StressBuffer[ nCore - 1 ];
	u32 nValue = 0;

	// gpu64: a render-loop-shaped access pattern -- large, sequential,
	// full-buffer writes, over and over, forever. Not trying to model any
	// particular future render loop precisely; just needs to generate
	// enough real memory traffic to contend for the shared L2 the way one
	// would, so the milestone 3 regression check (run concurrently with
	// this, on real hardware) can actually detect that contention if it's
	// a real problem.
	while ( 1 )
	{
		for ( unsigned i = 0; i < STRESS_BUFFER_BYTES; i++ )
			pBuffer[ i ] = (u8)( nValue + i );

		nValue++;
		s_PassCounter[ nCore ]++;
	}
}

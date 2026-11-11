// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- the ring buffer both cores talk through, and core 1's
 drain loop. See gpu64_3d.h and dev/milestone6_3d_design.md.

 Through Stage 14, what core 1 did was count: every class 1 opcode ran
 synchronously on core 0, inside the dispatch window where the C64 is already
 DMA-halted, and the ring only kept the cross-core path under real command
 traffic so the timing question milestone 6a opened stayed measurable while
 the pipeline was built.

 Stage 15a (dev/gap_filling_plan.md) gave execute() below a real job for
 three opcodes: CLEAR_VIEWPORT, DRAW_MESH and DRAW_NODE run for real on core
 1, via gpu64_3dExecuteRender() (gpu64_3d_class1.cpp, which still owns all
 the session state this needs and cannot export). 15a stalled the whole
 dispatch on the ring fully draining before it returned, for every one of
 the three -- a deliberate zero-concurrency window so there was exactly one
 owner of the framebuffer and the z-buffer at any instant, just relocated
 from "core 0, always" to "core 1, while core 0 waits".

 Stage 15b (same plan doc) narrows that stall to real observation points --
 page flip/vsync commit, a class 0 framebuffer op, a full ring -- instead of
 every dispatch, so a frame of DRAW_NODE calls actually queues and overlaps
 the C64 free-running instead of stalling on each one. This file's own job
 does not change: it still just drains the ring and, for the three render
 ops, calls gpu64_3dExecuteRender(). What changed is on the other side, in
 gpu64_3dDispatch() (gpu64_3d_class1.cpp) -- see the comment above
 drainAndFlush() there for what an observation point is and why every other
 class 1 opcode now drains the ring *before* running, not just after.
*/
#include "gpu64_3d_internals.h"
#include "gpu64_api.h"
#include <circle/util.h>

#ifdef GPU64_3D_ENABLED

Gpu64_3dRing        gpu64_3dRing;
Gpu64_3dWorkerStats gpu64_3dWorkerStats;
Gpu64_3dHostStats   gpu64_3dHost;

// Pushes one command onto the ring. Returns FALSE if it is full -- never
// waits, per the design's QUEUE_FULL rule. Core 0 side; it lives here rather
// than next to the dispatcher because the barrier below only makes sense read
// against the worker at the bottom of this file.
boolean gpu64_3dRingPush( u8 op, u8 nPage )
{
	const u32 head = gpu64_3dRing.head;
	const u32 next = ( head + 1 ) & GPU64_3D_RING_MASK;

	// The cached-tail check (gpu64_3d_internals.h): on a ring that is not
	// full this reads core-0-private state and never touches core 1's line.
	if ( next == gpu64_3dRing.tailCache )
	{
		gpu64_3dRing.tailCache = gpu64_3dRing.tail;	// the one coherence miss
		if ( next == gpu64_3dRing.tailCache )
			return FALSE;
	}

	Gpu64_3dCmd *pSlot = &gpu64_3dRing.slot[ head ];

	pSlot->op     = op;
	pSlot->flags  = 0;
	pSlot->id     = (u16)( gpu64Regs.id[ 0 ] | ( gpu64Regs.id[ 1 ] << 8 ) );
	pSlot->aux    = 0;
	pSlot->auxLen = 0;
	// Read once, here, on core 0 -- the whole point of the field. See
	// Gpu64_3dCmd::page (gpu64_3d_internals.h).
	pSlot->page   = nPage;
	memcpy( pSlot->arg, gpu64Regs.arg, GPU64_ARG_COUNT );

	// The slot must be visible before the head that advertises it. Without
	// this barrier core 1 can observe the new head against the previous
	// occupant of the slot -- a stale command executed with a fresh opcode,
	// which is the kind of bug that reproduces once an hour.
	asm volatile( "DMB ISH" ::: "memory" );

	gpu64_3dRing.head = next;
	return TRUE;
}

// --- core 1 side --------------------------------------------------------

// gpu64: the generic-timer event stream, so WFE wakes on its own instead of
// needing an SEV from core 0. Core 0 executing an SEV per command would put
// a store into its hot path for core 1's benefit, which is the wrong way
// round -- core 1 is the side with slack.
//
// Same mechanism gpu64_ladder.cpp uses; kept separate rather than shared
// because the ladder is a diagnostic build that will eventually be deleted.
#define GPU64_3D_EVNTI		8		// 2^8 ticks ~= 13us at 19.2MHz

static void enableEventStream( void )
{
	u64 v;
	asm volatile( "MRS %0, CNTKCTL_EL1" : "=r" (v) );
	v &= ~( (u64)0xf << 4 );
	v |= ( (u64)GPU64_3D_EVNTI << 4 );
	v |= ( (u64)1 << 2 );				// EVNTEN
	asm volatile( "MSR CNTKCTL_EL1, %0" :: "r" (v) );
	asm volatile( "ISB" );
}

// Records one drained command, and, for the render opcodes, actually
// executes it. pCmd is non-const now: gpu64_3dExecuteRender()/
// gpu64_3dExecuteRenderScene() write their result back into the slot for a
// later drainAndFlush() or pollLoopFrame() (both gpu64_3d_class1.cpp) to
// read.
//
// Every opcode besides those is still legal here with no handler: core 0
// still executes all of those itself, synchronously, but as of Stage 15b it
// does so *after* this drain loop has seen the entry go by, not before --
// gpu64_3dDispatch() pushes, then drains to here, then calls its own
// execute(u8) (gpu64_3d_class1.cpp). This function's count is the only trace
// any of them leaves on core 1. That is why unknownOp below is not a fault
// counter: it is expected to climb continuously, on every non-render class 1
// command, and gpu64_3dReport() (gpu64_3d_class1.cpp) does not treat it as
// one. GPU64_3D_OP_RENDER_SCENE is Stage 16's fourth real handler here --
// see its own case below and gpu64_3d_internals.h for what it is.
static void execute( Gpu64_3dCmd *pCmd )
{
	gpu64_3dWorkerStats.lastOp = pCmd->op;

	switch ( pCmd->op )
	{
	case GPU64_3D_OP_CLEAR_VIEWPORT:
	case GPU64_3D_OP_DRAW_MESH:
	case GPU64_3D_OP_DRAW_NODE:
		gpu64_3dExecuteRender( pCmd );
		break;

	// Stage 16: the autonomous loop's one synthetic op -- see its
	// definition in gpu64_3d_internals.h. Never arrives here except via
	// gpu64_3dDispatch()'s own LOOP_START/SCENE_COMMIT handlers.
	case GPU64_3D_OP_RENDER_SCENE:
		gpu64_3dExecuteRenderScene( pCmd );
		break;

	default:
		gpu64_3dWorkerStats.unknownOp++;
		break;
	}
}

void gpu64_3dWorker( void )
{
	enableEventStream();

	u32 tail = 0;

	while ( 1 )
	{
		if ( tail == gpu64_3dRing.head )
		{
			// Idle. WFE parks the core until the event stream fires, which
			// costs no bus cycle and no store -- the two things milestone 6a
			// proved core 0 cannot tolerate from another core.
			asm volatile( "WFE" );
			continue;
		}

		gpu64_3dWorkerStats.wakeups++;

		while ( tail != gpu64_3dRing.head )
		{
			// Pairs with the DMB in ringPush(): the head we just read must not
			// let us see the slot's previous contents.
			asm volatile( "DMB ISH" ::: "memory" );

			execute( &gpu64_3dRing.slot[ tail ] );

			tail = ( tail + 1 ) & GPU64_3D_RING_MASK;

			// For the three render ops, execute() just wrote pCmd->err/result
			// into the slot gpu64_3dDispatch() is stalled waiting to read back
			// (waitForDrain(), gpu64_3d_class1.cpp). Those writes must be
			// visible before the tail that tells core 0 to go read them --
			// pairs with the DMB core 0 issues after observing tail == head.
			asm volatile( "DMB ISH" ::: "memory" );

			// Publishing the tail per command rather than per batch keeps a
			// producer that is filling the ring from stalling on a consumer
			// that is draining it.
			gpu64_3dRing.tail = tail;
			gpu64_3dWorkerStats.consumed++;
		}
	}
}

#endif	// GPU64_3D_ENABLED

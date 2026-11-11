// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: the split page-flip mailbox transaction. See gpu64_flip.h for why
 this exists and why draining is a correctness requirement, not an
 optimisation detail.
*/
#include "gpu64_flip.h"
#include <circle/memory.h>
#include <circle/synchronize.h>
#include "lowlevel_arm64.h"

GPU64FLIPSTATS gpu64FlipStats = { 0, ~0u, 0, 0, 0, ~0u, 0, 0, 0, 0, 0 };

// The property buffer, as u32 words. Laid out once by gpu64_flipInit() and
// then only word 6 ever changes:
//
//   0  total buffer size in bytes
//   1  request/response code
//   2  tag id                       PROPTAG_SET_VIRTUAL_OFFSET
//   3  value buffer size            8
//   4  value length                 8 on request, 0x80000008 in the response
//   5  x offset                     always 0 -- pages stack vertically
//   6  y offset                     the flip
//   7  end tag
#define FLIP_WORDS		8
#define FLIP_BUFFER_BYTES	( FLIP_WORDS * 4 )

#define PROPTAG_SET_VIRTUAL_OFFSET	0x00048009

static volatile u32 *sBuffer     = 0;	// in the coherent (uncached) region
static u32           sBusAddress = 0;	// what the VideoCore is handed, and
					// what it echoes back on completion
static boolean       sAvailable  = FALSE;
static boolean       sPending    = FALSE;

static inline u32 flipNow( void )
{
	return read32( ARM_SYSTIMER_CLO );
}

boolean gpu64_flipInit( void )
{
	// Coherent slot 3: the first one Circle does not spoken for (0 is
	// CBcmPropertyTags' own buffer, 1 the GPIO virtual buffer, 2 the touch
	// buffer, and VCHIQ starts at 128). Deliberately *not* slot 0 -- that
	// one is reused by every other property call in the system, including
	// the ones this module's drain exists to protect.
	//
	// The coherent region is mapped uncached, so the VideoCore sees these
	// stores with no cache maintenance and the reply needs no invalidate.
	// A whole 4KB page for 32 bytes is wasteful and entirely worth it.
	uintptr pPage = CMemorySystem::GetCoherentPage( 3 );
	if ( pPage == 0 )
		return FALSE;

	sBuffer = (volatile u32 *)pPage;

	sBuffer[ 0 ] = FLIP_BUFFER_BYTES;
	sBuffer[ 1 ] = 0;				// CODE_REQUEST
	sBuffer[ 2 ] = PROPTAG_SET_VIRTUAL_OFFSET;
	sBuffer[ 3 ] = 8;
	sBuffer[ 4 ] = 8;
	sBuffer[ 5 ] = 0;
	sBuffer[ 6 ] = 0;
	sBuffer[ 7 ] = 0;				// PROPTAG_END

	// The low 4 bits carry the channel number, so the buffer has to be
	// 16-byte aligned for the address and the channel not to collide. A
	// page start always is; assert it anyway rather than corrupt a mailbox
	// write if the region ever moves.
	sBusAddress = BUS_ADDRESS( (uintptr)pPage );
	if ( ( sBusAddress & 0xF ) != 0 )
		return FALSE;

	sPending   = FALSE;
	sAvailable = TRUE;
	return TRUE;
}

boolean gpu64_flipAvailable( void )
{
	return sAvailable;
}

boolean gpu64_flipPost( u32 nOffsetY )
{
	if ( !sAvailable || sPending )
		return FALSE;

	// Reset the code, not just the offset: the VideoCore overwrote word 1
	// with its response code and word 4 with the response-flagged length on
	// the previous flip, and a stale response code would be read back as
	// success without the request ever being looked at.
	sBuffer[ 1 ] = 0;				// CODE_REQUEST
	sBuffer[ 4 ] = 8;
	sBuffer[ 6 ] = nOffsetY;

	// Uncached memory still needs the stores ordered before the mailbox
	// write that publishes them.
	DataSyncBarrier();

	// Full means the VideoCore has not drained our previous request. With
	// one flip in flight at a time it is not supposed to happen -- but this
	// runs inside gpu64_vsyncCommitFlip()'s DMA hold, on core 0, in the
	// polling loop. An unbounded spin on an MMIO register there is not a
	// slow path, it is a Pi that never samples the bus again; the C64 then
	// reads a floating IO2 and derails, which is precisely the signature
	// the stage 16 bench runs kept producing. So: bounded, counted, and on
	// expiry we decline the flip rather than post into a full mailbox.
	// CommitFlip() falls back to Circle's blocking call for this frame.
	u32 tFull = flipNow();
	while ( read32( MAILBOX1_STATUS ) & MAILBOX_STATUS_FULL )
	{
		if ( (s32)( flipNow() - tFull ) > GPU64_FLIP_POST_FULL_TIMEOUT_US )
		{
			gpu64FlipStats.postFullTimeouts++;
			return FALSE;
		}
	}

	write32( MAILBOX1_WRITE, BCM_MAILBOX_PROP_OUT | sBusAddress );

	sPending = TRUE;

	// Deliberately not timed here: the number that matters is how long the
	// *whole* commit chain holds the C64, not how long this function takes.
	// gpu64_commitFlip() in gpu64_fb.cpp owns the measurement.
	return TRUE;
}

void gpu64_flipDrain( void )
{
	if ( !sPending )
		return;

	u32 t0 = flipNow();

	for ( ;; )
	{
		if ( !( read32( MAILBOX0_STATUS ) & MAILBOX_STATUS_EMPTY ) )
		{
			u32 nResult = read32( MAILBOX0_READ );
			// Channel in the low nibble, exactly as CBcmMailBox::Read()
			// does it. Anything not addressed to the property channel is
			// not ours and is discarded the same way Circle would.
			if ( ( nResult & 0xF ) == BCM_MAILBOX_PROP_OUT )
				break;
			continue;
		}

		// Signed difference so this stays correct across the system
		// timer's 32-bit wrap.
		if ( (s32)( flipNow() - t0 ) > GPU64_FLIP_DRAIN_TIMEOUT_US )
		{
			// The VideoCore never answered. Give up on the fast path for
			// the rest of the session rather than pay this timeout on
			// every flip; CommitFlip() falls back to Circle's blocking
			// call, which is slow but proven.
			//
			// Counted since 2026-09-06: this happens inside the DMA hold,
			// so one of these is a GPU64_FLIP_DRAIN_TIMEOUT_US C64 halt,
			// and every flip after it is a ~900 us halt once a frame. That
			// is a session-wide behaviour change nothing used to report.
			gpu64FlipStats.drainTimeouts++;
			sAvailable = FALSE;
			sPending   = FALSE;
			return;
		}
	}

	DataMemBarrier();
	sPending = FALSE;

	u32 dt = flipNow() - t0;
	gpu64FlipStats.drainCount++;
	gpu64FlipStats.drainTotalUs += dt;
	if ( dt < gpu64FlipStats.drainMinUs ) gpu64FlipStats.drainMinUs = dt;
	if ( dt > gpu64FlipStats.drainMaxUs ) gpu64FlipStats.drainMaxUs = dt;
}

void gpu64_flipWarm( void )
{
	// gpu64: with the mailbox wait gone, the commit's remaining cost is
	// mostly the cold instruction cache of the path itself -- which the
	// ~900 us round trip used to render invisible. Warmed from the PAGE_FLIP
	// dispatch alongside gpu64_vsyncCommitFlip(), for the same reason: the
	// bus is already held there. See dev/progress_tracker.md's polling-loop
	// rules.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_flipPost, 1024 );
	FORCE_READ_LINEARa( (void*)gpu64_flipPost, 1024, 1024 );
}

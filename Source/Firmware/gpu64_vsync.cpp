// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: frame clock calibration. See gpu64_vsync.h for why this exists and
 why the bus-watch loop never calls any of it.
*/
#include "gpu64_vsync.h"
#include "gpu64_fb.h"

GPU64VSYNC gpu64Vsync = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

// One measurement pass: 31 blocking WaitForVerticalSync() mailbox round
// trips (one to find the boundary, GPU64_VSYNC_CAL_FRAMES to time). Split
// out of gpu64_vsyncCalibrate() so that function can retry a failed pass
// without duplicating it -- see the retry loop below for why a single pass
// is not reliable enough to trust at boot.
static boolean calibrateOnce( CGpu64FrameBuffer *pFB )
{
	// Start on a real boundary, so the span below covers whole frames and
	// not a partial one plus the latency of getting here.
	if ( !pFB->WaitForVSync() )
		return FALSE;
	u32 t0 = gpu64_vsyncNow();

	for ( unsigned i = 0; i < GPU64_VSYNC_CAL_FRAMES; i++ )
		if ( !pFB->WaitForVSync() )
			return FALSE;
	u32 t1 = gpu64_vsyncNow();

	u32 period = ( t1 - t0 ) / GPU64_VSYNC_CAL_FRAMES;
	if ( period < GPU64_VSYNC_MIN_PERIOD || period > GPU64_VSYNC_MAX_PERIOD )
		return FALSE;

	gpu64Vsync.periodUs   = period;
	gpu64Vsync.nextUs     = t1 + period;
	gpu64Vsync.frameCount = 0;
	gpu64Vsync.calibrated = 1;
	return TRUE;
}

// Retried up to GPU64_VSYNC_CAL_ATTEMPTS times: a single WaitForVerticalSync()
// failure this early is plausibly the HDMI monitor still locking sync right
// after power-on, not a display that will never sync at all (gpu64_vsync.h's
// own comment already names "a failed mailbox or a display that never syncs"
// as the two cases this guards against, but originally gave the first no
// second chance). Found 2026-08-30: a hardware run had every SCENE_COMMIT
// fail UNSUPPORTED for the whole session because this boot-time call failed
// once and nothing ever retried it -- Stage 16's SCENE_COMMIT is the first
// caller to hard-require gpu64Vsync.calibrated with no immediate-flip
// fallback the way PAGE_FLIP(ARG0=0) has, so this is the first opcode to
// make a cold calibration failure fatal to a whole feature instead of just
// leaving one optional command UNSUPPORTED. Cost of retrying: each failed
// attempt is one failed mailbox call (cheap) rather than a full timed pass,
// except the attempt that actually gets going, which costs the usual ~0.5s.
#define GPU64_VSYNC_CAL_ATTEMPTS	5

boolean gpu64_vsyncCalibrate( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 || !pFB->IsInitialized() )
		return FALSE;

	for ( unsigned attempt = 0; attempt < GPU64_VSYNC_CAL_ATTEMPTS; attempt++ )
		if ( calibrateOnce( pFB ) )
			return TRUE;

	return FALSE;
}

boolean gpu64_vsyncReanchor( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 || !pFB->IsInitialized() || !gpu64Vsync.calibrated )
		return FALSE;

	if ( !pFB->WaitForVSync() )
		return FALSE;

	gpu64Vsync.nextUs = gpu64_vsyncNow() + gpu64Vsync.periodUs;
	return TRUE;
}

void gpu64_vsyncResetState( void )
{
	gpu64Vsync.armed         = 0;
	gpu64Vsync.flipPending   = 0;
	gpu64Vsync.commitDue     = 0;
	gpu64Vsync.irqRequest    = 0;
	gpu64Vsync.irqReleaseReq = 0;
	// irqAsserted is deliberately left alone: it describes the state of a
	// physical pin, and only the bus-watch loop may change that. If the line
	// is still held, the loop's release path clears it.
	if ( gpu64Vsync.irqAsserted )
		gpu64Vsync.irqReleaseReq = 1;

	// A fresh session starts its frame numbering at zero, but the clock
	// itself keeps running -- the display did not stop between sessions.
	gpu64Vsync.frameCount = 0;
}

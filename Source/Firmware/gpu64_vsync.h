// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: the frame clock -- gpu64's vblank event source.

 The problem this solves: nothing in reuUsingPolling()'s bus-watch loop
 knows where the display raster is, so every vblank-related part of the API
 (docs/api_design.md) shipped as UNSUPPORTED in milestone 4. Circle does
 expose CBcmFrameBuffer::WaitForVerticalSync(), but it is a *blocking*
 mailbox round-trip to the VideoCore: calling it from the loop would stall
 the loop for up to a whole frame, which is exactly what the
 cycle-predictability rule forbids.

 So the loop never asks the VideoCore anything. Instead the frame period is
 measured once, at boot, by timing a run of WaitForVerticalSync() calls
 against the free-running 1MHz ARM system timer -- and from then on the loop
 finds the next frame boundary with a single MMIO read and a compare
 (gpu64_vsyncDue() below), which costs no more than the two
 read32(ARM_GPIO_GPLEV0) samples the loop already does every pass.

 This works because both clocks derive from the same 19.2MHz crystal on a
 Pi 3, so the ratio between them is fixed and the extrapolation does not
 wander the way two independent oscillators would. It is not exact, though:
 the measurement carries the mailbox's own latency as a constant offset, and
 residual period error accumulates over minutes. Hence gpu64_vsyncReanchor()
 -- one blocking vsync wait, so up to one frame of C64 halt -- exposed to the
 C64 as VBLANK_SYNC ($09) and done implicitly by VBLANK_ARM. Both are
 setup-time commands, not per-frame ones.
*/
#ifndef _gpu64_vsync_h
#define _gpu64_vsync_h

#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/types.h>

// How many frames to average the period over at boot. At 60Hz this costs
// half a second, spent before the C64 is doing anything gpu64 cares about.
// Longer is more accurate: the 1us timer resolution divided across N frames
// is the per-frame error, so 30 frames puts it around 0.03us/frame, i.e.
// roughly a millisecond of drift per 30000 frames (~8 minutes).
#define GPU64_VSYNC_CAL_FRAMES	30

// Sanity bounds on the measured period, in microseconds -- ~25Hz to ~333Hz.
// A measurement outside this is a failed mailbox or a display that never
// syncs, not a real mode, and leaves the clock uncalibrated (so every vblank
// feature keeps returning UNSUPPORTED rather than firing at a made-up rate).
#define GPU64_VSYNC_MIN_PERIOD	3000
#define GPU64_VSYNC_MAX_PERIOD	40000

struct GPU64VSYNC
{
	u32	periodUs;	// measured frame period
	u32	nextUs;		// system-timer value the next frame boundary is due at
	u32	frameCount;	// frame boundaries seen since calibration

	u8	calibrated;	// periodUs/nextUs are meaningful
	u8	armed;		// vblank IRQ armed (mirrors STATUS bit3)
	u8	flipPending;	// a deferred PAGE_FLIP is waiting for the boundary

	// The frame boundary has arrived and the flip is owed, but the commit
	// has not been allowed to take the bus yet. See gpu64_vsyncCommitFlip()
	// in rad_reu.cpp for why the commit waits for a sampled IO2 access.
	u8	commitDue;
	u32	commitDueFrame;	// frameCount when commitDue was raised
	u32	commitLateMax;	// worst frames-late a commit has ever run

	// IRQ handshake, deliberately shaped like REU's own irqTriggered /
	// irqRelease pair in rad_reu.h: the API sets a request, and the loop --
	// the only code allowed to touch bIRQ_OUT -- acts on it.
	u8	irqRequest;	// raise the cartridge IRQ at the next opportunity
	u8	irqAsserted;	// gpu64 is currently driving bIRQ_OUT low
	u8	irqReleaseReq;	// VBLANK_ACK asked for the line to be let go

};

extern GPU64VSYNC gpu64Vsync;

// --- provided by rad_reu.cpp (they need the DMA macros and REU state) ---
// Commits a pending deferred PAGE_FLIP inside a bounded DMA hold. Called
// only from the bus-watch loop, and only from its sampled-IO2 gate -- never
// straight off the frame boundary. See the gate's comment in rad_reu.cpp.
void gpu64_vsyncCommitFlip( void );
// Preloads the above into the instruction cache. Called from the PAGE_FLIP
// dispatch, which already holds the bus.
void gpu64_vsyncWarmCommit( void );

// Measures the frame period. Blocking (GPU64_VSYNC_CAL_FRAMES frames) --
// boot only, never from inside the bus-watch loop.
boolean gpu64_vsyncCalibrate( void );

// Re-pins the extrapolation to a real vsync without re-measuring the period.
// Blocking, but bounded by one frame. Safe from a command dispatch, which
// already holds the C64 halted for its duration.
boolean gpu64_vsyncReanchor( void );

// Clears the per-session dynamic state (armed, pending flip, IRQ handshake)
// while keeping the calibration, which is a property of the display and
// survives any number of REU sessions.
void gpu64_vsyncResetState( void );

static inline u32 gpu64_vsyncNow( void )
{
	return read32( ARM_SYSTIMER_CLO );
}

// The whole per-pass cost of the frame clock in the bus-watch loop: one
// compare against a value already in cache. Signed difference so it stays
// correct across the system timer's 32-bit wrap (~71 minutes).
static inline int gpu64_vsyncDue( u32 now )
{
	return gpu64Vsync.calibrated && (s32)( now - gpu64Vsync.nextUs ) >= 0;
}

// What happens at a frame boundary, minus the page flip. Inline, and it
// deliberately touches nothing but this struct: it runs in the bus-watch
// loop *before* the loop has sampled the bus for this cycle, so any time
// spent here is time the C64's next IO2 access could go unseen -- milestone
// 4's bug #2. A cross-TU call with a cold instruction cache is exactly the
// kind of delay that costs a cycle, so there isn't one.
static inline void gpu64_vsyncAdvance( u32 now )
{
	// Step to the boundary after the one that just passed. If we are more
	// than a whole frame late -- a long command ran through one -- do not
	// try to catch up by firing a burst of missed vblanks; just re-pin to
	// now, since a frame the C64 never got to act on is not worth
	// reporting.
	gpu64Vsync.nextUs += gpu64Vsync.periodUs;
	if ( (s32)( now - gpu64Vsync.nextUs ) >= 0 )
		gpu64Vsync.nextUs = now + gpu64Vsync.periodUs;

	gpu64Vsync.frameCount++;

	if ( gpu64Vsync.armed )
		gpu64Vsync.irqRequest = 1;
}

#endif

// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: why did class 1 stop accepting commands?

 Built 2026-09-10 from two bench photographs. Both runs of the quake3d
 instrument entered a state they never left: SCENE_COMMIT answered
 UNSUPPORTED ($06) on every remaining frame -- 2011 of 2182 in the first
 run, 11832 of 17326 in the second -- while the C64 side kept running,
 kept dispatching, and never stalled. On screen that is a frozen HDMI
 picture with a program that looks alive, which is exactly how the user
 described it ("it hanged the first time I touched it").

 $06 was already the right answer to show, but it is produced in four
 unrelated places and the screen could not say which:

   gpu64_apiDispatch()   class 1 or 2 issued while the display is not in
                         graphics mode -- refuses the whole class, so the
                         node updates stop landing too.
   SCENE_COMMIT          !s_LoopRunning: something stopped the loop.
   SCENE_COMMIT          !gpu64Vsync.calibrated: no frame clock. (Cannot
                         happen mid-run -- the flag is only ever set --
                         but it is one line to prove rather than assume.)
   SCENE_COMMIT          g_pGpu64FB == 0.

 Only the second is reachable more than once per session by anything the
 C64 does, and it has three producers of its own -- the LOOP_STOP opcode,
 SCENE_RESET, and gpu64_3dReset() on a session teardown -- so the counters
 below are paired with `loopStopCause`, which names the last one to fire.
 That turns "the loop is not running" into "a LOOP_STOP opcode executed at
 some point after LOOP_START", which is a statement about the command
 stream and therefore about the known lost/mis-sampled write defect.

 Read it with GET_HEALTH ($0A) bytes 80-95 (93-95 added after bench
 run 11 answered the which-of-four question and asked a sharper one). Everything here is written
 with the bus already held (dispatch time), so none of it costs the
 polling loop anything -- see the "Multicore" section of CLAUDE.md on why
 that distinction matters.

 Deliberately NOT reset by resetREU(): a session teardown is one of the
 things being measured, and zeroing the evidence in the same call that
 produces it would hide it. Callers take a difference between two
 GET_HEALTH snapshots, so a non-zero starting point costs nothing.
*/
#ifndef _gpu64_apidiag_h
#define _gpu64_apidiag_h

#include <circle/types.h>

// gpu64ApiDiag.loopStopCause
#define GPU64_LOOPSTOP_NONE		0
#define GPU64_LOOPSTOP_OPCODE		1	// the LOOP_STOP opcode ran
#define GPU64_LOOPSTOP_SCENE_RESET	2	// SCENE_RESET ran
#define GPU64_LOOPSTOP_SESSION		3	// gpu64_3dReset(), i.e. resetREU()
#define GPU64_LOOPSTOP_INIT		4	// gpu64_3dInit(), boot only
#define GPU64_LOOPSTOP_FULL_RESET	5	// the FULL_RESET opcode ran

// gpu64_apiDiagStateByte()
#define GPU64_DIAG_LOOP_RUNNING		0x01
#define GPU64_DIAG_CALIBRATED		0x02
#define GPU64_DIAG_MODE_GRAPHICS	0x04
#define GPU64_DIAG_FLIP_PENDING		0x08
#define GPU64_DIAG_FRAME_READY		0x10
#define GPU64_DIAG_BUSY			0x20
#define GPU64_DIAG_HAVE_FB		0x40

typedef struct
{
	u32	commitNoLoop;	// SCENE_COMMIT refused: the loop is not running
	u32	commitNoClock;	// SCENE_COMMIT refused: no calibrated frame clock
	u32	classRefusedMode;	// class 1/2 refused: display not in graphics mode
	u32	loopStops;		// times s_LoopRunning went TRUE -> FALSE

	u8	loopStopCause;	// GPU64_LOOPSTOP_*, the most recent one
	u8	lastRefuseState;	// gpu64_apiDiagStateByte() at the last refusal
	u8	lastRefuseOp;	// CMD_LO of that refusal
	u8	lastRefuseClass;	// CMD_HI of that refusal

	// gpu64 (2026-09-10, bench run 11): the provenance of whatever stopped
	// the loop. Run 11 caught the freeze in the act and answered the
	// which-of-four question -- commitNoLoop 32, loopStopCause 1, i.e. a
	// LOOP_STOP *opcode* executed at a point where the C64 program had not
	// reached its teardown and never sends one. So class 1 dispatched a $07
	// nobody wrote. These three say where that $07 came from, and they
	// split the two mechanisms that can produce it:
	//
	//   dispSeq == the previous dispatch's seq  ->  no new command arrived
	//     at all. The SEQ write never happened, so this was a phantom or
	//     duplicated CMD_LO write. Refusing any command whose SEQ has not
	//     advanced would kill the entire class, and the C64's existing
	//     SEQACK retry already copes with a refusal.
	//   dispSeq == previous + 1  ->  a real command arrived and its
	//     *data* byte was mis-sampled: the C64 wrote (almost certainly)
	//     $08 SCENE_COMMIT and the loop latched $07. SEQ gating cannot
	//     see that one; only a checksum or an opcode echo could.
	//   dispSeq further ahead  ->  commands were lost before this one too.
	//
	// dispOpPrev names the opcode that ran immediately before, which is
	// what says whether the stream was mid-frame or mid-teardown.
	//
	// dispSeq/dispOp are the live pair, updated at the top of every
	// gpu64_apiDispatch(); the loopStop* three are the frozen copy taken
	// the moment s_LoopRunning was cleared. GET_HEALTH bytes 93-95 report
	// the frozen copy.
	u8	dispSeq;		// SEQ this dispatch is acting on
	u8	dispSeqPrev;	// SEQ the dispatch before it acted on
	u8	dispOp;		// CMD_LO of this dispatch
	u8	dispOpPrev;		// CMD_LO of the dispatch before it

	u8	loopStopSeq;	// dispSeq at the moment the loop stopped
	u8	loopStopSeqPrev;	// dispSeqPrev at that moment
	u8	loopStopPrevOp;	// dispOpPrev at that moment

	// gpu64 (2026-09-10, bench run 14): run 11 proved a LOOP_STOP nobody
	// sent could execute. Run 14 proved the same mechanism reaches a
	// *destructive* opcode: the live scene lost its active camera at around
	// frame 2400 of 4096 and never got it back, so every LOOP_START after
	// that answered NO_CAMERA and every SCENE_COMMIT re-arm with it. Two
	// opcodes can do that -- SCENE_RESET ($00) and DESTROY_NODE ($22) on
	// the active camera -- and nothing in the block above could say which.
	//
	// The four below both prevent it and name it. Every destructive opcode
	// now demands the one-shot key in ARG15 (GPU64_KEY_DESTRUCTIVE,
	// gpu64_api.h), so a phantom one is refused rather than obeyed -- and
	// the refusal is counted here WITH ITS OPCODE, which is the reading
	// that was missing. camLost and sceneWipes count the events getting
	// through, so a run can still distinguish "prevented" from "never
	// happened".
	//
	// GET_HEALTH ($0A) bytes 76-79, which were reserved for exactly this.
	u32	keyRefused;		// destructive opcodes refused: no valid key
	u32	camLost;		// live scene's active camera went away
	u32	sceneWipes;		// SCENE_RESETs that executed on a built scene
	u8	keyRefusedOp;	// CMD_LO of the last key refusal -- the phantom's name
	u32	checkRefused;	// checked commands refused: ARG14 did not match (gpu64_api.h)
	u32	strictRefused;	// unchecked class 1 commands refused in strict mode
	u8	strictRefusedOp;	// CMD_LO of the last one -- the phantom's name
	u32	commitScanBusy;		// SCENE_COMMITs refused BUSY because the page
				// they would hand core 1 was still being scanned
} GPU64APIDIAG;

extern GPU64APIDIAG gpu64ApiDiag;

// The seven flags above, sampled now. Lives in gpu64_api.cpp because it is
// the only translation unit that can see all four of the framebuffer, the
// register file, the frame clock and (through gpu64_3dLoopRunning()) the
// class 1 loop.
u8 gpu64_apiDiagStateByte( void );

// Saturating, because these ride single bytes on the C64's screen and the
// question every one of them answers is which-one, not how-many.
static inline void gpu64_apiDiagBump( u32 *p )
{
	if ( *p < 0xffffffff ) ( *p )++;
}

#endif

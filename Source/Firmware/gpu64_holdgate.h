// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: the hold gate -- proving the C64's *next* cycle is a read.

 Built 2026-09-07 to close the hole that polling-loop rule 7 was written
 around rather than through. Rule 7 says a DMA hold may only be opened
 immediately after a sampled IO2 access (or, for the mirror, a read of
 $FFFF), because asserting bDMA_OUT pulls RDY *and* AEC: RDY halts the 6510
 only on a read cycle, while AEC tri-states the address bus regardless, so a
 write that is already in flight completes at a floating address and corrupts
 the C64. That is the Stage 16 derail, PASS 2026-09-06.

 The rule rested on a claim about instruction shapes: "an IO2 access is
 always the last cycle of its instruction, so the next cycle is an opcode
 fetch." That claim is true for absolute loads and stores, which is all the
 API itself uses, and false for two shapes a C64 program is perfectly
 entitled to emit:

   inc $DF52      c4 reads $DF52, c5 writes the old value, c6 the new one.
                  Firing after c4 asserts DMA into c5, a write.
   inc $DF52,X    c4 is a dummy read at the *un-fixed* address, c5 the real
                  read, c6/c7 the writes. Firing after c5 asserts into c6.
   sta $DF52,X    c4 is an unconditional dummy read at the un-fixed address
                  -- in IO2 -- and c5 is the write.

 The exposure was recorded as an API constraint and carried since milestone
 4. This module removes it.

 The replacement rule. Do not fire on the access; *arm* on it, and let the
 hold open later, on the first pair of cycles that proves itself. Let N be
 the cycle the previous pass of the loop sampled and N+1 the one the current
 pass just sampled. A hold may open during N+1 iff all four of these hold:

   1. N was a sampled IO2 access, or the $FFFF vector read;
   2. the previous pass really was the previous C64 cycle;
   3. N+1 is a read (CPU_READS_FROM_BUS);
   4. N+1's low address byte differs from N's.

 Note that the arming cycle is not in that list. The four terms are
 self-contained -- they are a statement about N and N+1 alone -- so the arm
 is only a request for the bus, and it stays pending until some later pair
 qualifies. That is what makes the rule safe through an `inc $DF52`: the arm
 taken on c4 cannot fire on c5 or c6 (both writes) and lands on the opcode
 fetch after c6, which is exactly right.

 Then cycle N+2 is provably a read as well, and that is what makes the hold
 safe -- because *no NMOS 6502 instruction, documented or undocumented, ever
 writes before its third cycle*, and neither does the interrupt sequence
 (two dummy reads first). So if N+1 is a read that is not part of the same
 access as N, N+1 is either an opcode fetch or an operand/dummy fetch, and
 N+2 cannot be a write.

 Term 4 is the one that is easy to get wrong. It exists for `inc $DFxx,X`,
 whose c4 dummy read and c5 real read are two different reads: without it,
 c5 would confirm c4's arm and the hold would open into c6, a write. For
 indexed addressing the un-fixed and the fixed address differ only in their
 *high* byte, so their low bytes are identical -- and the low byte is exactly
 what the loop can see on every cycle (ADDRESS0to7, sampled from the
 multiplexed bus before the mux flips). Term 4 also subsumes "N+1 is not the
 same IO2 register", so there is no separate test for that.

 Term 2 is what makes term 4 mean anything: two passes are the same C64 cycle
 apart only if the loop did not miss one in between. RESTART_CYCLE_COUNTER
 already stamps every pass at the same point in the cycle, and armPerC64
 (gpu64_holdgap.h) is the measured conversion, so the test is one subtract
 and one compare against 1.5 C64 cycles. A missed cycle is 2.0 away and
 fails it.

 Cost. Three register moves and one compare per pass of reuUsingPolling(),
 all of it *after* the bus sample, plus one predictable branch. The gate
 state lives in loop registers; only the counters below touch memory, and
 they are one 64-byte line kept resident by both warm paths (rule 4).

 What it costs the API. A commit or a mirror snapshot now lands on the first
 confirmed cycle pair after the access that armed it, rather than on the
 access itself. In handshake mode that is the next instruction, because the
 program is already polling STATUS; gpu64HoldGate.ageMax records the worst
 wait in C64 cycles and gpu64Vsync.commitLateMax the worst in frames.
 The dispatch path is unchanged in the common case (see rad_reu.cpp): it only
 defers when the immediately preceding cycle was an IO2 read at the same
 address, which is exactly the read-modify-write signature.

 Residuals, stated rather than hidden:

   - If the loop drops the sample for cycle N (the ~1/180000 defect in
     gpu64-io2-sampling-reliability), an RMW's second write can look like an
     unambiguous store and dispatch early. Bounded by the drop rate, and only
     for a program that does RMW on CMD_LO.
   - A deferred dispatch fires at the next confirmed-safe cycle, so a program
     that stages new ARG bytes between writing CMD_LO and polling STATUS
     would have them seen by the deferred command. The documented protocol
     is "write CMD_LO last, then poll STATUS", which cannot hit this -- and
     the RMW case, the one that actually defers, resolves on the very next
     cycle anyway (its second write dispatches immediately).
   - gpu64_mirrorIdleLoop() keeps the old unconfirmed gate: it runs before
     armPerC64 is calibrated, and the only code alive at that point is the
     KERNAL/BASIC IRQ path, which contains no RMW or indexed access to $FFFF.
*/
#ifndef _gpu64_holdgate_h
#define _gpu64_holdgate_h

#include <circle/types.h>

// What the gate is holding, if anything. Only one may be armed at a time.
// They are mutually exclusive in practice -- a mirror tick needs the API
// idle, a commit needs it active -- and a deferred dispatch, being a command
// the C64 is waiting on, wins over both.
#define GPU64_GATE_NONE			0
#define GPU64_GATE_COMMIT		1
#define GPU64_GATE_MIRROR		2
#define GPU64_GATE_DISPATCH		3
#define GPU64_GATE_SNIFF		4		// gpu64_sniff.h, diagnostic builds only
#define GPU64_GATE_MIRROR_IDLE	5		// the C64U's lone-$FFFF mirror clock (rad_reu.cpp)

// What the previous pass sampled, packed with the low address byte so the
// confirm's address test and the dispatch's ambiguity test are each a single
// compare. 0 means "nothing the gate can reason about". The $FFFF vector
// fetch is recorded as a read like any other; no IO2 register shares its low
// byte with $FF, so the encodings cannot collide.
#define GPU64_ANCHOR_WRITE		0x100
#define GPU64_ANCHOR_READ		0x300

// One 64-byte cache line, written only by core 0, read at dispatch time by
// the log path. Deliberately no section() attribute -- see the milestone-14
// linker-orphan regression in dev/progress_tracker.md.
typedef struct
{
	u32	armed;				// gates armed: accesses that wanted the bus
	u32	fired;				// arms that confirmed and opened a hold
	u32	declined;			// passes an armed gate could not fire on --
							//   the bulk of these are simply "no IO2 access
							//   on the previous cycle"
	u32	ageMax;				// worst wait, in C64 cycles, from arm to fire
	u32	dispatchDeferred;	// CMD_LO writes that took the deferred path
	u32	dispatchFired;		// deferred dispatches that eventually ran
	u32	pad[ 2 ];
} GPU64HOLDGATE;

extern GPU64HOLDGATE gpu64HoldGate;

#endif

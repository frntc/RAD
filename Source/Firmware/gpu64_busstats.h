// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: did the loop see the access, and did it see the right address?

 Built 2026-09-09 to settle one question left open by the 62291-frame Quake
 run of 2026-09-08. That run was clean on everything the screen could report
 -- STALLS 00, zero BUSY refusals, no menu drop -- except for 135 frames out
 of 62291 whose ERRCODE read back as $FF. $FF is not an errcode. It has
 exactly two producers, and nothing on that screen could tell them apart:

   (a) The read *was* serviced, but IO_ADDRESS was mis-sampled, so
       gpu64_apiReadReg() fell through its four cases to `return 0xFF`.
   (b) The read was never serviced at all -- the loop missed the access --
       and the C64 latched a floating bus that happened to read as $FF.

 The distinction matters because (a) and (b) have different fixes and
 different blast radii. Under (a) the firmware's *state* is fine and only
 the answer was wrong, so a C64-side re-read repairs it; under (b) a write
 in the same position would have been swallowed silently, which is the
 known ~1-in-180000 drop floor (memory note gpu64-io2-sampling-reliability)
 and cannot be repaired from the C64 side without a sequence number.

 Why the mis-sample is plausible at all. IO_ADDRESS (lowlevel_dma.h:53) is

     ( ( g3 >> 10 ) & 15 ) | ( ( g3 & 15 ) << 4 )

 -- the *low* nibble A0-A3 comes from GPIO 10-13, which are dedicated, and
 the *high* nibble A4-A7 comes from GPIO 0-3, which the LVC257 multiplexes
 with NMI, ROMH, IO1 and BUTTON. (Written when every gpu64 register lived at
 $DF0B-$DF23; since 2026-09-27 the block is $DF50-$DF68, see gpu64_api.h.)
 A mis-sample can only ever set bits, never clear them. A read of ERRCODE
 ($DF0E then) therefore degraded to $1E, $2E, $4E or $8E -- all inside the
 gpu64 window, all still
 decoded here, and all four returning $FF from gpu64_apiReadReg(). That is
 the whole of hypothesis (a), and it predicts a specific fingerprint: the
 bad addresses share the low nibble of the register the program was reading
 and differ only in the high nibble.

 So this module counts, separately, gpu64-window reads and writes the loop
 serviced, and how many of those carried an address no gpu64 register
 occupies. Read it with GET_HEALTH ($0A) bytes 48-63.

 The three ways to read the answer, given a C64-side program that issues a
 known number of reads and counts its own $FF answers (tests/
 gpu64_probe_bus.a does exactly this):

   readsBad == the C64's $FF count      -> (a). orBadAddr/andBadAddr name
                                           the exact address bits that
                                           moved.
   readsBad == 0, C64 saw $FF anyway    -> (b). The access never reached
                                           the loop; this is the drop floor
                                           showing up on the read side.
   reads    <  the C64's read count     -> (b), independently, and by how
                                           much: the difference is a direct
                                           measurement of the drop rate with
                                           a denominator, which no
                                           instrument in the tree has had.

 The same three readings apply to the write side, and `writes` is worth as
 much as any of them on its own: until now the drop rate for writes had a
 numerator (SEQ/SEQACK mismatches) and no denominator, so "1 in 70 commit
 dispatches" and "1 in 180000 writes" were never comparable numbers.

 Cost. The read-side bookkeeping is placed *after* WAIT_UP_TO_CYCLE(
 WAIT_CYCLE_READ2 ) and SET_GPIO( bOE_Dx | bDIR_Dx ), i.e. after the C64 has
 latched D and the transceiver has turned around, so not one ARM cycle of it
 sits inside the read's hard deadline (rule 6 exposure is to the loop's
 *next* sample only, the same window gpu64_holdGapRelease() already spends a
 store in). The good path is one load, one add and one store to a line both
 warm paths keep resident; everything else is under a branch that a correct
 access never takes. The write side sits after the register write or, for
 CMD_LO, after the bus has already been given back.

 This is deliberately NOT added to the $DF00-$DF0A REU decode, which is the
 path that latches into 100% transfer failure under write pressure. See the
 "bus is not reliable" section of CLAUDE.md.
*/
#ifndef _gpu64_busstats_h
#define _gpu64_busstats_h

#include <circle/types.h>

// One 64-byte cache line, written only by core 0, for the same reason as
// gpu64HoldGap: both warm paths preload it as a unit and a struct straddling
// two lines would only ever be half-warmed. No section() attribute -- see the
// milestone-14 linker-orphan regression in dev/progress_tracker.md.
typedef struct
{
	u32	reads;			// gpu64-window reads the loop serviced
	u32	writes;			// gpu64-window writes the loop serviced,
						//   CMD_LO dispatches included
	u32	readsBad;		// ... of those reads, ones at no readable register
	u32	writesBad;		// ... of those writes, ones at no writable register
	u8	lastBadAddr;	// most recent bad *read* address
	u8	orBadAddr;		// OR of every bad read address: bits that ever set
	u8	andBadAddr;		// AND of every bad read address: bits always set.
						//   Init 0xFF so "no samples" is distinguishable
						//   from "every bit moved".
	u8	lastBadWrite;	// most recent bad *write* address

	// gpu64: added 2026-09-11 after bench runs 16 and 17 both reported
	// andBadAddr == 0x80 -- A7 set in EVERY bad read address, without one
	// exception across some two thousand events. A7 is GPIO 3 in the g3
	// phase, i.e. the pin the LVC257 multiplexes with BUTTON, which idles
	// high. The read side has had or/and accumulators since it was built;
	// the write side has only ever had lastBadWrite, so there is no way to
	// ask whether bad writes carry the same fingerprint. If they do, reads
	// and writes are one mechanism in the address mux and should be chased
	// as one. If they do not, they are two problems and the A/B result
	// (which is overwhelmingly a *write* effect: 1536 bad writes against
	// 1373 bad reads in run 17) belongs to the other one.
	u8	orBadWrite;		// OR of every bad write address
	u8	andBadWrite;	// AND of every bad write address. Init 0xFF, same
						//   reason as andBadAddr.

	// gpu64: added 2026-09-09 for the quake3d SEQACK question. Both are
	// written by gpu64_apiDispatch(), i.e. with the bus already held, so
	// neither costs the polling loop anything.
	//
	// dispatches is the denominator the SEQ/SEQACK numerator never had:
	// commands the firmware actually executed. A C64 that counts its own
	// dispatches and finds the firmware short by exactly its SEQACK
	// mismatch count has proved the CMD_LO writes were lost. One that
	// finds them equal has proved the opposite -- every command ran, and
	// the mismatch is somewhere other than the command write.
	//
	// seqRepeat is the other half: dispatches that arrived with SEQ
	// unchanged since the previous dispatch. For a caller that increments
	// SEQ before every command -- which is the whole point of the
	// register -- that means the SEQ *write* was the one that was lost,
	// and the command ran against a stale sequence number. Zero for a
	// caller that never writes SEQ at all; the counter cannot tell that
	// case apart, so it is only meaningful for a program that does.
	u32	dispatches;
	u32	seqRepeat;

	// gpu64: added 2026-09-09. The first bench run of the dispatch pair
	// came back with readsBad at 4.3% of reads, which would have been an
	// alarming mis-sample rate if it had been one. It was not: `sta ARG,y`
	// -- the demos' argument-staging idiom -- makes the 6502 emit an
	// unconditional dummy READ at the very address it is about to write,
	// on the cycle before the write. ARG registers are write-only, so
	// gpu64_apiReadReg() answers 0xFF and the read was scored bad. The
	// 6502 discards that byte; nothing is wrong. Counting them apart
	// keeps readsBad a fault counter instead of an idiom counter.
	u32	argDummyReads;

	// gpu64: added 2026-09-11 after run 19. andBadWrite came back $80 --
	// A7 set in every bad write address -- which is exactly what "the true
	// address with A7 spuriously set" predicts: the writable registers are
	// $0B, $0C, $0F, $10, $11-$20 and $22, and $8B & $A0 is $80. But the
	// same run reported orBadWrite $FF, and that story predicts $BF. An
	// AND and an OR cannot tell a dominant mode from a single outlier, and
	// both accumulate since resetREU() rather than over the run, so one
	// stray sample at boot poisons the OR for good.
	//
	// A histogram of the high nibble separates them. If the bad writes pile
	// up in buckets 9 and A -- the ARG block with A7 set -- the mechanism is
	// A7 alone, the C64's argument writes are being redirected to $DF8B-
	// $DFA0 and silently discarded, and that is the undetected ARG-drop
	// defect with a cause. If they are spread across all sixteen, it is not
	// A7 and the AND was an artifact of which registers this demo writes.
	//
	// Sixteen u8 rather than u32, and carved out of the existing pad so the
	// struct stays exactly 64 bytes and one cache line -- see the note at
	// the top of the struct. Saturating, so a bucket that overflows reads
	// $FF instead of wrapping to a small number and lying.
	u8	badWriteNib[ 16 ];

	// gpu64 run 20 (2026-09-11) answered it, and the answer was neither
	// option: NIB 0F 8F 03, andBadWrite $C0. Three buckets, 143 of 249 bad
	// writes in bucket $F, and bits 7 and 6 of the address set in every one.
	//
	// That is not an address at all. IO_ADDRESS builds the low byte as
	// ( g3 >> 10 & 15 ) | ( g3 & 15 ) << 4: A0-A3 come from GPIO10-13, which
	// are wired straight to the address lines, and A4-A7 from GPIO0-3, which
	// are the LVC257 multiplexer's outputs. With MPLEX_SEL low those same
	// four pins are NMI, ROMH, IO1 and BUTTON. During an IO2 access IO1 is
	// inactive-high by definition and BUTTON is high unless a finger is on
	// it, so the signal nibble reads $C-$F -- which is exactly the observed
	// andBadWrite of $C0 (bits 3 and 2 of the nibble, i.e. IO1 and BUTTON)
	// and exactly the observed concentration in bucket $F. The three live
	// buckets are the four ways NMI and ROMH can sit, and ROMH is low on
	// every KERNAL and BASIC fetch.
	//
	// So a "bad write" is a REAL C64 register write whose high address
	// nibble was sampled through a multiplexer that had not switched yet.
	// The low nibble is untouched -- those pins are not multiplexed -- which
	// is why lastBadWrite $F9 is ARG8 ($DF19) wearing the signal nibble.
	// The write was discarded and the C64 was never told: the undetected
	// ARG-drop defect, with a cause that is ours and not the bus's.
	//
	// muxMiss counts sampled IO2 accesses whose high nibble came back in
	// the signal range, muxFixed how many of those a single re-read of
	// GPLEV0 repaired. If the diagnosis holds, muxMiss lands near the old
	// writesBad + readsBad and muxFixed within a hair of it, while
	// writesBad and readsBad collapse toward zero.
	u32	muxMiss;
	u32	muxFixed;

	// Run 24 replaced the second re-read with this counter. It had fired
	// zero times in every uncorrupted run, and it was an MMIO access on a
	// path the slack instrument then proved has no margin at all. This
	// counts the same event -- the first re-read came back with a nibble no
	// real access could produce -- without spending a bus read to learn it.
	// Nonzero means the access was discarded, so BUS A/BUS B will show it
	// too; the expected reading is zero.
	u32	muxUnfixed;
} GPU64BUSSTATS;

extern GPU64BUSSTATS gpu64BusStats;

// gpu64: added 2026-09-11 after run 23.
//
// Run 23 tried to cure the read-side failures with a GPLEV0 write barrier at
// the hold release and made them worse (MISSED 158 -> 232, phantoms +7 ->
// +53, and the GET_HEALTH readback itself corrupted). The placement was the
// error: an MMIO read there delays the loop's first sample after the C64
// restarts.
//
// The remaining candidate placement was inside the read service, just before
// WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ2 ). Run 24 killed it: SLK 00 FF said the
// loop arrives there already past that deadline, so there is no slack to
// spend. Run 25 published the magnitude -- SLK -FF 13AD FF, i.e. 255+ cycles
// past on 5037 of 609,817 reads, 255+ of them badly past.
//
// BUT RUN 25 WAS MEASURING THE WRONG EDGE, and the number above should be
// read only as what it is. Look at the read service: D is driven onto the
// pins by the GPCLR0/GPSET0 pair, and NOTHING waits before that. The one
// WAIT_UP_TO_CYCLE on the path, WAIT_CYCLE_READ2, gates SET_GPIO( bOE | bDIR
// ) -- the moment the transceiver is turned OFF. So a stamp taken below the
// drive scores how long the Pi over-HELD the bus after the C64 had already
// latched. That is a contention question, not a data-integrity one, and it
// cannot explain a wrong ERRCODE.
//
// What decides a wrong ERRCODE is the SETUP edge: was D on the pins before
// the C64 latched? RAD's target for that is WAIT_CYCLE_READ, which this path
// never waits for and until now never measured. From 2026-09-12 the stamp
// sits immediately after gpu64_apiReadReg() -- one MRS, the same one, moved
// up inside a branch already taken -- and the deadline is WAIT_CYCLE_READ.
//
// Only that MRS runs ahead of the drive; the compare and the stores happen
// after the C64 has latched, beside gpu64_busStatsRead(), which is documented
// as outside the read's hard window.
//
// How to read run 26. The margin is printed with a sign, and the sign is the
// whole verdict:
//   SLK +nn                   D was on the pins before RAD's own drive-by
//                             target, every single read. The firmware is not
//                             late, the read path is exonerated, and MISSED /
//                             errBad have to be explained somewhere else --
//                             the C64 side, or the sampling of the read
//                             request in the first place.
//   SLK -oo, expiredBig 0     late past the target but never by much. The
//                             target is conservative; treat as exonerated
//                             unless MISSED tracks it.
//   SLK -oo, expiredBig large the drive really is late, systematically, and
//                             that is a mechanism for every failed read on
//                             the screen. Then shorten the work between
//                             RESTART_CYCLE_COUNTER and the drive.
//
// Do not compare MISSED across runs to judge a change: the fault rate varies
// 30x run to run (run 21 309, run 22 158, run 24 195, run 25 293 from builds
// that differ by one MMIO read). Only the in-run A/B settles that kind of
// question.
//
// Run 26, the first reading of the right edge: SLK -CF 00DA 9D. The drive IS
// late -- 218 of 611,390 reads past WAIT_CYCLE_READ, 157 of them by more than
// 64 cycles, worst case 207. Rare (0.036%) but systematic, and the same order
// as errBad 54 + MISSED 390, so it is a live mechanism rather than noise.
//
// expiredMux is the follow-up, and it is a within-run test, not a comparison
// across runs. The mux re-read is the ONLY conditional MMIO access ahead of
// the drive, it fires on ~0.22% of accesses, and it costs a full peripheral
// round trip by construction -- that is the whole point of it being a barrier.
// So:
//   expiredMux ~= expired    the re-read is buying address correctness with
//                            read lateness. Both defects are then ours and
//                            the trade has to be made explicitly -- e.g. do
//                            the re-read only for writes, where there is no
//                            deadline, and let reads take the address twice.
//   expiredMux ~= 0          the re-read is exonerated; the lateness is in
//                            the unconditional part of the pass and no amount
//                            of trimming the repair will touch it.
typedef struct
{
	s32	minSlack;		// smallest (deadline - now) seen, in ARM cycles
	u32	expired;		// times that margin was <= 0
	u32	expiredBig;		// ... of those, ones more than 64 cycles past
	u32	expiredMux;		// ... of expired, ones whose pass paid for a mux re-read
	u32	pad[ 12 ];
} __attribute__( ( aligned( 64 ) ) ) GPU64READSLACK;

extern GPU64READSLACK gpu64ReadSlack;

// Called AFTER the C64 has latched and the transceiver has turned around.
__attribute__( ( always_inline ) ) inline void gpu64_readSlackNote( u64 deadline, u64 arrived, u32 muxRetried )
{
	s64 slack = (s64)deadline - (s64)arrived;

	if ( slack < (s64)gpu64ReadSlack.minSlack )
		gpu64ReadSlack.minSlack = (s32)slack;
	if ( slack <= 0 )
	{
		gpu64ReadSlack.expired++;
		if ( slack < -64 )
			gpu64ReadSlack.expiredBig++;
		if ( muxRetried )
			gpu64ReadSlack.expiredMux++;
	}
}


#endif

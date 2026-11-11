// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
//
// gpu64_sniff.h - IO2 bus sniffer for bringing gpu64 up on a new machine
//
// gpu64: added 2026-09-29 for the C64 Ultimate. The first bench session on
// it ended with the HDMI boot log still on screen after the game had started
// -- and the log is hidden by the first successful dispatch, so not one
// command landed. The RAD menu, the mirror and DMA all worked, so the Pi
// can drive and sample the bus; what was never proven is the one path none
// of those use: the C64's own CPU writing into IO2 and the Pi latching it.
//
// Nothing here can be observed on the host, and the bench can only report
// what it can show. So this instrument measures, in one run, each of the
// three places that path can fail:
//
//   1. Does IO2 reach the Pi at all?  io2Writes / io2Reads count every
//      sampled IO2 access, whatever the address. Zero means the machine
//      is not putting IO2 on the cartridge port for $DFxx -- a setting
//      (Ultimate: cartridge/bus-sharing I/O2, internal REU, Ultimate Audio),
//      not firmware.
//   2. Is the ADDRESS right?  a histogram of the low address byte, and the
//      first sixteen accesses raw. The API writes only $DF50-$DF67; writes
//      anywhere else are the address mux or the address lines.
//   3. Is the DATA right?  a write to SNIFF_DATA ($DF6F) samples the data
//      bus at nine points across the CPU half-cycle instead of one and
//      compares each against a fixed pattern (gpu64_probe_ultimate.prg
//      writes it). The row of verdicts is the machine's write-data window,
//      which says directly what WAIT_CYCLE_WRITEDATA should be.
//
// Output. The report is printed onto the HDMI log -- which, in the failing
// case, is still on screen -- once a second from inside a DMA hold, clocked
// by the IRQ vector fetch exactly like the mirror (the only hold that is
// already proven to work on the Ultimate). It prints only while the log is
// visible and only when a number changed, so a machine where commands work
// sees nothing of it once the first dispatch hides the log.
//
// Constant registers for the READ direction, answered by gpu64_apiReadReg()
// only after every real register has missed, so a normal read pays nothing:
//   $DF6A reads $A5, $DF6B reads $5A,
//   $DF6C the low byte of the number of SNIFF_SYNC writes seen,
//   $DF6D the low byte of the number of SNIFF_DATA writes seen.
//
// Compile-time only. Commenting out GPU64_SNIFF_ENABLED removes every trace
// from the polling loop.
//

#ifndef _gpu64_sniff_h
#define _gpu64_sniff_h

#include <circle/types.h>

//#define GPU64_SNIFF_ENABLED		// off since 2026-09-30 (tracker 95)

#define GPU64_SNIFF_CONST_A		0x6A		// reads $A5
#define GPU64_SNIFF_CONST_B		0x6B		// reads $5A
#define GPU64_SNIFF_SYNCS		0x6C		// reads syncs & 255
#define GPU64_SNIFF_DATAS		0x6D		// reads datas & 255
#define GPU64_SNIFF_SYNC		0x6E		// write: restart the pattern
#define GPU64_SNIFF_DATA		0x6F		// write: next pattern byte

#define GPU64_SNIFF_SLOTS		9
#define GPU64_SNIFF_RING		16

// The pattern gpu64_probe_ultimate.a writes after every SNIFF_SYNC. Every
// data line is seen both high and low, alone and among neighbours.
#define GPU64_SNIFF_PATTERN_LEN	16
#define GPU64_SNIFF_PATTERN \
	{ 0x00, 0xFF, 0x55, 0xAA, 0x01, 0x02, 0x04, 0x08, \
	  0x10, 0x20, 0x40, 0x80, 0x0F, 0xF0, 0x33, 0xCC }

typedef struct
{
	u32	io2Writes;					// every sampled IO2 write, any address
	u32	io2Reads;					// every sampled IO2 read, any address
	u32	vectorFetches;				// $FFFE+$FFFF read pairs: the report's own clock
	u32	vectorFakes;				// lone $FFFF reads, no $FFFE before them
	u16	histW[ 256 ];				// IO2 writes by low address byte
	u16	histR[ 256 ];				// IO2 reads by low address byte

	u32	ringN;						// accesses recorded, saturates at RING
	u8	ringAddr[ GPU64_SNIFF_RING ];
	u8	ringData[ GPU64_SNIFF_RING ];
	u8	ringWrite[ GPU64_SNIFF_RING ];

	// The write-data sweep. One entry per slot of gpu64SniffT[].
	u32	syncs;
	u32	datas;						// SNIFF_DATA writes seen
	u32	scored;						// ... of those, ones after a sync
	u32	patIdx;						// position in the pattern
	u32	ok[ GPU64_SNIFF_SLOTS ];	// sample == pattern byte
	u32	io2Low[ GPU64_SNIFF_SLOTS ];// IO2 still asserted at the sample
	u32	phiHigh[ GPU64_SNIFF_SLOTS ];// PHI2 still high at the sample
	u8	badBits[ GPU64_SNIFF_SLOTS ];// OR of (sample ^ expected)

	u32	jiffy;						// vector fetches since the last report
	u32	lastSig;					// what the last report printed
	u32	reports;					// reports printed so far
} GPU64SNIFF;

extern GPU64SNIFF gpu64Sniff;

// Sample points, ARM cycles after the CPU half-cycle anchor. Ascending --
// the sweep waits for each in turn. The fifth is RAD's default
// WAIT_CYCLE_WRITEDATA (470), so the row reads "earlier ... RAD ... later".
extern const u32 gpu64SniffT[ GPU64_SNIFF_SLOTS ];
extern const u8 gpu64SniffPattern[ GPU64_SNIFF_PATTERN_LEN ];

// Every sampled IO2 access, at the loop's common exit. Stores only.
static inline void gpu64_sniffNote( u8 addr, u32 isWrite, u8 data )
{
	if ( isWrite )
	{
		gpu64Sniff.io2Writes++;
		gpu64Sniff.histW[ addr ]++;
	} else
	{
		gpu64Sniff.io2Reads++;
		gpu64Sniff.histR[ addr ]++;
	}

	register u32 n = gpu64Sniff.ringN;
	if ( n < GPU64_SNIFF_RING )
	{
		gpu64Sniff.ringAddr[ n ] = addr;
		gpu64Sniff.ringData[ n ] = data;
		gpu64Sniff.ringWrite[ n ] = isWrite ? 1 : 0;
		gpu64Sniff.ringN = n + 1;
	}
}

// One SNIFF_DATA write's nine raw GPLEV0 samples. Runs after the last
// sample, i.e. with the C64's write already over.
static inline void gpu64_sniffScore( const u32 *raw, u32 shiftD0, u32 maskIO2, u32 maskPhi )
{
	gpu64Sniff.datas++;
	if ( gpu64Sniff.syncs == 0 )
		return;						// no reference yet

	register u8 want = gpu64SniffPattern[ gpu64Sniff.patIdx ];
	gpu64Sniff.patIdx = ( gpu64Sniff.patIdx + 1 ) & ( GPU64_SNIFF_PATTERN_LEN - 1 );
	gpu64Sniff.scored++;

	for ( u32 i = 0; i < GPU64_SNIFF_SLOTS; i++ )
	{
		register u8 got = ( raw[ i ] >> shiftD0 ) & 255;
		if ( got == want )
			gpu64Sniff.ok[ i ]++;
		else
			gpu64Sniff.badBits[ i ] |= got ^ want;
		if ( !( raw[ i ] & maskIO2 ) )
			gpu64Sniff.io2Low[ i ]++;
		if ( raw[ i ] & maskPhi )
			gpu64Sniff.phiHigh[ i ]++;
	}
}

// The HDMI report. Called with the bus held; formats and paints. Returns
// without painting when nothing changed since the last one.
void gpu64_sniffReport( void );

#endif

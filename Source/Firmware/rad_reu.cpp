/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
        - this REU emulation reproduces the behavior of Vice's emulation (https://sourceforge.net/projects/vice-emu/)
 Copyright (c) 2022-2025 Carsten Dachsbacher <frenetic@dachsbacher.de>

 This program is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/
#include "rad_reu.h"
#include "linux/kernel.h"
#include "gpu64_api.h"
#include "gpu64_vsync.h"
#include "gpu64_3d.h"
#include "gpu64_ladder.h"
#include "gpu64_holdgap.h"
#include "gpu64_holdgate.h"
#include "gpu64_busstats.h"
#include "gpu64_fb.h"

// gpu64: reaching CGpu64FrameBuffer::CommitFlip() from the bus-watch loop
// without including gpu64_fb.h, which pulls in Circle's framebuffer and
// character-generator headers -- the same reasoning as g_pRAD above.
boolean gpu64_commitFlip( void );
// gpu64: warms the mailbox post the commit ends in -- see gpu64_flip.h.
void gpu64_flipWarm( void );

// gpu64: forward-declared rather than pulling in rad_main.h (which drags in
// the whole Circle screen/HDMI-console stack) -- see rad_main.h for the
// actual definition/assignment. Used below to reach CRAD::showTestPattern()
// when the C64 writes the gpu64 trigger register at IO2 $DF0B.
class CRAD;
extern CRAD *g_pRAD;
void gpu64_showTestPattern( CRAD *pRAD );
void gpu64_showMirror( CRAD *pRAD, const u8 *screen, const u8 *color, u8 border, u8 background, u8 d018 );
void gpu64_showHoldingScreen( void );

// gpu64: set once a C64 program engages the gpu64 API -- currently that's
// just milestone 2's test-pattern trigger at $DF0B (and, since IO_ADDRESS is
// masked to 5 bits same as REU's own partial IO2 decode, really any of
// $DF0B/$DF2B/$DF4B/$DF6B/$DF8B/$DFAB/$DFCB/$DFEB -- any program that scans
// or fills across IO2, e.g. an REU-detection utility, can trip this
// incidentally). Per dev/bus_access_design.md, screen-mirror mode and
// framebuffer-API mode are mutually exclusive: once true, the periodic
// mirror snapshot in reuUsingPolling()'s main loop stops firing.
//
// Cleared in resetREU() below (called on every fresh entry into REU
// emulation) rather than never, per a real-hardware hang this caused: once
// set, it stayed set for the rest of the RPi's power-on session -- a PRG
// that genuinely tripped it once (deliberately or, per the aliasing above,
// by accident) silently disarmed the mirror for every REU session
// afterwards, with zero on-screen indication why. milestone 4's real
// command API may want its own, more deliberate "back to mirror" exit
// eventually; resetting on resetREU() is the simple fix for now.
u8 gpu64ApiActive = 0;

// gpu64: mirror liveness state (2026-09-06 rework -- see the long comment on
// gpu64_mirrorSnapshot() below for the whole design).
//
// File scope rather than reuUsingPolling() locals for two reasons: resetREU()
// has to clear them and cannot reach that function's registers, and the
// per-pass test at the top of the loop wants them on the same already-hot
// cache line as gpu64ApiActive above.
//
// enabled       -- the latch. Cleared the moment the machine stops being "a
//                  BASIC prompt with nothing running"; only resetREU(), i.e.
//                  a C64 reset, ever sets it again.
// seenPrompt    -- has this session ever observed the direct-mode prompt?
//                  Until it has, the CURLIN test below is disarmed, because
//                  the KERNAL's power-on RAM test runs for ~2s with $3A
//                  holding uninitialised garbage and would latch the mirror
//                  off before BASIC ever came up.
// irqWatchdog   -- main-loop passes (~1 C64 cycle each) since the last jiffy
//                  IRQ vector fetch was seen on the bus.
// awaitIrq      -- set by every re-arm, cleared by the first vector fetch
//                  after it. The watchdog does not run while this is set,
//                  because a freshly reset C64 does not have a jiffy IRQ yet
//                  and takes far longer to get one than the watchdog allows.
//                  See GPU64_MIRROR_IRQ_TIMEOUT below.
// jiffy         -- vector fetches counted toward the next snapshot.
u8 gpu64MirrorEnabled = 1;
static u8 gpu64MirrorSeenPrompt = 0;
static u32 gpu64MirrorIrqWatchdog = 0;
static u8 gpu64MirrorAwaitIrq = 1;
static u8 gpu64MirrorJiffy = 0;

// gpu64: the C64 Ultimate's fallback mirror clock (tracker 89d). At 64MHz
// turbo the C64U runs from its own ROM copy and the cartridge port shows an
// idle, pulled-up bus -- a lone "$FFFF read" -- on nearly every cycle, and
// the $FFFE/$FFFF vector pair the mirror is clocked by never appears. So a
// long run of lone $FFFF reads with no pair among them is taken as a
// substitute clock: GPU64_MIRROR_LONE_INTERVAL of them (~0.25s, since a pass
// is at most one C64 cycle) arm one snapshot. A pair resets the count, and a
// breadbin shows a pair 60 times a second and a lone $FFFF read never, so it
// cannot reach this.
#define GPU64_MIRROR_LONE_INTERVAL 250000
static u32 gpu64MirrorLone = 0;
// Snapshots that got past the BLNSW probe and painted HDMI.
static u32 gpu64MirrorPainted = 0;

u32 REU_SIZE_KB = 1024;

REUSTATE reu AAA;
u8 *reuMemory;
bool reuRunning;

static u64 armCycleCounter;

// gpu64: the hold-gap instrument (gpu64_holdgap.h). One cache line, core 0
// only. aligned(64) so it cannot share a line with anything else -- not
// because another core writes it, but because both warm paths preload it as
// a unit and a straddled struct would only be half-warmed.
GPU64HOLDGAP gpu64HoldGap __attribute__( ( aligned( 64 ) ) );

// gpu64: the hold gate's counters (gpu64_holdgate.h). Same shape and same
// reason as gpu64HoldGap above: one line of its own, warmed by both paths.
GPU64HOLDGATE gpu64HoldGate __attribute__( ( aligned( 64 ) ) );

// gpu64: the bus-sampling instrument (gpu64_busstats.h). Third line of the
// same shape, warmed by both paths for the same reason.
GPU64BUSSTATS gpu64BusStats __attribute__( ( aligned( 64 ) ) );
GPU64READSLACK gpu64ReadSlack __attribute__( ( aligned( 64 ) ) );

// Called after the read has already been answered -- after WAIT_CYCLE_READ2
// and the transceiver turn-around -- so none of this is inside the read's
// deadline. The good path is one increment; the rest is under a branch a
// correct access never takes.
//
// "Readable" here must stay identical to gpu64_apiReadReg()'s four cases
// (gpu64_api.h), because "bad" is defined as "that function returned its
// 0xFF default". If a register is ever added there, add it here too or the
// instrument will report a working read as a mis-sample.
__attribute__( ( always_inline ) ) inline void gpu64_busStatsRead( u8 addr )
{
	GPU64BUSSTATS *b = &gpu64BusStats;
	b->reads++;

	if ( addr == GPU64_REG_STATUS || addr == GPU64_REG_ERRCODE ||
			addr == GPU64_REG_RESULT || addr == GPU64_REG_SEQACK )
		return;

	// The 6502's dummy read from `sta ARG,y`, not a fault -- see
	// argDummyReads in gpu64_busstats.h. Nothing legitimately *reads*
	// an ARG register, so the whole range is that idiom.
	if ( addr >= GPU64_REG_ARG0 && addr <= GPU64_REG_ARG15 )
	{
		b->argDummyReads++;
		return;
	}

	b->readsBad++;
	b->lastBadAddr = addr;
	b->orBadAddr |= addr;
	b->andBadAddr &= addr;
}

// Same, for writes. "Writable" mirrors gpu64_apiWriteReg()'s cases plus
// CMD_LO, which never reaches that function because the caller dispatches it
// directly. A write outside the set is silently discarded there, so it is
// exactly the write-side equivalent of a $FF read.
__attribute__( ( always_inline ) ) inline void gpu64_busStatsWrite( u8 addr )
{
	GPU64BUSSTATS *b = &gpu64BusStats;
	b->writes++;

	if ( addr == GPU64_REG_CMD_HI || addr == GPU64_REG_CMD_LO ||
			addr == GPU64_REG_ID_LO || addr == GPU64_REG_ID_HI ||
			addr == GPU64_REG_SEQ ||
			( addr >= GPU64_REG_ARG0 && addr <= GPU64_REG_ARG15 ) )
		return;

	b->writesBad++;
	b->lastBadWrite = addr;
	b->orBadWrite |= addr;
	b->andBadWrite &= addr;

	// Saturating, and still on the branch a correct access never takes.
	u8 *n = &b->badWriteNib[ addr >> 4 ];
	if ( *n < 0xff )
		( *n )++;
}

// Called immediately after CLR_GPIO( bDMA_OUT ), i.e. with the bus already
// held, so this has no cycle budget to blow. stamp is armCycleCounter, which
// the caller's RESTART_CYCLE_COUNTER anchored at the start of the VIC
// half-cycle the assert happens in; both call sites are built the same way,
// so whatever bias that carries is identical on the release side and cancels.
__attribute__( ( always_inline ) ) inline void gpu64_holdGapAssert( u64 stamp, u8 kind )
{
	GPU64HOLDGAP *g = &gpu64HoldGap;
	g->asserts++;

	if ( g->lastKind == GPU64_HOLD_KIND_NONE )
		return;

	u64 d = stamp - g->lastRelease;
	u32 gap = ( d > 0xfffffffful ) ? 0xfffffffful : (u32)d;

	if ( gap < g->minGap )
		g->minGap = gap;

	u32 t = g->armPerC64;

	if ( g->lastKind == GPU64_HOLD_KIND_DISPATCH && kind == GPU64_HOLD_KIND_COMMIT )
	{
		if ( gap < g->minGapD2C )
			g->minGapD2C = gap;
		if ( t && gap < t * GPU64_HOLDGAP_CLOSE_CYCLES )
			g->d2cClose++;
	}

	// Bucket by whole C64 cycles. If armPerC64 never got measured everything
	// lands in the overflow bucket, which is a readable "no data" rather than
	// a divide by zero.
	if ( t == 0 )
	{
		g->bucket[ GPU64_HOLDGAP_BUCKETS - 1 ]++;
		return;
	}

	u32 b = 0;
	while ( b < GPU64_HOLDGAP_BUCKETS - 1 && gap >= t )
	{
		t <<= 1;
		b++;
	}
	g->bucket[ b ]++;
}

// Called immediately after SET_GPIO( bDMA_OUT ). Two stores to a line that
// both warm paths keep resident; see the cost note in gpu64_holdgap.h.
//
// gpu64 2026-09-11, run 23: a GPLEV0 read was added here as a posted-write
// barrier and REVERTED after one bench run. It made everything worse --
// MISSED 158 -> 232, phantoms +7 -> +53, two phantom LOOP_STOPs caught by
// the key gate, and the GET_HEALTH readback itself came back corrupted
// (muxFixed > muxMiss, 26879 "bad writes" with every nibble bucket zero and
// the AND accumulators untouched).
//
// The reason is rule 6, and it was predictable: a peripheral read here sits
// between SET_GPIO( bDMA_OUT ) and the loop's return to sampling, so it
// delays the first sample after the C64 restarts -- the least forgiving
// moment there is. "The C64 has only just restarted, so there is slack" was
// exactly backwards; that is when the loop has the least of it.
//
// The underlying hypothesis is untouched by this: serving a C64 read is
// three posted GPIO writes against a hard deadline, and the failure still
// concentrates 55x on the instruction just after a hold release. What is
// disproven is this *placement*. The next candidate is inside the read
// service itself, before WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ2 ), where a
// barrier would consume slack the loop was going to spin away anyway -- but
// only if that slack exists, which is what gpu64BusStats' readSlack pair now
// measures before anything is spent on it.
__attribute__( ( always_inline ) ) inline void gpu64_holdGapRelease( u64 stamp, u8 kind )
{
	gpu64HoldGap.lastRelease = stamp;
	gpu64HoldGap.lastKind = kind;
}

static volatile u8 forceRead;

#include "lowlevel_dma.h"

void resetREU()
{
	// gpu64: see the comment on gpu64ApiActive's declaration above -- this
	// is what actually clears it now.
	gpu64ApiActive = 0;

	// gpu64: and this is the mirror's only re-arm. A C64 reset is the one
	// event after which "BASIC owns the machine again" is true by
	// construction, so it is the one event that undoes the latch.
	gpu64MirrorEnabled = 1;
	gpu64MirrorSeenPrompt = 0;
	gpu64MirrorIrqWatchdog = 0;
	gpu64MirrorAwaitIrq = 1;
	gpu64MirrorJiffy = 0;
	gpu64MirrorLone = 0;
	// gpu64: the API register file resets with it -- a fresh REU session must
	// not inherit a previous program's staged ARGs, sticky CMD_HI or error.
	gpu64_apiReset();

	// gpu64: and so do the hold instruments, so a bench run's numbers are
	// that run's and not a previous program's.
	//
	// armPerC64 is the one field that survives. It is a property of the two
	// clocks, not of the run, and resetREU() is called from *inside* the
	// polling loop when the C64 derails -- long after the start-up block
	// that measures it, and with no way to measure it again. Zeroing it
	// there would silently disarm the hold gate's consecutiveness test
	// (gpu64_holdgate.h) for the rest of the session.
	register u32 armPerC64 = gpu64HoldGap.armPerC64;
	memset( &gpu64HoldGap, 0, sizeof( gpu64HoldGap ) );
	gpu64HoldGap.armPerC64 = armPerC64;
	gpu64HoldGap.minGap = 0xffffffff;
	gpu64HoldGap.minGapD2C = 0xffffffff;

	memset( &gpu64HoldGate, 0, sizeof( gpu64HoldGate ) );

	memset( &gpu64BusStats, 0, sizeof( gpu64BusStats ) );
	memset( &gpu64ReadSlack, 0, sizeof( gpu64ReadSlack ) );
	gpu64ReadSlack.minSlack = 0x7fffffff;
	// andBadAddr accumulates by AND, so it starts all-ones. With no bad
	// samples it reads back 0xFF, which the probe reports as "no data"
	// rather than as "every address bit was set".
	gpu64BusStats.andBadAddr = 0xff;
	gpu64BusStats.andBadWrite = 0xff;

	reu.irqRelease = 0;

    reu.irqTriggered = 0;
    reu.reuWaitForFF00 = 0;

    reu.status = (reu.status & ~REU_STATUS_256K_CHIPS) | reu.preset;
    reu.command = REU_COMMAND_FF00_DISABLED;
    reu.length = reu.shadow_length = 0xffff;
    reu.addrC64 = 0;
    reu.addrREU = reu.shadow_addrREU = 0;//(u32)reu.regBankUnused << 16;
    reu.bank = reu.shadow_bank = reu.regBankUnused;
    reu.IRQmask = REU_INTERRUPT_UNUSED_BITMASK;
    reu.addrREUCtrl = REU_ADDR_UNUSED_BITS;

	reu.releaseDMA = 0;

	reu.contiguousWrite = 0;
	reu.contiguousVerify = 0;
	reu.contiguous1ByteWrites = 0;
}

void initializeDMATimings()
{
	reu.WAIT_FOR_SIGNALS = WAIT_FOR_SIGNALS;
	reu.WAIT_CYCLE_MULTIPLEXER = WAIT_CYCLE_MULTIPLEXER;
	reu.WAIT_CYCLE_READ = WAIT_CYCLE_READ;
	reu.WAIT_CYCLE_WRITEDATA = WAIT_CYCLE_WRITEDATA;
	reu.WAIT_CYCLE_READ2 = WAIT_CYCLE_READ + 20;
	reu.WAIT_CYCLE_READ_VIC2 = WAIT_CYCLE_READ_VIC2;
	reu.WAIT_CYCLE_WRITEDATA_VIC2 = WAIT_CYCLE_WRITEDATA_VIC2;
	reu.WAIT_CYCLE_MULTIPLEXER_VIC2 = WAIT_CYCLE_MULTIPLEXER_VIC2;
	reu.WAIT_TRIGGER_DMA = WAIT_TRIGGER_DMA;
	reu.WAIT_RELEASE_DMA = WAIT_RELEASE_DMA;
	reu.TIMING_OFFSET_CBTD = TIMING_OFFSET_CBTD;
	reu.TIMING_DATA_HOLD = TIMING_DATA_HOLD;
	reu.TIMING_TRIGGER_DMA = TIMING_TRIGGER_DMA;
	reu.TIMING_ENABLE_ADDRLATCH = TIMING_ENABLE_ADDRLATCH;
	reu.TIMING_READ_BA_WRITING = TIMING_READ_BA_WRITING;
	reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING;
	reu.TIMING_ENABLE_DATA_WRITING = TIMING_ENABLE_DATA_WRITING;
	reu.TIMING_BA_SIGNAL_AVAIL = TIMING_BA_SIGNAL_AVAIL;
	reu.TIMING_RW_BEFORE_ADDR = TIMING_RW_BEFORE_ADDR;
	reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING_MINUS_RW_BEFORE_ADDR = TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING - TIMING_RW_BEFORE_ADDR;

	reu.CACHING_L1_WINDOW_KB = CACHING_L1_WINDOW_KB * 1024;
	reu.CACHING_L2_OFFSET_KB = CACHING_L2_OFFSET_KB * 1024;
	reu.CACHING_L2_PRELOADS_PER_CYCLE = CACHING_L2_PRELOADS_PER_CYCLE;
}

void initREU( void *mempool )
{
	reuMemory = (u8*)mempool;

	reu.reuSize = REU_SIZE_KB * 1024;

	reu.wrapAround = 0x80000; 
    reu.wrapAroundDRAM = reu.wrapAround; // except 1700
    reu.wrapStoring = reu.wrapAround - 1;

    reu.regBankUnused = REU_BANK_UNUSED_BITS;
	reu.preset = REU_STATUS_256K_CHIPS;

	switch ( REU_SIZE_KB )
	{
	case 128:
		reu.preset = 0;
		reu.wrapAround = 
		reu.wrapAroundDRAM = 0x20000; 
		break;
	case 256:
	case 512:
		break;
	default:
        reu.regBankUnused = 0;
	    reu.wrapAroundDRAM = reu.reuSize;
	    reu.wrapStoring = reu.reuSize - 1;
		break;
	}

	resetREU();
	initializeDMATimings();
}


__attribute__( ( always_inline ) ) inline void reuUpdateRegisters( u16 host_addr, u32 reu_addr, int len, u8 new_status_or_mask )
{
    reu_addr &= reu.wrapStoring;

    reu.status |= new_status_or_mask;

    if ( !(reu.command & REU_COMMAND_AUTOLOAD)) 
    {
        // no autoload
		if ( BITS_ALL_CLR( reu.addrREUCtrl, REU_ADDR_FIX_C64 ) )
			reu.addrC64 = host_addr;

		if ( BITS_ALL_CLR( reu.addrREUCtrl, REU_ADDR_FIX_REU ) )
		{
			reu.addrREU = reu_addr & 0xffff;
			reu.bank = ( reu_addr >> 16 ) & 0xff;
		}

        reu.length = len & 0xFFFF;
    } else 
    {
        reu.addrC64 = reu.shadow_addrC64;
        reu.addrREU = reu.shadow_addrREU;
		reu.bank = reu.shadow_bank;
        reu.length = reu.shadow_length;
    }

	if ( BITS_ALL_SET( new_status_or_mask, REU_STATUS_END_OF_BLOCK ) )
	{
		// check for interrupt, if no verify error
		if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_END_BLOCK | REU_INTERRUPTS_ENABLED ) )
		{
			reu.status |= REU_STATUS_INTERRUPT_PENDING;
			reu.irqTriggered = 1;
		}
	}

	if ( BITS_ALL_SET( new_status_or_mask, REU_STATUS_VERIFY_ERROR ) )
	{
		if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_VERIFY | REU_INTERRUPTS_ENABLED ) )
		{
			reu.status |= REU_STATUS_INTERRUPT_PENDING;
			reu.irqTriggered = 1;
		}
	}
}

#define REU_GET_INCREMENT   (1 - ( ( reu.addrREUCtrl >> 6 ) & 1 ))
#define REU_GET_C64INCREMENT   (1 - ( ( reu.addrREUCtrl >> 7 ) & 1 ))

__attribute__( ( always_inline ) ) inline void REU_INCREMENT_ADDRESS( u32 &r_a )
{
    u32 next = ( r_a & 0x0007ffff) + reu.incrREU;

    if (next == reu.wrapAround) 
        next = 0;
    
    r_a = (r_a & 0x00f80000) | next;
}

__attribute__( ( always_inline ) ) inline u32 REU_GET_NEXT_ADDRESS( u32 r_a )
{
    u32 next = ( r_a & 0x0007ffff) + reu.incrREU;
    if (next == reu.wrapAround) 
        next = 0;
    return (r_a & 0x00f80000) | next;
}

#define REU_INCREMENT_C64ADDRESS( a ) { a = ( a + reu.incrC64 ) & 0xffff; }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

__attribute__( ( always_inline ) ) inline void reuStore( u32 reu_addr, u8 value )
{
    reu_addr &= reu.wrapAroundDRAM - 1;
    if (reu_addr < reu.reuSize ) 
        reuMemory[reu_addr] = value;
}
#pragma GCC diagnostic pop

__attribute__( ( always_inline ) ) inline u8 reuLoad( u32 reu_addr )
{
    reu_addr &= reu.wrapAroundDRAM - 1;
	if ( reu_addr < reu.reuSize )
		return reuMemory[ reu_addr ];
    return 0xff;
}

__attribute__( ( always_inline ) ) inline u8 reuLoad32( u32 reu_addr )
{
	reu_addr &= reu.wrapAroundDRAM - 1;
	if ( reu_addr < reu.reuSize )
		return *(u32*)&reuMemory[ reu_addr ];
    return 0xff;
}

__attribute__( ( always_inline ) ) inline void reuPrefetch( u32 reu_addr )
{
	CACHE_PRELOAD_REU( &reuMemory[ ( reu_addr&~63 ) & ( reu.wrapAroundDRAM - 1 ) ] );
}

__attribute__( ( always_inline ) ) inline void reuPrefetchL1( u32 reu_addr )
{
	CACHE_PRELOADL1STRM( &reuMemory[ ( reu_addr&~63 ) & ( reu.wrapAroundDRAM - 1 ) ] );
}

__attribute__( ( always_inline ) ) inline void reuPrefetchW( u32 reu_addr )
{
	CACHE_PRELOADL1STRMW( &reuMemory[ ( reu_addr&~63 ) & ( reu.wrapAroundDRAM - 1 ) ] );
}


// gpu64: milestone 3 default-state screen mirror -- global rather than
// stack-local since reuUsingPolling() below is a cycle-critical,
// register-heavy function and these are too big to want living on its stack.
static u8 gpu64MirrorScreen[ 1000 ];
static u8 gpu64MirrorColor[ 1000 ];
static u8 gpu64MirrorBorder = 0, gpu64MirrorBackground = 0;
// $D018 as read, not decoded: the decode is pure arithmetic and belongs in
// showMirror(), where there is no C64 waiting on the bus.
static u8 gpu64MirrorD018 = 0x15;

// gpu64: the mirror's clock is now the C64's own jiffy IRQ, counted at the
// $FFFF vector fetch in reuUsingPolling() below, not main-loop passes. The
// KERNAL programs CIA1 timer A for ~60Hz on both PAL and NTSC, so 15 ticks
// is ~4 snapshots a second. Measured in VICE with a non-stopping checkpoint
// on a load of $FFFF: exactly 178 hits in 178 jiffies at the BASIC prompt --
// one per jiffy, no strays -- and 0 hits on the NMI vector $FFFA/$FFFB, so
// RESTORE cannot clock the mirror. 15 ticks is the rate the old pass counter
// was calibrated to, and fast enough that a typed LOAD line and the cursor
// blink read normally on HDMI.
#define GPU64_MIRROR_JIFFY_INTERVAL 15

// gpu64: main-loop passes (~1 C64 cycle each) without a jiffy IRQ vector
// fetch before the mirror concludes that BASIC no longer owns the machine.
// ~1s at 985kHz, i.e. ~60 jiffies -- short enough that an ML program which
// takes the IRQ over stops the mirror well inside a second.
//
// It only starts counting once a vector fetch has actually been seen
// (gpu64MirrorAwaitIrq). That is not a refinement, it is the whole reason a
// reset used to leave HDMI frozen for good, reported from the bench
// 2026-09-06: "when I reset, the C64 resets but mirror stays freezed". The
// KERNAL's reset path runs RAMTAS *before* its CLI, and RAMTAS is a
// per-byte read/write/verify sweep of all ~38.9K of free RAM at ~50 cycles a
// byte -- close to two seconds with interrupts masked throughout. The
// re-arm on the reset line fired correctly; the watchdog then expired in the
// middle of the RAM test and cleared the latch again, and nothing but
// another reset could ever set it. The comment that used to be here put the
// RAM test at ~200ms, which is where the error came from.
#define GPU64_MIRROR_IRQ_TIMEOUT 1000000

// gpu64: GPIO samples without a PHI2 edge before the loops below conclude the
// C64 has no clock -- i.e. that it is switched off, with the Pi still running
// on its own supply. Each iteration is one MMIO read, so this is ~30-60ms
// against a half-cycle of ~500ns: three orders of magnitude of margin, which
// is what makes a false positive on a live machine unreachable. Same value
// gpu64_mirrorIdleLoop() has always used for its own bounded wait.
#define GPU64_NOCLOCK_TIMEOUT 1000000

// gpu64: consecutive polling-loop passes that must all read the cartridge
// button as pressed before reuUsingPolling() hands the machine back to the
// RAD menu. See the debounced test near the bottom of the loop for why a
// single sample is not trustworthy. A pass is roughly one C64 cycle, so this
// is ~1ms -- two orders of magnitude below the ~50ms a finger holds a button,
// and far above any settling transient on the multiplexed pin.
#define GPU64_BUTTON_DEBOUNCE 1000
// Idle-loop passes the reset line must read low before its release counts.
#define GPU64_IDLE_RESET_MIN 1000

// gpu64 2026-09-10: DO NOT add a settling delay between SET_GPIO( bMPLEX_SEL )
// and the g3 read below. It was tried, at the bench, and it killed the bus
// outright -- first command BAD_ARGS, no frame clock, GET_HEALTH's DMA never
// delivering a byte through eight retries.
//
// The reasoning that led there is still correct as far as it goes.
// WAIT_UP_TO_CYCLE is a do-while against PMCCNTR_EL0 and armCycleCounter, an
// ABSOLUTE deadline anchored once per pass by RESTART_CYCLE_COUNTER, so a
// target already passed runs its body once and returns. Right for a deadline,
// wrong for a settling time -- and the multiplexer wait is a settling time.
// The patch was one MRS after the SET_GPIO plus WAIT_UP_TO_CYCLE_AFTER( 100,
// stamp ), sized to be a no-op on a punctual pass: the nominal gap is
// WAIT_CYCLE_MULTIPLEXER - WAIT_FOR_SIGNALS - TIMING_OFFSET_CBTD =
// 235 - 80 - 40 = 115 cycles with the shipped rad.cfg.
//
// What that assumed, and what the bench disproved, is that the pass is ever
// punctual here. It is not. Between the g2 read and this point sit two
// peripheral-bus accesses whose latency is not in anybody's cycle budget, so
// WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER ) is already falling through --
// the mux's settling time is whatever those accesses happen to cost, not
// what the constant says. A floor that only bites "when the pass was late"
// therefore bit on every pass, and 100 cycles of added spin per C64 cycle is
// past what the loop has.
//
// Two things follow. Raising WAIT_CYCLE_MULTIPLEXER in rad.cfg is equally
// useless -- same anchor, already passed. And the real settling margin here
// is unknown and unmeasured: it is a side effect, not a design. Measure it
// before touching it again.

// gpu64: the two C64 zero-page bytes the snapshot probes before it commits
// to the 2ms burst.
//
// CURLIN+1 ($3A) is BASIC's current line number, high byte: $FF while a
// direct-mode statement is being executed, the running line's number while a
// program executes. Measured in VICE (x64sc, binary monitor) on 2026-09-06:
// $FF at the prompt after any command, $00 for line 10 / $00 for line 20
// during RUN, $FF for the whole of a LOAD. So "$3A != $FF" is the canonical
// "a BASIC program is RUNning", and it is true from the first statement of
// RUN, before the program has drawn anything.
//
// The one exception, also measured, is the *cold* prompt: BASIC's cold start
// zeroes CURLIN, so $3A reads $00 from power-on until the first direct-mode
// statement runs, which is indistinguishable from "executing line 0-255".
// That is why the latch is armed by a prompt sighting first (see below) and
// why $3A takes no part in the bail decision -- gating the snapshot on
// $3A == $FF would mean never mirroring the fresh boot screen at all, which
// is the mirror's entire reason to exist.
//
// BLNSW ($CC) is the KERNAL's cursor-blink switch: 0 exactly while the
// input loop at the prompt is blinking the cursor, non-zero whenever the
// KERNAL is doing something else -- printing, LISTing, or driving the serial
// bus for LOAD/SAVE. It is not a program/no-program signal (it says nothing
// about ML), it is a "safe to steal 2ms of bus right now" signal: the IEC
// protocol is software-timed on both ends and a 2ms freeze mid-byte is
// exactly how a LOAD turns into ?LOAD ERROR. Measured in the same VICE
// session: $00 at the cold prompt and at the prompt between commands, $01
// for the whole of a RUN, $01 across SEARCHING FOR / the serial transfer of
// a LOAD (true drive emulation).
#define GPU64_C64_CURLIN_HI 0x003A
#define GPU64_C64_BLNSW     0x00CC

// gpu64: milestone 3 screen mirror. Grabs a brief DMA burst -- the same
// CLR_GPIO(bDMA_OUT)/DMA_READBYTE_P1..P3 cycle-stealing technique REU's own
// Store/Fetch transfers already use in handle_transfer.h, not a new kind of
// bus takeover -- to read the default screen RAM ($0400-$07E7) and color RAM
// ($D800-$DBE7), plus the current border/background color ($D020/$D021),
// then hands them to CRAD::showMirror() (rad_main.cpp) for rendering.
//
// WHEN this runs was reworked on 2026-09-06, for two independent reasons.
//
// (1) Correctness. This was the last async DMA hold in the tree: it fired on
// a free-running pass counter, so CLR_GPIO(bDMA_OUT) landed on whatever C64
// cycle the loop happened to be in. That is polling-loop rule 7 -- halting
// the 6510 through RDY stops it only on *read* cycles, and a write in flight
// completes with AEC already tri-stating the address bus, i.e. into a
// floating address. It is the same defect that killed the C64 through the
// whole Stage 16 campaign, and it was live here at 4 holds a second. It has
// never been implicated (BASIC's idle loop is nearly all reads, and the
// prompt touches IO2 never, so the sampled-IO2 gate the flip commit uses was
// not available to it) -- but "never observed" is not "cannot happen".
//
// The gate this now uses instead is the C64's own IRQ sequence: the loop
// sees the vector fetch at $FFFF (bTriggerFF00 is a hardware $FFxx decode,
// so unlike $A480 or $EA31 it is actually visible), and the very next 6510
// cycle after that fetch is the handler's opcode fetch -- a read, by
// construction, the same guarantee the IO2 gate rests on. It arrives once
// per jiffy, which is also exactly the clock the mirror wants.
//
// (2) Scope. The mirror exists so the user can see the READY prompt on HDMI
// and LOAD something without switching to the VIC-II screen. Once a program
// is actually running it has no business stealing 2ms of bus 4 times a
// second, so it now latches off for good, re-armed only by a C64 reset
// (resetREU()). Three things latch it off:
//
//   - CURLIN+1 != $FF, probed below: a BASIC program is RUNning.
//   - no jiffy IRQ for ~1s (GPU64_MIRROR_IRQ_TIMEOUT, checked in the loop):
//     something has taken the IRQ over or masked it -- an ML program.
//   - the gpu64 API being engaged at all (gpu64ApiActive), as before.
//
// Residual case, documented rather than fixed: an ML program SYSed from
// direct mode that leaves the KERNAL IRQ alone and never touches gpu64 keeps
// the latch armed. It costs almost nothing -- BLNSW will be non-zero, so
// every tick bails after the two probe bytes, a ~6 cycle hold at 4Hz -- and
// HDMI simply holds the last mirrored frame, which is the intended
// end state anyway.
// Scoped per dev/bus_access_design.md: fixed default addresses only (no VIC
// bank detection yet -- a program that relocates its screen will render
// wrong until that's added; $D018's charset select *is* followed), standard
// text mode only, and gpu64's own bundled copy
// of the C64 character ROM (gpu64_c64font.h) rather than peeking the C64's
// real char ROM, which the CPU can't see when it's banked out anyway. That
// copy is const; the mirror used to draw from font_bin, which is neither the
// real ROM nor immutable -- see the note in CRAD::showMirror().
//
// __attribute__ set to match reuUsingPolling() below (same file, same
// reasoning: this file's cache-preloading/alignment choices are load-bearing
// for cycle-precise timing, see the comment above reuLoad32() usage in
// reuUsingPolling()'s tail). Needs its own instruction-cache preload before
// first use -- see warmCache() in rad_main.cpp -- since it's called from
// inside reuUsingPolling() but isn't covered by that function's own preload
// window.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
void gpu64_mirrorSnapshot()
{
	register u32 g2;
	register u16 c_a;
	register u8 x;

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first -- see the long note in
	// gpu64_blobRead() below. The gate now calls us from inside the CPU
	// half-cycle, having just sampled the bus (it used to be the VIC half,
	// from the top of the loop); either way a bare WAIT_FOR_VIC_HALFCYCLE is
	// not a sync. It falls straight through when we are already in the VIC
	// half, RESTART_CYCLE_COUNTER then anchors mid-half-cycle,
	// TIMING_TRIGGER_DMA has already elapsed, and CLR_GPIO lands at an
	// undefined phase.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
	CLR_GPIO( bDMA_OUT );

	// gpu64: probe first, burst second. Both bytes are read every tick --
	// CURLIN unconditionally, because BLNSW is non-zero for the whole of a
	// BASIC program's run and gating the CURLIN read on it would mean never
	// noticing RUN at all.
	c_a = GPU64_C64_CURLIN_HI;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	register u8 curlinHi = x;

	c_a = GPU64_C64_BLNSW;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	register u8 blnsw = x;

	// gpu64: the prompt sighting that arms the CURLIN latch. Direct mode plus
	// a blinking cursor is BASIC idle at READY and nothing else. It cannot
	// fire before the user's first command (see the $3A note above), which is
	// exactly the point -- $3A is only trustworthy once it has been seen to
	// hold $FF, and until then the latch stays disarmed rather than reading
	// the cold prompt's $00 as "line 0 is running".
	if ( curlinHi == 0xFF && blnsw == 0 )
		gpu64MirrorSeenPrompt = 1;
	else
	if ( curlinHi != 0xFF && gpu64MirrorSeenPrompt )
		// A BASIC program is running. Done mirroring until the next reset.
		gpu64MirrorEnabled = 0;

	// gpu64: BLNSW alone decides whether we may take the bus for 2ms; $3A
	// decides whether we keep mirroring at all, and has already had its say
	// above.
	register u8 bail = ( blnsw != 0 ) ? 1 : 0;

	if ( bail )
	{
		// gpu64: give the bus back on a cycle boundary, exactly like the
		// command dispatch does -- not wherever the probe finished. The two
		// DMA_READBYTE_P3s above already dropped the address latch and the
		// bus transceiver (DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER with
		// releaseDMA false), so the DMA line is all that is still ours.
		WAIT_FOR_CPU_HALFCYCLE
		WAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		SET_GPIO( bDMA_OUT );
		return;
	}

	for ( u16 i = 0; i < 1000; i++ )
	{
		c_a = 0x0400 + i;
		DMA_READBYTE_P1( c_a );
		DMA_READBYTE_P2();
		DMA_READBYTE_P3( x, false );
		gpu64MirrorScreen[ i ] = x;
	}

	for ( u16 i = 0; i < 1000; i++ )
	{
		c_a = 0xD800 + i;
		DMA_READBYTE_P1( c_a );
		DMA_READBYTE_P2();
		DMA_READBYTE_P3( x, false );
		gpu64MirrorColor[ i ] = x & 0x0F;
	}

	// gpu64: $D018, so the mirror follows PRINT CHR$(14)/CHR$(142) between
	// the ROM's two charsets. One more byte on a hold that has just read
	// 2000 of them; showMirror() does the decoding.
	c_a = 0xD018;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	gpu64MirrorD018 = x;

	c_a = 0xD020;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, false );
	gpu64MirrorBorder = x & 0x0F;

	c_a = 0xD021;
	DMA_READBYTE_P1( c_a );
	DMA_READBYTE_P2();
	DMA_READBYTE_P3( x, true );		// last byte: release DMA back to the C64
	gpu64MirrorBackground = x & 0x0F;

	gpu64_showMirror( g_pRAD, gpu64MirrorScreen, gpu64MirrorColor, gpu64MirrorBorder, gpu64MirrorBackground, gpu64MirrorD018 );
	gpu64MirrorPainted++;
}

// gpu64: the C64 has stopped clocking the bus. With the Pi on its own supply
// the cartridge outlives the machine, so this is the ordinary consequence of
// switching the C64 off -- not a fault -- and it used to leave HDMI holding
// whatever was last scanned out, which is indistinguishable from a hung
// cartridge.
//
// Both bus loops call this from inside their bounded half-cycle wait, which
// is the one place in either of them with no deadline at all: there is no
// C64 to serve, so the mailbox round-trips below cost nothing that anything
// is waiting for.
//
// gpu64_apiFullReset() runs here as well as on the reset line because the
// holding screen needs a graphics-mode page 0 to be painted into, and the
// display may well have been left in 80x50 text mode by whatever was running
// when the power went off.
void gpu64_deadClockEnter( void )
{
	gpu64_apiFullReset( TRUE );
	gpu64_showHoldingScreen();
}

// gpu64: ...and the C64 is back. Unconditionally re-running the full reset
// covers the case the reset line does not: a clock interruption that never
// pulses /RESET would otherwise strand the display on the holding screen
// forever. On a real power-cycle /RESET is asserted at this moment anyway,
// and the reset branch's own one-shot will simply find nothing left to do.
void gpu64_deadClockLeave( void )
{
	gpu64_apiFullReset( TRUE );
}

// gpu64: what the boot idle loop saw, printed onto the HDMI log whenever
// the mirror has not painted for two seconds (tracker 89d). The C64U's
// start-up has failed three bench rounds in a row with nothing on either
// screen to say why; this puts the answer on the one screen that works.
typedef struct
{
	u32	passes, pairs, lone;			// this second
	u32	noClocks, resets, snaps, fallbacks, autoResets;	// running totals
	u32	seconds;
} GPU64IDLESTATS;

static GPU64IDLESTATS gpu64Idle;

// Once per power session: set by the automatic reset, cleared when the C64's
// clock comes back after being off.
static u8 gpu64IdleAutoResetDone = 0;

extern u8 gpu64MachineCameLate;
extern u64 gpu64MachineWaited;

static char *idleDec( char *p, u32 v )
{
	char t[ 12 ];
	int n = 0;
	do { t[ n++ ] = '0' + v % 10; v /= 10; } while ( v );
	while ( n ) *p++ = t[ --n ];
	return p;
}

static char *idleStr( char *p, const char *s )
{
	while ( *s ) *p++ = *s++;
	return p;
}

// Two log lines, each inside the log's 40 columns:
//   IDLE 12s P 985123 V 60 VX 0
//   NC 0 R 1 SN 3/3 FB 0 AR 0 LATE 1 W 4.2
// P passes, V $FFFE/$FFFF pairs, VX lone $FFFF reads -- all per second.
// NC dead-clock episodes, R reset releases, SN snapshots taken/painted,
// FB fallback (lone-$FFFF) snapshots, AR automatic resets, LATE/W whether
// and how long (seconds) checkIfMachineRunning() waited for the clock.
static void gpu64_idleReport( const char *pEvent )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	static char line[ 160 ];
	char *p = line;

	if ( pEvent )
	{
		p = idleStr( p, pEvent );
		*p++ = '\n';
	} else
	{
		p = idleStr( p, "IDLE " ); p = idleDec( p, gpu64Idle.seconds );
		p = idleStr( p, "s P " ); p = idleDec( p, gpu64Idle.passes );
		p = idleStr( p, " V " ); p = idleDec( p, gpu64Idle.pairs );
		p = idleStr( p, " VX " ); p = idleDec( p, gpu64Idle.lone );
		*p++ = '\n';
		p = idleStr( p, "NC " ); p = idleDec( p, gpu64Idle.noClocks );
		p = idleStr( p, " R " ); p = idleDec( p, gpu64Idle.resets );
		p = idleStr( p, " SN " ); p = idleDec( p, gpu64Idle.snaps );
		*p++ = '/'; p = idleDec( p, gpu64MirrorPainted );
		p = idleStr( p, " FB " ); p = idleDec( p, gpu64Idle.fallbacks );
		p = idleStr( p, " AR " ); p = idleDec( p, gpu64Idle.autoResets );
		p = idleStr( p, " LATE " ); p = idleDec( p, gpu64MachineCameLate );
		u32 tenths = (u32)( gpu64MachineWaited / 140000000ULL );
		p = idleStr( p, " W " ); p = idleDec( p, tenths / 10 );
		*p++ = '.'; p = idleDec( p, tenths % 10 );
		*p++ = '\n';
	}

	// Forced visible: the log may have been hidden by a program in an
	// earlier session, and apiFullReset() clears the page without repainting
	// it -- which is exactly the black HDMI this is here to explain.
	pFB->LogEnable( TRUE );
	pFB->LogWrite( line, (unsigned)( p - line ) );
}

// gpu64: pulse /RESET the way CRAD::Run()'s boot pulse does.
static void gpu64_idlePulseReset( void )
{
	OUT_GPIO( RESET_OUT );
	CLR_GPIO( bRESET_OUT );
	DELAY( 1 << 25 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );
}

static inline u64 idleNow( void )
{
	u64 t;
	asm volatile( "mrs %0, cntvct_el0" : "=r" ( t ) );
	return t;
}

// gpu64: the boot-phase mirror. RAD spends everything between "the C64 is
// running" and "the user pressed the button" in rad_main.cpp's radIsWaiting
// spin, which used to be a bare 1250-cycle button poll -- so HDMI showed
// nothing at all until the user had blind-navigated the RAD menu into REU
// mode. Reported from the bench 2026-09-06: "after start it does not
// activate, I have to press blindly REU menu button and then X to go to
// BASIC."
//
// This replaces that spin. It is the same $FFFF vector-fetch gate the REU
// polling loop runs (see the long comment at that gate, and rule 7 in
// CLAUDE.md): sample the bus every C64 cycle, and when the 6510 is fetching
// the high byte of the IRQ vector -- last cycle of the interrupt sequence,
// next cycle provably an opcode fetch -- take the bus and snapshot. Nothing
// else is being serviced here, so a missed sample costs one jiffy tick and
// nothing more; the phase discipline is what matters, not the throughput.
//
// Safe to run at this point in the boot flow because everything the sample
// sequence and the snapshot need is already set up: gpioInit() has
// configured the latch/OE/DIR/MPLEX/DMA pins, initREU() has filled in the
// reu.TIMING_* fields the WAIT_UP_TO_CYCLE calls read, and
// checkIfMachineRunning() has already blocked until the C64 clock is
// measurably alive -- so the half-cycle waits below cannot hang on a dead
// clock in the normal case. The abnormal case (clock stops while we sit
// here) is handled by bounding the top-of-loop wait rather than by hoping,
// because a hang here would also swallow the RAD button and leave the user
// with no way into the menu.
//
// Returns when the RAD button is pressed, i.e. exactly where the old spin's
// "goto hijacking" was.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
void gpu64_mirrorIdleLoop( void )
{
	register u32 g2 = bBUTTON, g3;
	register u16 resetCount = 0;
	register u32 ipl = 0;
	register u32 noClock = 0;
	register u32 noClockLatched = 0;
	register u32 prevVecLo = 0;			// the previous pass sampled a read of $FFFE

	// gpu64: wall-clock bookkeeping for the fallback clock, the HDMI report
	// and the start-up supervisor (tracker 89d). CNTVCT_EL0 is a system
	// register, not MMIO, and this loop has no per-cycle deadline anyway.
	u64 frq;
	asm volatile( "mrs %0, cntfrq_el0" : "=r" ( frq ) );
	u64 armedAt = idleNow();			// entry, reset release or clock return
	u64 lastPair = 0, lastFallback = 0;
	u64 lastSecond = armedAt, lastPaint = armedAt;
	u32 seenPair = 0;
	u32 paintedSeen = gpu64MirrorPainted, paintedAtArm = gpu64MirrorPainted;

	// gpu64: warm both this loop and the snapshot it calls. warmCache() runs
	// on the way into REU emulation, which is *after* this -- nothing has
	// preloaded either one yet at this point in the boot.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_mirrorIdleLoop, 1024 * 2 )
	FORCE_READ_LINEARa( (void*)gpu64_mirrorIdleLoop, 1024 * 2, 1024 * 2 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_mirrorSnapshot, 1024 * 4 )
	FORCE_READ_LINEARa( (void*)gpu64_mirrorSnapshot, 1024 * 4, 1024 * 4 );

	// gpu64: the mirror is armed on every entry here. resetREU() is the
	// other arm point and only runs once REU emulation starts; a user who
	// power-cycles into the menu and back out must not find the latch stuck
	// from a previous session.
	gpu64MirrorEnabled = 1;
	gpu64MirrorSeenPrompt = 0;
	gpu64MirrorIrqWatchdog = 0;
	gpu64MirrorAwaitIrq = 1;
	gpu64MirrorJiffy = 0;

mirrorIdleLoop:

	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER

	while ( 1 )
	{
		// gpu64: bounded form of WAIT_FOR_VIC_HALFCYCLE, the same as the REU
		// loop's. The reason differs, and it is why the button test below
		// comes first here and last there: at this point the RAD button still
		// has to work, and a C64 whose clock stopped while we were watching
		// would otherwise strand the user in a spin with no exit. On timeout
		// we simply fall through with whatever g2 held: the button test below
		// is all that still matters.
		noClock = 0;
		{
			register u32 timeout = 0;
			do {
				g2 = read32( ARM_GPIO_GPLEV0 );
				if ( ++timeout > 1000000 ) { noClock = 1; break; }
			} while ( CPU_HALF_CYCLE );
		}

		// gpu64: the button is tested before anything that needs the C64
		// clock, so a stopped clock degrades to exactly the button poll this
		// loop replaced rather than to a hang.
		if ( BUTTON_PRESSED )
			return;

		if ( noClock )
		{
			if ( !noClockLatched )
			{
				noClockLatched = 1;
				gpu64Idle.noClocks++;
				gpu64_deadClockEnter();
			}
			prevVecLo = 0;
			continue;
		}
		if ( noClockLatched )
		{
			noClockLatched = 0;
			gpu64_deadClockLeave();
			// A new power session: the start-up supervisor may act again.
			gpu64IdleAutoResetDone = 0;
			seenPair = 0;
			armedAt = lastPaint = idleNow();
			paintedAtArm = gpu64MirrorPainted;
		}

		// gpu64: same incremental i-cache refresh the REU loop does. The
		// snapshot's showMirror() paints 40x25 cells into the framebuffer and
		// cleans a page, which is more than enough to evict this loop between
		// ticks -- and a cold miss in the sample sequence is a misread
		// address, which under the gate below is a DMA hold opened on a cycle
		// nothing is known about. Rule 4.
		void *p = (u8*)( && mirrorIdleLoop ) + ipl;
		CACHE_PRELOADIKEEP( p );
		ipl += 64; if ( ipl >= 1024 * 2 ) ipl = 0;

		// gpu64: re-arm on the C64's reset line -- pressing the RAD reset
		// button at the BASIC prompt is the user's way of saying "start
		// mirroring again", and before this it did nothing visible on HDMI at
		// all. resetREU() does the same thing for the REU-emulation half of
		// the machine's life; this is the boot-phase half, where resetREU()
		// never runs.
		//
		// On the RELEASE edge, not the assert: while the line is held the
		// 6510 is stopped and no jiffy IRQ arrives, so a press held longer
		// than GPU64_MIRROR_IRQ_TIMEOUT (~1s -- easy for a thumb) would let
		// the watchdog below retire the mirror again right after arming it.
		// Holding the watchdog clear for the duration and arming when the
		// machine actually restarts avoids having to reason about which of
		// the two wins.
		//
		// Debounced (tracker 93): the edge counts only after the line read
		// low on GPU64_IDLE_RESET_MIN consecutive passes (~1 ms; a real
		// press or power-up pulse lasts far longer). On the C64U a single
		// low sample was enough, and the bench counted 3306 display resets
		// at one BASIC prompt.
		if ( CPU_RESET )
		{
			if ( resetCount < GPU64_IDLE_RESET_MIN )
				resetCount++;
			gpu64MirrorIrqWatchdog = 0;
		} else
		if ( resetCount < GPU64_IDLE_RESET_MIN )
			resetCount = 0;			// a glitch, not a reset
		else
		{
			resetCount = 0;
			gpu64MirrorEnabled = 1;
			gpu64MirrorSeenPrompt = 0;
			gpu64MirrorIrqWatchdog = 0;
			gpu64MirrorAwaitIrq = 1;
			gpu64MirrorJiffy = 0;
			gpu64Idle.resets++;
			armedAt = lastPaint = idleNow();
			// gpu64: and the display back to its post-boot state, so a
			// reset taken at the menu-idle mirror behaves exactly like one
			// taken during REU emulation. Free here -- nothing is being
			// emulated at radIsWaiting, so the VideoCore work this may do
			// has nobody waiting on it.
			gpu64_apiFullReset( TRUE );
		}

		// gpu64: the liveness timer, same as the REU loop's. Without the
		// KERNAL IRQ there is no clock and no prompt to mirror.
		//
		// Suspended until the first vector fetch after a re-arm: the C64 we
		// just watched come out of reset spends ~2s in RAMTAS with interrupts
		// masked, which is twice the timeout. Waiting forever is the right
		// behaviour anyway -- a re-armed mirror with no IRQ takes no bus and
		// paints nothing, so there is nothing to time out of.
		if ( gpu64MirrorEnabled && !gpu64MirrorAwaitIrq )
		{
			if ( ++gpu64MirrorIrqWatchdog >= GPU64_MIRROR_IRQ_TIMEOUT )
				gpu64MirrorEnabled = 0;
		}

		// gpu64: the bus sample, copied cycle-for-cycle from
		// reuUsingPolling(). g2 carries RESET/BUTTON/RW/$FFxx, g3 the
		// multiplexed low address byte.
		SET_GPIO( bDIR_Dx );
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( reu.WAIT_FOR_SIGNALS + reu.TIMING_OFFSET_CBTD );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );

		WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		// gpu64: this fires the snapshot directly on the vector fetch,
		// i.e. on the original unconfirmed form of rule 7 -- it is the one
		// hold in the tree that gpu64_holdgate.h's forward confirmation was
		// deliberately NOT applied to. Two reasons, and both have to hold:
		//
		//  - The confirm's term 2 ("the previous pass really was the
		//    previous C64 cycle") is a comparison of PMCCNTR_EL0 stamps
		//    against gpu64HoldGap.armPerC64, and armPerC64 is measured on
		//    the way into REU emulation, which is *after* this loop. There
		//    is no calibration here to compare against, and a confirmation
		//    whose adjacency term always passes is not a confirmation.
		//
		//  - What the rule protects against is a program that reaches
		//    $FFFF, or an IO2 address, on a cycle that is not its
		//    instruction's last -- a read-modify-write, or an indexed
		//    access whose un-fixed address lands there. The only code alive
		//    at this point in the boot is the KERNAL and BASIC at the READY
		//    prompt, which reach $FFFF exactly once, as the interrupt
		//    sequence's vector fetch. Once a program that could do
		//    otherwise is running, this loop is no longer the one sampling
		//    the bus: reuUsingPolling() is, and it uses the confirmed gate.
		//
		// If this loop ever gains a caller that runs with user code live,
		// that second reason evaporates and this needs the real gate --
		// which means moving the armPerC64 calibration ahead of it first.
		//
		// Both reasons assume the gate sees only real vector fetches, and
		// on the C64 Ultimate a bare $FFFF read does not mean one: its
		// cartridge port shows $FFFF reads on cycles that are no fetch at
		// all, in bulk. Firing a hold every 15 of those starved the booting
		// KERNAL (~30s to READY), painted uninitialised screen RAM onto
		// HDMI, and opened holds on cycles nothing was known about. So the
		// fetch counts only when the previous pass read $FFFE -- the pair is
		// the interrupt sequence's last two cycles, and a stray $FFFF is not
		// preceded by it.
		register u32 vecHi = ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFF;
		register u64 now = idleNow();
		gpu64Idle.passes++;

		if ( vecHi && prevVecLo )
		{
			gpu64Idle.pairs++;
			seenPair = 1;
			lastPair = now;
		} else
		if ( vecHi )
			gpu64Idle.lone++;

		if ( vecHi && prevVecLo && gpu64MirrorEnabled )
		{
			gpu64MirrorIrqWatchdog = 0;
			gpu64MirrorAwaitIrq = 0;

			if ( ++gpu64MirrorJiffy >= GPU64_MIRROR_JIFFY_INTERVAL )
			{
				gpu64MirrorJiffy = 0;
				gpu64Idle.snaps++;
				gpu64_mirrorSnapshot();
			}
		} else
		// gpu64: the C64U's substitute clock (see GPU64_MIRROR_LONE_INTERVAL).
		// Here it is timed in wall-clock rather than passes, since this loop
		// has no deadline to protect: four snapshots a second, only while no
		// pair has been seen for two seconds, and not in the first three
		// seconds after a reset -- a snapshot before the KERNAL has cleared
		// screen RAM paints garbage. The hold fires on the lone read itself,
		// unconfirmed, like the pair's above. On the C64U that cycle is not a
		// CPU cycle at all, and the first bench session there fired a hold
		// every 15 of them without harming the machine.
		if ( vecHi && gpu64MirrorEnabled &&
				now - lastPair > 2 * frq && now - armedAt > 3 * frq &&
				now - lastFallback > frq / 4 )
		{
			lastFallback = now;
			gpu64Idle.snaps++;
			gpu64Idle.fallbacks++;
			gpu64_mirrorSnapshot();
			vecHi = 0;					// the pass after a hold is not adjacent
		}

		prevVecLo = ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFE && !vecHi;

		// gpu64: once a second -- the report, and the start-up supervisor.
		if ( now - lastSecond >= frq )
		{
			lastSecond = now;
			gpu64Idle.seconds++;

			if ( gpu64MirrorPainted != paintedSeen )
			{
				paintedSeen = gpu64MirrorPainted;
				lastPaint = now;
			}

			// The mirror has not painted for two seconds: say why, on HDMI.
			// Silent while it works, so the report never fights the mirror.
			if ( gpu64MirrorEnabled && now - lastPaint > 2 * frq )
			{
				gpu64_idleReport( 0 );
				prevVecLo = 0;
			}

			// A machine that has shown neither an IRQ nor a prompt six
			// seconds after it started is not running BASIC. On the C64U this
			// is the power-on black screen (tracker 89c/89d): the machine
			// comes up after the Pi's boot reset and stays dark until the
			// user presses the cartridge's reset. Do that press for them,
			// once per power session. A breadbin's IRQ is live ~2s after a
			// reset, so it never gets here.
			if ( !gpu64IdleAutoResetDone && !seenPair &&
					gpu64MirrorPainted == paintedAtArm && now - armedAt > 6 * frq )
			{
				gpu64IdleAutoResetDone = 1;
				gpu64Idle.autoResets++;
				gpu64_idleReport( "AUTO RESET: no IRQ, no prompt in 6s" );
				gpu64_idlePulseReset();

				gpu64MirrorEnabled = 1;
				gpu64MirrorSeenPrompt = 0;
				gpu64MirrorIrqWatchdog = 0;
				gpu64MirrorAwaitIrq = 1;
				gpu64MirrorJiffy = 0;
				armedAt = lastPaint = idleNow();
				paintedAtArm = gpu64MirrorPainted;
				prevVecLo = 0;
			}

			gpu64Idle.passes = gpu64Idle.pairs = gpu64Idle.lone = 0;
		}
	}
}

// gpu64: commits a vblank-deferred PAGE_FLIP. Everything else a frame
// boundary does is inlined into the loop (gpu64_vsyncAdvance(),
// gpu64_vsync.h) precisely so this is the only call on the path -- and this
// one only happens when a flip is actually pending.
//
// WHEN it is called matters as much as what it does. The frame boundary only
// *arms* it (gpu64Vsync.commitDue); the loop fires it from the sampled-IO2
// gate just before noREUAccess:, where the C64's next cycle is provably a
// read. Taking the bus next to a C64 write cycle is what killed the C64 in
// bench runs M and N -- the long comment at that gate has the reasoning and
// the evidence.
//
// As of 2026-09-07 that gate no longer fires on the IO2 access itself: it
// arms there and opens one C64 cycle later, once that cycle has confirmed
// itself a read at a different address. gpu64_holdgate.h has the four terms
// and the two instruction shapes that made the old form unsound.
//
// The commit is bracketed in a DMA hold, exactly like the command dispatch
// and the mirror snapshot. SetVirtualOffset() is a mailbox round-trip to the
// VideoCore whose cost nobody has measured (it was open question 1 in
// dev/milestone4_2d_api_design.md), and this runs while the C64 is
// free-running through the loop -- so without the hold, a slow mailbox call
// would silently eat the C64's next IO2 access, which is milestone 4's
// bug #2 all over again. With the hold, the cost stops mattering for
// correctness and only makes the burst longer.
//
// This function's instruction cache is warmed by gpu64_vsyncWarmCommit()
// below, called from the PAGE_FLIP dispatch that queues the flip -- i.e.
// while the bus is already held, rather than here where warming would itself
// be the unprotected delay it is meant to prevent. Same __attribute__ pair
// as gpu64_mirrorSnapshot() above.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
void gpu64_vsyncCommitFlip( void )
{
	// The WAIT_FOR_*_HALFCYCLE macros sample the GPIO bank into a variable
	// they expect to be called g2, same as gpu64_mirrorSnapshot() above.
	register u32 g2;
	(void)g2;

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first, same reason as
	// gpu64_mirrorSnapshot() above -- WAIT_FOR_VIC_HALFCYCLE alone is not a
	// sync and the bus would be taken at whatever phase the caller finished
	// on. The gate now calls us from inside the CPU half-cycle (it has just
	// sampled the bus), possibly late enough to have slipped past the
	// falling edge, which is exactly the case the dispatch hold's own
	// WAIT_FOR_CPU_HALFCYCLE covers. This fires once a frame for the whole
	// run, which is exactly the shape of damage that accumulates.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
	CLR_GPIO( bDMA_OUT );
	gpu64_holdGapAssert( armCycleCounter, GPU64_HOLD_KIND_COMMIT );

	gpu64_commitFlip();

	// gpu64 (2026-09-08): the whole STATUS update happens INSIDE the hold,
	// and the FRAME_READY harvest happens here rather than only at the next
	// dispatch. Two things follow from the placement, both deliberate:
	//
	// 1. gpu64_3dPollLoopFrame() reads gpu64_3dRing.tail, which core 1
	//    writes. CLAUDE.md forbids that read anywhere reuUsingPolling() can
	//    reach with the bus free-running -- but the bus is held here, so the
	//    per-C64-cycle deadline that rule is about does not exist, exactly
	//    as it does not for a command dispatch. Its i-cache is warmed by
	//    gpu64_vsyncWarmCommit() alongside this function's own.
	// 2. The three writes are now atomic from the C64's point of view: it
	//    cannot sample STATUS at all between them. Clearing BUSY after the
	//    release left a window in which a program polling the documented
	//    "FRAME_READY set and BUSY clear" predicate could see the frame
	//    ready but the flip apparently still pending, and go round again.
	//
	// Cost is one non-blocking modular-distance test and three stores added
	// to a hold that already contains a VideoCore mailbox round trip.
	gpu64_3dPollLoopFrame();
	gpu64Vsync.flipPending = 0;
	gpu64Regs.status &= ~GPU64_STATUS_BUSY;

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	SET_GPIO( bDMA_OUT );
	gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_COMMIT );
}

// gpu64: called from the PAGE_FLIP dispatch, with the bus held, to warm the
// commit path before the loop needs it. The dispatch that queues a flip is
// the biggest instruction-cache consumer in the system and will have evicted
// whatever warmCache() preloaded at REU start -- see the general rule in
// dev/progress_tracker.md. Doing it here rather than at the point of use is
// what keeps the loop's own exposure to a single short call.
void gpu64_vsyncWarmCommit( void )
{
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_vsyncCommitFlip, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_vsyncCommitFlip, 1024 * 2, 1024 * 2 );

	// gpu64: and the mailbox post the commit now ends in, which lives in
	// another translation unit and is not covered by the preload above.
	gpu64_flipWarm();

	// gpu64 (2026-09-08): and the FRAME_READY harvest the commit now ends
	// in, which is in gpu64_3d_class1.cpp and so is not covered by the
	// preload above either. Warming it here, from a dispatch that already
	// holds the bus, is rule 5 -- warming it at the point of use would put
	// the delay inside the once-a-frame hold it is meant to shorten.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_3dPollLoopFrame, 512 );
	FORCE_READ_LINEARa( (void*)gpu64_3dPollLoopFrame, 512, 512 );

	// gpu64: and the hold-gap instrument's single line, so the store the
	// commit's release does cannot take a cold miss in the one window where
	// the C64 is already free-running again (gpu64_holdgap.h, rule 4).
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGap, sizeof( gpu64HoldGap ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGap, sizeof( gpu64HoldGap ), sizeof( gpu64HoldGap ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGate, sizeof( gpu64HoldGate ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGate, sizeof( gpu64HoldGate ), sizeof( gpu64HoldGate ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64BusStats, sizeof( gpu64BusStats ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64BusStats, sizeof( gpu64BusStats ), sizeof( gpu64BusStats ) * 8 );
}

// gpu64: forces every cache line of a buffer resident before a DMA burst
// walks it, and does it with real loads rather than a prfm hint -- prfm is
// advisory and the hardware is free to drop it, and this is the one place
// where "mostly warmed" and "warmed" differ by a corrupted byte.
//
// This replaced a 1024-byte warm that was the cause of the last failing
// conformance check. An 8000-byte upload came back with a handful of wrong
// bytes, always beyond offset 1024 -- eleven of them across five hardware
// runs, the lowest at 1057 -- and a second, warm upload of the same data was
// clean every time. Past the warm window every 64th store lands on a cold
// line, the core has to read-allocate it from an SDRAM the VideoCore is also
// using, and the stall outlasts the C64 cycle the burst is riding. The byte
// sampled is then whatever the bus moved on to.
//
// Free in bus terms: the C64 is DMA-halted here, and the warm costs one load
// per cache line against a burst that spends a whole C64 cycle per byte.
static inline void gpu64_warmBuffer( const u8 *p, u32 len )
{
	__attribute__( ( unused ) ) volatile u8 forceRead;
	for ( u32 i = 0; i < len; i += 64 )
		forceRead = p[ i ];
	if ( len )
		forceRead = p[ len - 1 ];
}

// gpu64: milestone 4 blob transfers -- the "commands by reference" half of
// the API (docs/api_design.md). A C64-space transfer is the same brief,
// bounded DMA burst gpu64_mirrorSnapshot() above already proves on hardware,
// just parameterized instead of hardcoded to screen RAM; an REU-space
// transfer never touches the bus at all, since REU memory is ours.
//
// Same __attribute__s and the same cache-preload requirement as
// gpu64_mirrorSnapshot() -- see warmCache() in rad_main.cpp.
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
u8 gpu64_blobRead( u8 space, u32 addr, u32 len, u8 *pDst )
{
	if ( len == 0 )
		return GPU64_ERR_OK;

	if ( space == GPU64_SPACE_REU )
	{
		if ( addr + len > reu.reuSize )
			return GPU64_ERR_OUT_OF_RANGE;
		memcpy( pDst, &reuMemory[ addr ], len );
		return GPU64_ERR_OK;
	}

	if ( space != GPU64_SPACE_C64 )
		return GPU64_ERR_OUT_OF_RANGE;

	if ( addr + len > 65536 )
		return GPU64_ERR_OUT_OF_RANGE;

	// The bus is already held: this is only ever reached from a CMD_LO
	// dispatch, which asserts DMA around the whole command (see the write
	// handler in reuUsingPolling()). Sync to a cycle boundary and go.
	register u32 g2;
	register u16 c_a;
	register u8 x;

	// gpu64: warm this function's own instruction cache immediately before
	// the burst, not just once in warmCache(). The dispatch that got us here
	// runs a lot of code first -- a CLEAR alone walks 64000 framebuffer
	// bytes and cleans them -- which is more than enough to evict what
	// warmCache() preloaded at REU start. The observed symptom was exact:
	// the first launch of a program lost the first two bytes of its first
	// blob read, every subsequent RUN was clean. This is the same
	// preload-then-force-read pair warmCache() uses, just at the point where
	// it is actually needed. Free in bus terms: the C64 is DMA-halted here,
	// so nothing is racing us.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_blobRead, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_blobRead, 1024 * 2, 1024 * 2 );
	// ...and the destination, ALL of it. This used to warm the first 1024
	// bytes only, which is what made every large transfer come back with a
	// few wrong bytes -- see gpu64_warmBuffer() above for the evidence.
	gpu64_warmBuffer( pDst, len );

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first, so this catches the *transition*
	// into the VIC half-cycle. WAIT_FOR_VIC_HALFCYCLE alone returns
	// immediately if we are already in one -- and we get here at whatever
	// phase the dispatcher's argument decoding happened to finish, unlike
	// handle_transfer.h, which enters straight off the loop's own sync. The
	// symptom of getting this wrong is subtle: DMA_READBYTE_P2's
	// WAIT_FOR_CPU_HALFCYCLE re-syncs the burst, so only the first byte or
	// two of a transfer come back wrong, at random.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );

	for ( u32 i = 0; i < len; i++ )
	{
		c_a = (u16)( addr + i );
		DMA_READBYTE_P1( c_a );
		DMA_READBYTE_P2();
		DMA_READBYTE_P3( x, false );			// never releases: dispatch owns the hold
		pDst[ i ] = x;
	}

	return GPU64_ERR_OK;
}

__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
u8 gpu64_blobWrite( u8 space, u32 addr, u32 len, const u8 *pSrc )
{
	if ( len == 0 )
		return GPU64_ERR_OK;

	if ( space == GPU64_SPACE_REU )
	{
		if ( addr + len > reu.reuSize )
			return GPU64_ERR_OUT_OF_RANGE;
		memcpy( &reuMemory[ addr ], pSrc, len );
		return GPU64_ERR_OK;
	}

	if ( space != GPU64_SPACE_C64 )
		return GPU64_ERR_OUT_OF_RANGE;

	if ( addr + len > 65536 )
		return GPU64_ERR_OUT_OF_RANGE;

	// The destination window (SET_DMA_WINDOW, $0C). Every readback opcode
	// takes its destination in ARG bytes that SEQ/SEQACK does not cover, so
	// one dropped store aims this burst somewhere the client never named --
	// and this is the only direction in which a dropped byte makes the Pi
	// write into C64 memory. A client that declares a window gets a refused
	// transfer instead of its own code overwritten; length 0 is the default
	// and means unrestricted, so this costs nothing for anyone who has not
	// opted in. Two comparisons, dispatch-time, bus already held.
	if ( gpu64DmaWinLen != 0 &&
	     ( addr < (u32)gpu64DmaWinBase ||
	       addr + len > (u32)gpu64DmaWinBase + (u32)gpu64DmaWinLen ) )
		return GPU64_ERR_OUT_OF_RANGE;

	// Write direction: same cycle-stealing burst as the read above, using the
	// macros REU's own Stash transfer uses (handle_transfer.h). This is the
	// first time gpu64 pushes data *back* to the C64 rather than reading it.
	// Bus already held by the dispatch wrapper, same as gpu64_blobRead().
	register u32 g2;
	register u16 c_a;

	// gpu64: warm this function's own instruction cache immediately before
	// the burst, not just once in warmCache(). The dispatch that got us here
	// runs a lot of code first -- a CLEAR alone walks 64000 framebuffer
	// bytes and cleans them -- which is more than enough to evict what
	// warmCache() preloaded at REU start. The observed symptom was exact:
	// the first launch of a program lost the first two bytes of its first
	// blob read, every subsequent RUN was clean. This is the same
	// preload-then-force-read pair warmCache() uses, just at the point where
	// it is actually needed. Free in bus terms: the C64 is DMA-halted here,
	// so nothing is racing us.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_blobWrite, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_blobWrite, 1024 * 2, 1024 * 2 );
	gpu64_warmBuffer( pSrc, len );			// all of it -- see gpu64_blobRead()

	// gpu64: WAIT_FOR_CPU_HALFCYCLE first, so this catches the *transition*
	// into the VIC half-cycle. WAIT_FOR_VIC_HALFCYCLE alone returns
	// immediately if we are already in one -- and we get here at whatever
	// phase the dispatcher's argument decoding happened to finish, unlike
	// handle_transfer.h, which enters straight off the loop's own sync. The
	// symptom of getting this wrong is subtle: DMA_READBYTE_P2's
	// WAIT_FOR_CPU_HALFCYCLE re-syncs the burst, so only the first byte or
	// two of a transfer come back wrong, at random.
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );

	for ( u32 i = 0; i < len; i++ )
	{
		c_a = (u16)( addr + i );
		DMA_WRITEBYTE_P1( c_a, pSrc[ i ] );
		DMA_WRITEBYTE_P2( false );			// never releases: dispatch owns the hold
	}

	return GPU64_ERR_OK;
}

// gpu64: reuUsingPolling()'s own entry address, captured on the way in. Only
// used by gpu64_apiWarmPollingLoop() below.
static void *s_PollLoopBase = 0;

// gpu64: re-warms everything the polling loop depends on and a command
// dispatch has just evicted. Called at the end of a class 1 dispatch, with
// the bus still held -- rule 5 in dev/progress_tracker.md's polling-loop
// timing rules: warm from upstream, where the C64 is already stopped, never
// at the point of use.
//
// Why this is needed at all is rule 4: "preloaded at start-up" is not
// durable. A single class 0 CLEAR walking 64000 bytes was enough to evict
// warmCache()'s work; a DRAW_MESH walks a framebuffer, a z-buffer and an
// arena, which is far more. Two distinct casualties:
//
//   1. The loop's instruction window. It partly self-heals -- the loop rolls
//      a CACHE_PRELOADIKEEP 64 bytes forward every pass -- but that is ~106
//      passes to come fully back, and the loop runs cold for all of them.
//   2. gpu64Regs and gpu64Vsync. rad_main.cpp's warmCache() preloads these
//      L1KEEP precisely because the loop touches them on every ARG write and
//      every STATUS read, and *nothing* re-warms them after a dispatch. This
//      is the same hole gpu64_vsyncWarmCommit() already plugs for the flip
//      commit path.
void gpu64_apiWarmPollingLoop( void )
{
	// gpu64: re-warm the deferred-flip commit path if one is armed, and do it
	// *before* the loop preload below so the loop is always the last thing to
	// touch the instruction cache before the bus is released.
	//
	// gpu64_vsyncWarmCommit() is otherwise issued exactly once, by the
	// PAGE_FLIP / SCENE_COMMIT that arms the flip. Until Stage 16 that was
	// enough: the only program that ever armed a deferred flip
	// (gpu64_vblank_demo.a) then sat in a STATUS read loop until BUSY
	// cleared, so nothing ran between the arm and the boundary. Stage 16's
	// SCENE_COMMIT returns immediately and the C64 keeps issuing commands, so
	// several full dispatches now run inside the armed window, each one the
	// biggest instruction-cache consumer in the system -- and the loop preload
	// below cannot cover the commit path, which sits 0xE00 bytes *before*
	// s_PollLoopBase (0x85a00 vs 0x86800 as linked), outside the window.
	//
	// So the commit was firing cold once a frame, at the one place where a
	// stall between RESTART_CYCLE_COUNTER and the GPIO write takes or gives
	// back the bus at an undefined phase. Warming here is rule 5: every caller
	// still holds the bus, so the preload costs hold time rather than a missed
	// C64 access, and it only runs while a flip is actually armed.
	//
	// BENCH A/B, 2026-09-05: still a hypothesis. The three runs that first
	// carried it showed gpu64_loop_test's SEQ/SEQACK drop detector at a
	// remarkably constant ~0.19 lost C64 writes per accepted flip (4/26,
	// 25/129, 10/51) where the pre-fix run reported zero. Set this to 0 to
	// build the other arm and find out whether this warm is the author of
	// those drops or merely the first build to run alongside them.
#define GPU64_WARM_COMMIT_AT_DISPATCH 1

#if GPU64_WARM_COMMIT_AT_DISPATCH
	if ( gpu64Vsync.flipPending )
		gpu64_vsyncWarmCommit();
#endif

	if ( s_PollLoopBase )
		CACHE_PRELOAD_INSTRUCTION_CACHE( s_PollLoopBase, GPU64_POLL_IPL_WINDOW );

	CACHE_PRELOAD_DATA_CACHE( &gpu64Regs, sizeof( GPU64REGS ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64Regs, sizeof( GPU64REGS ), sizeof( GPU64REGS ) * 8 );

	CACHE_PRELOAD_DATA_CACHE( &gpu64Vsync, sizeof( GPU64VSYNC ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64Vsync, sizeof( GPU64VSYNC ), sizeof( GPU64VSYNC ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGap, sizeof( gpu64HoldGap ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGap, sizeof( gpu64HoldGap ), sizeof( gpu64HoldGap ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64HoldGate, sizeof( gpu64HoldGate ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64HoldGate, sizeof( gpu64HoldGate ), sizeof( gpu64HoldGate ) * 8 );
	CACHE_PRELOAD_DATA_CACHE( &gpu64BusStats, sizeof( gpu64BusStats ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64BusStats, sizeof( gpu64BusStats ), sizeof( gpu64BusStats ) * 8 );
}

// gpu64: this and the other four functions sharing the ".text.section_polling"
// section (gpu64_mirrorSnapshot, gpu64_vsyncCommitFlip, gpu64_blobRead,
// gpu64_blobWrite, below) are grouped together deliberately -- see each
// site's own comment for why. The ".text." prefix is load-bearing, not
// cosmetic: circle.ld (external/, gitignored, not ours to edit) places
// *(.text*) before it sets _etext, and translationtable64.cpp's MMU setup
// marks every 64KB page whose base is >= _etext execute-never (PXN). A bare
// section name -- what this used to be -- is an orphan section ld places
// *after* .text, i.e. entirely past _etext, so every function in it lived on
// borrowed time: fine as long as the whole group fit in the last 64KB page
// shared with the rest of .text, a silent crash the moment growth elsewhere
// in the image pushed reuUsingPolling's tail across the next page boundary.
// That is exactly what stage 14 did: two harmless-looking Stage-14 builds
// (12 opcodes, no DRAW_NODE) fit; the two that also linked DRAW_NODE did
// not, and the ~2KB tail of this function that landed past the boundary
// faulted on fetch -- IO2 unserviced, floating bus reads, dead RAD menu,
// power-cycle required. Diagnosed 2026-08-29 by hardware bisection down to
// DRAW_NODE, then confirmed by reading _etext and section_polling's
// placement out of the ELF with nm/readelf, not by guessing. The ".text."
// prefix makes this function part of .text by construction, so it is always
// on the PXN=0 side of _etext regardless of image size.
#if 1
__attribute__( ( optimize( "align-functions=256" ) ) )
__attribute__( ( section( ".text.section_polling" ) ) )
u8 reuUsingPolling( int step )
{
	register u32 g2 = bBUTTON, g3;
	register u32 muxRetried = 0;
	register u16 resetCount = 0;

	// gpu64: consecutive passes that have read the button as pressed.
	register u16 buttonCount = 0;

	// gpu64: the dead-clock detector's two flags -- one per pass, one across
	// passes. See the bounded wait at the top of the loop body below.
	register u32 noClock = 0;
	register u32 noClockLatched = 0;

	// gpu64: milestone 6's load-ladder instrumentation (gpu64_ladder.h).
	// Expands to nothing unless GPU64_LADDER_ENABLED is defined.
	GPU64_LADDER_LOCALS

	u16 ipl = 0;

	// gpu64: the hold gate (gpu64_holdgate.h). All of it lives in registers
	// for the life of the loop -- the per-pass cost is the two moves at the
	// bottom of the body and one predicted-not-taken branch at the gate.
	register u32 gateArmed = GPU64_GATE_NONE;
	register u32 gateD = 0;				// CMD_LO payload of a deferred dispatch
	register u32 gateAge = 0;			// C64 cycles this arm has been waiting
	register u32 prevAnchor = 0;		// what the *previous* pass sampled, as
										//   GPU64_ANCHOR_* | low address byte
	register u64 prevStamp = 0;			// armCycleCounter of the previous pass
	register u32 gateSpan = 0;			// ARM cycles still counting as "next cycle"
	register u32 prevVecLo = 0;			// the previous pass sampled a read of $FFFE

	// gpu64: the loop's own base address, published so a dispatch can re-warm
	// the window it just evicted (gpu64_apiWarmPollingLoop() below). The label
	// is local to this function, so there is no other way to reach it from
	// another translation unit.
	s_PollLoopBase = (void *)( && reuEmulationMainLoop );

	if ( step <= 1 )
	{
		void *p = ( && reuEmulationMainLoop );
		CACHE_PRELOAD_INSTRUCTION_CACHE( p, GPU64_POLL_IPL_WINDOW );

		if ( step == 1 ) return 0;

		// gpu64: this block is also where armPerC64 comes from
		// (gpu64_holdgap.h). Each pass through the pair below is exactly one
		// C64 cycle, and RESTART_CYCLE_COUNTER already stamps every one of
		// them, so the conversion factor costs two compares per iteration
		// against a ~600-ARM-cycle half-cycle -- and the bus is held for all
		// of it. The window starts at 4000 rather than 0 so the measurement
		// is taken with this loop's own i-cache lines already resident.
		u64 calib0 = 0, calib1 = 0;

		for ( u16 i = 0; i < 20000; i++ )
		{
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			RESTART_CYCLE_COUNTER
			if ( i == 4000 ) calib0 = armCycleCounter;
			if ( i == 19999 ) calib1 = armCycleCounter;
		}

		gpu64HoldGap.armPerC64 = (u32)( ( calib1 - calib0 ) / ( 19999 - 4000 ) );

		SET_GPIO( bDMA_OUT );
		gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_STARTUP );
	}

	// gpu64: "the pass I have just taken sampled the C64 cycle immediately
	// after the one I armed on" is the load-bearing claim of the hold gate,
	// and this is what proves it. RESTART_CYCLE_COUNTER stamps every pass at
	// the same point in the cycle, so two consecutive passes are one
	// armPerC64 apart and a skipped cycle is two; half a C64 cycle of slack
	// covers the jitter of arriving late into the CPU half without coming
	// anywhere near the skipped-cycle case.
	//
	// armPerC64 is 0 only on the VSF-injection entry, reuUsingPolling( 2 ),
	// which skips the calibration block above entirely. There the span is
	// made infinite, so the gate keeps the pre-2026-09-07 behaviour -- fire
	// on the next pass whatever its age. That is no weaker than what shipped,
	// because what shipped tested nothing at all; it is simply the one path
	// this fix cannot strengthen.
	gateSpan = gpu64HoldGap.armPerC64
			? gpu64HoldGap.armPerC64 + ( gpu64HoldGap.armPerC64 >> 1 )
			: 0xffffffff;

reuEmulationMainLoop:

	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER

	while ( 1 )
	{
		// gpu64: bounded form of WAIT_FOR_VIC_HALFCYCLE, so a C64 that has
		// been switched off is detected instead of stranding this loop in a
		// spin forever. The Pi now runs from its own supply and outlives the
		// machine, which makes a stopped clock an ordinary event rather than
		// a fault.
		//
		// This is the wait it is safe to bound, and the distinction matters:
		// it is not the precision anchor. The anchor is the
		// SET_GPIO(bDIR_Dx) / WAIT_FOR_CPU_HALFCYCLE / RESTART_CYCLE_COUNTER
		// sequence further down, which re-syncs from scratch and is what
		// every sample timing is measured from. The added increment and
		// compare sit inside a spin already dominated by an MMIO read, and
		// on the timeout path the pass is abandoned before the bus sample
		// rather than taken late -- so a false positive would cost a lost
		// sample, never a mistimed one. (It cannot happen anyway: see
		// GPU64_NOCLOCK_TIMEOUT.)
		noClock = 0;
		{
			register u32 timeout = 0;
			do {
				g2 = read32( ARM_GPIO_GPLEV0 );
				if ( ++timeout > GPU64_NOCLOCK_TIMEOUT ) { noClock = 1; break; }
			} while ( CPU_HALF_CYCLE );
		}

		if ( noClock )
		{
			if ( !noClockLatched )
			{
				noClockLatched = 1;
				gpu64_deadClockEnter();
			}
			prevVecLo = 0;
			continue;
		}
		if ( noClockLatched )
		{
			noClockLatched = 0;
			gpu64_deadClockLeave();
		}

		GPU64_LADDER_SAMPLE

		void *p = (u8*)( && reuEmulationMainLoop ) + ipl;
		CACHE_PRELOADIKEEP( p );
		ipl += 64; if ( ipl >= GPU64_POLL_IPL_WINDOW ) ipl = 0;

		if ( CPU_RESET )
		{
			// gpu64: saturate rather than wrap. The test below used to be
			// "> 1000", so resetREU() ran on every pass for as long as the
			// line stayed low -- and then again after each 65536-pass wrap
			// of this u16. Harmless for a block of memsets; not harmless for
			// gpu64_apiFullReset(), which can reprogram the VideoCore. One
			// shot per reset episode.
			if ( resetCount <= 1001 )
				resetCount ++;
			if ( resetCount == 1001 )
			{
				// gpu64: the C64 has derailed and taken the REU with it.
				// That is the run whose ladder numbers matter most, and
				// until now it was also the run that lost them -- the test
				// PRG never gets to its closing LOG_ENABLE(1). Dump on the
				// way out. Costs a full log repaint, which is free here:
				// nothing is being timed any more.
				GPU64_LADDER_DUMP
				resetREU();
				// gpu64: and the display half of the same reset -- mode,
				// palette, border, pages and text planes, none of which
				// resetREU() has ever touched. Without it a program that
				// left the display in 80x50 text mode kills the screen
				// mirror for good: showMirror() blits into the graphics
				// draw page with no mode check, so every snapshot after the
				// reset lands somewhere the VideoCore is not scanning out.
				//
				// Heavy work is safe at this exact point and nowhere near
				// it. /RESET is still asserted -- this fires ~1ms into a
				// pulse that lasts far longer -- so the 6510 is held halted
				// by the C64's own reset circuit. There is no bus deadline
				// to miss, nothing to serve, and no rule-7 exposure,
				// because there is no cycle in flight to corrupt.
				gpu64_apiFullReset( TRUE );
			}
		} else
			resetCount = 0;

		// gpu64 (2026-09-09): debounced, because this is the ONLY path by
		// which reuUsingPolling() ever hands the machine back to the RAD
		// menu -- CPU_RESET above deliberately does not -- and it used to
		// act on a single unfiltered sample of a pin that does not always
		// mean what the macro's name says.
		//
		// bBUTTON is GPIO 3. So is A7_IN (gpio_defs.h): the LVC257 carries
		// the control lines and the low address byte on the same four pins,
		// told apart only by MPLEX_SEL. The g2 this test reads is the one
		// sampled at the top of the pass, NOT the settled sample at
		// WAIT_FOR_SIGNALS that IO2_ACCESS uses -- and the button is active
		// low, so any pass on which bit 3 does not read as the button reads
		// as a press. A7 is 0 for every gpu64 register ($DF50-$DF68), which
		// is what a class-1 program polls flat out.
		//
		// Reported from the bench 2026-09-08: the Quake demo "switched to
		// the RAD menu after a while", mid-run, with nobody touching the
		// button. Which of the ways bit 3 can lie was responsible is not
		// established; requiring the press to persist closes all of them at
		// once. A finger holds the button for tens of milliseconds, i.e.
		// tens of thousands of passes, so the threshold below is invisible
		// to a real press and unreachable by a transient.
		//
		// Cost on the common path is one store, the same shape as
		// resetCount above.
		if ( BUTTON_PRESSED )
		{
			if ( ++buttonCount >= GPU64_BUTTON_DEBOUNCE )
			{
				GPU64_LADDER_DUMP
				return 2;
			}
		} else
			buttonCount = 0;

		// gpu64: default-state screen mirror (milestone 3), watchdog half.
		// The snapshot itself is NOT taken here any more -- this point is
		// ahead of the loop's bus sample, so nothing is known about the cycle
		// a DMA assert would land on, and that was rule 7 live at 4Hz. It now
		// fires from the $FFFF vector-fetch gate further down, where the
		// C64's next cycle is provably a read. See gpu64_mirrorSnapshot().
		//
		// All that is left here is the liveness timer: if the jiffy IRQ stops
		// arriving, BASIC is no longer driving the machine and the mirror
		// retires until the next reset.
		// Suspended until the first vector fetch after a re-arm -- see
		// GPU64_MIRROR_IRQ_TIMEOUT: the reset that re-armed the latch is
		// followed by ~2s of RAMTAS with interrupts masked, and timing out
		// inside that window is what froze HDMI after every reset.
		if ( !gpu64ApiActive && gpu64MirrorEnabled && !gpu64MirrorAwaitIrq )
		{
			if ( ++gpu64MirrorIrqWatchdog >= GPU64_MIRROR_IRQ_TIMEOUT )
				gpu64MirrorEnabled = 0;
		}

		// gpu64: the frame clock (gpu64_vsync.h). Only meaningful once a
		// program has engaged the API -- the mirror above has no use for a
		// vblank and shares the loop pass with this. The common case is the
		// inlined test alone: one MMIO read and a compare.
		if ( gpu64ApiActive )
		{
			register u32 gpu64Now = gpu64_vsyncNow();
			if ( gpu64_vsyncDue( gpu64Now ) )
			{
				gpu64_vsyncAdvance( gpu64Now );
				gpu64Regs.status |= GPU64_STATUS_VBLANK_PENDING;
				// gpu64: do not commit here. This point is *ahead* of the
				// loop's bus sample, so nothing is yet known about the cycle
				// the DMA assert would land on -- and a commit that lands
				// beside a C64 write cycle corrupts the C64 (bench runs
				// M/N, 2026-09-06; see gpu64_vsyncCommitFlip()). Record that
				// the flip is owed and let the sampled-IO2 gate below fire
				// it. Two flags rather than one because flipPending is the
				// API's "a flip is in flight" state, visible to the C64 as
				// STATUS.BUSY, while commitDue is purely this loop's.
				if ( gpu64Vsync.flipPending && !gpu64Vsync.commitDue )
				{
					gpu64Vsync.commitDue = 1;
					gpu64Vsync.commitDueFrame = gpu64Vsync.frameCount;
				}
			}
		}

		SET_GPIO( bDIR_Dx );
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER

		// gpu64 2026-09-12: did THIS pass pay for a mux re-read? Run 26
		// proved the drive really is late sometimes (SLK -CF, 218 reads past
		// the target, 157 of them badly past), and the re-read is the one
		// conditional MMIO access ahead of the drive -- so it is the first
		// thing to convict or exonerate, and this splits the late reads by
		// whether it fired. A register zeroed once per pass is the whole
		// cost: one instruction, no memory, no peripheral bus. Deliberately
		// not the kind of addition that broke run 23.
		muxRetried = 0;

		WAIT_UP_TO_CYCLE( reu.WAIT_FOR_SIGNALS + reu.TIMING_OFFSET_CBTD );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );

		WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );

		// gpu64 2026-09-11, run 20: catch the multiplexer in the wrong phase.
		//
		// IO_ADDRESS takes A0-A3 from GPIO10-13, which are wired straight to
		// the address lines, and A4-A7 from GPIO0-3, which are the LVC257's
		// outputs. With MPLEX_SEL low those four pins are NMI, ROMH, IO1 and
		// BUTTON instead. SET_GPIO above is a posted write to the peripheral
		// bus; when it lands late the '257 has not switched by the time this
		// read returns, and the high nibble of every address sampled in that
		// pass is the signal nibble.
		//
		// It is identifiable because during an IO2 access IO1 is inactive-
		// high by definition and BUTTON is high unless a finger is on it, so
		// the signal nibble is always $C-$F -- and gpu64 has no register up
		// there, nor does the REU. Run 20 measured 249 such writes and 333
		// such reads in 89662 writes, every one of them in the half of the
		// run where core 1 was rendering and none in the half where it was
		// not. See gpu64_busstats.h.
		//
		// The repair is one more GPLEV0 read. A peripheral read is ordered
		// behind the posted write and costs the full peripheral latency, so
		// by the time it returns the '257 has certainly switched -- which is
		// why this is a barrier and not the settling delay that killed the
		// bus on 2026-09-10 (see the note above WAIT_CYCLE_MULTIPLEXER). It
		// is gated on IO2_ACCESS, a test of g2 the next few lines make
		// anyway, so a pass that is not ours pays nothing: on any other
		// address the high nibble is genuinely $C-$F a quarter of the time.
		//
		// g3 feeds IO_ADDRESS and nothing else -- IO2_ACCESS, ADDRESS_FFxx
		// and the R/W decode all read g2 -- so re-reading it can only change
		// the sampled address, which is the thing being repaired.
		// Run 21 tightened the test from "is it in the signal range" to "is
		// it a nibble a real access could have produced". The REU decodes
		// $DF00-$DF0A and (then) the API $DF0B-$DF23, so the high nibble of
		// any access either of them cared about was $0, $1 or $2.
		// The old >= $C let a half-switched $9 through as repaired, which is
		// why run 21's muxFixed read exactly equal to muxMiss while 37 bad
		// writes survived, every one of them in $90-$9F. The second re-read
		// says whether those settle or are stably wrong.
		// Run 24 dropped the second re-read. It had fired zero times in
		// every uncorrupted run since it shipped, and it is an MMIO read on
		// a path that run 24 proved has no margin left: SLK came back
		// 00/FF, i.e. the loop was already past WAIT_CYCLE_READ2 by the time
		// it reached the wait, at least 255 times. muxUnfixed keeps the same
		// information -- did one re-read suffice? -- without spending a
		// second bus access to learn it. If it is ever nonzero the access is
		// discarded and BUS A/BUS B will show it as a bad read or write.
		// 2026-09-27: the register block moved to $DF50-$DF68, so the valid
		// nibbles are now $0, $5 and $6 and the test is a mask lookup rather
		// than a threshold. Still two ALU ops, and still only on IO2 passes.
		// A threshold cannot express it: $5/$6 sit between invalid nibbles.
		if ( IO2_ACCESS && ( ( GPU64_MUX_BAD_NIBBLES >> ( g3 & 15 ) ) & 1 ) )
		{
			gpu64BusStats.muxMiss++;
			muxRetried = 1;
			g3 = read32( ARM_GPIO_GPLEV0 );

			if ( !( ( GPU64_MUX_BAD_NIBBLES >> ( g3 & 15 ) ) & 1 ) )
				gpu64BusStats.muxFixed++;
			else
				gpu64BusStats.muxUnfixed++;
		}

		CLR_GPIO( bMPLEX_SEL );

		register u8 D = 0;
		register u8 writeFF00 = 0;

		if ( !IO2_ACCESS && !ADDRESS_FFxx )
			goto noREUAccess;

		if ( CPU_WRITES_TO_BUS )
		{
			if ( ADDRESS_FFxx && ADDRESS0to7 == 0 && reu.reuWaitForFF00 )
			{
				writeFF00 = 1;
			} else
			if ( IO2_ACCESS )
			{
				SET_BANK2_INPUT
				///SET_GPIO( bDIR_Dx );
				CLR_GPIO( bOE_Dx );				// Dx = enable
#ifdef GPU64_SNIFF_ENABLED
				// gpu64: the write-data sweep (gpu64_sniff.h). Only
				// SNIFF_DATA pays for it; every other write takes the
				// single sample below, exactly as before.
				if ( IO_ADDRESS == GPU64_SNIFF_DATA )
				{
					u32 sniffRaw[ GPU64_SNIFF_SLOTS ];
					for ( u32 si = 0; si < GPU64_SNIFF_SLOTS; si++ )
					{
						WAIT_UP_TO_CYCLE( gpu64SniffT[ si ] );
						sniffRaw[ si ] = read32( ARM_GPIO_GPLEV0 );
					}
					SET_GPIO( bOE_Dx );			// Dx = disable
					SET_BANK2_OUTPUT
					D = ( sniffRaw[ 4 ] >> D0 ) & 255;
					gpu64_sniffScore( sniffRaw, D0, bIO2, bPHI );
				} else
#endif
				{
				WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_WRITEDATA );
				D = ( read32( ARM_GPIO_GPLEV0 ) >> D0 ) & 255;
				SET_GPIO( bOE_Dx );				// Dx = disable
				SET_BANK2_OUTPUT
				}
			}

			// gpu64: full 8-bit decode, not REU's own "& 0x1f" -- $DF50-$DFFF
			// is gpu64's register window (docs/api_design.md), and the 5-bit
			// mask would alias it back onto REU's 11 registers.
			register u8 addr = IO_ADDRESS;

			if ( ( IO2_ACCESS && addr == 0x01 && BITS_ALL_SET( D, REU_COMMAND_EXECUTE | REU_COMMAND_FF00_DISABLED ) )
					|| writeFF00 )
			{
				if ( !writeFF00 ) reu.command = D;
				#include "handle_transfer.h"
				reu.reuWaitForFF00 = 0;
				GPU64_LADDER_SKIP
			} else
			if ( IO2_ACCESS  )
			{
				register u8 addr = IO_ADDRESS;

				// gpu64: $DF50-$DFFF is gpu64's own register window -- the
				// command API's registers (docs/api_design.md). REU keeps
				// $DF00-$DF0A below, and $DF0B-$DF4F belongs to neither. Note this is the full 8-bit address:
				// REU's own "& 0x1f" decode would alias the whole gpu64
				// window down onto REU's 11 registers.
				//
				// Everything the API does happens synchronously here, in the
				// same immediate-mode model milestone 2's test pattern and
				// milestone 3's mirror already use on real hardware. The C64
				// free-runs through this loop (see the "Operating modes"
				// section of dev/project_description.md); a command that
				// moves a payload steals the bus for one bounded burst,
				// proportional to a length the C64 itself chose.
				if ( addr >= GPU64_REG_CMD_HI )
				{
					if ( addr == GPU64_REG_CMD_LO )
					{
						// gpu64: is this write provably the last cycle of its
						// instruction? (rule 7, gpu64_holdgate.h)
						//
						// Looking backwards is enough here, and it is the only
						// direction available -- the hold has to open on the
						// very next cycle or the C64 runs on. A write to CMD_LO
						// that is *not* an instruction's last cycle is a
						// read-modify-write's first write, and an RMW always
						// reads the same address on the cycle immediately
						// before it (inc $DF51: c4 reads, c5 and c6 write;
						// inc $DF51,X: c4 dummy-reads, c5 reads, c6 and c7
						// write). So: previous *C64 cycle*, an IO2 read, same
						// low address byte -- and the write is ambiguous.
						//
						// Everything else dispatches exactly as it has since
						// milestone 4, same instruction sequence, same timing.
						// That is deliberate: this is the one hold in the tree
						// that a thousand hardware runs have already hardened,
						// and it is worth more than the tidiness of routing it
						// through the gate below.
						//
						// The ambiguous case arms the gate instead. An RMW's
						// *second* write then dispatches immediately here --
						// its previous cycle is a write, not a read -- which
						// discards the deferred arm and runs the command once,
						// with the incremented value, on the instruction's
						// genuine last cycle. `inc $DF51` becomes correct
						// rather than merely safe.
						if ( prevAnchor == ( GPU64_ANCHOR_READ | addr ) &&
								(u32)( armCycleCounter - prevStamp ) <= gateSpan )
						{
							gateArmed = GPU64_GATE_DISPATCH;
							gateD = D;
							gateAge = 0;
							gpu64HoldGate.armed++;
							gpu64HoldGate.dispatchDeferred++;
							// Counted at the point the write was *sampled*,
							// which is what the denominator means, not at
							// the point the deferred dispatch later fires.
							gpu64_busStatsWrite( addr );
							GPU64_LADDER_SKIP
						} else
						{
						// A pending arm cannot survive an immediate dispatch:
						// it would fire again later, with a stale payload.
						gateArmed = GPU64_GATE_NONE;

						// gpu64: hold the bus for the whole command.
						//
						// Found on the first hardware test: staging ARG
						// bytes and then writing CMD_LO only works if the
						// C64 cannot issue a write while gpu64 is executing
						// the previous one. It can -- the C64 free-runs
						// through this loop -- and a CLEAR is ~64000 byte
						// writes plus a cache clean, during which the CPU
						// gets through dozens of instructions whose IO2
						// writes this loop never sees. The symptom was a
						// command that vanished entirely and a following one
						// drawn from half-stale arguments.
						//
						// So dispatch runs with the CPU DMA-halted, exactly
						// like an REU transfer: bounded by the command's own
						// work, and the C64 resumes at its next instruction
						// with nothing missed. Blob transfers inside the
						// command no longer release the bus themselves --
						// the release below is the only one.
						// gpu64: WAIT_FOR_CPU_HALFCYCLE first, so this catches
						// the *transition* -- see gpu64_blobRead(). We normally
						// arrive here inside the CPU half-cycle (the loop just
						// sampled the write), where this costs nothing, but the
						// register decode above has no cycle budget and a slip
						// past the falling edge would otherwise leave the bare
						// WAIT_FOR_VIC_HALFCYCLE falling through.
						WAIT_FOR_CPU_HALFCYCLE
						WAIT_FOR_VIC_HALFCYCLE
						RESTART_CYCLE_COUNTER
						WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
						CLR_GPIO( bDMA_OUT );
						gpu64_holdGapAssert( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );

						gpu64_apiDispatch( D );

						// gpu64: give the bus back on a cycle boundary, not
						// wherever the command happened to finish. Every
						// other release in RAD (DMA_READBYTE_P3,
						// DMA_WRITEBYTE_P2 via
						// DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER) syncs to
						// the start of a VIC half-cycle first; releasing
						// mid-cycle restarts the 6502 at an undefined phase,
						// which is how a program survives the first command
						// or two and then quietly derails.
						WAIT_FOR_CPU_HALFCYCLE
						WAIT_FOR_VIC_HALFCYCLE
						RESTART_CYCLE_COUNTER
						SET_GPIO( bDMA_OUT );
						gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );
						gpu64_busStatsWrite( addr );
						GPU64_LADDER_SKIP
						}
					} else
					{
						// Inlined (gpu64_api.h): a cross-TU call here has to
						// finish before the loop's next sample, and an
						// i-cache miss on it loses the C64's next IO2 access
						// outright -- which showed up as commands running on
						// half-written argument blocks.
						gpu64_apiWriteReg( addr, D );
						gpu64_busStatsWrite( addr );
					}
				} else
				// gpu64 2026-09-27: $DF0B-$DF4F belongs to nobody we
				// emulate -- on an Ultimate it is the Command Interface at
				// $DF1B-$DF1F. Without this test those writes would fall
				// into REU's switch, whose tail resets reu.pl/reu.pl2 and
				// calls reuPrefetch() on every write, which is the REU
				// write-pressure latch CLAUDE.md forbids feeding. One compare,
				// and only on a path that is not gpu64's.
				if ( addr <= GPU64_REG_REU_LAST )
				{
					switch ( addr )
					{
					case 0x00:
						break;
					case 0x01:
						reu.command = D;

						if ( ( D & REU_COMMAND_EXECUTE ) && !( D & REU_COMMAND_FF00_DISABLED ) )
							reu.reuWaitForFF00 = 1;
						break;
					case 0x02:
						reu.addrC64 = reu.shadow_addrC64 = ( reu.shadow_addrC64 & 0xff00 ) | D;
						break;
					case 0x03:
						reu.addrC64 = reu.shadow_addrC64 = ( reu.shadow_addrC64 & 0x00ff ) | ( D << 8 );
						break;
					case 0x04:
						reu.addrREU = reu.shadow_addrREU = ( reu.shadow_addrREU & 0xff00 ) | D;
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x05:
						reu.addrREU = reu.shadow_addrREU = ( reu.shadow_addrREU & 0x00ff ) | ( D << 8 );
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x06:
						reu.bank = reu.shadow_bank = D & ~reu.regBankUnused;
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x07:
						reu.length = reu.shadow_length = ( reu.shadow_length & 0xff00 ) | D;
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x08:
						reu.length = reu.shadow_length = ( reu.shadow_length & 0x00ff ) | ( D << 8 );
						reu.pl = reu.CACHING_L2_OFFSET_KB; reu.pl2 = 0;
						break;
					case 0x09:
						reu.IRQmask = D | REU_INTERRUPT_UNUSED_BITMASK;
						if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_END_BLOCK | REU_INTERRUPTS_ENABLED ) &&
								BITS_ALL_SET( reu.status, REU_STATUS_END_OF_BLOCK ) )
						{
							reu.status |= REU_STATUS_INTERRUPT_PENDING;
							reu.irqTriggered = 1;
						}
						if ( BITS_ALL_SET( reu.IRQmask, REU_INTERRUPT_VERIFY | REU_INTERRUPTS_ENABLED ) &&
								BITS_ALL_SET( reu.status, REU_STATUS_VERIFY_ERROR ) )
						{
							reu.status |= REU_STATUS_INTERRUPT_PENDING;
							reu.irqTriggered = 1;
						}
						break;
					case 0x0A:
						reu.addrREUCtrl = D | REU_ADDR_UNUSED_BITS;
						break;
					}
					reuPrefetch( reu.addrREU | ( (u32)reu.bank << 16 ) );
				}
			}
		} else
			// CPU READS FROM BUS
			if ( IO2_ACCESS )
			{
				// gpu64: full 8-bit decode again. This also fixes a real
				// landmine on the read path: with the old 5-bit mask, a read
				// from anywhere in gpu64's window indexed
				// ((u8*)&reu.status)[addr] with addr up to 0x1f, walking off
				// the end of REUSTATE and returning whatever happened to be
				// there. gpu64's registers get their own bounds-checked read
				// (gpu64_apiReadReg), and REU's indexing now only ever sees
				// 0x00-0x0A.
				register u8 addr = IO_ADDRESS;

				register u8 disableIRQ = 0;
				u64 ccArrived = 0;
				// this is how this looks like in readable form:
				/*				switch ( addr )
							{
							case 0x00:	D = reu.status;
										reu.status &= ~( REU_STATUS_VERIFY_ERROR | REU_STATUS_END_OF_BLOCK | REU_STATUS_INTERRUPT_PENDING );
										disableIRQ = 1;
										break;
							case 0x01:	D = reu.command;							break;
							case 0x02:	D = reu.addrC64 & 255;					break;
							case 0x03:	D = reu.addrC64 >> 8;					break;
							case 0x04:	D = reu.addrREU & 255;						break;
							case 0x05:	D = ( reu.addrREU >> 8 ) & 255;				break;
							case 0x06:	D = reu.bank | reu.regBankUnused;		break;
							case 0x07:	D = reu.length & 255;					break;
							case 0x08:	D = reu.length >> 8;						break;
							case 0x09:	D = reu.IRQmask;							break;
							case 0x0A:	D = reu.addrREUCtrl;						break;
							default:	D = 0xFF;									break;
							}*/
				if ( addr >= GPU64_REG_CMD_HI )
				{
					// Inlined (gpu64_api.h) -- this runs against a hard
					// deadline: D has to be on the bus by WAIT_CYCLE_READ
					// (the drive-by target; WAIT_CYCLE_READ2 below only
					// gates the release), so a cold i-cache means the C64
					// latches whatever was on the bus instead of the
					// register. That was the intermittently wrong ERRCODE.
					D = gpu64_apiReadReg( addr );

					// gpu64 2026-09-12: stamp the SETUP edge, not the
					// release edge. Run 25 measured against
					// WAIT_CYCLE_READ2 from below the drive, which is the
					// point the transceiver is turned OFF -- so it scored
					// how long the Pi over-HELD the bus, a number that says
					// nothing about whether the C64 latched the right byte.
					// What decides a wrong ERRCODE is whether D reached the
					// pins before the C64's latch, and D reaches them on the
					// GPCLR0/GPSET0 pair immediately below. So the stamp
					// belongs here, and the deadline it is compared against
					// is WAIT_CYCLE_READ -- RAD's drive-by target, which
					// nothing in this path actually waits for.
					//
					// Costs nothing new: it is the same single MRS the run
					// 25 build already spent, moved up inside a branch that
					// had already been taken. The REU path (addr <= $0A)
					// never reaches it.
					READ_CYCLE_COUNTER( ccArrived );
				} else
				if ( addr <= GPU64_REG_REU_LAST )
				{
					D = ( (u8 *)&reu.status )[ addr ];
					if ( addr == 0 )
					{
						reu.status &= ~( REU_STATUS_VERIFY_ERROR | REU_STATUS_END_OF_BLOCK | REU_STATUS_INTERRUPT_PENDING );
						disableIRQ = 1;
					} else
						if ( addr == 6 )
						{
							D |= reu.regBankUnused;
						}
				} else
				{
					// gpu64 2026-09-27: $DF0B-$DF4F is not ours. Leave the
					// data bus alone -- on an Ultimate the Command Interface
					// is driving it -- and never index REUSTATE past $0A.
					goto io2ReadNotOurs;
				}

				register u32 DD = D << D0;
				write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
				write32( ARM_GPIO_GPSET0, DD );
				SET_BANK2_OUTPUT

					if ( disableIRQ && reu.irqRelease )
					{
						reu.irqRelease = 0;
						write32( ARM_GPIO_GPSET0, bIRQ_OUT );
						INP_GPIO_IRQ();
					}

				WAIT_UP_TO_CYCLE( reu.WAIT_CYCLE_READ2 );
				SET_GPIO( bOE_Dx | bDIR_Dx );

				// gpu64: bus-sampling instrument (gpu64_busstats.h).
				// Deliberately here and not up beside the decode: the C64
				// has latched D and the transceiver has turned around, so
				// this is outside the read's hard deadline and only inside
				// the loop's next-sample window -- the same handful of ARM
				// cycles gpu64_holdGapRelease() already spends there.
				if ( addr >= GPU64_REG_CMD_HI )
				{
					gpu64_busStatsRead( addr );
					gpu64_readSlackNote( reu.WAIT_CYCLE_READ + armCycleCounter, ccArrived, muxRetried );
				}
			}
	io2ReadNotOurs:

		// gpu64: the deferred flip commit fires here, and only here.
		//
		// Reaching this point with IO2_ACCESS set means the loop has just
		// serviced a C64 load or store to $DFxx -- and an absolute-addressed
		// load or store spends its *last* cycle on that access. So the C64's
		// next cycle is an opcode fetch (or, if an interrupt is pending, the
		// interrupt sequence's two dummy reads, which come before its three
		// pushes): a read, either way. Asserting DMA_OUT now therefore halts
		// the CPU via RDY on a read cycle, with no write in flight.
		//
		// That guarantee is the whole point. The old code committed from the
		// frame-clock branch at the top of the pass, where the cycle about to
		// happen is unknown, and the C64's own multiplexed bus makes it
		// unknowable: the CPU only drives R/W during the PHI2-high half, by
		// which time it is too late to halt that cycle. Runs M and N put a
		// number on the cost -- a test that only *reads* a register inside
		// the armed window survived 300 flips, while one that *writes*
		// registers (without dispatching) died after 114 and 72. Same
		// dispatch, same hold, same everything else; writes were the only
		// difference.
		//
		// Consequence for API users: a deferred PAGE_FLIP now lands on the
		// program's next $DFxx access rather than exactly at the boundary.
		// Every handshake-mode program polls STATUS while BUSY is set, so in
		// practice that is the very next instruction. commitLateMax records
		// the worst case seen, for GET_HEALTH/logging to report.
		//
		// That was the reasoning as shipped, and it had a hole: the "last
		// cycle" claim is true of absolute loads and stores, which is all the
		// API itself uses, and false of a read-modify-write (inc $DF52 reads
		// on c4 and writes on c5 and c6) and of an indexed store, whose
		// unconditional dummy read can land in IO2 one cycle before the
		// write. Recorded as an API constraint at the time; closed
		// 2026-09-07 by not firing here at all.
		//
		// What happens instead is that this only *arms* the gate, and the
		// confirm at noREUAccess: below opens the hold one C64 cycle later,
		// after that cycle has proved itself a read at a different address.
		// The reasoning is in gpu64_holdgate.h. Everything above still holds
		// -- it is why an IO2 access is the right thing to arm on -- it is
		// just no longer the whole proof.
		//
		// gateArmed is tested so a deferred dispatch, armed a few lines
		// above, is not overwritten by a commit; the commit simply arms on
		// the next IO2 access instead.
		if ( gpu64Vsync.commitDue && IO2_ACCESS && !gateArmed )
		{
			gateArmed = GPU64_GATE_COMMIT;
			gateAge = 0;
			gpu64HoldGate.armed++;
		}

		// gpu64: and the screen mirror fires here, for the same reason and on
		// the same kind of guarantee.
		//
		// The C64 has no reason to touch IO2 at a BASIC prompt, so the mirror
		// cannot use the gate above. What it can use is the interrupt
		// sequence itself. Reaching this point with ADDRESS_FFxx set on a
		// read of $FF means the 6510 is fetching the high byte of the
		// IRQ/BRK vector -- the last cycle of the interrupt sequence, whose
		// two dummy reads and three pushes are already behind it. The next
		// cycle is the handler's opcode fetch. A read, by construction, so
		// DMA_OUT halts the CPU there with no write in flight.
		//
		// It is also the jiffy clock: this fires ~60 times a second while the
		// KERNAL IRQ is alive, and stops the moment it isn't -- which is the
		// signal the watchdog at the top of the loop is timing.
		//
		// The NMI vector is $FFFA/$FFFB, so RESTORE cannot reach this. The
		// write case is excluded because a write to $FFxx says nothing about
		// the next cycle (and $FF00 is REU's own transfer trigger, handled
		// far above). This gate carried the same hole as the commit gate --
		// an RMW whose read cycle landed on $FFFF would be followed by a
		// write -- and it is closed the same way: arm here, confirm below.
		// The watchdog and the jiffy counter are not part of the hold and
		// still update on the access itself.
		//
		// And only a $FFFF read that directly follows a $FFFE read counts
		// (prevVecLo) -- the interrupt sequence's two vector fetches. The C64
		// Ultimate's cartridge port shows a read of $FFFF on cycles that are
		// no vector fetch at all, in bulk: the sniffer counted 1.27e9 of them
		// in one session. Each one was a jiffy here, and in the idle loop a
		// snapshot hold every 15 of them. A lone $FFFF says nothing; the
		// $FFFE/$FFFF pair is the vector fetch itself.
		if ( ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFF &&
				prevVecLo && !gpu64ApiActive && gpu64MirrorEnabled )
		{
			gpu64MirrorIrqWatchdog = 0;
			gpu64MirrorAwaitIrq = 0;
			gpu64MirrorLone = 0;

			if ( ++gpu64MirrorJiffy >= GPU64_MIRROR_JIFFY_INTERVAL && !gateArmed )
			{
				gpu64MirrorJiffy = 0;
				gateArmed = GPU64_GATE_MIRROR;
				gateAge = 0;
				gpu64HoldGate.armed++;
			}
		} else
		// gpu64: the C64U's substitute clock -- a long run of lone $FFFF reads
		// with no vector pair among them (GPU64_MIRROR_LONE_INTERVAL). It
		// neither feeds the watchdog nor clears awaitIrq: those belong to the
		// real IRQ. The arm is its own kind so the confirm below can accept
		// the one pair of cycles an idle C64U port ever shows, $FFFF twice.
		if ( ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFF &&
				!gpu64ApiActive && gpu64MirrorEnabled )
		{
			if ( ++gpu64MirrorLone >= GPU64_MIRROR_LONE_INTERVAL && !gateArmed )
			{
				gpu64MirrorLone = 0;
				gateArmed = GPU64_GATE_MIRROR_IDLE;
				gateAge = 0;
				gpu64HoldGate.armed++;
			}
		}

#ifdef GPU64_SNIFF_ENABLED
		// gpu64: the sniff report rides the same vector-fetch arm as the
		// mirror, once a second, and only while the HDMI log is showing --
		// on a machine where commands work, the first dispatch hides the
		// log and this never fires again. The mirror wins a tie; the
		// report simply tries on the next fetch.
		if ( ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFF && !prevVecLo )
			gpu64Sniff.vectorFakes++;
		else
		if ( ADDRESS_FFxx && !CPU_WRITES_TO_BUS && ADDRESS0to7 == 0xFF )
		{
			gpu64Sniff.vectorFetches++;
			if ( ++gpu64Sniff.jiffy >= 60 && !gateArmed &&
					g_pGpu64FB && g_pGpu64FB->LogEnabled() )
			{
				gpu64Sniff.jiffy = 0;
				gateArmed = GPU64_GATE_SNIFF;
				gateAge = 0;
				gpu64HoldGate.armed++;
			}
		}
#endif

	noREUAccess:
#ifdef GPU64_SNIFF_ENABLED
		// gpu64: every sampled IO2 access, any address (gpu64_sniff.h).
		if ( IO2_ACCESS )
			gpu64_sniffNote( IO_ADDRESS, CPU_WRITES_TO_BUS, D );
#endif
		// gpu64: the hold gate's confirm. Every hold this loop opens that is
		// not the CMD_LO dispatch opens here, and nowhere else.
		//
		// The proof is about two cycles, and neither of them is the one that
		// armed the gate. Let N be the cycle the *previous* pass sampled and
		// N+1 the one this pass just sampled. The hold may open during N+1
		// iff all four of these hold:
		//
		//   1. N was an IO2 access, or the $FFFF vector fetch -- prevAnchor.
		//   2. the previous pass really was the previous C64 cycle: the two
		//      RESTART_CYCLE_COUNTER stamps are under 1.5 armPerC64 apart. A
		//      skipped cycle is 2.0 apart and fails, and without this test
		//      term 4 would be comparing against an unrelated cycle.
		//   3. N+1 is a read. A write here says N was *not* an instruction's
		//      last cycle -- it was a read-modify-write's read, or its first
		//      write -- and asserting DMA under a write in flight tri-states
		//      the address bus beneath it. That is the Stage 16 derail.
		//   4. N+1 is a read at a *different* low address byte than N. This
		//      is what separates inc $DFxx,X's dummy read (c4) from its real
		//      read (c5): indexed addressing only ever fixes up the high
		//      byte, so those two share a low byte and c5 must not be allowed
		//      to confirm c4. The same test covers the page-crossing forms of
		//      lda $DFxx,X and lda ($xx),Y, and subsumes "not the same
		//      register twice".
		//
		// Together those make N+1 an opcode fetch, hence N+2 a read, because
		// no 6502 instruction and no interrupt sequence writes before its
		// third cycle. Full argument in gpu64_holdgate.h.
		//
		// Note what is *not* in the list: how long ago the arm happened. The
		// four terms are self-contained, so an arm that cannot fire simply
		// waits -- through the rest of an inc $DF52, or through a whole REU
		// transfer -- and fires on the first pair of cycles that qualifies.
		// gateAge counts how long that took, in C64 cycles.
		//
		// GPU64_GATE_MIRROR_IDLE also accepts two adjacent lone $FFFF reads,
		// which is what an idle C64U port shows on nearly every cycle and
		// the four terms can never confirm, because the low byte does not
		// change. On a real 6510 two adjacent $FFFF reads are a JAM, and that
		// arm is only ever taken after a quarter of a million lone $FFFF
		// reads with no IRQ vector pair among them, which a live 6510 never
		// produces.
		if ( gateArmed )
		{
			if ( prevAnchor &&
					(u32)( armCycleCounter - prevStamp ) <= gateSpan &&
					CPU_READS_FROM_BUS &&
					( ADDRESS0to7 != ( prevAnchor & 0xff ) ||
					  ( gateArmed == GPU64_GATE_MIRROR_IDLE && ADDRESS_FFxx &&
					    prevAnchor == ( GPU64_ANCHOR_READ | 0xFF ) ) ) )
			{
				register u32 kind = gateArmed;
				gateArmed = GPU64_GATE_NONE;
				gpu64HoldGate.fired++;
				if ( gateAge > gpu64HoldGate.ageMax )
					gpu64HoldGate.ageMax = gateAge;

				if ( kind == GPU64_GATE_COMMIT )
				{
					register u32 late = gpu64Vsync.frameCount - gpu64Vsync.commitDueFrame;
					if ( late > gpu64Vsync.commitLateMax )
						gpu64Vsync.commitLateMax = late;

					gpu64Vsync.commitDue = 0;
					gpu64_vsyncCommitFlip();
				} else
				if ( kind == GPU64_GATE_MIRROR || kind == GPU64_GATE_MIRROR_IDLE )
				{
					// An arm can wait; the API may have woken up since.
					if ( !gpu64ApiActive && gpu64MirrorEnabled )
						gpu64_mirrorSnapshot();
				} else
#ifdef GPU64_SNIFF_ENABLED
				if ( kind == GPU64_GATE_SNIFF )
				{
					// Open and close inline, like the deferred dispatch
					// below: the report itself is cold and slow, and only
					// runs once the C64 is already stopped.
					WAIT_FOR_CPU_HALFCYCLE
					WAIT_FOR_VIC_HALFCYCLE
					RESTART_CYCLE_COUNTER
					WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
					CLR_GPIO( bDMA_OUT );

					gpu64_sniffReport();

					WAIT_FOR_CPU_HALFCYCLE
					WAIT_FOR_VIC_HALFCYCLE
					RESTART_CYCLE_COUNTER
					SET_GPIO( bDMA_OUT );
				} else
#endif
				{
					// The deferred dispatch. Same sequence as the inline one
					// above, deliberately duplicated rather than shared: a
					// call would have to be made with the bus still free and
					// could take a cold i-cache miss there (rule 5), and this
					// branch is inside the window the loop preloads.
					gpu64HoldGate.dispatchFired++;

					WAIT_FOR_CPU_HALFCYCLE
					WAIT_FOR_VIC_HALFCYCLE
					RESTART_CYCLE_COUNTER
					WAIT_UP_TO_CYCLE( reu.TIMING_TRIGGER_DMA );
					CLR_GPIO( bDMA_OUT );
					gpu64_holdGapAssert( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );

					gpu64_apiDispatch( gateD );

					WAIT_FOR_CPU_HALFCYCLE
					WAIT_FOR_VIC_HALFCYCLE
					RESTART_CYCLE_COUNTER
					SET_GPIO( bDMA_OUT );
					gpu64_holdGapRelease( armCycleCounter, GPU64_HOLD_KIND_DISPATCH );
				}

				GPU64_LADDER_SKIP
			} else
			{
				gateAge++;
				gpu64HoldGate.declined++;
			}
		}

		// gpu64: the gate's whole per-pass cost, and it has to run on every
		// path -- the non-IO2 fast path above jumps straight to this label,
		// which is exactly why the gate lives here and not beside the arms.
		//
		// prevAnchor records what this pass sampled, for the next pass to
		// reason about: nothing, an IO2 write, an IO2 read, or the $FFFF
		// vector fetch, packed with the low address byte so both of the
		// address comparisons above are a single compare. prevStamp is this
		// pass's RESTART_CYCLE_COUNTER anchor -- and when the pass held the
		// bus, it is the *release* anchor, which is the right one: the C64
		// was stopped in between, so the next pass really does sample the
		// next C64 cycle.
		prevAnchor = IO2_ACCESS
				? ( CPU_READS_FROM_BUS ? GPU64_ANCHOR_READ : GPU64_ANCHOR_WRITE ) | ADDRESS0to7
				: ( ADDRESS_FFxx && CPU_READS_FROM_BUS && ADDRESS0to7 == 0xFF )
					? ( GPU64_ANCHOR_READ | 0xFF )
					: 0;
		prevStamp = armCycleCounter;
		prevVecLo = ADDRESS_FFxx && CPU_READS_FROM_BUS && ADDRESS0to7 == 0xFE;

		if ( reu.irqTriggered && !CPU_IRQ_LOW )
		{
			reu.irqTriggered = 0;
			reu.irqRelease = 1;
			write32( ARM_GPIO_GPCLR0, bIRQ_OUT );
			OUT_GPIO_IRQ();
		}

		// gpu64: the vblank IRQ, shaped exactly like REU's above -- same
		// physical line, same "only assert when nobody else is pulling it
		// low" guard. The difference is the release: REU lets go when the
		// C64 reads the register that explains the interrupt, whereas
		// gpu64's is explicit, on VBLANK_ACK (see doSystem() $03), because
		// nothing gpu64 exposes is read-to-clear.
		//
		// Gated on gpu64ApiActive, like the frame clock at the top of the
		// loop: nothing here can have anything to do until a program has
		// engaged the API, and the loop's tail is the part that has to be
		// finished before the next WAIT_FOR_VIC_HALFCYCLE. One byte test
		// (and gpu64ApiActive is already hot -- the mirror above reads it
		// every pass) beats two struct loads on every pass of a loop that
		// runs a million times a second while nothing is happening.
		if ( gpu64ApiActive && gpu64Vsync.irqRequest && !CPU_IRQ_LOW )
		{
			gpu64Vsync.irqRequest = 0;
			gpu64Vsync.irqAsserted = 1;
			write32( ARM_GPIO_GPCLR0, bIRQ_OUT );
			OUT_GPIO_IRQ();
		}

		if ( gpu64ApiActive && gpu64Vsync.irqReleaseReq )
		{
			gpu64Vsync.irqReleaseReq = 0;
			gpu64Vsync.irqAsserted = 0;
			// Only drive the pin back up if REU is not also holding it
			// down; if it is, REU's own release path owns the line and
			// letting go here would cancel an interrupt the C64 has not
			// acknowledged yet.
			if ( !reu.irqRelease )
			{
				write32( ARM_GPIO_GPSET0, bIRQ_OUT );
				INP_GPIO_IRQ();
			}
		}

		// cache preloading is the most crucial part of emulating a REU on a RPi
		// changing anything below might make everything less stable
		for ( int i = 0; i < reu.CACHING_L2_PRELOADS_PER_CYCLE; i++ )
		{
			reuPrefetch( ( reu.addrREU | ( (u32)reu.bank << 16 ) ) + reu.pl );
			reu.pl += 64;
			if ( reu.pl >= reu.length + 64 ) reu.pl = 0;
		}

		CACHE_PRELOADL1STRM( &reuMemory[ ( ( ( reu.pl2 + reu.addrREU ) | ( (u32)reu.bank << 16 ) ) & ~63 ) & ( reu.reuSize - 1 ) ] );
		reu.pl2 += 64; if ( reu.pl2 >= min( reu.CACHING_L1_WINDOW_KB - 64, reu.length ) ) reu.pl2 = 0;

		forceRead = reuLoad32( 0 );
	}
}
#endif
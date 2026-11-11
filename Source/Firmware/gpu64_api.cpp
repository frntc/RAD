// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: IO2 command API register file, dispatcher and class-0 opcodes.
 See gpu64_api.h and docs/api_design.md.
*/
#include "gpu64_api.h"
#include "gpu64_fb.h"
#include "gpu64_vsync.h"
#include "gpu64_flip.h"
#include "gpu64_holdgap.h"
#include "gpu64_holdgate.h"
#include "gpu64_busstats.h"
#include "gpu64_apidiag.h"
#include "gpu64_ladder.h"
#include "gpu64_3d.h"
#include "gpu64_raster.h"
#include "gpu64_text.h"
#include <circle/util.h>
#include <circle/bcmpropertytags.h>

// gpu64: set once a program actually drives the API, which stops the
// milestone 3 screen mirror (the two modes are mutually exclusive). Declared
// in rad_reu.cpp, which also clears it on every fresh REU session.
extern u8 gpu64ApiActive;

// --- register file ------------------------------------------------------
// Global, and reached from the polling loop without a call -- see the
// comment on GPU64REGS in gpu64_api.h for why. The short aliases keep the
// rest of this file reading the way it did.
GPU64REGS gpu64Regs = { 0, 0, GPU64_ERR_OK, { 0, 0 }, 0, { 0 } };

u16 gpu64DmaWinBase = 0;
u16 gpu64DmaWinLen  = 0;			// 0 = the whole 64K, the compatible default

#define sCmdHi	gpu64Regs.cmdHi
#define sStatus	gpu64Regs.status
#define sErr	gpu64Regs.err
#define sId		gpu64Regs.id
#define sResult	gpu64Regs.result
#define sArg	gpu64Regs.arg
#define sSeq	gpu64Regs.seq
#define sSeqAck	gpu64Regs.seqAck

// gpu64: payload staging. Static rather than stack-local -- these are far too
// big for reuUsingPolling()'s stack, and the dispatcher is called from inside
// its loop.
static u8 sBlobA[ 65536 ];
static u8 sBlobB[ 65536 ];
static u8 sBlobC[ 65536 ];

// Matrix inversion works in double precision regardless of the operand
// format (see MAT_INVERSE below), on an n x 2n augmented matrix.
#define GPU64_MAX_INVERSE_N	64
static double sInvWork[ GPU64_MAX_INVERSE_N * GPU64_MAX_INVERSE_N * 2 ];

GPU64APIDIAG gpu64ApiDiag;

// gpu64 (2026-09-10): the one-shot key's holding place -- gpu64_api.h has the
// contract. Written once per dispatch, read by whichever class the dispatch
// belongs to.
u8 gpu64ApiKey = 0;

// gpu64 (2026-09-27): strict mode -- gpu64_api.h, GPU64_KEY_CHECKED. Set by
// the first checked SCENE_COMMIT, cleared by gpu64_apiFullReset() (a C64
// reset or FULL_RESET) and by a keyed SCENE_RESET.
static boolean sStrict = FALSE;

// gpu64: the seven-flag snapshot behind GET_HEALTH byte 86 -- see
// gpu64_apidiag.h. Cheap enough to call on every refusal: four loads and a
// virtual call that only happens on a path that has already decided to fail.
u8 gpu64_apiDiagStateByte( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	u8 st = 0;
#ifdef GPU64_3D_ENABLED
	if ( gpu64_3dLoopRunning() )				st |= GPU64_DIAG_LOOP_RUNNING;
#endif
	if ( gpu64Vsync.calibrated )				st |= GPU64_DIAG_CALIBRATED;
	if ( pFB && pFB->GetMode() == GPU64_MODE_GRAPHICS )	st |= GPU64_DIAG_MODE_GRAPHICS;
	if ( gpu64Vsync.flipPending )				st |= GPU64_DIAG_FLIP_PENDING;
	if ( gpu64Regs.status & GPU64_STATUS_FRAME_READY )	st |= GPU64_DIAG_FRAME_READY;
	if ( gpu64Regs.status & GPU64_STATUS_BUSY )		st |= GPU64_DIAG_BUSY;
	if ( pFB && pFB->IsInitialized() )			st |= GPU64_DIAG_HAVE_FB;
	return st;
}

void gpu64_apiReset( void )
{
	sCmdHi  = 0;
	sStatus = 0;
	sErr    = GPU64_ERR_OK;
	sId[ 0 ] = sId[ 1 ] = 0;
	sResult = 0;
	for ( unsigned i = 0; i < GPU64_ARG_COUNT; i++ )
		sArg[ i ] = 0;
	sSeq = sSeqAck = 0;
	// Back to unrestricted. A C64 reset means a new program owns the
	// machine and the window the last one declared describes a memory map
	// that no longer exists -- and leaving it set would make every readback
	// the new program tries answer OUT_OF_RANGE for reasons nothing on its
	// side could explain.
	gpu64DmaWinBase = 0;
	gpu64DmaWinLen  = 0;
	sStrict = FALSE;
	// The frame clock's calibration survives -- it describes the display,
	// not the session -- but nothing else about the vblank state does.
	gpu64_vsyncResetState();

	// Class 1's session state goes the same way: ring emptied, every
	// uploaded resource freed. See dev/milestone6_3d_design.md's Resource
	// lifecycle -- without this a RUN/STOP+RESTORE leaks the whole arena and
	// the next program starts against stale IDs.
#ifdef GPU64_3D_ENABLED
	gpu64_3dReset();
#endif

	// Class 2 goes the same way: every texture freed, the view back to the
	// full surface, lighting back to identity.
#ifdef GPU64_RASTER_ENABLED
	gpu64_rasterReset();
#endif
}

// --- argument accessors -------------------------------------------------
static inline u16 argU16( unsigned i )
{
	return (u16)( sArg[ i ] | ( sArg[ i + 1 ] << 8 ) );
}

static inline int argS16( unsigned i )
{
	return (int)(s16)argU16( i );
}

// Full 6-byte blob descriptor: space, 24-bit addr, 16-bit len.
static inline void argBlob( unsigned i, u8 *pSpace, u32 *pAddr, u32 *pLen )
{
	*pSpace = sArg[ i ];
	*pAddr  = (u32)sArg[ i + 1 ] | ( (u32)sArg[ i + 2 ] << 8 ) | ( (u32)sArg[ i + 3 ] << 16 );
	*pLen   = (u32)sArg[ i + 4 ] | ( (u32)sArg[ i + 5 ] << 8 );
}

// 4-byte compact descriptor (math operands): space + 24-bit addr, no length
// -- dimensions and element size imply it.
static inline void argCompact( unsigned i, u8 *pSpace, u32 *pAddr )
{
	*pSpace = sArg[ i ];
	*pAddr  = (u32)sArg[ i + 1 ] | ( (u32)sArg[ i + 2 ] << 8 ) | ( (u32)sArg[ i + 3 ] << 16 );
}

// --- fixed-point helpers ------------------------------------------------
//
// Elements are signed 16-bit 8.8. Products accumulate at full width (s64,
// since a 255-term dot product of 16.16 values overflows s32), then come back
// to 8.8 by rounding half away from zero and saturating rather than wrapping
// -- a saturated highlight looks like clipping, a wrapped one looks like a
// hardware fault. Documented in docs/api_design.md because C64 code can see
// the difference.
static inline s16 fixRead( const u8 *p, u32 i )
{
	return (s16)( p[ 2 * i ] | ( p[ 2 * i + 1 ] << 8 ) );
}

// gpu64: rounds half away from zero, which is what docs/api_design.md
// specifies. The bias has to be paired with a division that truncates
// toward zero, NOT with an arithmetic shift: >> floors, so on a negative
// accumulator the -128 bias is applied a second time and every negative
// 8.8 product came back one LSB further from zero than it should --
// -1.0 * 1.0 returned $FEFF (-1.00391) instead of $FF00. Positive products
// were never affected, which is why this survived a hardware-verified
// milestone: a one-LSB error on negatives only is invisible in a rendered
// frame. Found by gpu64_test_math, predicted on a PC and confirmed on
// hardware as exactly three red lines (ROUND NEG, MUL MINUS ONE,
// SCALE NEG) and no others.
static inline void fixWrite( u8 *p, u32 i, s64 acc )
{
	acc = ( acc >= 0 ) ? ( acc + 128 ) / 256 : -( ( -acc + 128 ) / 256 );
	if ( acc >  32767 ) acc =  32767;
	if ( acc < -32768 ) acc = -32768;
	p[ 2 * i ]     = (u8)( (u16)acc & 0xff );
	p[ 2 * i + 1 ] = (u8)( ( (u16)acc >> 8 ) & 0xff );
}

// Same saturation, for values that are already 8.8 (add/sub/transpose).
static inline void fixStore( u8 *p, u32 i, s32 v )
{
	if ( v >  32767 ) v =  32767;
	if ( v < -32768 ) v = -32768;
	p[ 2 * i ]     = (u8)( (u16)v & 0xff );
	p[ 2 * i + 1 ] = (u8)( ( (u16)v >> 8 ) & 0xff );
}

static inline float fltRead( const u8 *p, u32 i )
{
	float f;
	memcpy( &f, p + 4 * i, 4 );
	return f;
}

static inline void fltWrite( u8 *p, u32 i, float f )
{
	memcpy( p + 4 * i, &f, 4 );
}

// --- class 0 dispatch ---------------------------------------------------

// gpu64: LOG_ENABLE(1) is the only command exempt from the log auto-hide, so
// it is the one place a bench program can ask the firmware to say something
// back. Milestone 4c's flip rework exists to move a measured cost, so it
// reports the measurement -- otherwise the only way to know whether the
// mailbox split actually helped is to believe it did. See gpu64_flip.h.
static char *logDec( char *p, u32 v )
{
	char tmp[ 10 ];
	unsigned n = 0;
	do { tmp[ n++ ] = (char)( '0' + v % 10 ); v /= 10; } while ( v != 0 );
	while ( n-- ) *p++ = tmp[ n ];
	return p;
}

static char *logTriple( char *p, u32 nMin, u32 nTotal, u32 nMax, u32 nCount )
{
	if ( nCount == 0 )
		{ *p++ = '-'; return p; }
	p = logDec( p, nMin );  *p++ = '/';
	p = logDec( p, nTotal / nCount ); *p++ = '/';
	p = logDec( p, nMax );  *p++ = 'u'; *p++ = 's';
	return p;
}

static void logFlipStats( CGpu64FrameBuffer *pFB )
{
	if ( pFB == 0 )
		return;

	// Two lines, and every field is a u32 -- sized for the absurd case
	// (ten digits everywhere) rather than the realistic one, because this
	// writes without bounds checking.
	char line[ 192 ];
	char *p = line;

	const char *pTag = gpu64_flipAvailable() ? "FLIP fast n=" : "FLIP SLOW n=";
	for ( const char *q = pTag; *q; q++ ) *p++ = *q;
	p = logDec( p, gpu64FlipStats.postCount );

	*p++ = ' '; *p++ = 'h'; *p++ = '=';
	p = logTriple( p, gpu64FlipStats.postMinUs, gpu64FlipStats.postTotalUs,
			  gpu64FlipStats.postMaxUs, gpu64FlipStats.postCount );
	*p++ = '\n';

	for ( const char *q = "FLIP wait n="; *q; q++ ) *p++ = *q;
	p = logDec( p, gpu64FlipStats.drainCount );
	*p++ = ' ';
	p = logTriple( p, gpu64FlipStats.drainMinUs, gpu64FlipStats.drainTotalUs,
			  gpu64FlipStats.drainMaxUs, gpu64FlipStats.drainCount );
	*p++ = ' '; *p++ = 's'; *p++ = '=';
	p = logDec( p, gpu64FlipStats.slowCount );
	*p++ = '\n';

	pFB->LogWrite( line, (unsigned)( p - line ) );
}

// gpu64: milestone 6's load-ladder table rides the same reporting hook as
// the flip stats -- LOG_ENABLE(1) is still the one command a bench program
// has for asking the firmware to say something back. Compiles away in a
// normal build (gpu64_ladder.h).
static void logLadderStats( CGpu64FrameBuffer *pFB )
{
#ifdef GPU64_LADDER_ENABLED
	if ( pFB == 0 )
		return;

	// One header line plus one line per rung, each well inside the log's 40
	// columns; sized generously because gpu64_ladderReport() writes without
	// bounds checking, exactly like logFlipStats() above. Static, per the
	// note on the blob buffers at the top of this file: this runs on
	// reuUsingPolling()'s stack.
	static char line[ 64 * ( GPU64_LADDER_RUNGS + 2 ) ];
	char *p = gpu64_ladderReport( line );

	pFB->LogWrite( line, (unsigned)( p - line ) );
#else
	(void)pFB;
#endif
}

// gpu64: milestone 6's class 1 counters ride the same LOG_ENABLE(1) hook as
// the flip stats and the ladder table. Reading gpu64_3dWorkerStats here means
// core 0 touches a line core 1 writes -- normally forbidden (milestone 6a
// rounds 8-10) -- but this runs only from the log path, never from
// reuUsingPolling()'s PHI-locked hot loop, so the coherence miss has nowhere
// to make core 0 late. Do not call it from anywhere else.
static void logGpu64_3dStats( CGpu64FrameBuffer *pFB )
{
#ifdef GPU64_3D_ENABLED
	if ( pFB == 0 )
		return;

	// One line, well inside the log's 40 columns. Static for the same reason
	// the blob buffers at the top of this file are: this runs on
	// reuUsingPolling()'s stack.
	static char line[ 96 ];
	char *p = gpu64_3dReport( line );
	*p++ = '\n';

	pFB->LogWrite( line, (unsigned)( p - line ) );
#else
	(void)pFB;
#endif
}

// gpu64: class 2's counters on the same LOG_ENABLE(1) hook. What a bench
// round reads here is `rej=` -- a batch that lost bytes on the way across the
// bus rejects records that should have been accepted, which is the
// length-readback half of CLAUDE.md's rule 3.
static void logGpu64_RasterStats( CGpu64FrameBuffer *pFB )
{
#ifdef GPU64_RASTER_ENABLED
	if ( pFB == 0 )
		return;

	// One line, inside the log's 40 columns. Static for the same reason the
	// blob buffers at the top of this file are.
	static char line[ 96 ];
	char *p = gpu64_rasterReport( line );
	*p++ = '\n';

	pFB->LogWrite( line, (unsigned)( p - line ) );
#else
	(void)pFB;
#endif
}

// gpu64: the crash-path escape hatch, added after round 2 (2026-08-23).
//
// Both ladder rounds so far ended with the C64 derailed and back in the RAD
// menu, which means the test PRG never reached its closing LOG_ENABLE(1)
// and the whole table was lost -- the run cost a reflash and told us
// nothing quantitative. A run that fails is exactly the run whose numbers
// matter most, so the loop now dumps the table on its way out instead.
//
// Forces the log back on first: the auto-hide turned it off at the first
// command of the session, and DrawLogOverlay() is a no-op while it is off.
void gpu64_ladderDumpNow( void )
{
#ifdef GPU64_LADDER_ENABLED
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	pFB->LogEnable( TRUE );
	logFlipStats( pFB );
	logLadderStats( pFB );
	logGpu64_3dStats( pFB );
	logGpu64_RasterStats( pFB );
#endif
}

// --- health telemetry ---------------------------------------------------
// Power and thermal state, read from the VideoCore. The bench has twice shown
// a failure that looks exactly like a brownout -- mis-sampled IO2 writes, a
// torn RAD menu on entry, and finally a 6502 running wild over its own screen
// RAM -- and reseating the power cable made it go away once. That is a
// coincidence with a plausible story, not evidence, so this makes the Pi say
// for itself whether its supply ever sagged.
//
// GET_THROTTLED's bits 16-19 are sticky since boot, so a once-a-second poll
// cannot miss an event however brief it was. The cost is a mailbox round trip
// -- the same order as the blocking page flip milestone 4d removed -- so this
// is a housekeeping call, never a per-frame one.
struct GPU64HEALTH
{
	u32	throttled;		// last raw GET_THROTTLED word
	u32	sticky;			// OR of every word ever read
	u16	tempC10;		// last core temperature, tenths of a degree
	u16	tempMax10;		// highest seen
	u16	samples;
	u8	flagged;		// border already recoloured for an event
};
static GPU64HEALTH s_Health = { 0, 0, 0, 0, 0, 0 };

// gpu64: the latched half of GET_THROTTLED as it stood at the last session
// reset. The VideoCore's *_EVER bits latch until the *Pi* reboots, and the
// Pi now outlives the C64 -- so simply zeroing s_Health on a C64 reset
// achieves nothing: the very next GET_HEALTH reads the same latched word
// back and re-reddens the border for an under-voltage that belongs to a
// previous session. Masking this baseline out of the alarm test is what
// makes the sticky flags session-scoped, while an event that happens after
// the reset still shows through.
static u32 s_HealthEverBase = 0;

// Little-endian 16-bit, clamped -- these counters are diagnostics, and a
// wrapped one would read as healthy.
static inline void saturate16( u8 *p, u32 v )
{
	if ( v > 0xffff ) v = 0xffff;
	p[ 0 ] = (u8)( v & 0xff );
	p[ 1 ] = (u8)( v >> 8 );
}

// GET_THROTTLED bits. The low four are "right now", the high four latch.
// C64 palette index 2, red -- deliberately not a colour any demo sets.
#define GPU64_HEALTH_BORDER_ALARM	2

#define GPU64_THR_UNDERVOLT_NOW		0x00000001
#define GPU64_THR_UNDERVOLT_EVER	0x00010000

struct TPropertyTagThrottled
{
	TPropertyTag	Tag;
	u32		nValue;
}
PACKED;

static void readHealth( void )
{
	// bEarlyUse = TRUE skips the spinlock. Core 0 is the only caller and the
	// multicore rules forbid core 1 from touching MMIO at all, so there is
	// nothing to serialise against.
	CBcmPropertyTags Tags( TRUE );

	TPropertyTagThrottled Thr;
	if ( Tags.GetTag( PROPTAG_GET_THROTTLED, &Thr, sizeof Thr, 0 ) )
	{
		s_Health.throttled = Thr.nValue;
		s_Health.sticky |= Thr.nValue;
	}

	TPropertyTagTemperature Temp;
	Temp.nTemperatureId = TEMPERATURE_ID;
	if ( Tags.GetTag( PROPTAG_GET_TEMPERATURE, &Temp, sizeof Temp, 4 ) )
	{
		s_Health.tempC10 = (u16)( Temp.nValue / 100 );
		if ( s_Health.tempC10 > s_Health.tempMax10 )
			s_Health.tempMax10 = s_Health.tempC10;
	}

	s_Health.samples++;

	// A brownout tends to take the C64 down with it, and a hung C64 cannot
	// read a register or print a status line. The HDMI border can say it
	// without the C64's help and goes on saying it after everything else has
	// stopped: red the moment the VideoCore admits to an under-voltage.
	if ( !s_Health.flagged
	     && ( s_Health.sticky & ~s_HealthEverBase
		  & ( GPU64_THR_UNDERVOLT_NOW | GPU64_THR_UNDERVOLT_EVER ) ) )
	{
		s_Health.flagged = 1;
		if ( g_pGpu64FB )
			g_pGpu64FB->SetBorder( GPU64_HEALTH_BORDER_ALARM );
	}
}

// gpu64: back to the health state the Pi boots with, as far as the hardware
// permits. Refreshing first is deliberate: s_Health.throttled would otherwise
// hold whatever the last GET_HEALTH saw, which in a session that never issued
// one is zero -- and a zero baseline masks nothing.
static void healthReset( void )
{
	// The baseline read must not trip the alarm it is being taken to
	// suppress. s_HealthEverBase still holds the PREVIOUS baseline at this
	// point -- zero on a Pi's first reset of a session -- so the mask that
	// makes the sticky flags session-scoped does not exist yet, and an
	// under-voltage latched before this session reddens the border on its
	// way to being excluded from it. Bench run 33 ran its whole session
	// with a red border for that reason. Setting `flagged` over the read is
	// what suppresses the test inside readHealth(); it is cleared again
	// below, so a genuine in-session event still shows through.
	s_Health.flagged = 1;
	readHealth();
	s_HealthEverBase = s_Health.throttled & 0xffff0000;
	s_Health.sticky = 0;
	s_Health.flagged = 0;
	s_Health.tempMax10 = 0;
	s_Health.samples = 0;
}

// gpu64: everything a C64 reset must undo that gpu64_apiReset() above does
// not -- which is the whole of the display.
//
// gpu64_apiReset() has always cleared the register file and both drawing
// classes, but nothing ever reset CGpu64FrameBuffer: mode, palette, border,
// draw/visible page and the 80x50 text planes all survived a reset and a
// whole change of program. The sharpest consequence was that a program which
// left the display in text mode killed the screen mirror outright --
// CRAD::showMirror() blits into PageBuffer( GetDrawPage() ), the *graphics*
// page, with no mode check, so every snapshot after the reset painted
// somewhere the VideoCore was not scanning out and HDMI stayed frozen on the
// old text screen for good.
//
// Deliberately does NOT touch gpu64Regs. Both bus loops call this right
// after resetREU(), which already calls gpu64_apiReset() and owns the
// register file; FULL_RESET ($0B) calls it from inside a dispatch, where
// clearing sSeq/sSeqAck would break the sequence-gap detector the whole
// command protocol rests on.
//
// It is called from the reset branch rather than from resetREU() itself on
// purpose: resetREU() also runs on every fresh entry into REU emulation, and
// blanking the display there would wipe the mirror the moment the user
// switches the RAD menu to REU.
void gpu64_apiFullReset( boolean bReprogram )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;

	// A new program owns the machine now, and it may not check anything.
	sStrict = FALSE;

	gpu64_vsyncResetState();

	// Stage 15b observation point, and the strongest reason this cannot be
	// a plain memset: a class 1 render may still be queued on core 1 with
	// one of the pages below as its target. Drain before mutating.
#ifdef GPU64_3D_ENABLED
	gpu64_3dSync();
#endif
	gpu64_flipDrain();

	if ( pFB )
	{
		// A no-op unless a program actually left text mode; when it did,
		// this reprograms the VideoCore, which is the expensive path and
		// the reason the callers below are all edge-triggered.
		pFB->SetMode( GPU64_MODE_GRAPHICS );
		// Tracker 93: on the C64U, when the machine powers on after the Pi,
		// the palette the VideoCore scans out goes wrong entry by entry,
		// and 3306 SET_PALETTEs never put it right. Re-sending the whole
		// set-up is what boot does, and boot's colours are right.
		if ( bReprogram )
			pFB->Reprogram();
		pFB->ResetPalette();
		pFB->ResetPages();
		// ClearAllPages() memsets the border band too, so SetBorder() has
		// to follow it -- the same order SetMode() uses.
		pFB->ClearAllPages();
		pFB->SetBorder( 0 );
	}

	gpu64_textReset();
	healthReset();
}

static u8 doSystem( u8 op )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;

	switch ( op )
	{
	case 0x00:					// NOP
		// Stage 15b: NOP is otherwise genuinely side-effect-free, which
		// makes it the natural "force a sync" primitive for a program that
		// wants a queued CLEAR_VIEWPORT/DRAW_MESH/DRAW_NODE's own RESULT
		// right now and has nothing else to issue -- see gpu64_3dSync()'s
		// comment and docs/class1-3d-mesh-reference.md's "Deferred RESULT"
		// note. Unlike ARENA_STATUS or UPLOAD_MESH, NOP has no RESULT
		// contract of its own to clobber the flushed value with.
#ifdef GPU64_3D_ENABLED
		gpu64_3dSync();
#endif
		return GPU64_ERR_OK;

	case 0x01:					// RESET_STATE
		sStatus = 0;
		gpu64_vsyncResetState();
		// Stage 15b observation point: ResetPages() touches every page's
		// content, which a class 1 render still queued (not yet run on
		// core 1) also targets -- see gpu64_3dSync()'s comment.
#ifdef GPU64_3D_ENABLED
		gpu64_3dSync();
#endif
		if ( pFB ) pFB->ResetPages();
		// The text planes are part of the state RESET_STATE names, and
		// resetting them while the graphics display is up is exactly the
		// stage-a-screen-then-switch case gpu64_text.h describes -- so this
		// repaints only if text mode happens to be the live one.
		gpu64_textReset();
		if ( pFB && pFB->GetMode() == GPU64_MODE_TEXT )
			gpu64_textRenderRows( 0, GPU64_TXT_ROWS );
		return GPU64_ERR_OK;

	case 0x0B:					// FULL_RESET
		// The whole display back to the state it has one instruction after
		// the Pi boots -- graphics mode, C64 palette, border 0, page 0
		// drawn and visible, every page black, text planes cleared. This is
		// what a C64 reset now runs by itself (resetREU(), rad_reu.cpp); the
		// opcode exists so a program can get there without one, which until
		// now every demo had to approximate with an unconditional
		// TEXT_MODE 0 at start-up.
		//
		// A setup-time command, never a per-frame one: leaving text mode
		// reprograms the VideoCore, and the C64 is halted for all of it.
		//
		// Deliberately leaves gpu64ApiActive set and the mirror latched
		// off. A program issuing this still owns the screen; only a real
		// C64 reset means BASIC has the machine back.
		//
		// STATUS is cleared AFTER the reset, not before. gpu64_apiFullReset()
		// runs gpu64_3dSync() as its Stage 15b observation point, and
		// pollLoopFrame() inside it can harvest a loop frame that core 1
		// finished while this command was being assembled -- setting
		// FRAME_READY and stamping RESULT. Clearing first let that land on
		// top of the clear, so a FULL_RESET could answer with FRAME_READY set
		// and a previous frame's page number in RESULT. The /RESET path never
		// showed it: resetREU() calls gpu64_apiReset() first, which drains
		// the loop frame, so its gpu64_apiFullReset() has nothing to harvest.
		// The opcode is the only caller with a render possibly still in
		// flight.
		//
		// gpu64 (2026-09-27): and the loop stops with it. FULL_RESET used
		// to leave a running loop running: gpu64_apiFullReset()'s drain
		// harvested the frame in flight, the clear below then erased its
		// FRAME_READY, and with no frame left to raise it again every
		// SCENE_COMMIT answered BUSY for the rest of the run -- HDMI frozen,
		// C64 alive. runsim found it: a data fault turned the game's
		// unchecked GET_HEALTH $0A into $0B. Stopped, the loop is re-armed
		// by the next SCENE_COMMIT like any other stop. Not in
		// gpu64_apiFullReset() itself: the /RESET path has already stopped
		// the loop as SESSION, and that cause is the one worth keeping.
		gpu64_apiFullReset();
#ifdef GPU64_3D_ENABLED
		gpu64_3dLoopFullReset();
#endif
		sStatus = 0;
		sResult = 0;
		return GPU64_ERR_OK;

	case 0x02:					// VBLANK_ARM
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( sArg[ 0 ] == 1 )
		{
			if ( !gpu64Vsync.calibrated )
				return GPU64_ERR_UNSUPPORTED;
			// Arming is a setup-time command, so it is the natural place to
			// pay for a re-anchor: the extrapolated clock is at its most
			// accurate right after one, and the up-to-one-frame halt this
			// costs happens once rather than per frame. See gpu64_vsync.h.
			gpu64_vsyncReanchor();
			gpu64Vsync.armed = 1;
			sStatus |= GPU64_STATUS_VBLANK_ARMED;
		} else
		{
			gpu64Vsync.armed = 0;
			sStatus &= ~GPU64_STATUS_VBLANK_ARMED;
			// Disarming while the line is still held would leave the C64
			// with an IRQ it can no longer explain.
			if ( gpu64Vsync.irqAsserted )
				gpu64Vsync.irqReleaseReq = 1;
			gpu64Vsync.irqRequest = 0;
		}
		return GPU64_ERR_OK;

	case 0x03:					// VBLANK_ACK
		sStatus &= ~GPU64_STATUS_VBLANK_PENDING;
		// Releasing bIRQ_OUT is the bus-watch loop's job -- it owns every
		// GPIO in the cartridge and is the only place that knows whether
		// REU is holding the same line. This just asks.
		if ( gpu64Vsync.irqAsserted )
			gpu64Vsync.irqReleaseReq = 1;
		return GPU64_ERR_OK;

	case 0x04:					// SET_DRAW_PAGE
		if ( sArg[ 0 ] >= GPU64_FB_PAGES )
			return GPU64_ERR_BAD_ARGS;
		// Stage 15b observation point. A queued class 1 render now carries
		// its own target page in its ring slot (Gpu64_3dCmd::page), stamped
		// when it was pushed, so this call can no longer redirect a draw
		// that was already issued -- that hazard is closed by construction.
		// The drain stays because the *other* half of the observation-point
		// contract still holds: RESULT/ERRCODE for a render pushed before
		// this call become valid only at a drain, and a program that changes
		// the draw page expects everything it drew for the old one to have
		// landed first. See gpu64_3dSync().
#ifdef GPU64_3D_ENABLED
		gpu64_3dSync();
#endif
		if ( pFB ) pFB->SetDrawPage( sArg[ 0 ] );
		return GPU64_ERR_OK;

	case 0x05:					// PAGE_FLIP
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( pFB == 0 )
			return GPU64_ERR_UNSUPPORTED;
		// Text mode has one page at a fixed scanout address -- there is
		// nothing to flip to. Refusing is better than silently succeeding:
		// a program porting a double-buffered loop to text mode finds out
		// at the first flip rather than by wondering why nothing moves.
		if ( pFB->GetMode() != GPU64_MODE_GRAPHICS )
			return GPU64_ERR_UNSUPPORTED;
		if ( sArg[ 0 ] == 1 )
		{
			if ( !gpu64Vsync.calibrated )
				return GPU64_ERR_UNSUPPORTED;
			// One flip can be outstanding at a time. Asking for a second
			// changes nothing, per the spec's "a failed dispatch does
			// nothing" rule -- the queued one still lands on its own
			// boundary. The C64 polls STATUS bit0 to know when.
			if ( gpu64Vsync.flipPending )
				return GPU64_ERR_BUSY;
			// Stage 15b observation point: a class 1 render queued before
			// this call is still drawing into the current draw page: if
			// the flip armed here fires before that draw actually runs on
			// core 1, the presented page is missing it. Draining first
			// means the page this arms to present is always exactly what
			// had been drawn to it as of this dispatch -- see
			// gpu64_3dSync().
#ifdef GPU64_3D_ENABLED
			gpu64_3dSync();
#endif
			// Everything expensive about a flip happens now, while the C64
			// is halted for this dispatch anyway; the loop is then left
			// with only the SetVirtualOffset to do at the boundary.
			pFB->PrepareFlip();
			// Warm the commit path now, while the bus is held for this
			// dispatch -- see gpu64_vsyncWarmCommit() in rad_reu.cpp for
			// why it cannot be done at the point of use.
			gpu64_vsyncWarmCommit();
			gpu64Vsync.flipPending = 1;
			sStatus |= GPU64_STATUS_BUSY;
			return GPU64_ERR_OK;
		}
		// Same observation point as the armed branch above, for the
		// immediate flip.
#ifdef GPU64_3D_ENABLED
		gpu64_3dSync();
#endif
		pFB->Flip();
		return GPU64_ERR_OK;

	case 0x06:					// GET_INFO
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		if ( len < 16 )
			return GPU64_ERR_BAD_ARGS;

		u8 info[ 16 ];
		memset( info, 0, sizeof info );
		info[ 0 ] = 'G'; info[ 1 ] = '6'; info[ 2 ] = '4';
		info[ 3 ] = 1;					// version major
		info[ 4 ] = 0;					// version minor
		// Geometry is per *mode*, not per build: in text mode the surface
		// is 640x400 with one page and a 64x72 border. A program that reads
		// the info block after TEXT_MODE therefore gets numbers it can
		// actually draw against -- and pages = 1 is how it learns PAGE_FLIP
		// is not available without trying it.
		const boolean bText = pFB && pFB->GetMode() == GPU64_MODE_TEXT;
		const unsigned nW = bText ? GPU64_TXT_WIDTH : GPU64_FB_WIDTH;
		const unsigned nH = bText ? GPU64_TXT_HEIGHT : GPU64_FB_HEIGHT;
		info[ 5 ] = (u8)( nW & 0xff );
		info[ 6 ] = (u8)( nW >> 8 );
		info[ 7 ] = (u8)( nH & 0xff );
		info[ 8 ] = (u8)( nH >> 8 );
		info[ 9 ] = 8;					// bits per pixel
		info[ 10 ] = bText ? 1 : GPU64_FB_PAGES;
		// Bitmap of implemented classes, built from the build's own
		// toggles rather than written out by hand -- this byte is how a
		// program discovers class 2 exists, and a hardcoded literal here
		// had already gone stale once (it still said "class 1 not" with
		// class 1 shipping).
		info[ 11 ] = 0x01;				// class 0
#ifdef GPU64_3D_ENABLED
		info[ 11 ] |= 0x02;				// class 1, the 3D pipeline
#endif
#ifdef GPU64_RASTER_ENABLED
		info[ 11 ] |= 0x04;				// class 2, the raster layer
#endif
		info[ 12 ] = bText ? GPU64_TXT_BORDER_W : GPU64_BORDER_W;
		info[ 13 ] = bText ? GPU64_TXT_BORDER_H : GPU64_BORDER_H;
		// Measured frame period in microseconds, 0 if the frame clock could
		// not be calibrated -- which is also how a program can tell in
		// advance that every vblank feature will return UNSUPPORTED.
		info[ 14 ] = (u8)( gpu64Vsync.periodUs & 0xff );
		info[ 15 ] = (u8)( ( gpu64Vsync.periodUs >> 8 ) & 0xff );
		return gpu64_blobWrite( space, addr, 16, info );
	}

	case 0x0C:					// SET_DMA_WINDOW
	{
		// ARG0-1 base, ARG2-3 length, both little-endian, C64 space. A
		// length of 0 restores the default and means "anywhere".
		//
		// This is the one guard a client can put between a lost ARG byte
		// and the Pi writing over its code. It does not make the
		// destination correct -- nothing can, the address arrives over the
		// same unprotected registers -- it makes a wrong one land inside a
		// buffer the program has already written off, or fail outright.
		//
		// RESULT gets a checksum of what was stored, because a client
		// cannot read ARG back: the register file answers $FF for
		// everything except STATUS/ERRCODE/RESULT/SEQACK. Verifying the
		// window therefore has to go through RESULT, and the same
		// range-then-retry rule as every other readback applies
		// (docs/error-codes.md).
		const u16 base = (u16)( sArg[ 0 ] | ( sArg[ 1 ] << 8 ) );
		const u16 len  = (u16)( sArg[ 2 ] | ( sArg[ 3 ] << 8 ) );
		if ( len != 0 && (u32)base + (u32)len > 65536 )
			return GPU64_ERR_OUT_OF_RANGE;
		gpu64DmaWinBase = base;
		gpu64DmaWinLen  = len;
		sResult = (u8)( sArg[ 0 ] ^ sArg[ 1 ] ^ sArg[ 2 ] ^ sArg[ 3 ] ^ 0xA5 );
		return GPU64_ERR_OK;
	}

	case 0x0A:					// GET_HEALTH
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		if ( len < 12 )
			return GPU64_ERR_BAD_ARGS;

		readHealth();

		u8 h[ 132 ];
		memset( h, 0, sizeof h );
		h[ 0 ] = (u8)( s_Health.throttled & 0xff );
		h[ 1 ] = (u8)( ( s_Health.throttled >> 8 ) & 0xff );
		h[ 2 ] = (u8)( ( s_Health.throttled >> 16 ) & 0xff );
		h[ 3 ] = (u8)( ( s_Health.throttled >> 24 ) & 0xff );
		h[ 4 ] = (u8)( s_Health.sticky & 0xff );
		h[ 5 ] = (u8)( ( s_Health.sticky >> 8 ) & 0xff );
		h[ 6 ] = (u8)( ( s_Health.sticky >> 16 ) & 0xff );
		h[ 7 ] = (u8)( ( s_Health.sticky >> 24 ) & 0xff );
		h[ 8 ] = (u8)( s_Health.tempC10 & 0xff );
		h[ 9 ] = (u8)( s_Health.tempC10 >> 8 );
		h[ 10 ] = (u8)( s_Health.tempMax10 & 0xff );
		h[ 11 ] = (u8)( s_Health.tempMax10 >> 8 );

		// gpu64: bytes 12-19, added 2026-09-06, are the flip path's
		// "impossible" counters. They live here rather than behind
		// LOG_ENABLE because a bench program cannot turn the log on without
		// changing the thing it is measuring (dev/hw_testing.md), and
		// GET_HEALTH is already the one opcode whose job is "tell me what
		// the Pi thinks of its own state". Saturated to 16 bits: the
		// question is zero-or-not, not how many.
		saturate16( h + 12, gpu64FlipStats.drainTimeouts );
		saturate16( h + 14, gpu64FlipStats.postFullTimeouts );
		saturate16( h + 16, gpu64FlipStats.slowCount );
		saturate16( h + 18, gpu64FlipStats.postCount );

		// gpu64: bytes 20-35, added 2026-09-06, are the hold-gap instrument
		// (gpu64_holdgap.h) -- how close in time two DMA holds get. Same
		// reasoning as the block above for why they ride GET_HEALTH.
		//
		// The three gap figures are raw ARM cycles saturated to 16 bits,
		// which is the useful range by construction: 65535 ARM cycles is
		// ~54 C64 cycles, so anything that saturates is by definition not a
		// collision. armPerC64 ships with them so the reader never has to
		// assume a clock.
		saturate16( h + 20, gpu64HoldGap.minGap );
		saturate16( h + 22, gpu64HoldGap.minGapD2C );
		saturate16( h + 24, gpu64HoldGap.armPerC64 );
		saturate16( h + 26, gpu64HoldGap.bucket[ 0 ] );
		saturate16( h + 28, gpu64HoldGap.bucket[ 1 ] );
		saturate16( h + 30, gpu64HoldGap.bucket[ 2 ] );
		saturate16( h + 32, gpu64HoldGap.d2cClose );
		saturate16( h + 34, gpu64HoldGap.asserts );

		// gpu64: bytes 36-47, added 2026-09-07, are the hold *gate*
		// (gpu64_holdgate.h) -- the forward-confirmation rule that replaced
		// "fire on the IO2 access and hope it was the last cycle". Riding
		// GET_HEALTH for the same reason as the two blocks above.
		//
		// What to read: armed == fired means every request for the bus got
		// it, which is the expected steady state. ageMax is the interesting
		// one -- the worst wait from arm to hold, in C64 cycles -- because a
		// number above about 10 says a program is reaching the API in a way
		// the gate keeps declining. dispatchDeferred counts read-modify-write
		// on CMD_LO, and should be 0 for every program in this tree.
		saturate16( h + 36, gpu64HoldGate.armed );
		saturate16( h + 38, gpu64HoldGate.fired );
		saturate16( h + 40, gpu64HoldGate.ageMax );
		saturate16( h + 42, gpu64HoldGate.declined );
		saturate16( h + 44, gpu64HoldGate.dispatchDeferred );
		saturate16( h + 46, gpu64HoldGate.dispatchFired );

		// gpu64: bytes 48-63, added 2026-09-09, are the bus-sampling
		// instrument (gpu64_busstats.h) -- how many IO2 accesses to the
		// gpu64 window the polling loop actually serviced, and how many of
		// those carried an address no gpu64 register occupies. Riding
		// GET_HEALTH for the same reason as the three blocks above.
		//
		// reads and writes are full 32-bit little-endian, not saturated:
		// they are denominators, and a saturated denominator is useless.
		// The counts a caller wants are always *differences* between two
		// GET_HEALTH calls, so wrap-around at 2^32 costs nothing.
		//
		// How to read the block is in gpu64_busstats.h; the short version
		// is that readsBad > 0 means an address was mis-sampled, while
		// reads falling short of what the C64 issued means the access never
		// arrived at all. orBadAddr and andBadAddr name the address bits
		// that moved -- expect them in the high nibble, A4-A7, which are the
		// pins the LVC257 multiplexes.
		h[ 48 ] = (u8)( gpu64BusStats.reads & 0xff );
		h[ 49 ] = (u8)( ( gpu64BusStats.reads >> 8 ) & 0xff );
		h[ 50 ] = (u8)( ( gpu64BusStats.reads >> 16 ) & 0xff );
		h[ 51 ] = (u8)( ( gpu64BusStats.reads >> 24 ) & 0xff );
		h[ 52 ] = (u8)( gpu64BusStats.writes & 0xff );
		h[ 53 ] = (u8)( ( gpu64BusStats.writes >> 8 ) & 0xff );
		h[ 54 ] = (u8)( ( gpu64BusStats.writes >> 16 ) & 0xff );
		h[ 55 ] = (u8)( ( gpu64BusStats.writes >> 24 ) & 0xff );
		saturate16( h + 56, gpu64BusStats.readsBad );
		saturate16( h + 58, gpu64BusStats.writesBad );
		h[ 60 ] = gpu64BusStats.lastBadAddr;
		h[ 61 ] = gpu64BusStats.orBadAddr;
		h[ 62 ] = gpu64BusStats.andBadAddr;
		h[ 63 ] = gpu64BusStats.lastBadWrite;

		// gpu64: bytes 64-79, added 2026-09-09, are the two dispatch-side
		// counters (gpu64_busstats.h). Full 32-bit and unsaturated for the
		// same reason as reads/writes above: dispatches is a denominator.
		//
		// The pair answers the question the 2026-09-09 quake3d run left
		// open -- 1284 SEQACK mismatches in 7703 SCENE_COMMITs, against a
		// bus the probe had just measured as clean over 111000 accesses.
		// Three readings, and they do not overlap:
		//
		//   dispatches short by the mismatch count -> the CMD_LO writes
		//     were lost and the commands never ran.
		//   dispatches match, seqRepeat == the mismatch count -> the
		//     commands all ran; the SEQ writes were lost.
		//   dispatches match, seqRepeat == 0 -> nothing was lost at all
		//     and the mismatch is in the C64's *read* of SEQACK.
		h[ 64 ] = (u8)( gpu64BusStats.dispatches & 0xff );
		h[ 65 ] = (u8)( ( gpu64BusStats.dispatches >> 8 ) & 0xff );
		h[ 66 ] = (u8)( ( gpu64BusStats.dispatches >> 16 ) & 0xff );
		h[ 67 ] = (u8)( ( gpu64BusStats.dispatches >> 24 ) & 0xff );
		h[ 68 ] = (u8)( gpu64BusStats.seqRepeat & 0xff );
		h[ 69 ] = (u8)( ( gpu64BusStats.seqRepeat >> 8 ) & 0xff );
		h[ 70 ] = (u8)( ( gpu64BusStats.seqRepeat >> 16 ) & 0xff );
		h[ 71 ] = (u8)( ( gpu64BusStats.seqRepeat >> 24 ) & 0xff );

		// 72-75: the `sta ARG,y` dummy reads readsBad used to swallow.
		// Not a fault count -- it is here so the reader can see that the
		// read side's big number has a boring explanation, and so that
		// readsBad above can be trusted as a fault counter.
		h[ 72 ] = (u8)( gpu64BusStats.argDummyReads & 0xff );
		h[ 73 ] = (u8)( ( gpu64BusStats.argDummyReads >> 8 ) & 0xff );
		h[ 74 ] = (u8)( ( gpu64BusStats.argDummyReads >> 16 ) & 0xff );
		h[ 75 ] = (u8)( ( gpu64BusStats.argDummyReads >> 24 ) & 0xff );

		// gpu64: bytes 76-79, added 2026-09-10 out of bench run 14, are
		// the destructive-opcode key's instrument -- gpu64_api.h for the
		// contract, gpu64_apidiag.h for what run 14 showed. 77 is the one
		// that names a phantom: the opcode of the last destructive command
		// refused for want of a key. 78 and 79 count the damage that still
		// got through, so "prevented" and "never happened" stay apart.
		h[ 76 ] = (u8)( gpu64ApiDiag.keyRefused > 255 ? 255 : gpu64ApiDiag.keyRefused );
		h[ 77 ] = gpu64ApiDiag.keyRefusedOp;
		h[ 78 ] = (u8)( gpu64ApiDiag.camLost > 255 ? 255 : gpu64ApiDiag.camLost );
		h[ 79 ] = (u8)( gpu64ApiDiag.sceneWipes > 255 ? 255 : gpu64ApiDiag.sceneWipes );

		// gpu64: bytes 80-95, added 2026-09-10, say WHICH of the four
		// producers of GPU64_ERR_UNSUPPORTED a class 1 program is stuck
		// on -- gpu64_apidiag.h has the whole reasoning.
		//
		// How to read it: with CMT 06 large on the C64's screen, exactly
		// one of the three counters below is large too. commitNoLoop
		// large means loopStopCause names what stopped the loop;
		// classRefusedMode large means the display left graphics mode and
		// the node updates were being refused as well, not just the
		// commit. lastRefuseState is the seven-flag snapshot taken at the
		// most recent refusal, which is the state the run actually died
		// in -- not the state at GET_HEALTH time, by which point LOOP_STOP
		// has legitimately cleared the loop flag anyway.
		saturate16( h + 80, gpu64ApiDiag.commitNoLoop );
		saturate16( h + 82, gpu64ApiDiag.classRefusedMode );
		saturate16( h + 84, gpu64ApiDiag.commitNoClock );
		h[ 86 ] = gpu64ApiDiag.lastRefuseState;
		h[ 87 ] = gpu64ApiDiag.loopStopCause;
		saturate16( h + 88, gpu64ApiDiag.loopStops );
		h[ 90 ] = gpu64ApiDiag.lastRefuseOp;
		h[ 91 ] = gpu64ApiDiag.lastRefuseClass;
		h[ 92 ] = gpu64_apiDiagStateByte();

		// 96-97: the write-side twin of bytes 61-62. Added 2026-09-11
		// because runs 16 and 17 both reported byte 62 (andBadAddr) as
		// $80 -- A7 set in every bad READ address -- and there was no way
		// to ask the same question of writes, which is where the core-1
		// A/B effect overwhelmingly lives. Same fingerprint means one
		// mechanism in the address mux; different means two problems.
		h[ 96 ] = gpu64BusStats.orBadWrite;
		h[ 97 ] = gpu64BusStats.andBadWrite;

		// 98-113: bad write addresses by high nibble, the discriminator
		// bytes 96-97 could not give. See gpu64_busstats.h for what the two
		// shapes mean.
		for ( unsigned i = 0; i < 16; i++ )
			h[ 98 + i ] = gpu64BusStats.badWriteNib[ i ];

		// 114-121: the multiplexer-phase counters. muxMiss is how many
		// sampled IO2 accesses came back wearing the signal nibble, muxFixed
		// how many of those the re-read repaired. See gpu64_busstats.h.
		h[ 114 ] = (u8)( gpu64BusStats.muxMiss & 0xff );
		h[ 115 ] = (u8)( ( gpu64BusStats.muxMiss >> 8 ) & 0xff );
		h[ 116 ] = (u8)( ( gpu64BusStats.muxMiss >> 16 ) & 0xff );
		h[ 117 ] = (u8)( ( gpu64BusStats.muxMiss >> 24 ) & 0xff );
		// 118-119: of the late reads, how many were in a pass that paid for a
		// mux re-read. Overwrites what used to be muxFixed, which stopped
		// being printed in run 25 -- BUS A/BUS B reading 0000 0000 is the
		// direct proof the repair works, so the derived counter was spending
		// four bytes to say it again. Saturating u16.
		{
			u32 em = gpu64ReadSlack.expiredMux;
			if ( em > 65535 ) em = 65535;
			h[ 118 ] = (u8)( em & 0xff );
			h[ 119 ] = (u8)( em >> 8 );
		}

		// 120-121: muxUnfixed, wide. h[122] has always carried it clamped
		// to 255, on the reasoning that the expected value is zero and BUS
		// A/BUS B reading 0000 0000 was the direct proof of the repair.
		// Run 28 (2026-09-20) killed that reasoning both ways: BUS A/BUS B
		// were lost to a poisoned health sample, and BUS BAD came back with
		// 12252 bad accesses against a muxMiss of 14607 -- which says the
		// re-read repaired about a sixth of them, not all of them, and says
		// it only by inference because the direct counter was saturated at
		// 255. The repair rate is the whole question now, so publish it.
		{
			u32 uf = gpu64BusStats.muxUnfixed;
			if ( uf > 65535 ) uf = 65535;
			h[ 120 ] = (u8)( uf & 0xff );
			h[ 121 ] = (u8)( uf >> 8 );
		}

		// 122: the mux canary, one byte. Expected zero; nonzero means one
		// re-read did not suffice and the access was discarded.
		h[ 122 ] = gpu64BusStats.muxUnfixed > 255 ? 255
					: (u8)gpu64BusStats.muxUnfixed;

		// 123-125: the read-window overshoot. Run 24's h[126] clamped the
		// minimum at zero, which answered "is the margin gone" (it is) and
		// destroyed "by how much" -- and how much is the whole question,
		// because WAIT_CYCLE_READ2 is RAD's conservative target and not the
		// C64's hard limit. A few cycles past it is nothing; a few hundred
		// is a quarter of a C64 cycle. So publish the WORST overshoot as a
		// positive number, and count the events wide enough to have a rate
		// against BUS RW's read total instead of saturating at 255.
		{
			s32 ms = gpu64ReadSlack.minSlack;
			s32 over = ms < 0 ? -ms : 0;
			if ( over > 255 ) over = 255;
			h[ 123 ] = (u8)over;

			u32 ex = gpu64ReadSlack.expired;
			if ( ex > 65535 ) ex = 65535;
			h[ 124 ] = (u8)( ex & 0xff );
			h[ 125 ] = (u8)( ex >> 8 );
		}

		// 126: the positive side of the same minimum, for the run where
		// the overshoot finally reads zero. Only meaningful then.
		// 127: how many overshoots were big enough to matter -- 64 ARM
		// cycles, ~46ns, about 4.5% of a C64 cycle. A max on its own cannot
		// tell one freak pass from a systematic one; this pair can.
		{
			s32 ms = gpu64ReadSlack.minSlack;
			if ( ms < 0 ) ms = 0;
			if ( ms > 255 ) ms = 255;
			h[ 126 ] = (u8)ms;
			h[ 127 ] = gpu64ReadSlack.expiredBig > 255 ? 255
						: (u8)gpu64ReadSlack.expiredBig;
		}

		// 93-95: where the command that stopped the loop came from. Only
		// meaningful when byte 87 reads GPU64_LOOPSTOP_OPCODE; the reading
		// is (93 - 94), the distance the sequence number moved for that
		// dispatch -- 0 means no command arrived and the CMD_LO write was
		// a phantom, 1 means a real command's opcode byte was corrupted.
		// gpu64_apidiag.h has the full case analysis.
		h[ 93 ] = gpu64ApiDiag.loopStopSeq;
		h[ 94 ] = gpu64ApiDiag.loopStopSeqPrev;
		h[ 95 ] = gpu64ApiDiag.loopStopPrevOp;

		// 128-131: the trailer that makes the block self-verifying, and the
		// reason a caller would ever ask for more than 128 bytes.
		//
		// Every other readback in gpu64 can be checked. This one could not:
		// all 128 bytes are allocated, so the only test a client had was
		// "poison the buffer and see whether the poison is gone", which
		// passes on a block that arrived only in PART. Run 29 (2026-09-20)
		// is what that costs -- the A/B experiment's 32-bit columns came
		// back at 3.4 million against a run total of 190 thousand, because
		// poison surviving in one 4-byte field gives one huge positive
		// delta and one huge negative one and the poison check never saw
		// it. The blob path is the one CLAUDE.md rule 3 names outright: any
		// bulk transfer through REU DMA needs a checksum or a length
		// readback. The health block is a bulk transfer and had neither.
		//
		// A magic plus two independent check bytes: XOR catches any single
		// wrong byte, the additive sum catches a transposition XOR is blind
		// to. Both cover 0-129, so the magic is checked by them too.
		//
		// Purely additive. A caller asking for 128 gets the same 128 bytes
		// it always did; the trailer exists only for one that asks for 132.
		{
			h[ 128 ] = 'G';
			h[ 129 ] = '6';
			u8 x = 0, a = 0;
			for ( unsigned i = 0; i < 130; i++ )
			{
				x = (u8)( x ^ h[ i ] );
				a = (u8)( a + h[ i ] );
			}
			h[ 130 ] = x;
			h[ 131 ] = a;
		}

		// Length-compatible with the 12-, 20-, 36-, 48-, 64-, 80- and
		// 128-byte contracts: a caller that asks for any of them still gets
		// exactly what it always got.
		return gpu64_blobWrite( space, addr,
				len >= 132 ? 132 : ( len >= 128 ? 128 : ( len >= 112 ? 112
						: ( len >= 96 ? 96 : ( len >= 80 ? 80
						: ( len >= 64 ? 64 : ( len >= 48 ? 48
						: ( len >= 36 ? 36
						: ( len >= 20 ? 20 : 12 ) ) ) ) ) ) ) ), h );
	}

	case 0x08:					// SET_BORDER
		if ( pFB ) pFB->SetBorder( sArg[ 0 ] );
		return GPU64_ERR_OK;

	case 0x07:					// LOG_ENABLE
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( pFB ) pFB->LogEnable( sArg[ 0 ] ? TRUE : FALSE );
		// Turning the log *on* is a request to be told something, so this is
		// where the flip cost gets reported (logFlipStats() above).
		if ( sArg[ 0 ] ) { logFlipStats( pFB ); logLadderStats( pFB ); logGpu64_3dStats( pFB ); logGpu64_RasterStats( pFB ); }
		return GPU64_ERR_OK;

	case 0x09:					// VBLANK_SYNC
		// The escape hatch for a long-running program: the frame clock is an
		// extrapolation and drifts over minutes (gpu64_vsync.h), so this
		// re-pins it to a real vsync. Costs up to one frame of C64 halt, so
		// it is an occasional housekeeping call, not a per-frame one.
		if ( !gpu64Vsync.calibrated )
			return GPU64_ERR_UNSUPPORTED;
		if ( gpu64Vsync.flipPending )
			return GPU64_ERR_BUSY;
		return gpu64_vsyncReanchor() ? GPU64_ERR_OK : GPU64_ERR_UNSUPPORTED;
	}

	return GPU64_ERR_BAD_OPCODE;
}

static u8 doDraw( u8 op )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;

	// Stage 15b observation point: every op below reads or writes the
	// current draw page's pixels directly, the same page a queued-but-not-
	// yet-run class 1 render also targets. One drain here, rather than one
	// per opcode, both fixes the race and keeps a program's CLEAR-then-
	// DRAW_NODE-then-RECT (or the reverse) in the order it issued them --
	// see gpu64_3dSync().
#ifdef GPU64_3D_ENABLED
	gpu64_3dSync();
#endif

	switch ( op )
	{
	case 0x10:					// CLEAR
		pFB->Clear( sArg[ 0 ] );
		return GPU64_ERR_OK;

	case 0x20:					// SET_PIXEL
		pFB->SetPixel( argS16( 0 ), argS16( 2 ), sArg[ 4 ] );
		return GPU64_ERR_OK;

	case 0x21:					// LINE
		pFB->Line( argS16( 0 ), argS16( 2 ), argS16( 4 ), argS16( 6 ), sArg[ 8 ] );
		return GPU64_ERR_OK;

	case 0x22:					// RECT
		pFB->Rect( argS16( 0 ), argS16( 2 ), (int)argU16( 4 ), (int)argU16( 6 ), sArg[ 8 ] );
		return GPU64_ERR_OK;

	case 0x23:					// RECT_FILL
		pFB->RectFill( argS16( 0 ), argS16( 2 ), (int)argU16( 4 ), (int)argU16( 6 ), sArg[ 8 ] );
		return GPU64_ERR_OK;

	case 0x30:					// PAL_SET
		pFB->SetPaletteEntry( sArg[ 0 ], sArg[ 1 ], sArg[ 2 ], sArg[ 3 ] );
		pFB->CommitPalette();
		return GPU64_ERR_OK;

	case 0x31:					// PAL_LOAD
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		u8 first = sArg[ 6 ];
		u8 count = sArg[ 7 ];

		if ( count == 0 )
			return GPU64_ERR_OK;			// no-op, not an error
		if ( len != (u32)count * 3 )
			return GPU64_ERR_BAD_ARGS;
		if ( (u32)first + count > 256 )
			return GPU64_ERR_BAD_ARGS;

		u8 res = gpu64_blobRead( space, addr, len, sBlobA );
		if ( res != GPU64_ERR_OK )
			return res;

		for ( u32 i = 0; i < count; i++ )
			pFB->SetPaletteEntry( (u8)( first + i ), sBlobA[ i * 3 ], sBlobA[ i * 3 + 1 ], sBlobA[ i * 3 + 2 ] );
		pFB->CommitPalette();
		return GPU64_ERR_OK;
	}

	case 0x40:					// BLIT
	case 0x41:					// BLIT_KEYED
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		int dstX = argS16( 6 );
		int dstY = argS16( 8 );
		u32 w = argU16( 10 );
		u32 h = argU16( 12 );

		if ( w == 0 || h == 0 )
			return GPU64_ERR_OK;
		if ( len != w * h )
			return GPU64_ERR_BAD_ARGS;

		u8 res = gpu64_blobRead( space, addr, len, sBlobA );
		if ( res != GPU64_ERR_OK )
			return res;

		pFB->Blit( sBlobA, dstX, dstY, w, h, ( op == 0x41 ) ? (int)sArg[ 14 ] : -1 );
		return GPU64_ERR_OK;
	}

	case 0x42:					// READ_RECT
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		int srcX = argS16( 6 );
		int srcY = argS16( 8 );
		u32 w = argU16( 10 );
		u32 h = argU16( 12 );

		if ( w == 0 || h == 0 )
			return GPU64_ERR_OK;
		if ( len != w * h )
			return GPU64_ERR_BAD_ARGS;
		// Reading back is specified as an error, not a clip: a partly
		// offscreen read has no defined content to return.
		if ( srcX < 0 || srcY < 0 ||
		     srcX + (int)w > GPU64_FB_WIDTH || srcY + (int)h > GPU64_FB_HEIGHT )
			return GPU64_ERR_BAD_ARGS;

		pFB->ReadRect( sBlobA, (unsigned)srcX, (unsigned)srcY, w, h );
		return gpu64_blobWrite( space, addr, len, sBlobA );
	}
	}

	return GPU64_ERR_BAD_OPCODE;
}

// --- math ---------------------------------------------------------------

// Byte count of an m x n matrix, or 0 if it does not fit a blob.
static u32 matBytes( u32 rows, u32 cols, u32 elemSize )
{
	u32 n = rows * cols * elemSize;
	if ( n == 0 || n > 65535 )
		return 0;
	return n;
}

static u8 matInverse( u32 n, u32 elemSize, const u8 *pA, u8 *pC )
{
	if ( n > GPU64_MAX_INVERSE_N )
		return GPU64_ERR_BAD_ARGS;

	// Gauss-Jordan on an augmented [A | I], in double precision whatever the
	// operand format is: an 8.8 pivot divides to nothing very quickly, and
	// doing the elimination in the operand's own format would lose the
	// result long before the matrix is actually singular.
	double *w = sInvWork;
	const u32 stride = 2 * n;

	for ( u32 i = 0; i < n; i++ )
		for ( u32 j = 0; j < n; j++ )
		{
			double v = ( elemSize == 2 ) ? (double)fixRead( pA, i * n + j ) / 256.0
						     : (double)fltRead( pA, i * n + j );
			w[ i * stride + j ] = v;
			w[ i * stride + n + j ] = ( i == j ) ? 1.0 : 0.0;
		}

	for ( u32 col = 0; col < n; col++ )
	{
		u32 pivot = col;
		double best = w[ col * stride + col ];
		if ( best < 0 ) best = -best;

		for ( u32 r = col + 1; r < n; r++ )
		{
			double v = w[ r * stride + col ];
			if ( v < 0 ) v = -v;
			if ( v > best ) { best = v; pivot = r; }
		}

		if ( best < 1e-9 )
			return GPU64_ERR_SINGULAR;

		if ( pivot != col )
			for ( u32 j = 0; j < stride; j++ )
			{
				double t = w[ col * stride + j ];
				w[ col * stride + j ] = w[ pivot * stride + j ];
				w[ pivot * stride + j ] = t;
			}

		double d = w[ col * stride + col ];
		for ( u32 j = 0; j < stride; j++ )
			w[ col * stride + j ] /= d;

		for ( u32 r = 0; r < n; r++ )
		{
			if ( r == col )
				continue;
			double f = w[ r * stride + col ];
			if ( f == 0.0 )
				continue;
			for ( u32 j = 0; j < stride; j++ )
				w[ r * stride + j ] -= f * w[ col * stride + j ];
		}
	}

	for ( u32 i = 0; i < n; i++ )
		for ( u32 j = 0; j < n; j++ )
		{
			double v = w[ i * stride + n + j ];
			if ( elemSize == 2 )
				fixWrite( pC, i * n + j, (s64)( v * 65536.0 ) );	// v<<16, then >>8 in fixWrite
			else
				fltWrite( pC, i * n + j, (float)v );
		}

	return GPU64_ERR_OK;
}

// --- text mode, $50-$5F -------------------------------------------------
//
// A separate branch of the class 0 dispatcher rather than more cases in
// doDraw(), because none of doDraw()'s preamble applies: these ops do not
// touch a draw page, are not an observation point for a queued class 1
// render (TEXT_MODE is, and says so), and are legal with the graphics
// display up. See gpu64_text.h for the model.
static u8 doText( u8 op )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;

	switch ( op )
	{
	case 0x50:					// TEXT_MODE
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		if ( sArg[ 0 ] == pFB->GetMode() )
			return GPU64_ERR_OK;		// already there, and a no-op reprogram costs a mailbox round trip
		// Stage 15b observation point, and the strongest one in the API:
		// this reallocates the framebuffer, so a class 1 render still
		// queued for core 1 would be drawing into memory the VideoCore has
		// handed back. Drain before touching the display.
#ifdef GPU64_3D_ENABLED
		gpu64_3dSync();
#endif
		// A flip armed for a page that is about to stop existing has to
		// land (or be cancelled) first; flipDrain() at the top of every
		// dispatch has already retired an immediate one, this covers the
		// deferred case.
		if ( gpu64Vsync.flipPending )
			return GPU64_ERR_BUSY;
		if ( !gpu64_textSetMode( sArg[ 0 ] ) )
			return GPU64_ERR_UNSUPPORTED;
		return GPU64_ERR_OK;

	case 0x51:					// TEXT_CLEAR
		gpu64_textClear( sArg[ 0 ], sArg[ 1 ] );
		return GPU64_ERR_OK;

	case 0x52:					// TEXT_PUT
		// Off-grid is clipped, not an error -- the same contract SET_PIXEL
		// has, so a program plotting a moving cursor need not special-case
		// the edges.
		gpu64_textPut( sArg[ 0 ], sArg[ 1 ], sArg[ 2 ], sArg[ 3 ], sArg[ 4 ] );
		return GPU64_ERR_OK;

	case 0x53:					// TEXT_WRITE
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		if ( len == 0 )
			return GPU64_ERR_OK;
		if ( len > GPU64_TXT_COLS )
			len = GPU64_TXT_COLS;		// a row is the most that can land anyway
		u8 res = gpu64_blobRead( space, addr, len, sBlobA );
		if ( res != GPU64_ERR_OK )
			return res;
		gpu64_textWrite( sArg[ 6 ], sArg[ 7 ], sArg[ 8 ], sArg[ 9 ],
				 sBlobA, len, sArg[ 10 ] );
		return GPU64_ERR_OK;
	}

	case 0x54:					// TEXT_UPLOAD
	{
		u8 space; u32 addr, len;
		argBlob( 0, &space, &addr, &len );
		u8 plane = sArg[ 6 ];
		u32 first = (u32)sArg[ 7 ] | ( (u32)sArg[ 8 ] << 8 );
		if ( plane > GPU64_TXT_PLANE_BG )
			return GPU64_ERR_BAD_ARGS;
		if ( len == 0 )
			return GPU64_ERR_OK;
		// Checked here, not in gpu64_textUpload(), so a caller that gets
		// the arithmetic wrong is told rather than truncated: a partial
		// upload would leave the planes in a state the program does not
		// know about.
		if ( first >= GPU64_TXT_CELLS || first + len > GPU64_TXT_CELLS )
			return GPU64_ERR_OUT_OF_RANGE;
		u8 res = gpu64_blobRead( space, addr, len, sBlobA );
		if ( res != GPU64_ERR_OK )
			return res;
		gpu64_textUpload( plane, first, sBlobA, len );
		return GPU64_ERR_OK;
	}

	case 0x55:					// TEXT_CHARSET
		if ( sArg[ 0 ] > 1 )
			return GPU64_ERR_BAD_ARGS;
		gpu64_textSetCharset( sArg[ 0 ] );
		return GPU64_ERR_OK;

	case 0x56:					// TEXT_SCROLL
		gpu64_textScroll( sArg[ 0 ], sArg[ 1 ], sArg[ 2 ] );
		return GPU64_ERR_OK;

	case 0x57:					// TEXT_FILL
		gpu64_textFill( (int)(s8)sArg[ 0 ], (int)(s8)sArg[ 1 ],
				sArg[ 2 ], sArg[ 3 ],
				sArg[ 4 ], sArg[ 5 ], sArg[ 6 ] );
		return GPU64_ERR_OK;

	case 0x58:					// TEXT_REFRESH
		gpu64_textRenderRows( 0, GPU64_TXT_ROWS );
		return GPU64_ERR_OK;
	}

	return GPU64_ERR_BAD_OPCODE;
}

static u8 doMath( u8 op )
{
	// Low nibble picks the operation, high nibble the element format:
	// $8x = 8.8 fixed point (2 bytes), $9x = IEEE float32 (4 bytes).
	const u32 elemSize = ( ( op & 0xF0 ) == 0x80 ) ? 2 : 4;
	const u8 sub = op & 0x0F;

	u8 spaceA, spaceB, spaceC;
	u32 addrA, addrB, addrC;
	u8 res;

	switch ( sub )
	{
	case 0x00:					// MAT_MUL
	{
		u32 m = sArg[ 0 ], k = sArg[ 1 ], n = sArg[ 2 ];
		if ( m == 0 || k == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytesA = matBytes( m, k, elemSize );
		u32 bytesB = matBytes( k, n, elemSize );
		u32 bytesC = matBytes( m, n, elemSize );
		if ( bytesA == 0 || bytesB == 0 || bytesC == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 3, &spaceA, &addrA );
		argCompact( 7, &spaceB, &addrB );
		argCompact( 11, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytesA, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;
		res = gpu64_blobRead( spaceB, addrB, bytesB, sBlobB );
		if ( res != GPU64_ERR_OK ) return res;

		for ( u32 i = 0; i < m; i++ )
			for ( u32 j = 0; j < n; j++ )
			{
				if ( elemSize == 2 )
				{
					s64 acc = 0;
					for ( u32 x = 0; x < k; x++ )
						acc += (s64)fixRead( sBlobA, i * k + x ) * (s64)fixRead( sBlobB, x * n + j );
					fixWrite( sBlobC, i * n + j, acc );
				} else
				{
					float acc = 0.0f;
					for ( u32 x = 0; x < k; x++ )
						acc += fltRead( sBlobA, i * k + x ) * fltRead( sBlobB, x * n + j );
					fltWrite( sBlobC, i * n + j, acc );
				}
			}

		return gpu64_blobWrite( spaceC, addrC, bytesC, sBlobC );
	}

	case 0x01:					// MAT_ADD
	case 0x02:					// MAT_SUB
	{
		u32 m = sArg[ 0 ], n = sArg[ 1 ];
		if ( m == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( m, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 2, &spaceA, &addrA );
		argCompact( 6, &spaceB, &addrB );
		argCompact( 10, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;
		res = gpu64_blobRead( spaceB, addrB, bytes, sBlobB );
		if ( res != GPU64_ERR_OK ) return res;

		for ( u32 i = 0; i < m * n; i++ )
		{
			if ( elemSize == 2 )
			{
				s32 a = fixRead( sBlobA, i ), b = fixRead( sBlobB, i );
				fixStore( sBlobC, i, ( sub == 0x01 ) ? a + b : a - b );
			} else
			{
				float a = fltRead( sBlobA, i ), b = fltRead( sBlobB, i );
				fltWrite( sBlobC, i, ( sub == 0x01 ) ? a + b : a - b );
			}
		}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x03:					// MAT_SCALE
	{
		u32 m = sArg[ 0 ], n = sArg[ 1 ];
		if ( m == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( m, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 2, &spaceA, &addrA );
		argCompact( 6, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;

		if ( elemSize == 2 )
		{
			s64 s = (s64)(s16)argU16( 10 );		// the scalar, inline in ARG10-11
			for ( u32 i = 0; i < m * n; i++ )
				fixWrite( sBlobC, i, (s64)fixRead( sBlobA, i ) * s );
		} else
		{
			float s = fltRead( &sArg[ 10 ], 0 );	// ARG10-13
			for ( u32 i = 0; i < m * n; i++ )
				fltWrite( sBlobC, i, fltRead( sBlobA, i ) * s );
		}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x04:					// MAT_TRANSPOSE
	{
		u32 m = sArg[ 0 ], n = sArg[ 1 ];
		if ( m == 0 || n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( m, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 2, &spaceA, &addrA );
		argCompact( 6, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;

		for ( u32 i = 0; i < m; i++ )
			for ( u32 j = 0; j < n; j++ )
			{
				if ( elemSize == 2 )
					fixStore( sBlobC, j * m + i, fixRead( sBlobA, i * n + j ) );
				else
					fltWrite( sBlobC, j * m + i, fltRead( sBlobA, i * n + j ) );
			}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x05:					// MAT_IDENTITY
	{
		u32 n = sArg[ 0 ];
		if ( n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( n, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 1, &spaceC, &addrC );

		memset( sBlobC, 0, bytes );
		for ( u32 i = 0; i < n; i++ )
		{
			if ( elemSize == 2 )
				fixStore( sBlobC, i * n + i, 0x0100 );	// 1.0 in 8.8
			else
				fltWrite( sBlobC, i * n + i, 1.0f );
		}

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}

	case 0x06:					// MAT_INVERSE
	{
		u32 n = sArg[ 0 ];
		if ( n == 0 )
			return GPU64_ERR_BAD_ARGS;

		u32 bytes = matBytes( n, n, elemSize );
		if ( bytes == 0 )
			return GPU64_ERR_OUT_OF_RANGE;

		argCompact( 1, &spaceA, &addrA );
		argCompact( 5, &spaceC, &addrC );

		res = gpu64_blobRead( spaceA, addrA, bytes, sBlobA );
		if ( res != GPU64_ERR_OK ) return res;

		res = matInverse( n, elemSize, sBlobA, sBlobC );
		if ( res != GPU64_ERR_OK )
			return res;				// singular: nothing written

		return gpu64_blobWrite( spaceC, addrC, bytes, sBlobC );
	}
	}

	return GPU64_ERR_BAD_OPCODE;
}

void gpu64_apiDispatch( u8 op )
{
	// gpu64: reliability-protocol detector-only build (dev/reliability_protocol_design.md).
	// Publish the SEQ this dispatch is acting on unconditionally, before
	// anything can early-return -- the C64 side wrote SEQ (and, ordering
	// permitting, its args) before CMD_LO, so whatever value sits in
	// gpu64Regs.seq right now is the one this dispatch belongs to. This is
	// only a detector: it does not retry and does not change dispatch
	// behaviour, it just gives the C64 something reliable-by-construction to
	// compare its own SEQ against (SEQACK is read-only, so a mismatch can
	// only mean the CMD_LO/ARG/SEQ write sequence was not what the C64 sent).
	// gpu64: two counters, added 2026-09-09, that turn that detector from a
	// numerator into a measurement (gpu64_busstats.h). seqRepeat has to be
	// sampled *before* the publication below, because the publication is
	// what destroys the evidence: a caller that increments SEQ before every
	// command and still arrives with sSeq == sSeqAck lost the SEQ write, not
	// the command.
	if ( sSeq == sSeqAck )
		gpu64BusStats.seqRepeat++;
	gpu64BusStats.dispatches++;

	// gpu64 (2026-09-10): remember this dispatch and the one before it, so
	// that if something in it stops the class 1 loop, loopStop() can freeze
	// the provenance of the command that did it -- gpu64_apidiag.h. Four
	// byte stores at dispatch time, with the bus already held.
	gpu64ApiDiag.dispSeqPrev = gpu64ApiDiag.dispSeq;
	gpu64ApiDiag.dispSeq     = sSeq;
	gpu64ApiDiag.dispOpPrev  = gpu64ApiDiag.dispOp;
	gpu64ApiDiag.dispOp      = op;

	sSeqAck = sSeq;

	// gpu64 (2026-09-10, bench run 14): spend the destructive-opcode key --
	// gpu64_api.h. Here, ahead of every early return and for every class, so
	// that the key a command staged cannot survive into the command after it.
	// A dispatch that wanted the key has already had it latched; a dispatch
	// that did not has just disarmed ARG15 for the phantom that may follow.
	gpu64ApiKey = sArg[ GPU64_REG_KEY ];
	sArg[ GPU64_REG_KEY ] = 0;

	// gpu64 (2026-09-25, bench run 41): the optional per-command check --
	// gpu64_api.h, GPU64_KEY_CHECKED. Ahead of every class, so a command
	// whose bytes did not arrive as sent executes nowhere.
	if ( ( gpu64ApiKey & 0xF0 ) == GPU64_KEY_CHECKED )
	{
		const unsigned n = gpu64ApiKey & 0x0F;
		u8 x = gpu64ApiKey ^ sCmdHi ^ op ^ sId[ 0 ] ^ sId[ 1 ];
		for ( unsigned i = 0; i < n && i < GPU64_REG_CHECK; i++ )
			x ^= sArg[ i ];
		if ( n > GPU64_REG_CHECK || x != sArg[ GPU64_REG_CHECK ] )
		{
			gpu64ApiDiag.checkRefused++;
			sErr = GPU64_ERR_BAD_ARGS;
			sStatus |= GPU64_STATUS_ERROR;
			return;
		}

		// A program that commits a checked frame checks everything it
		// sends from then on -- that is what turns strict mode on.
		if ( sCmdHi == 1 && op == GPU64_3D_OP_SCENE_COMMIT )
			sStrict = TRUE;
	}
	else if ( sStrict && sCmdHi == 1 && !gpu64_apiStrictExempt( op ) )
	{
		// gpu64 (2026-09-27, bench video): a phantom -- a CMD_LO write the
		// C64 never made -- finds ARG15 already spent to zero, so the check
		// above never runs and it executes on whatever ARG bytes the last
		// real command staged. E1M1's SET_PERSPECTIVE ran on SET_POSITION's
		// coordinates: the view jumped to a 145-degree field of view for
		// the eight frames the refresh ring took to put it back. The one
		// thing a phantom cannot forge is the key, so once the program has
		// shown it keys everything, an unkeyed command is not its own.
		if ( gpu64ApiKey == GPU64_KEY_DESTRUCTIVE )
		{
			if ( op == GPU64_3D_OP_SCENE_RESET )
				sStrict = FALSE;	// a program starting over
		}
		else
		{
			gpu64ApiDiag.strictRefused++;
			gpu64ApiDiag.strictRefusedOp = op;
			sErr = GPU64_ERR_BAD_ARGS;
			sStatus |= GPU64_STATUS_ERROR;
			return;
		}
	}

	// gpu64: finish any page flip still in flight before anything else runs
	// (gpu64_flip.h). Two reasons, and both are correctness, not tidiness:
	// this command may draw into a page the VideoCore has not yet stopped
	// scanning out, and it may itself want Circle's mailbox, whose Flush()
	// would eat our reply at 20 ms a go. Normally a no-op -- the reply is
	// long since ready by the time the C64 issues its next command.
	gpu64_flipDrain();

	u8 res;

	// Classes 1 and 2 both draw into a framebuffer page, and in text mode
	// there is no page -- PageBase() returns 0 and every one of their writes
	// would land nowhere (gpu64_fb.cpp). Refusing here says so, once, in
	// front of both classes, rather than letting a whole scene render into
	// a null pointer and report OK.
	if ( sCmdHi != 0 && g_pGpu64FB && g_pGpu64FB->GetMode() != GPU64_MODE_GRAPHICS )
	{
		// gpu64 (2026-09-10): one of the four producers of the $06 that
		// both bench runs of that day ended stuck in -- gpu64_apidiag.h.
		gpu64_apiDiagBump( &gpu64ApiDiag.classRefusedMode );
		gpu64ApiDiag.lastRefuseState = gpu64_apiDiagStateByte();
		gpu64ApiDiag.lastRefuseOp    = op;
		gpu64ApiDiag.lastRefuseClass = sCmdHi;
		sErr = GPU64_ERR_UNSUPPORTED;
		sStatus |= GPU64_STATUS_ERROR;
		return;
	}

#ifdef GPU64_3D_ENABLED
	if ( sCmdHi == 1 )
	{
		// Class 1 does not execute here -- it stages a command and hands it
		// to core 1 (dev/milestone6_3d_design.md, Architecture). What comes
		// back is the *acceptance* result, not the command's outcome: a
		// class 1 OK means core 1 has the command, and STATUS bit4 or RESULT
		// is what says it finished. That is the whole point of the split.
		res = gpu64_3dDispatch( op );
	} else
#endif
#ifdef GPU64_RASTER_ENABLED
	if ( sCmdHi == 2 )
	{
		// Class 2 executes here and now, on core 0, exactly like a class 0
		// draw op -- the C64 is halted for the dispatch, so there is nothing
		// for another core to overlap with. See
		// dev/milestone8_raster_design.md.
		res = gpu64_rasterDispatch( op );
	} else
#endif
	if ( sCmdHi != 0 )
	{
		sErr = GPU64_ERR_BAD_CLASS;
		sStatus |= GPU64_STATUS_ERROR;
		return;
	} else
#ifdef GPU64_3D_ENABLED
	// gpu64 (2026-09-24, bench run 38): the class 1 loop owns the display
	// -- pages, flip, vsync state, palette -- and class1-3d-mesh-reference.md
	// has always said a class 0 framebuffer op against it is illegal. It was
	// never enforced, and the one sender that ignores the documentation is
	// the bus: a class 1 command whose CMD_HI write arrives as 0 runs as its
	// class 0 twin. SET_VIEWPORT $01 became RESET_STATE (pages and vsync
	// reset under a running loop), SET_BACKGROUND $05 a PAGE_FLIP,
	// SET_POSITION/SET_ORIENTATION $30/$31 PAL_SET/PAL_LOAD -- run 38's lost
	// colours, and the likeliest cause of its frozen HDMI. What stays legal
	// is what does not touch the display: NOP, GET_INFO, LOG_ENABLE,
	// VBLANK_SYNC, GET_HEALTH, SET_DMA_WINDOW, the matrix ops, and
	// FULL_RESET, which is how a new program takes over from one that left
	// the loop running.
	if ( gpu64_3dLoopRunning() && op < 0x80 &&
	     op != 0x00 && op != 0x06 && op != 0x07 && op != 0x09 &&
	     op != 0x0A && op != 0x0B && op != 0x0C )
		res = GPU64_ERR_BUSY;
	else
	// gpu64 (2026-09-28, bench: "the screen was completely dark, like
	// powered off"): of the class 0 ops left legal above, three change what
	// is on the screen -- FULL_RESET blacks every page and stops the loop,
	// LOG_ENABLE paints the log over it, VBLANK_SYNC halts the C64 for up
	// to a frame. Strict mode never covered them (it is a class 1 rule),
	// and class 0 is where a phantom finds CMD_HI 0: GET_HEALTH $0A with
	// bit 0 flipped, or any ARG byte of $0B written after a class 0 command
	// and before the next class 1 one rewrites CMD_HI. A strict program
	// that is committing frames never sends any of the three unkeyed, so
	// one that arrives is not its own. A new program taking over a loop
	// the last one left running is unaffected: no commit has arrived for
	// the half second it took to load.
	if ( sStrict && ( op == 0x07 || op == 0x09 || op == 0x0B ) &&
	     !gpu64_apiKeyed() && ( gpu64ApiKey & 0xF0 ) != GPU64_KEY_CHECKED &&
	     gpu64_3dCommitsLive() )
	{
		gpu64_apiDiagBump( &gpu64ApiDiag.keyRefused );
		gpu64ApiDiag.keyRefusedOp = op;
		res = GPU64_ERR_BAD_ARGS;
	}
	else
#endif
	if ( op < 0x10 )
		res = doSystem( op );
	else if ( op >= 0x50 && op < 0x60 )
		res = doText( op );
	else if ( op < 0x80 )
		res = doDraw( op );
	else if ( op < 0xA0 )
		res = doMath( op );
	else
		res = GPU64_ERR_BAD_OPCODE;

	sErr = res;
	if ( res == GPU64_ERR_OK )
	{
		sStatus &= ~GPU64_STATUS_ERROR;

		// gpu64: the log overlay is a bring-up aid for the default state.
		// Once a program owns the screen it should not have firmware text
		// painted over its output on every flip -- and PrepareFlip() was
		// walking 25 rows of glyphs per frame to do it. So the first
		// successful command of a session hides it, on the same "a program
		// engaged the API" transition that stops the mirror. LOG_ENABLE
		// itself is exempt, so a program that explicitly asks for the log
		// as its first command gets it.
		if ( !gpu64ApiActive && op != 0x07 )
		{
			CGpu64FrameBuffer *pFB = g_pGpu64FB;
			if ( pFB ) pFB->LogEnable( FALSE );
		}

		// A successful dispatch is what counts as "a program engaged the
		// API" -- deliberately not any write into the window, so an REU
		// detection routine scanning IO2 can't disarm the mirror by
		// accident the way the milestone 2 trigger could.
		// gpu64: the milestone 6 load ladder starts here rather than at
		// REU-session start (gpu64_ladder.h). The ladder's rungs are 4s
		// wall-clock windows, and the user spends an unknown -- and
		// unrepeatable -- amount of time in the RAD menu and at the BASIC
		// LOAD prompt before the test PRG runs. Anchoring the clock to the
		// first successful command puts the same rung under the same C64
		// workload on every run.
#ifdef GPU64_LADDER_ENABLED
		if ( !gpu64ApiActive )
			gpu64_ladderArm();
#endif

		gpu64ApiActive = 1;
	} else
		sStatus |= GPU64_STATUS_ERROR;

	// gpu64: class 0's re-warm, and the only one that runs inside the armed
	// window of a deferred flip.
	//
	// Classes 1 and 2 call gpu64_apiWarmPollingLoop() at the end of their own
	// dispatch (gpu64_3d_class1.cpp, gpu64_raster_class2.cpp); class 0 never
	// did, on the reasoning that its ops are small. That reasoning does not
	// hold once a flip is armed: gpu64_vsyncWarmCommit() is issued once by the
	// PAGE_FLIP that arms it, and every class 0 dispatch between that arm and
	// the vblank boundary then evicts the commit path again with nothing to
	// put it back. The commit fires cold, and a stall between
	// RESTART_CYCLE_COUNTER and the GPIO write takes or releases the bus at an
	// undefined phase -- polling-loop rule 3.
	//
	// BENCH, 2026-09-06, gpu64_probe_latch (dev/hw_testing.md): the FLI0
	// rung -- 2000 *immediate* flips, no armed window -- ran 2000/2000 clean
	// twice. The FLIW rung -- 300 deferred flips, each one polled to
	// completion so the armed window stays empty -- ran 300/300 clean twice,
	// at exactly 60/s. The FLIP rung, which is FLIW with dispatches issued
	// inside the armed window and nothing else different, derailed the C64
	// both times. Same firmware path, same rate, same DMA holds: the armed
	// window is the mechanism, which is this missing call.
	//
	// Guarded on flipPending so ordinary class 0 traffic keeps the hold length
	// it has always had -- the cost only appears in the at-most-one-frame
	// window where it is the fix. It is last in the dispatch so the loop
	// preload it ends with is the final instruction-cache touch before the bus
	// is released.
	if ( sCmdHi == 0 && gpu64Vsync.flipPending )
		gpu64_apiWarmPollingLoop();
}

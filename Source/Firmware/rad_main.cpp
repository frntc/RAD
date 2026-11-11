/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
 Copyright (c) 2022-2026 Carsten Dachsbacher <frenetic@dachsbacher.de>

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

// required for C128
#define FORCE_RESET_VECTORS

#include "rad_main.h"
#include "gpu64_c64font.h"
#include "build_id.h"
#include "dirscan.h"
#include "c64screen.h"
#include "linux/kernel.h"
#include "config.h"
#include "rad_iecdevice.h"

static const char DRIVE[] = "SD:";
static const char FILENAME_CONFIG[] = "SD:RAD/rad.cfg";

// REU
#include "rad_reu.h"
#define REU_MAX_SIZE_KB	(16384)
u8 mempool[ REU_MAX_SIZE_KB * 1024 + 8192 ] AAA = {0};
u8 *mempoolPtr = &mempool[ 0 ];
u32 mempoolBytes = sizeof( mempool );	// gpu64: the level pack sits above the REU in here
u8 prgLaunch[ 65536 + 2 ] AAA = {0};

// low-level communication code
u64 armCycleCounter;
#include "lowlevel_dma.h"

// GeoRAM
#include "rad_georam.h"
#include "gpu64_api.h"
#include "gpu64_vsync.h"
#include "gpu64_ladder.h"
#include "gpu64_level.h"

// VSF
u8 vsf[ 17 * 1024 * 1024 ] = {0};

void warmCache()
{
	for ( int i = min( REU_SIZE_KB, 256 ) * 1024 - 64; i >= 0; i -= 64 )
		CACHE_PRELOADL2KEEP( &mempool[ i ] );

	CACHE_PRELOAD_DATA_CACHE( &reu, sizeof( REUSTATE ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &reu, sizeof( REUSTATE ), sizeof( REUSTATE ) * 8 );

	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)reuUsingPolling, GPU64_POLL_PRELOAD_SIZE );
	FORCE_READ_LINEARa( (void*)reuUsingPolling, GPU64_POLL_PRELOAD_SIZE, 65536 );

	// gpu64: gpu64_mirrorSnapshot() is called from inside reuUsingPolling()'s
	// cycle-critical loop but is a separate function, so it isn't covered by
	// the preload above -- an instruction-cache miss mid-burst could blow the
	// per-byte DMA_READBYTE timing the same way an uncached reuUsingPolling()
	// itself would.
	//
	// 4KB, not the 2KB guess this used to carry (copied from
	// geoRAMUsingPolling below): the 2026-09-06 mirror rework took the
	// function from 0x694 to 0x9f0 bytes, i.e. straight past a window nobody
	// had ever measured it against, which would have left the tail of the
	// colour-RAM burst running cold. Measured with nm -S on kernel8.elf --
	// re-check it there if this function grows again.
	extern void gpu64_mirrorSnapshot( void );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_mirrorSnapshot, 1024 * 4 );
	FORCE_READ_LINEARa( (void*)gpu64_mirrorSnapshot, 1024 * 4, 65536 );

	// gpu64: milestone 4's blob transfers are in the same position -- called
	// from inside the polling loop, not covered by its preload window, and
	// per-byte cycle-critical once the burst starts.
	extern u8 gpu64_blobRead( u8, u32, u32, u8 * );
	extern u8 gpu64_blobWrite( u8, u32, u32, const u8 * );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_blobRead, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_blobRead, 1024 * 2, 65536 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_blobWrite, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_blobWrite, 1024 * 2, 65536 );

	// gpu64: the deferred page flip's commit is in the same position again
	// -- called from the polling loop, outside its preload window. The
	// PAGE_FLIP dispatch re-warms it every time it queues a flip (see
	// gpu64_vsyncWarmCommit in rad_reu.cpp), since a dispatch will have
	// evicted whatever this preloaded; this covers the first one.
	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)gpu64_vsyncCommitFlip, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)gpu64_vsyncCommitFlip, 1024 * 2, 65536 );

	// gpu64: the API register file is touched inline from the polling loop
	// on every ARG write and every STATUS/ERRCODE read (see GPU64REGS in
	// gpu64_api.h), so it wants the same treatment REUSTATE gets above.
	CACHE_PRELOAD_DATA_CACHE( &gpu64Regs, sizeof( GPU64REGS ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64Regs, sizeof( GPU64REGS ), sizeof( GPU64REGS ) * 8 );

	// gpu64: and the frame-clock state it reads every single pass.
	CACHE_PRELOAD_DATA_CACHE( &gpu64Vsync, sizeof( GPU64VSYNC ), CACHE_PRELOADL1KEEP )
	FORCE_READ_LINEAR32a( &gpu64Vsync, sizeof( GPU64VSYNC ), sizeof( GPU64VSYNC ) * 8 );
}

void warmCacheGeoRAM()
{
	for ( int k = 0; k < 1024; k += 64 )
		CACHE_PRELOADL2STRM( &mempool[ k ] );

	FORCE_READ_LINEARa( &mempool[ 0 ], 1024, 1024 * 64 );

	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)geoRAMUsingPolling, 1024 * 2 );
	FORCE_READ_LINEARa( (void*)geoRAMUsingPolling, 1024 * 2, 65536 );
}

static u16 resetVector = 0xFCE2;

// emulate GAME-cartridge to start C128 (also works on C64) with custom reset-vector => forces C128 in C64 mode
void startForcedResetVectors()
{
	register u32 g2, g3;

	const u8 romh[] = { 
		0x4c, 0x0a, 0xe5, 0x4c, 0x00, 0xe5, 0x52, 0x52,
		0x42, 0x59, 0x43, 0xfe, 0xe2, 0xfc, 0x48, 0xff };

	CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)startForcedResetVectors, 1024 * 4 );
	CACHE_PRELOADL1STRM( romh );
	FORCE_READ_LINEARa( (void*)startForcedResetVectors, 1024 * 4, 65536 );
	FORCE_READ_LINEARa( (void*)romh, 16, 1024 );

	OUT_GPIO( DMA_OUT );
	OUT_GPIO( GAME_OUT );

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( 100 );
	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bOE_Dx | bRW_OUT | bDMA_OUT | bDIR_Dx );
	INP_GPIO_RW();
	INP_GPIO_IRQ();

	CLR_GPIO( bGAME_OUT );
	CLR_GPIO( bMPLEX_SEL );

	DELAY( 1 << 20 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );

	u32 nCycles = 0, nRead = 0;
	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( 50 );
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		if ( nCycles ++ > 100000 )
		{
			OUT_GPIO( RESET_OUT );
			CLR_GPIO( bRESET_OUT );
			DELAY( 1 << 18 );
			SET_GPIO( bRESET_OUT );
			INP_GPIO( RESET_OUT );
			nRead = nCycles = 0;
		}

		if ( ROMH_ACCESS && CPU_READS_FROM_BUS )
		{
			u8 d = 0;
			if ( ADDRESS0to7 == 0xfc || ADDRESS0to7 == 0xfd ) nRead ++;
			if ( ADDRESS0to7 == 0xfc ) d = resetVector & 255;
			if ( ADDRESS0to7 == 0xfd ) d = resetVector >> 8;

			{
				register u32 DD = ( ( d ) & 255 ) << D0;
				write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
				write32( ARM_GPIO_GPSET0, DD );
				SET_BANK2_OUTPUT
				WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ );
				SET_GPIO( bOE_Dx | bDIR_Dx );
			}

			if ( nRead >= 2 )
			{
				WAIT_FOR_VIC_HALFCYCLE
				SET_GPIO( bGAME_OUT );
				break;
			}
		}
		WAIT_FOR_VIC_HALFCYCLE
		RESET_CPU_CYCLE_COUNTER
	}
}


//#define REU_PROTOCOL
#ifdef REU_PROTOCOL
REUPROT reuProtocol[ 65536 ];
u32 nReuProtocol = 0;
#endif

#include "rad_hijack.h"

volatile u8 bla = 0;

u8 reuImageIsNuvie( u8 *m )
{
	u8 pat1[ 16 ] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff };	// 0x8f00f0
	u8 pat2[ 7 ] = { 0x30, 0x30, 0x31, 0x16, 0x31, 0x2e, 0x30 }; // 0x0000f5
	if ( memcmp( pat1, &m[ 0x8f00f0 ], 16 ) == 0 || memcmp( pat2, &m[ 0x0000f5 ], 7 ) == 0 )
		return SPECIAL_NUVIE;
	return 0;
}

bool reuImageIsBlureu( u8 *m, u32 size )
{
   	u32 crc = 0xffffffff;
	for ( u32 i = 0; i < size; i++ )
	{
      	crc ^= *(m++);
      	for ( u8 j = 0; j < 8; j++ ) 
        	crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
	}
	if ( crc == 0xd569cb25 )
		return SPECIAL_BLUREU;
	return 0;
}

u32 temperature;

#define WAIT_FOR_READY_PROMPT \
	int done;												\
	do {													\
		for ( u32 i = 0; i < radWaitCycles; i++ ) 					\
			emuWAIT_FOR_VIC_HALFCYCLE						\
		done = checkForReadyPrompt( !go64mode );			\
	} while ( !done );


// gpu64: the default-state splash -- the "Hondani" wordmark in greys, centred
// on gpu64's own 320x200x8 framebuffer.
//
// This replaces the milestone 1 checkerboard, which existed only to prove the
// Pi could drive HDMI at all. That question has been answered on hardware for
// three milestones now, so the boot screen may as well say whose machine this
// is. Everything here is palette indices, and it deliberately uses only the
// greys already in the reset C64 palette (see gpu64_fb.cpp) -- black, dark
// grey, grey, light grey, white -- so it needs no palette of its own and
// cannot disturb a program's.
//
// The glyphs are the C64's own character ROM (gpu64_c64font.h) scaled up by
// GPU64_LOGO_SCALE, outlined by smearing the same mask one scaled pixel in
// all eight directions first. That is a lot cheaper than carrying a second,
// larger font, and at 4x the Commodore face reads as deliberately blocky
// rather than as a stretched terminal font -- and it is the same face the
// machine on the other end of the cartridge port draws with, which is the
// point.
#define GPU64_LOGO_TEXT		"HONDANI"
#define GPU64_LOGO_SCALE	4
// One character cell of the ROM font.
#define GPU64_LOGO_GLYPH_H	8

// Sat at the vertical centre until 2026-08-23; now anchored to the bottom of
// the frame, this far above it. The wordmark plus its rule is treated as one
// block, so the gap below the rule is what this measures -- moving the
// baseline does not need the glyph maths re-derived.
#define GPU64_LOGO_BOTTOM	16

static void gpu64DrawLogoGlyph( u8 *p, unsigned nPitch, int x0, int y0, char c, u8 nInk, boolean bGradient )
{
	// Charset 0 -- uppercase/graphics, the set a C64 boots into, and the one
	// whose capitals are the wordmark's.
	const u8 *pGlyph =
		gpu64C64Font[ GPU64_CHARSET_UPPER ][ gpu64_c64ScreenCode( c, GPU64_CHARSET_UPPER ) ];

	for ( unsigned gy = 0; gy < GPU64_LOGO_GLYPH_H; gy++ )
	{
		u8 bits = pGlyph[ gy ];
		if ( bits == 0 )
			continue;

		// Top of the letter catches the light, the bottom falls away --
		// three flat bands rather than a real ramp, because the reset
		// palette only has three greys plus white to work with.
		u8 ink = nInk;
		if ( bGradient )
			ink = ( gy < 3 ) ? 1 : ( gy < 6 ? 15 : 12 );

		for ( unsigned gx = 0; gx < 8; gx++ )
		{
			if ( !( bits & ( 0x80 >> gx ) ) )
				continue;

			int px = x0 + (int)gx * GPU64_LOGO_SCALE;
			int py = y0 + (int)gy * GPU64_LOGO_SCALE;

			for ( int sy = 0; sy < GPU64_LOGO_SCALE; sy++ )
			{
				int y = py + sy;
				if ( y < 0 || y >= (int)GPU64_FB_HEIGHT )
					continue;
				for ( int sx = 0; sx < GPU64_LOGO_SCALE; sx++ )
				{
					int x = px + sx;
					if ( x < 0 || x >= (int)GPU64_FB_WIDTH )
						continue;
					p[ (size_t)y * nPitch + x ] = ink;
				}
			}
		}
	}
}

void CRAD::showTestPattern( void )
{
	// gpu64: called both once at boot and again on every C64-side trigger,
	// so the face colour alternates -- that is what makes a real trigger
	// visibly distinguishable from the leftover boot-time draw.
	static unsigned callCount = 0;
	callCount++;
	boolean bGradient = ( callCount & 1 ) != 0;

	u8 *p = m_Gpu64FB.PageBuffer( m_Gpu64FB.GetDrawPage() );
	if ( p == 0 )
		return;
	unsigned nPitch = m_Gpu64FB.GetPitch();

	for ( unsigned y = 0; y < GPU64_FB_HEIGHT; y++ )
		memset( p + (size_t)y * nPitch, 0, GPU64_FB_WIDTH );

	const char *pText = GPU64_LOGO_TEXT;
	unsigned nChars = strlen( pText );
	int nGlyphW = 8 * GPU64_LOGO_SCALE;
	int x0 = ( (int)GPU64_FB_WIDTH  - (int)nChars * nGlyphW ) / 2;

	// Block height is the glyphs, the gap to the rule, and the rule's two
	// lines -- the same terms the rule's own y is built from below, so the
	// two stay in step.
	int nBlockH = GPU64_LOGO_GLYPH_H * GPU64_LOGO_SCALE + GPU64_LOGO_SCALE * 2 + 2;
	int y0 = (int)GPU64_FB_HEIGHT - nBlockH - GPU64_LOGO_BOTTOM;

	// Outline first, face second: eight offset copies in dark grey, then the
	// glyph itself on top of them.
	static const int dx[ 8 ] = { -1, 0, 1, -1, 1, -1, 0, 1 };
	static const int dy[ 8 ] = { -1, -1, -1, 0, 0, 1, 1, 1 };

	for ( unsigned pass = 0; pass < 2; pass++ )
		for ( unsigned i = 0; i < nChars; i++ )
		{
			int cx = x0 + (int)i * nGlyphW;

			if ( pass == 0 )
			{
				for ( unsigned d = 0; d < 8; d++ )
					gpu64DrawLogoGlyph( p, nPitch,
						cx + dx[ d ] * GPU64_LOGO_SCALE,
						y0 + dy[ d ] * GPU64_LOGO_SCALE,
						pText[ i ], 11, FALSE );
			} else
				gpu64DrawLogoGlyph( p, nPitch, cx, y0, pText[ i ], 15, bGradient );
		}

	// A grey rule under the wordmark, the width of the wordmark itself.
	int ruleY = y0 + GPU64_LOGO_GLYPH_H * GPU64_LOGO_SCALE + GPU64_LOGO_SCALE * 2;
	if ( ruleY >= 0 && ruleY + 1 < (int)GPU64_FB_HEIGHT )
		for ( int x = x0; x < x0 + (int)nChars * nGlyphW; x++ )
			if ( x >= 0 && x < (int)GPU64_FB_WIDTH )
			{
				p[ (size_t)ruleY * nPitch + x ] = 12;
				p[ (size_t)( ruleY + 1 ) * nPitch + x ] = 11;
			}

	m_Gpu64FB.CleanPage( m_Gpu64FB.GetDrawPage() );
}

// gpu64: only one CRAD is ever constructed (see main() below); g_pRAD is set
// from its constructor (rad_main.h). This wrapper lets rad_reu.cpp's bus-hijack
// loop trigger the pattern from the C64 side without pulling in rad_main.h's
// full Circle/screen include stack -- see the forward declaration in
// rad_reu.cpp.
CRAD *g_pRAD = nullptr;

void gpu64_showTestPattern( CRAD *pRAD )
{
	if ( pRAD )
		pRAD->showTestPattern();
}

// gpu64: extern rather than #include "font.h" -- that header is a raw
// generated .bin-to-array dump with no include guard and no `extern`,
// designed for exactly one translation unit (rad_hijack.cpp already includes
// it) to own the storage; including it a second time here would duplicate
// the 4KB array and fail to link.
//
// The old note here said font_bin's menu-phase mutation was "long done and
// stable to read from" by the time the mirror runs. Stable, yes -- but the
// mutation is not undone: the menu's oscilloscope permanently overwrites
// screen codes $40-$DF, cursor glyph $A0 included. showMirror() reads the
// character ROM instead now; only showMenuMirror() still wants font_bin,
// and it wants the scribbles, because they are on the C64's screen too.
extern unsigned char font_bin[ 4096 ];

// gpu64: milestone 3 screen mirror, rendering into the 8bpp framebuffer.
//
// This got simpler with milestone 4's display change rather than harder: the
// C64's 40x25 text at 8x8 pixels per glyph is exactly 320x200, so the mirror
// is now a 1:1 fit with no upscaling at all (it used to draw at 2x into a
// COLOR16 framebuffer), and colour RAM's 4-bit values index palette entries
// 0-15 directly, since the reset palette *is* the C64 palette.
void CRAD::showMirror( const u8 *screen, const u8 *color, u8 border, u8 background, u8 d018 )
{
	u8 *p = m_Gpu64FB.PageBuffer( m_Gpu64FB.GetDrawPage() );
	if ( p == 0 )
		return;
	unsigned nPitch = m_Gpu64FB.GetPitch();

	// gpu64: the C64's own character ROM, not font_bin. font_bin is RAD's
	// lookalike face -- six scanlines tall where the ROM is seven, so every
	// character came out a pixel low with a gap above it -- and
	// rad_hijack.cpp's menu oscilloscope has already scribbled through screen
	// codes $40-$DF by the time the mirror runs. Screen code $A0 is in that
	// range, and $A0 is the reverse space a blinking cursor lands on: the
	// cursor showed as one stray row instead of a filled 8x8 cell. The ROM's
	// $80-$FF half is genuine reverse video, so the cursor needs no special
	// case.
	//
	// Which half: $D018 bits 3-1 are the VIC's character base within its
	// 16KB bank, in 2KB steps. Bank 0 is assumed throughout the mirror (see
	// the scoping note above gpu64_mirrorSnapshot() in rad_reu.cpp), and
	// there base 2 is the ROM's uppercase/graphics half at $1000 and base 3
	// its lowercase/uppercase half at $1800 -- exactly what PRINT CHR$(14)
	// and CHR$(142) switch between. Any other base is a charset the program
	// built in RAM, which the mirror has no copy of; fall back to uppercase
	// rather than draw 1000 cells of garbage.
	const u8 ( *font )[ 8 ] =
		gpu64C64Font[ ( ( ( d018 >> 1 ) & 7 ) == 3 ) ? GPU64_CHARSET_LOWER
													 : GPU64_CHARSET_UPPER ];

	for ( unsigned cy = 0; cy < 25; cy++ )
	{
		for ( unsigned cx = 0; cx < 40; cx++ )
		{
			unsigned cellIdx = cy * 40 + cx;
			const u8 *glyph = font[ screen[ cellIdx ] ];
			u8 fg = color[ cellIdx ] & 15;

			for ( unsigned gy = 0; gy < 8; gy++ )
			{
				u8 rowBits = glyph[ gy ];
				u8 *pDst = p + (size_t)( cy * 8 + gy ) * nPitch + cx * 8;
				for ( unsigned gx = 0; gx < 8; gx++ )
					pDst[ gx ] = ( rowBits & ( 0x80 >> gx ) ) ? fg : background;
			}
		}
	}

	// $D020 now has somewhere to go: the framebuffer carries a real border
	// around the 320x200 surface (see GPU64_BORDER_W in gpu64_fb.h). Only
	// repaint it when it actually changed -- SetBorder() touches both pages
	// and cleans them, which is far more work than the snapshot itself.
	if ( border != m_Gpu64FB.GetBorder() )
		m_Gpu64FB.SetBorder( border );

	m_Gpu64FB.CleanPage( m_Gpu64FB.GetDrawPage() );
}

// gpu64: same free-function indirection as gpu64_showTestPattern() above --
// lets rad_reu.cpp's gpu64_mirrorSnapshot() reach CRAD::showMirror() without
// pulling in the full Circle/screen include stack.
void gpu64_showMirror( CRAD *pRAD, const u8 *screen, const u8 *color, u8 border, u8 background, u8 d018 )
{
	if ( pRAD )
		pRAD->showMirror( screen, color, border, background, d018 );
}

// gpu64: and the same indirection for the dead-clock holding screen, which
// rad_reu.cpp paints when the C64 stops clocking the bus. Goes through
// g_pGpu64FB rather than CRAD because the framebuffer is the only thing it
// needs -- see gpu64_deadClockEnter().
void gpu64_showHoldingScreen( void )
{
	if ( g_pGpu64FB )
		g_pGpu64FB->ShowHoldingScreen();
}

// gpu64: the menu's charset for rows 0-3. Same extern-not-#include reasoning
// as font_bin above: rad_hijack.cpp owns the storage.
//
// Note the menu mirror is the one text surface that must *keep* font_bin,
// where showMirror() above no longer may. The menu is RAD's own screen, drawn
// with a charset RAD uploads into the C64's character RAM -- oscilloscope
// scribbles and all -- so font_bin is exactly what the C64 is displaying.
extern u8 font_logo[ 4096 ];

// gpu64: mirror of the RAD menu itself (bench request, 2026-09-06: "Can you
// mirror the REU menu also?" / "It does not need to mirror the REU menu
// completely. Just screen and color is sufficient.").
//
// The menu composes its display out of three charsets switched by $D018
// raster splits -- rasterCommandsPAL in rad_hijack.cpp puts font_logo in at
// raster 33, font_bin's lowercase half at 83, and its uppercase half at 219,
// which against a first text raster of 51 is rows 0-3, 4-20 and 21-24. A
// single-charset render would show the file browser in graphics characters,
// so the row bands are worth the four lines they cost here. Everything else
// the menu does with rasters -- the border/background colour splits, the
// sprites -- is deliberately not reproduced: "screen and color is
// sufficient".
void CRAD::showMenuMirror( const u8 *screen, const u8 *color )
{
	u8 *p = m_Gpu64FB.PageBuffer( m_Gpu64FB.GetDrawPage() );
	if ( p == 0 )
		return;
	unsigned nPitch = m_Gpu64FB.GetPitch();

	for ( unsigned cy = 0; cy < 25; cy++ )
	{
		const u8 *charset;
		if ( cy < 4 )
			charset = font_logo;			// PAGE2_UPPERCASE, raster 33
		else if ( cy < 21 )
			charset = font_bin + 2048;		// PAGE1_LOWERCASE, raster 83
		else
			charset = font_bin;				// PAGE1_UPPERCASE, raster 219

		for ( unsigned cx = 0; cx < 40; cx++ )
		{
			unsigned cellIdx = cy * 40 + cx;
			const u8 *glyph = &charset[ (unsigned)screen[ cellIdx ] * 8 ];
			u8 fg = color[ cellIdx ] & 15;

			for ( unsigned gy = 0; gy < 8; gy++ )
			{
				u8 rowBits = glyph[ gy ];
				u8 *pDst = p + (size_t)( cy * 8 + gy ) * nPitch + cx * 8;
				for ( unsigned gx = 0; gx < 8; gx++ )
					pDst[ gx ] = ( rowBits & ( 0x80 >> gx ) ) ? fg : 0;
			}
		}
	}

	if ( m_Gpu64FB.GetBorder() != 0 )
		m_Gpu64FB.SetBorder( 0 );

	m_Gpu64FB.CleanPage( m_Gpu64FB.GetDrawPage() );
}

// gpu64: free-function handle for rad_hijack.cpp, same indirection as above.
void gpu64_showMenuMirror( const u8 *screen, const u8 *color )
{
	if ( g_pRAD )
		g_pRAD->showMenuMirror( screen, color );
}

void CRAD::Run( void )
{
	// gpu64: first thing logged, every boot. The card's config.txt names the
	// image the Pi loads (kernel_rad.img), which for a long time was NOT the
	// name build.sh deployed to -- so the hardware silently ran stale
	// firmware while three rounds of newly added diagnostics "produced no
	// output". Stamping the build into the log makes that failure mode
	// self-evident: if this line doesn't match the build you just deployed,
	// nothing below it is telling you anything about your current source.
	// GPU64_BUILD_ID is regenerated by tools/build.sh on every run
	// (Source/Firmware/build_id.h, git-ignored).
	logger->Write( "gpu64", LogNotice, "Run: bc0 build " GPU64_BUILD_ID );

	showTestPattern();

	m_EMMC.Initialize();

	EnableIRQs();
	initSerialOverUSB_IECDevice( &m_Interrupt, &m_Timer, &m_DeviceNameService, false );

	gpioInit();

	setDefaultTimings( AUTO_TIMING_RPI3PLUS_C64C128 );
	readConfig( logger, DRIVE, FILENAME_CONFIG );

	OUT_GPIO( RESET_OUT );
	CLR_GPIO( bRESET_OUT );
	DELAY( 1 << 25 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );


	DisableIRQs();

	register u32 g2;

	// this also initializes timing values
	REU_SIZE_KB = 128;
	initREU( mempool );

	initHijack();

#ifdef REU_PROTOCOL
	nReuProtocol = 0;
#endif

	#ifdef FORCE_RESET_VECTORS
	resetVector = 0xfce2;
	#endif

	u32 prgSize = 0;
	u8 isC128PRG = 0;
	u8 go64mode = 0;
	int res = 0;

	DisableIRQs();

	checkIfMachineRunning();		
	DELAY( 1 << 27 );

	// gpu64: a machine powered on after the Pi booted missed the reset pulse
	// above -- it went out while the machine was off. On the C64 Ultimate
	// that leaves both screens black until the user presses the cartridge's
	// reset button (bench 2026-09-29), and that press is known to work. So
	// do the same: give the machine ~2s to finish its own power-up (the
	// clock appears before the C64U's core is settled), check the clock is
	// still there, and pulse reset exactly as the boot pulse does. Never
	// taken on a breadbin, whose clock is up before the Pi is.
	{
		extern u8 gpu64MachineCameLate;
		if ( gpu64MachineCameLate )
		{
			const u64 settle = 2800000000ULL;
			WAIT_CYCLES( settle );
			checkIfMachineRunning();

			OUT_GPIO( RESET_OUT );
			CLR_GPIO( bRESET_OUT );
			DELAY( 1 << 25 );
			SET_GPIO( bRESET_OUT );
			INP_GPIO( RESET_OUT );

			checkIfMachineRunning();
			DELAY( 1 << 27 );
		}
	}

	while ( 1 )
	{
		prgSize = 0;
		isC128PRG = 0;
		go64mode = 0;
		res = 0;

		extern u32 radStartup, radStartupSize, radSilentMode, radWaitCycles;
		if ( radStartup == 1 )
		{
			meSize0 = radStartupSize;
			radLoadREUImage = radLaunchPRG = false;
			#ifdef STATUS_MESSAGES
			setStatusMessage( &statusMsg[ 0 ], " " );
			setStatusMessage( &statusMsg[ 80 ], " " );
			#endif
			goto startREUEmulation;
		} else
		if ( radStartup == 2 )
		{
			meSize1 = radStartupSize;
			radLoadGeoImage = radLaunchPRG = radLaunchGEORAM = false;
			#ifdef STATUS_MESSAGES
			setStatusMessage( &statusMsg[ 0 ], " " );
			setStatusMessage( &statusMsg[ 80 ], " " );
			#endif
			goto startGeoRAMEmulation;
		} else
		if ( radStartup == 3 )
		{
			goto hijacking;
		}


	radIsWaiting:
		CLR_GPIO( bMPLEX_SEL );

		// gpu64: this used to be a bare 1250-cycle button poll, which meant
		// HDMI stayed dark from power-on until the user had blind-navigated
		// the RAD menu into REU mode -- reported from the bench 2026-09-06.
		// gpu64_mirrorIdleLoop() (rad_reu.cpp) is the same poll plus the
		// $FFFF vector-fetch mirror gate, and returns on the same button
		// press this loop was watching for. Everything it needs -- the GPIO
		// setup from gpioInit(), the bus timings from initREU(), and a C64
		// clock proven live by checkIfMachineRunning() -- is already in place
		// by the time control reaches here.
		{
			extern void gpu64_mirrorIdleLoop( void );
			gpu64_mirrorIdleLoop();
			goto hijacking;
		}


	hijacking:
		// gpu64: m_CPUThrottle removed (see rad_main.h) -- its mailbox calls hang
		// on this board. temperature is only used for the on-screen menu display.
		temperature = 0;

		SyncDataAndInstructionCache();
		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)hijackC64, 1024 * 10 );
		FORCE_READ_LINEARa( (void*)hijackC64, 1024 * 10, 65536 );

		extern u8 justBooted;
		justBooted = 1;

		///////////////////////////////////////////////////////////////////////
		//
		// goto menu
		//
		///////////////////////////////////////////////////////////////////////

		// gpu64: the menu mirror paints C64 palette indices 0-15 into a
		// graphics page. Whatever ran before the button press may have left
		// the display in 80x50 text mode, a 256-colour level palette or a
		// flipped page -- and then the menu reached HDMI in the wrong colours
		// (bench 2026-09-29). The C64 program is being thrown away anyway:
		// the menu always ends in a C64 reset.
		{
			extern void gpu64_apiFullReset( boolean bReprogram );
			gpu64_apiFullReset( TRUE );
		}

		res = hijackC64( false );			// after hijackC64 the CPU is still halted by DMA

		WAIT_FOR_CPU_HALFCYCLE
		WAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bGAME_OUT | bOE_Dx | bRW_OUT | bDMA_OUT );
		INP_GPIO( RW_OUT );
		INP_GPIO( IRQ_OUT );
		OUT_GPIO( RESET_OUT );
		CLR_GPIO( bRESET_OUT );

		// do we want to boot a C128 in C64-mode?
		go64mode = 0;

		// load PRG (if any) to figure out whether we want to force a C128 into C64-mode
		prgSize = 0;
		if ( radLaunchPRG )
		{
			readFile( logger, (char*)DRIVE, (char*)radLaunchPRGFile, prgLaunch, &prgSize );
			isC128PRG = *(u16*)prgLaunch == 0x1c01 ? 1 : 0;

			if ( isC128 )
			{
				if ( !isC128PRG && radLaunchPRG_NORUN_128 && !( *(u16 *)prgLaunch == 0x0801 ) )
					go64mode = 0; else
				{
					if ( ( !isC128PRG && radLaunchPRG ) || radLaunchGEORAM )
						go64mode = 1;
				}
			}

			// if we're not launching automatically: be nice and print load address
			if ( !isC128PRG && !( *(u16 *)prgLaunch == 0x0801 ) )
			{
				char *strAddr = strstr( &statusMsg[ 120 ], "$0000)" );
				char tmp[ 40 ];
				sprintf( tmp, "$%04X", *(u16 *)prgLaunch );
				for ( int i = 0; i < 5; i++ )
					strAddr[ i ] = tmp[ i ];
				memcpy( &statusMsg[ 80 ], &statusMsg[ 120 ], 40 );
			}
		}

		if ( radLaunchGEORAM || radLaunchVSF )
			go64mode = 1;

		//
		// let the machine start booting (meanwhile we'll load or initialize the REU/GeoRAM image and emulation)
		//
		CLR_GPIO( bMPLEX_SEL );
	#ifdef FORCE_RESET_VECTORS
		if ( go64mode )
		{
			startForcedResetVectors();
		} else
	#endif
		{
			DELAY( 1 << 18 );
			SET_GPIO( bRESET_OUT );
			INP_GPIO( RESET_OUT );
		}

		if ( res == RUN_REBOOT )
		{
			reboot(); 
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// REU emulation
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 1 )
		{
		startREUEmulation:
			REU_SIZE_KB = 128 << meSize0;
			initREU(mempool);
			resetREU();

			if ( radLoadREUImage )
			{
				u32 size;
				readFile( logger, (char*)DRIVE, (char*)radImageSelectedFile, mempool, &size );

				reu.isSpecial = reuImageIsNuvie( mempool );
				if ( !reu.isSpecial )
					reu.isSpecial = reuImageIsBlureu( mempool, size );
		} else
			{
				memset( mempool, 0, reu.reuSize );
				#ifdef STATUS_MESSAGES
				char tmp[ 40 ];
				sprintf( tmp, "%dK REU", reu.reuSize / 1024 );
				setStatusMessage( &statusMsg[ 0 ], tmp );
				#endif
			}

			reu.isModified = 0;

			// gpu64 (milestone 18): the one moment the SD card can be
			// touched. Everything after this runs inside
			// reuUsingPolling(), where an EMMC transfer's MMIO traffic
			// would wreck core 0's per-C64-cycle bus timing -- so the
			// level file is read into RAM here and LOAD_LEVEL only ever
			// parses memory. Deliberately ahead of the i-cache preloads
			// below, which it would otherwise evict.
			gpu64_levelPreload();

			if ( radLaunchPRG )
			{
				// wait for "READY." to appear on screen
				WAIT_FOR_READY_PROMPT
				injectAndStartPRG( prgLaunch, prgSize, true );
			} else
			if ( radSilentMode != 0xffffffff )
			{
				// wait for "READY." to appear on screen
				WAIT_FOR_READY_PROMPT
				injectMessage( false );
			}

			SyncDataAndInstructionCache();
			warmCache();

			for ( u32 i = 0; i < 1000; i++ )
				emuWAIT_FOR_VIC_HALFCYCLE

			CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)reuUsingPolling, GPU64_POLL_PRELOAD_SIZE );
			FORCE_READ_LINEARa( (void*)reuUsingPolling, GPU64_POLL_PRELOAD_SIZE, 65536 );

			resetREU();

			// gpu64: the multicore feasibility spike's liveness check
			// (dev/milestone6_3d_design.md) -- samples cores 1-3's pass
			// counters across a known interval to prove they are alive and
			// streaming. Compiled out with the spike itself: with
			// GPU64_MULTICORE_STRESS_ENABLED off it can only ever print
			// zeroes, and its DELAY plus the logger's own framebuffer work
			// would run between the reuUsingPolling() preload above and the
			// call below -- evicting exactly the instruction cache that
			// preload exists to fill.
#ifdef GPU64_MULTICORE_STRESS_ENABLED
			u32 before[ 4 ] = {
				CGpu64MultiCore::s_PassCounter[ 0 ], CGpu64MultiCore::s_PassCounter[ 1 ],
				CGpu64MultiCore::s_PassCounter[ 2 ], CGpu64MultiCore::s_PassCounter[ 3 ] };
			DELAY( 1 << 24 );
			logger->Write( "gpu64", LogNotice,
				"Run: multicore stress-loop pass delta core1=%u core2=%u core3=%u (0 means that core never ran)",
				(unsigned)( CGpu64MultiCore::s_PassCounter[ 1 ] - before[ 1 ] ),
				(unsigned)( CGpu64MultiCore::s_PassCounter[ 2 ] - before[ 2 ] ),
				(unsigned)( CGpu64MultiCore::s_PassCounter[ 3 ] - before[ 3 ] ) );
#endif

			reuUsingPolling();
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// GeoRAM emulation
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 2 )
		{
		startGeoRAMEmulation:
			// GeoRAM
			geoSizeKB = 512 << meSize1;

			geoRAM_Init();

			if ( radLoadGeoImage )
			{
				u32 size;
				static const char DRIVE[] = "SD:";
				readFile( logger, (char*)DRIVE, (char*)radImageSelectedFile, geo.RAM, &size );
			} else
			{
				#ifdef STATUS_MESSAGES
				char tmp[ 40 ];
				sprintf( tmp, "%dK GEORAM", geoSizeKB );
				setStatusMessage( &statusMsg[ 0 ], tmp );
				#endif
			}

			if ( radLaunchPRG )
			{
				// wait for "READY." to appear on screen
				WAIT_FOR_READY_PROMPT
				injectAndStartPRG( prgLaunch, prgSize, true ); 
			} else
			{
				if ( radLaunchGEORAM )
				{
					// wait for "READY." to appear on screen
					WAIT_FOR_READY_PROMPT
					injectKeyInput( "SYS56832", true ); 
				} else
				if ( radSilentMode != 0xffffffff )
				{
					// wait for "READY." to appear on screen
					WAIT_FOR_READY_PROMPT
					injectMessage( true );
				}
			}

			geo.isModified = 0;

			SyncDataAndInstructionCache();
			warmCacheGeoRAM();

			// DMA remained low after inject code above 
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			RESTART_CYCLE_COUNTER
			WAIT_UP_TO_CYCLE( 100 );
			OUT_GPIO( DMA_OUT );
			SET_GPIO( bDMA_OUT );

			geoRAMUsingPolling();

			// GUI checks reu.x
			reu.isModified = geo.isModified;
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// no memory expansion (but possibly PRG start)
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 3 ) // no memory expansion
		{
			extern int radSpecialBasicCommand;
			if ( radSpecialBasicCommand )
			{
				WAIT_FOR_READY_PROMPT

				setStatusMessage( &statusMsg[ 0 ], "SIDKICK CONFIGURATION" );
				setStatusMessage( &statusMsg[ 40 ], " " );
				setStatusMessage( &statusMsg[ 80 ], " " );
				injectMessage( false );

				injectKeyInput( "SYS54301", false ); 
				radSpecialBasicCommand = 0;
			} else
			{
				if ( radLaunchPRG )
				{
					WAIT_FOR_READY_PROMPT
					injectAndStartPRG( prgLaunch, prgSize, false ); 
				} else
				if ( radSilentMode != 0xffffffff )
				{
					WAIT_FOR_READY_PROMPT
					#ifdef STATUS_MESSAGES
					setStatusMessage( &statusMsg[ 0 ], "RAD DISABLED" );
					setStatusMessage( &statusMsg[ 80 ], " " );
					injectMessage( false );
					#endif
				}
			}
			goto radIsWaiting;
		} else
		///////////////////////////////////////////////////////////////////////
		//
		// VSF loading (and possibly REU emulation)
		//
		///////////////////////////////////////////////////////////////////////
		if ( res == RUN_MEMEXP + 4 ) 
		{
			// load VSF
			u32 vsfSize;
			readFile( logger, (char*)DRIVE, (char*)radImageSelectedFile, vsf, &vsfSize );
			reu.isSpecial = false;

			u8 *vsfREU = getVSFModule( vsf, vsfSize, (char *)"REU1764" );
			
			REU_SIZE_KB = 0;
			if ( vsfREU )
			{
				REU_SIZE_KB = (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 0 ] + ( (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 1 ] << 8 ) + ( (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 2 ] << 16 ) + ( (int)vsfREU[ VSF_SIZE_MODULE_HEADER + 3 ] << 24 );
				initREU(mempool);

				// transfer register content
		        u8 *reuRegisterData = &vsfREU[ VSF_SIZE_MODULE_HEADER + 4 ];
				reu.status = reuRegisterData[ 0 ];
				reu.command = reuRegisterData[ 1 ];
				reu.shadow_addrC64 = reu.addrC64 = (u16)reuRegisterData[ 2 ] + ( (u16)reuRegisterData[ 3 ] << 8 );
				reu.shadow_addrREU = reu.addrREU = (u16)reuRegisterData[ 4 ] + ( (u16)reuRegisterData[ 5 ] << 8 );
				reu.shadow_bank = reu.bank = reuRegisterData[ 6 ];
				reu.shadow_length = reu.length = (u16)reuRegisterData[ 7 ] + ( (u16)reuRegisterData[ 8 ] << 8 );
				reu.IRQmask = reuRegisterData[ 9 ];
				reu.addrREUCtrl = reuRegisterData[ 10 ];

				// copy REU data
		        u8 *reuMem = &vsfREU[ VSF_SIZE_MODULE_HEADER + 20 ];
				memcpy( mempool, reuMem, REU_SIZE_KB * 1024 );
			}
			reu.isModified = 0;

			resetAndInjectVSF( vsf, vsfSize );

			goto radIsWaiting;
		} 

		OUT_GPIO( GAME_OUT );
		OUT_GPIO( DMA_OUT );
		OUT_GPIO_IRQ();
		SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bGAME_OUT | bOE_Dx | bRW_OUT | bDMA_OUT | bDIR_Dx );
		INP_GPIO_RW();
		INP_GPIO_IRQ();

		goto hijacking;
	}
}

__attribute__((optimize("align-functions=256"))) void CRAD::FIQHandler( void *pParam )
{
}

int main( void )
{
	CRAD kernel;
	if ( kernel.Initialize() )
		kernel.Run();

	halt();
	return EXIT_HALT;
}


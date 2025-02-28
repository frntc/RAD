/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
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
#include "rad_hijack.h"
#include "rad_reu.h"
#include "dirscan.h"
#include "linux/kernel.h"
#include <circle/machineinfo.h>

//#define DEBUG_REBOOT_RPI_ON_R

#define PLAY_MUSIC

static u64 armCycleCounter;

u8 isC128 = 0;
u8 isC64 = 0;	// only set once we're sure

// isNTSC == 0 => PAL: 312 rasterlines, 63 cycles
// isNTSC == 1 => NTSC: 262 (0..261) rasterlines, 64 cycles, 6567R56A
// isNTSC == 2 => NTSC: 263 (0..262) rasterlines, 65 cycles, 6567R8
u8 isNTSC = 0;
u8 isRPiZero2 = 0;

u8 justBooted = 0;
char SIDKickVersion[ 32 ] = {0};
u8 supportDAC = 0;
 u32 hasSIDKick;

extern void *pFIQ;
extern void warmCache( void *fiqh );

#include "lowlevel_dma.h"

static u8 refreshGraphic = 0;

static u8 SIDType;
#ifdef PLAY_MUSIC
static u32 nWAVSamples = 0;
static void convertWAV2RAW_inplace( u8 *_data );
static u32 wavPosition = 0;
static u8 *wavMemory;
#endif

#include "mahoney_lut.h"
#include "font.h"

static u32 g2, g3;

#define WAIT_FOR_RASTERLINE( r )					\
	do {											\
		emuReadByteREU( y, 0xd012, false, {} );		\
		emuReadByteREU( x, 0xd011, false, {} );		\
		t = ( x & 128 ) << 1; t |= y;				\
	} while ( t != r );


#define POKE( a, v ) { DMA_WRITEBYTE_P1( a, v ); DMA_WRITEBYTE_P2( false ); }
#define POKE_NG( a, v ) { DMA_WRITEBYTE_P1( a, v ); DMA_WRITEBYTE_P2_NG( false ); }
#define PEEK( a, v ) { DMA_READBYTE_P1( a ); DMA_READBYTE_P2(); DMA_READBYTE_P3( v, false ); }
#define PEEK_NG( a, v ) { DMA_READBYTE_P1( a ); DMA_READBYTE_P2(); DMA_READBYTE_P3_NG( v, false ); }

#define BUSAVAIL( g ) 		( (g) & bBA )


void SPOKE_NG( u16 a, u8 v )
{
	u32 g2;

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	POKE_NG( a, v );

	return;
}

void SPOKE( u16 a, u8 v )
{
	u32 g2;

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	POKE( a, v );
}

void SPEEK( u16 a, u8 &v )
{
	WAIT_FOR_CPU_HALFCYCLE

wait4bus_SPEEK:
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	PEEK( a, v )
}

void NOP( u32 nCycles )
{
	for ( u32 i = 0; i < nCycles; i++ )
	{
		WAIT_FOR_CPU_HALFCYCLE
		WAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
	}
}

#define BUS_RESYNC {			\
	WAIT_FOR_CPU_HALFCYCLE		\
	WAIT_FOR_VIC_HALFCYCLE		\
	RESTART_CYCLE_COUNTER }

u8 detectSID()
{
	u8 y;
	BUS_RESYNC
	POKE( 0xd412, 0xff );
	POKE( 0xd40e, 0xff );
	POKE( 0xd40f, 0xff );
	POKE( 0xd412, 0x20 );
	NOP( 3 );
	PEEK( 0xd41b, y );
	// (y==2) -> 8580 // (y==3) -> 6581
	if ( y == 2 ) return 8580 & 255;
	if ( y == 3 ) return 6581 & 255;
	return 0;
}

#define POKE_GAME POKE
#define PEEK_GAME PEEK

#define MEMCPY( dst, src, length ) {				\
	for ( int i = 0; i < (int)length; i++ )			\
		POKE( dst + i, ((u8*)(src))[ i ] ); }				

#define SMEMCPY( dst, src, length ) {				\
	for ( int i = 0; i < (int)length; i++ )			\
		SPOKE( dst + i, ((u8*)(src))[ i ] ); }				


u8 PETSCII2ScreenCode( u8 c )
{
	if ( c < 32 ) return c + 128;
	if ( c < 64 ) return c;
	if ( c < 96 ) return c - 64;
	if ( c < 128 ) return c - 32;
	if ( c < 160 ) return c + 64;
	if ( c < 192 ) return c - 64;
	return c - 128;
}

extern int convChar( char c, u32 convert );

#define SCREEN1				0x6400
#define CHARSET				0x6800
#define CHARSET2			0x7800
#define PAGE1_LOWERCASE		(((SCREEN1 >> 6) & 0xF0) | (((CHARSET+0x800) >> 10) & 0x0E))
#define PAGE1_UPPERCASE		(((SCREEN1 >> 6) & 0xF0) | (((CHARSET) >> 10) & 0x0E))
#define PAGE2_LOWERCASE		(((SCREEN1 >> 6) & 0xF0) | (((CHARSET2+0x800) >> 10) & 0x0E))
#define PAGE2_UPPERCASE		(((SCREEN1 >> 6) & 0xF0) | (((CHARSET2) >> 10) & 0x0E))

u8 c64ScreenRAM[ 1024 * 4 ];
u8 c64ColorRAM[ 1024 * 4 ];

// create color look up tables for the menu
const u8 fadeTab[ 16 ] = { 0, 15, 9, 14, 2, 11, 0, 10, 9, 0, 2, 0, 11, 5, 6, 12 };
u8 fadeTabStep[ 16 ][ 6 ];

u32 frameCount = 0;

const u8 colorCycle[ 16 ][ 17 ] =
{
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 12, 1, 13, 5, 11, 5, 13, 1, 3, 14, 6, 14, 3, 0, 0, 0, 0 },
	{ 12, 1, 7, 15, 10, 8, 2, 9, 2, 8, 10, 15, 7, 0, 0, 0, 0 },
	{ 16, 12, 12, 15, 1, 15, 12, 12, 15, 15, 15, 15, 15, 15, 15, 15, 15 },
	{ 16, 15, 15, 12, 12, 15, 1, 15, 12, 12, 15, 15, 15, 15, 15, 15, 15 },
	{ 16, 15, 15, 15, 15, 12, 12, 15, 1, 15, 12, 12, 15, 15, 15, 15, 15 },
	{ 16, 15, 15, 15, 15, 15, 15, 12, 12, 15, 1, 15, 12, 12, 15, 15, 15 },
	{ 16, 15, 15, 15, 15, 15, 15, 15, 15, 12, 12, 15, 1, 15, 12, 12, 15 },
	{ 16, 12, 15, 15, 15, 15, 15, 15, 15, 15, 15, 12, 12, 15, 1, 15, 12 },
	{ 16, 15, 12, 12, 15, 15, 15, 15, 15, 15, 15, 15, 15, 12, 12, 15, 1 },
	{ 16, 15, 1, 15, 12, 12, 15, 15, 15, 15, 15, 15, 15, 15, 15, 12, 12 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
};

void printC64( u32 x, u32 y, const char *t, u8 color, u8 flag, u32 convert, u32 maxL )
{
	u32 l = min( strlen( t ), maxL );

	for ( u32 i = 0; i < l; i++ )
	{
		u32 c = t[ i ], c2 = c;

		// screen code conversion 
		if ( convert == 1 )
		{
			c2 = PETSCII2ScreenCode( c );
		} else
		{
			if ( convert == 2 && c >= 'a' && c <= 'z' )
				c = c + 'A' - 'a';
			if ( convert == 3 && c >= 'A' && c <= 'Z' )
				c = c + 'a' - 'A';
			if ( ( c >= 'a' ) && ( c <= 'z' ) )
				c2 = c + 1 - 'a';
			if ( c == '_' )
				c2 = 100;
		}

		u8 col = color;
		if ( color > 16 ) // special animated colors
		{
			u8 fade = color >> 5;
			u8 cyc  = color & 15;
			u8 n = colorCycle[ cyc ][ 0 ];
			col = fadeTabStep[ colorCycle[ cyc ][ 1 + ( ((frameCount>>2)) % n ) ] ][ fade ];
		}
		c64ScreenRAM[ x + y * 40 + i ] = c2 | flag;
		c64ColorRAM[ x + y * 40 + i ] = col;
	}
}

extern void printBrowser( int fade );

const int osziPosY = 21;

#define MOVEUP 4
#define MOVEDOWN 3

static u8 curRasterCommand = 0;
const u8 nRasterCommands = 21;
u16 keyScanRasterLinePAL = 275;
u16 rasterCommandsPAL[ nRasterCommands ][ 3 ] = {
	{ 28+5-MOVEUP, 0, 12 },
	{ 29+5-MOVEUP, 0, 15 },
	{ 30+5-MOVEUP, 0, 12 },

	{ 32+5-MOVEUP, 1, PAGE2_UPPERCASE },

	{ 44-MOVEUP, 0, 11 },
	{ 45-MOVEUP, 0, 12 },
	{ 46-MOVEUP, 0, 11 },
	{ 49-MOVEUP, 0, 0 },
	{ 50-MOVEUP, 0, 11 },
	{ 51-MOVEUP, 0, 0 },

	{ 52+31, 1, PAGE1_LOWERCASE },

	{ osziPosY * 8 + 51 - 1*0, 1, PAGE1_UPPERCASE },

	{ 251+MOVEDOWN, 0, 11 },
	{ 252+MOVEDOWN, 0, 0 },
	{ 253+MOVEDOWN, 0, 11 },
	{ 256+MOVEDOWN, 0, 12 },
	{ 257+MOVEDOWN, 0, 11 },
	{ 258+MOVEDOWN, 0, 12 },
	{ 267+MOVEDOWN, 0, 15 },
	{ 268+MOVEDOWN, 0, 12 },
	{ 269+MOVEDOWN, 0, 15 },
};

u16 keyScanRasterLineNTSC = 13;

u16 rasterCommandsNTSC[ nRasterCommands ][ 3 ] = {
	{ 28, 0, 12 },
	{ 29, 0, 15 },
	{ 30, 0, 12 },

	{ 32, 1, PAGE2_UPPERCASE },

	{ 43, 0, 11 },
	{ 44, 0, 12 },
	{ 45, 0, 11 },
	{ 49, 0, 0 },
	{ 50, 0, 11 },
	{ 51, 0, 0 },

	{ 52+31, 1, PAGE1_LOWERCASE },

	{ osziPosY * 8 + 51 - 1*0, 1, PAGE1_UPPERCASE },

	{ 251, 0, 11 },
	{ 252, 0, 0 },
	{ 253, 0, 11 },
	{ 257, 0, 12 },
	{ 258, 0, 11 },
	{ 259, 0, 12 },
	{  10, 0, 15 },
	{  11, 0, 12 },
	{  12, 0, 15 },
};

u16 (*rasterCommands)[ 3 ] = rasterCommandsPAL;
u16 keyScanRasterLine = keyScanRasterLinePAL;

static u16 osziPos = 0;
static u8 oszi[ 320 ];

const unsigned char keyTable[ 64 ] = 
{
	VK_DELETE, '3',        '5', '7', '9', '+', '?', '1',
	VK_RETURN, 'W',        'R', 'Y', 'I', 'P', '*', 95,
	VK_RIGHT,  'A',        'D', 'G', 'J', 'L', ';', 0,
	VK_F7,     '4',        '6', '8', '0', '-', VK_HOME, '2',
	VK_F1,     'Z',        'C', 'B', 'M', '.', VK_SHIFT_R, VK_SPACE,
	VK_F3,     'S',        'F', 'H', 'K', ':', '=', VK_COMMODORE,
	VK_F5,     'E',        'T', 'U', 'O', '@', '^', 'Q',
	VK_DOWN,   VK_SHIFT_L, 'X', 'V', 'N', ',', '/', 0, 
};


int meType = 0, old_meType = 0; // 0 = REU, 1 = GEORAM, 2 = NONE, 3 = VSF (only set temporary)
int meSize0 = 3, meSize1 = 0;
const char *meSizeStr[ 9 ] = { "64 KB ", "128 KB", "256 KB", "512 KB", "1 MB  ", "2 MB  ", "4 MB  ", "8 MB  ", "16 MB " };

const char DEFAULT_NUVIE_PLAYER[] = "SD:RAD/nuvieplayer.prg";
static const char FILENAME_CONFIG[] = "SD:RAD/rad.cfg";

bool radLaunchPRG = false;
bool radLaunchPRG_NORUN_128 = false;
bool radLaunchGEORAM = false;
bool radLaunchVSF = false;
char radLaunchPRGFile[ 1024 ];

bool radMemImageModified = false;

bool radLoadREUImage = false;
bool radLoadGeoImage = false;
char radImageSelectedPrint[ 22 ];
char radImageSelectedFile[ 1024 ];
char radImageSelectedName[ 1024 ];

#ifdef STATUS_MESSAGES
const char statusHeader[] = ".- RAD EXPANSION UNIT -.";
char statusMsg[ 40 * 8 ];
#endif

static u8 toupper( u8 c )
{
	if ( c >= 'a' && c <= 'z' )
		return c + 'A' - 'a';
	return c;
}

void setStatusMessage( char *msg, const char *tmp )
{
#ifdef STATUS_MESSAGES
	int l = strlen( tmp );
	memset( msg, 32, 40 );
	msg[ 40 ] = 0;
	if ( l == 0 ) return;
	if ( l > 40 ) l = 40;

	for ( int i = 0; i < l; i++ )
		msg[ ( 40 - l ) / 2 + i ] = toupper( tmp[ i ] );
#endif
}

#include "C64Side/ultimax_init.h"

u8 checkIfMachineRunning()
{
	justBooted = 1;

	bool running = false;

	RESET_CPU_CYCLE_COUNTER
	u64 start, duration;
	do {
		WAIT_FOR_CPU_HALFCYCLE
		WAIT_FOR_VIC_HALFCYCLE
		READ_CYCLE_COUNTER( start );
		for ( u32 i = 0; i < 1000; i++ )
		{
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
		}
		READ_CYCLE_COUNTER( duration );
		duration -= start;

		// target value about 1000 * 1400 (+/- depending on PAL/NTSC, RPi clock speed)
		if ( duration > 1200000 && duration < 1600000)
			running = true;
	} while ( !running );

	return 1;
}


void checkForC128()
{
	// check if we're running on a C128
	u8 y;
	SPEEK( 0xd030, y );

	isC128 = 0;
	if ( y == 0xfe || y == 0xfc )
		isC128 = 1;

	if ( !isC128 ) isC64 = 1;

	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
}

void checkForNTSC()
{
	isNTSC = 0;

	BUS_RESYNC

	u8 y;
	u16 curRasterLine;
	u16 maxRasterLine = 0;
	u16 lastRasterLine = 9999;

	for ( int i = 0; i < 313; i++ )
	{
		do {
			SPEEK( 0xd012, y );
			curRasterLine = y;
		} while ( curRasterLine == lastRasterLine );
		lastRasterLine = curRasterLine;

		SPEEK( 0xd011, y );
		if ( y & 128 ) curRasterLine += 256;

		if ( curRasterLine > maxRasterLine )
			maxRasterLine = curRasterLine;
	}

	if ( maxRasterLine < 300 )
		isNTSC = maxRasterLine - 260;

	// isNTSC == 0 => PAL: 312 rasterlines, 63 cycles
	// isNTSC == 1 => NTSC: 262 (0..261) rasterlines, 64 cycles, 6567R56A
	// isNTSC == 2 => NTSC: 263 (0..262) rasterlines, 65 cycles, 6567R8

	if ( isNTSC )
	{
		rasterCommands = rasterCommandsNTSC;
		keyScanRasterLine = keyScanRasterLineNTSC;
	} else
	{
		rasterCommands = rasterCommandsPAL;
		keyScanRasterLine = keyScanRasterLinePAL;
	}
}

void checkForRPiZero()
{
	isRPiZero2 = 0;
	if ( CMachineInfo::Get()->GetMachineModel() == MachineModelZero2W )
	{
		//rpiHasAudioJack = false;
		isRPiZero2 = 1;
	}
}


void waitAndHijack( register u32 &g2 )
{
	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER
	WAIT_FOR_VIC_HALFCYCLE

	u32 cycles = 0;
	do
	{
		emuWAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( TIMING_BA_SIGNAL_AVAIL );
		g2 = read32( ARM_GPIO_GPLEV0 );
		cycles ++;
	} while ( ( g2 & bBA ) && cycles < 25000 );

	emuWAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	// now we are in a badline ...
	// ... and it is safe to assert DMA ...
	WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA ); // 80ns after falling Phi2
	OUT_GPIO( DMA_OUT );
	CLR_GPIO( bDMA_OUT );


	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

	checkForC128();
	checkForNTSC();
	isNTSC = 0;
}

#include "C64Side/ultimax_memcfg.h"

void startWithUltimax( bool doReset = true )
{
	register u32 g2, g3;
	u8 nNOPs = 0;

restartProcedure:
	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bOE_Dx | bRW_OUT );
	INP_GPIO( RW_OUT );
	INP_GPIO( IRQ_OUT );
	OUT_GPIO( RESET_OUT );
	OUT_GPIO( GAME_OUT );
	CLR_GPIO( bRESET_OUT | bGAME_OUT | bDMA_OUT );

	CACHE_PRELOAD_DATA_CACHE( &ultimax_memcfg[ 0 ], 256, CACHE_PRELOADL2KEEP )
	FORCE_READ_LINEAR32a( &ultimax_memcfg, 256, 256 * 32 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( && ultimaxCRTCFG, 4096 );

	DELAY( 1 << 20 );

	int step = 0;

ultimaxCRTCFG:
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER
	WAIT_FOR_VIC_HALFCYCLE
	SET_GPIO( bRESET_OUT | bDMA_OUT );
	INP_GPIO( RESET_OUT );

	u32 firstAccess = 0, cycleCounter = 1;

	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER						
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS+ TIMING_OFFSET_CBTD );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		u8 addr = ADDRESS0to7;

		if ( ADDRESS_FFxx && CPU_READS_FROM_BUS )
		{
			u8 D = ultimax_memcfg[ addr ];

			if ( addr == step )
				step ++;

			register u32 DD = ( ( D ) & 255 ) << D0;
			write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
			write32( ARM_GPIO_GPSET0, DD );
			SET_BANK2_OUTPUT
			WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ );
			SET_GPIO( bOE_Dx | bDIR_Dx );

			if ( !firstAccess ) firstAccess = cycleCounter;

			if ( D == 0xEA ) nNOPs ++;
		}

		WAIT_FOR_VIC_HALFCYCLE

		cycleCounter ++;

		if ( cycleCounter > 1000000 ) goto restartProcedure;

		if ( nNOPs > 12 )
		{
			CLR_GPIO( bDMA_OUT );
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			return;
		}
		
		//	return;
	}
}


void waitAndHijackMenu( register u32 &g2 )
{
	startWithUltimax();
		
	WAIT_FOR_CPU_HALFCYCLE
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

	checkForC128();
	checkForNTSC();

	SET_GPIO( bGAME_OUT );

}


void injectAndStartPRG( u8 *prg, u32 prgSize, bool holdDMA )
{
	register u32 g2;

	CACHE_PRELOAD_DATA_CACHE( &prg[ 0 ], 65536, CACHE_PRELOADL2STRM )
	FORCE_READ_LINEAR32a( &prg, 65536, 65536 * 8 );

#ifdef STATUS_MESSAGES
	CACHE_PRELOAD_DATA_CACHE( &statusMsg[ 0 ], 4 * 40, CACHE_PRELOADL2STRM )
#endif

	CACHE_PRELOADL2STRM( &prg[ 0 ] );

	u32 addr = prg[ 0 ] + ( prg[ 1 ] << 8 );

	waitAndHijack( g2 );

	// now we are after the badline and DMA is asserted => we have control over the bus
	for ( int i = 0; i < (int)prgSize - 2; i++ ) 
	{
		CACHE_PRELOADL2STRM( &prg[ i + 1 ] );
		u8 d = ( (u8*)( &prg[ 2 ] ) )[ i ];
		SPOKE( addr + i, d );
		SPOKE( addr + i, d );
	}

	u8 bgColor, cursorColor;
	SPEEK( 0xd021, bgColor );
	bgColor &= 15;

	SPEEK( 0x0286, cursorColor );
	cursorColor &= 15;

	const u8 color2keycode[ 16 ] = { 0x90, 0x05, 0x1c, 0x9f, 0x9c, 0x1e, 0x1f, 0x9e, 0x81, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9a, 0x9b };

	if ( addr != 0x1c01 && addr != 0x0801 )
	{
		// only injected something into memory
	} else
	if ( isC128 && addr == 0x1c01 )
	{
		//SPOKE( 0xba, 8 );		// last used device = 8

		SPOKE( 0x2d, 0x01 );	// pointer to start addr of BASIC
		SPOKE( 0x2e, 0x1c );
		SPOKE( 0xfb, 0x01 );	// pointer to start addr of BASIC
		SPOKE( 0xfc, 0x1c );

		// ... and set BASIC program end and begin/end of BASIC variable addr
		u32 end = 0x1c01 + prgSize - 2;
		SPOKE( 0x1210, end & 255 );
		SPOKE( 0x1211, end >> 8 );

		// fake "rU:[RETURN]" using keyboard buffer 
		u16 kb = 0x34a;
		SPOKE( kb++, 0x52 ); // r
		SPOKE( kb++, 0xd5 ); // U
		SPOKE( kb++, 0x3a ); // :
		SPOKE( kb++, color2keycode[ cursorColor ] ); // color
		SPOKE( kb++, 0x0d ); // [RETURN]
		SPOKE( 0xd0, kb - 0x34a );

		SPOKE( 0x286, bgColor );
		SPOKE( 0x287, bgColor );
	} else
	{
		SPOKE( 0xba, 8 );		// last used device = 8

		SPOKE( 0x2b, 0x01 );	// pointer to start addr of BASIC
		SPOKE( 0x2c, 0x08 );

		// ... and set BASIC program end and begin/end of BASIC variable addr
		u32 end = 0x0801 + prgSize - 2;
		SPOKE( 0x2d, end & 255 );
		SPOKE( 0x2e, end >> 8 );
		SPOKE( 0x2f, end & 255 );
		SPOKE( 0x30, end >> 8 );
		SPOKE( 0x31, end & 255 );
		SPOKE( 0x32, end >> 8 );
		SPOKE( 0xae, end & 255 );
		SPOKE( 0xaf, end >> 8 );

		// fake "rU:[RETURN]" using keyboard buffer 
		u16 kb = 0x277;
		SPOKE( kb++, 0x52 ); // r
		SPOKE( kb++, 0xd5 ); // U
		SPOKE( kb++, 0x3a ); // :
		SPOKE( kb++, color2keycode[ cursorColor ] ); // color
		SPOKE( kb++, 0x0d ); // [RETURN]
		SPOKE( 0xc6, kb - 0x277 );

		SPOKE( 0x286, bgColor );
		SPOKE( 0x287, bgColor );
	}

#ifdef STATUS_MESSAGES
	extern u32 radSilentMode;
	if ( radSilentMode != 0xffffffff )
	{
		for ( int j = 0; j < 40; j++ )
		{
			SPOKE( 0x0400 + 23 * 40 + j, PETSCII2ScreenCode( statusMsg[ j + 80 ] ) );
			SPOKE( 0xd800 + 23 * 40 + j, 1 );
			SPOKE( 0x0400 + 24 * 40 + j, PETSCII2ScreenCode( statusMsg[ j ] ) );
			SPOKE( 0xd800 + 24 * 40 + j, 1 );
		}
	}
#endif

	if ( !holdDMA )
	{
		BUS_RESYNC
		emuWAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER						
		SET_GPIO( bDMA_OUT );
	}
}

u16 getResetVector()
{
	register u32 g2;
	u8 x;
	u16 vec;

	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bGAME_OUT | bOE_Dx | bRW_OUT );
	INP_GPIO( RW_OUT );
	INP_GPIO( IRQ_OUT );
	OUT_GPIO( RESET_OUT );
	CLR_GPIO( bRESET_OUT );

	CLR_GPIO( bGAME_OUT );
	CLR_GPIO( bMPLEX_SEL );
	DELAY( 1 << 18 );
	SET_GPIO( bRESET_OUT );
	INP_GPIO( RESET_OUT );

	// wait until CPU runs
	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS + 10 );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		if ( ADDRESS_FFxx && CPU_READS_FROM_BUS && ADDRESS0to7 == 0xfc )
		{
			SET_GPIO( bGAME_OUT );
			break;
		}
	}

	// now hijack computer and get reset vector
	waitAndHijack( g2 );

	SPEEK( 0xfffc, x );	vec = x;
	SPEEK( 0xfffd, x );	vec |= (u16)x << 8;

	return vec;
}


void injectKeyInput( const char *s, bool holdDMA )
{
	register u32 g2;

	waitAndHijack( g2 );

	u8 bgColor;
	SPEEK( 0xd021, bgColor );
	bgColor &= 15;

	SPOKE( 0xba, 8 );	// last used device = 8

	u16 kb = 0x277;
		
	for ( u32 j = 0; j < strlen( s ); j++ )
		SPOKE( kb++, s[ j ] );

	SPOKE( kb++, 0x3a );
	SPOKE( kb++, 0x0d );
	SPOKE( 0xc6, kb - 0x277 );

	SPOKE( 0x286, bgColor );
	SPOKE( 0x287, bgColor );

#ifdef STATUS_MESSAGES
	extern u32 radSilentMode;
	if ( radSilentMode != 0xffffffff )
	{
		for ( int j = 0; j < 40; j++ )
		{
			SPOKE( 0x400 + 23 * 40 + j, PETSCII2ScreenCode( statusMsg[ j + 80 ] ) );
			SPOKE( 0xd800 + 23 * 40 + j, 1 );
			SPOKE( 0x400 + 24 * 40 + j, PETSCII2ScreenCode( statusMsg[ j ] ) );
			SPOKE( 0xd800 + 24 * 40 + j, 1 );
		}
	}
#endif

	if ( !holdDMA )
	{
		BUS_RESYNC
		emuWAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		SET_GPIO( bDMA_OUT );
	}
}


void injectMessage( bool holdDMA )
{
	register u32 g2;

	extern u32 radSilentMode;
	if ( radSilentMode == 0xffffffff )
		return;

	waitAndHijack( g2 );

#ifdef STATUS_MESSAGES
	for ( int j = 0; j < 40; j++ )
	{
		SPOKE( 0x400 + 23 * 40 + j, PETSCII2ScreenCode( statusMsg[ j + 80 ] ) );
		SPOKE( 0xd800 + 23 * 40 + j, 1 );
		SPOKE( 0x400 + 24 * 40 + j, PETSCII2ScreenCode( statusMsg[ j ] ) );
		SPOKE( 0xd800 + 24 * 40 + j, 1 );
	}
#endif

	if ( !holdDMA )
	{
		BUS_RESYNC
		emuWAIT_FOR_VIC_HALFCYCLE
		RESTART_CYCLE_COUNTER
		SET_GPIO( bDMA_OUT );
	}
}



int checkForReadyPrompt( u8 c128mode )
{
	register u32 g2;
	int res = 0;

	waitAndHijack( g2 );

	u8 cursorRow, cursorCol;
	u8 cursorRow128, cursorCol128;

	if ( isC128 /*&& c128mode*/ )
	{
		SPEEK( 0xeb, cursorRow128 );
		SPEEK( 0xec, cursorCol128 );
	} 

	SPEEK( 0xd6, cursorRow );
	SPEEK( 0xd3, cursorCol );

	char prompt[ 7 ] = { 18, 5, 1, 4, 25, 46, 0 }; // screencodes of "READY."

	u32 ofs = ( cursorRow - 1 ) * 40 + cursorCol;
	u32 ofs128 = ( cursorRow128 - 1 ) * 40 + cursorCol128;

	char screen[ 7 ];
	for ( u32 i = 0; i < 6; i ++ )
		SPEEK( 0x400 + ofs + i, *(u8*)&screen[ i ] );

	screen[ 6 ] = 0;
	if ( strcmp( prompt, screen ) == 0 )
		res = 1; 

	if ( isC128 && !res )
	{
		for ( u32 i = 0; i < 6; i ++ )
			SPEEK( 0x400 + ofs128 + i, *(u8*)&screen[ i ] );
		if ( strcmp( prompt, screen ) == 0 )
			res = 1; 
	}

	BUS_RESYNC
	emuWAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER						
	SET_GPIO( bDMA_OUT );

	return res;
}

u8 starPos[ 256 ];

void initMenu()
{
	radLaunchPRG = false;
	radLaunchPRG_NORUN_128 = false;
	radLaunchGEORAM = false;
	radLaunchPRGFile[ 0 ] = 0; 
	
	radLoadREUImage = false;
	radLoadGeoImage = false;
	strcpy( radImageSelectedPrint, "_____________________" );

	// prepare a few things for color fading and updates
	for ( int j = 0; j < 6; j++ )
		for ( int i = 0; i < 16; i++ )
		{
			if ( j == 0 ) 
				fadeTabStep[ i ][ j ] = i; else
				fadeTabStep[ i ][ j ] = fadeTab[ fadeTabStep[ i ][ j - 1 ] ]; 
		}

	for ( int i = 0; i < 256; i++ )
		starPos[ i ] = (i * 17) % 193;
}

void removeFileExt( char *s )
{
	for ( int i = strlen( s ) - 1; i >= 0; i-- )
		if ( s[ i ] == '.' ) 
		{
			s[ i ] = 0;
			return;
		}
}

u16 lastRasterLine = 1234;

bool screenUpdated = true;

u8 imageNameEdit = 0;
char imageNameStr[ 60 ] = { 0 };
u8 imageNameStrLength = 0;
const u8 *mahoneyLUT;

static u8 fadeToHelp = 0;
static u8 showHelp = 0;
static u8 fadeToTimings = 0;
static u8 showTimings = 0;

void printHelpScreen( int fade )
{
	u8 xp = 4;
	u8 yp = 8;

	const u8 c1 = fadeTabStep[ 1 ][ fade ];
	const u8 c2 = fadeTabStep[ 3 ][ fade ];
	const u8 c3 = fadeTabStep[ 13 ][ fade ];
	const u8 c4 = fadeTabStep[ 11 ][ fade ];

	printC64( xp, ++yp, "                                      ", 0, 0, 0, 39 );
	printC64( xp, ++yp, "Keyboard Commands                   ", c1, 0, 0, 39 );
	printC64( xp, ++yp, "T,+,-    change expansion type/size ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "CURSOR   navigate in browser        ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "F1/F3    page up/down               ", c2, 0, 0, 39 );
//	printC64( xp, ++yp, "HOME   first entry                  ", c2, 0, 0, 39 );
//	printC64( xp, ++yp, "DEL    go one directory up          ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "HOME/DEL first entry, directory up  ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "RETURN   start PRG/select image,    ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "         2x autostart NUVIE/GeoRAM  ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "U        unmount REU/GeoRAM image   ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "N        name&save modified image   ", c2, 0, 0, 39 );
	printC64( xp, ++yp, "\x5c        adjust bus timings         ", c2, 0, 0, 39 );

	yp = 10;
	printC64( xp, ++yp, "T,+,-", c3, 0, 0, 39 );
	printC64( xp, ++yp, "CURSOR", c3, 0, 0, 39 );
	printC64( xp, ++yp, "F1/F3", c3, 0, 0, 39 );
	printC64( xp, ++yp, "HOME/DEL", c3, 0, 0, 39 );
	//printC64( xp, ++yp, "DEL", c3, 0, 0, 39 );
	printC64( xp, ++yp, "RETURN", c3, 0, 0, 39 );
	++yp;
	printC64( xp, ++yp, "U", c3, 0, 0, 39 );
	printC64( xp, ++yp, "N", c3, 0, 0, 39 );
	printC64( xp, ++yp, "\x5c", c3, 0, 1, 39 );

	{
		extern u32 temperature;
		char bb[ 64 ];
		char b[64];
		bb[ 0 ] = 0;
		if ( hasSIDKick == 1 )
		{
			strcpy( bb, SIDKickVersion );
			bb[ 10-3 ] = '/';
			bb[ 11-3 ] = 0;
		} else
		if ( hasSIDKick == 2 )
		{
			strcpy( bb, "SKpico/" );
			bb[ 7 ] = 0;
		} 
		extern u8 supportDAC;
		if ( SIDKickVersion[ 0 ] && supportDAC )
		{
			sprintf( b, "%sDAC", bb );
		} else
		{
			if ( SIDType == 0 )
				sprintf( b, "SID-type n/a", bb ); else
			{
				if ( SIDType == ( 6581 & 255 ) )
					sprintf( b, "%s6581", bb ); else
					sprintf( b, "%s8580", bb );
			}
		}
		sprintf( bb, "%s %dC, %s/%s, %s", isRPiZero2 ? "RPi0" : "RPi3", temperature, isC128 ? "C128" : "C64", isNTSC ? "NTSC" : "PAL", b );
		printC64( xp, ++yp, bb, c4, 0, 0, 39 );
	}

}

void transferTimingValues( int *timingValues )
{
	timingValues[ 0 ] = reu.WAIT_FOR_SIGNALS;
	timingValues[ 1 ] = reu.WAIT_CYCLE_READ;
	timingValues[ 2 ] = reu.WAIT_CYCLE_WRITEDATA;
	//timingValues[ 3 ] = reu.WAIT_CYCLE_READ_BADLINE;
	timingValues[ 4 ] = reu.WAIT_CYCLE_READ_VIC2;
	timingValues[ 5 ] = reu.WAIT_CYCLE_WRITEDATA_VIC2;
	timingValues[ 6 ] = reu.WAIT_CYCLE_MULTIPLEXER;
	timingValues[ 7 ] = reu.WAIT_CYCLE_MULTIPLEXER_VIC2;
	timingValues[ 8 ] = reu.WAIT_TRIGGER_DMA;
	timingValues[ 9 ] = reu.WAIT_RELEASE_DMA;
	timingValues[ 10 ] = reu.TIMING_OFFSET_CBTD;
	timingValues[ 11 ] = reu.TIMING_DATA_HOLD;
	timingValues[ 12 ] = reu.TIMING_TRIGGER_DMA;
	timingValues[ 13 ] = reu.TIMING_ENABLE_ADDRLATCH;
	timingValues[ 14 ] = reu.TIMING_READ_BA_WRITING;
	timingValues[ 15 ] = reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING;
	timingValues[ 16 ] = reu.TIMING_ENABLE_DATA_WRITING;
	timingValues[ 17 ] = reu.TIMING_BA_SIGNAL_AVAIL;
	timingValues[ 18 ] = reu.CACHING_L1_WINDOW_KB / 1024;
	timingValues[ 19 ] = reu.CACHING_L2_OFFSET_KB / 1024;
	timingValues[ 20 ] = reu.CACHING_L2_PRELOADS_PER_CYCLE;
	timingValues[ 21 ] = reu.TIMING_RW_BEFORE_ADDR;

	WAIT_FOR_SIGNALS = reu.WAIT_FOR_SIGNALS;
	WAIT_CYCLE_MULTIPLEXER = reu.WAIT_CYCLE_MULTIPLEXER;
	WAIT_CYCLE_READ = reu.WAIT_CYCLE_READ;
	WAIT_CYCLE_WRITEDATA = reu.WAIT_CYCLE_WRITEDATA;
	WAIT_CYCLE_READ_VIC2 = reu.WAIT_CYCLE_READ_VIC2;
	WAIT_CYCLE_WRITEDATA_VIC2 = reu.WAIT_CYCLE_WRITEDATA_VIC2;
	WAIT_CYCLE_MULTIPLEXER_VIC2 = reu.WAIT_CYCLE_MULTIPLEXER_VIC2;
	WAIT_TRIGGER_DMA = reu.WAIT_TRIGGER_DMA;
	WAIT_RELEASE_DMA = reu.WAIT_RELEASE_DMA;
	TIMING_OFFSET_CBTD = reu.TIMING_OFFSET_CBTD;
	TIMING_DATA_HOLD = reu.TIMING_DATA_HOLD;
	TIMING_TRIGGER_DMA = reu.TIMING_TRIGGER_DMA;
	TIMING_ENABLE_ADDRLATCH = reu.TIMING_ENABLE_ADDRLATCH;
	TIMING_READ_BA_WRITING = reu.TIMING_READ_BA_WRITING;
	TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING;
	TIMING_ENABLE_DATA_WRITING = reu.TIMING_ENABLE_DATA_WRITING;
	TIMING_BA_SIGNAL_AVAIL = reu.TIMING_BA_SIGNAL_AVAIL;
	TIMING_RW_BEFORE_ADDR = reu.TIMING_RW_BEFORE_ADDR;
}

void printTimingsScreen( int fade )
{
	u8 xp = 4;
	u8 yp = 8;

	const u8 c1 = fadeTabStep[ 1 ][ fade ];
	const u8 c2 = fadeTabStep[ 3 ][ fade ];
	const u8 c3 = fadeTabStep[ 13 ][ fade ];
	const u8 c4 = fadeTabStep[ 12 ][ fade ];

	printC64( xp, ++yp, "                                      ", 0, 0, 0, 39 );
	#ifdef OLD_BUS_PROTOCOL
	printC64( xp, ++yp, "Bus Timings (old protocol)            ", c1, 0, 0, 39 );
	#else
	printC64( xp, ++yp, "Bus Timing Adjustments                ", c1, 0, 0, 39 );
	#endif

	char bb[ 64 ];
	xp = 9;

	sprintf( bb, "TRIGGER DMA:         %3d", reu.TIMING_TRIGGER_DMA );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
	sprintf( bb, "DATA HOLD:           %3d", reu.TIMING_DATA_HOLD );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
	sprintf( bb, "ENABLE ADDRESS:      %3d", reu.TIMING_ENABLE_ADDRLATCH );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
	sprintf( bb, "TIMING OFFSET:       %3d", reu.TIMING_OFFSET_CBTD );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );


#ifdef OLD_BUS_PROTOCOL
	sprintf( bb, "READ BA/DMA WRITE:   %3d", reu.TIMING_READ_BA_WRITING );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
#else
	sprintf( bb, "WRITE DATA:          %3d", reu.WAIT_CYCLE_WRITEDATA );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
#endif

#ifdef OLD_BUS_PROTOCOL
	sprintf( bb, "READ BA/DMA READ:    %3d", reu.TIMING_BA_SIGNAL_AVAIL );
#else
	sprintf( bb, "READ BA:             %3d", reu.TIMING_BA_SIGNAL_AVAIL + 675 - 261 );
#endif

	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
	sprintf( bb, "ENABLE RW+ADDR:      %3d", reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );

#ifdef OLD_BUS_PROTOCOL
	sprintf( bb, "ENABLE DATA:         %3d", reu.TIMING_ENABLE_DATA_WRITING );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
	sprintf( bb, "RW BEFORE ADDR:      %3d", reu.TIMING_RW_BEFORE_ADDR );
	printC64( xp, ++yp, bb, c2, 0, 0, 39 );
#endif 

	yp = 10;
	xp = 4;
	printC64( xp, ++yp, "A/Q", c3, 0, 0, 39 );
	printC64( xp, ++yp, "S/W", c3, 0, 0, 39 );
	printC64( xp, ++yp, "D/E", c3, 0, 0, 39 );
	printC64( xp, ++yp, "F/R", c3, 0, 0, 39 );
	printC64( xp, ++yp, "G/T", c3, 0, 0, 39 );
	printC64( xp, ++yp, "H/Y", c3, 0, 0, 39 );
	printC64( xp, ++yp, "J/U", c3, 0, 0, 39 );
#ifdef OLD_BUS_PROTOCOL
	printC64( xp, ++yp, "K/I", c3, 0, 0, 39 );
	printC64( xp, ++yp, "L/O", c3, 0, 0, 39 );
#endif

	printC64( xp, ++yp, "B Back, P Keep Permanently", c4, 0, 0, 39 );
}


u32 readKeyRenderMenu( int fade )
{
	u8 a;
	static int lastKey = -1, repKey = 0;

	SPOKE( 0xdc00, 0 );
	SPEEK( 0xdc01, a );
						
	if ( a != 255 )
	{
		u8 matrix[ 8 ];
		u8 a = 1, x, y;
		for ( u8 i = 0; i < 8; i++ )
		{
			SPOKE( 0xdc00, ~a );
			SPEEK( 0xdc01, matrix[ i ] );
			a <<= 1;
		}

		int k = 0;

		y = ( ( matrix[ 1 ] ^ 0xff ) & 0b10000000 ) >> 1;
		y |= ( matrix[ 7 ] ^ 0xff ) & 0b10100100;
		y |= ( matrix[ 6 ] ^ 0xff ) & 0b00011000;
		x = matrix[ 0 ] ^ 0xff;

		bool shift = ( y & 80 ) ? true : false;
		bool ckey = ((~matrix[7]) & 0x20);
		if ( x & 8 && !shift ) k = VK_F7; else
		if ( x & 16 && !shift ) k = VK_F1; else
		if ( x & 32 && !shift ) k = VK_F3; else
		if ( x & 64 && !shift ) k = VK_F5; else
		if ( x & 8 && shift ) k = VK_F8; else
		if ( x & 16 && shift ) k = VK_F2; else
		if ( x & 32 && shift ) k = VK_F4; else
		if ( x & 64 && shift ) k = VK_F6; else
		if ( x & 128 && shift ) k = VK_UP; else
		if ( x & 128 && !shift ) k = VK_DOWN; else
		if ( x & 4 && shift ) k = VK_LEFT; else
		if ( x & 4 && !shift ) k = VK_RIGHT; else
		if ( x & 2 && !shift ) k = VK_RETURN; else
		if ( x & 2 && shift) k = VK_SHIFT_RETURN; else
		if ( x & 1 ) k = VK_DELETE;
		if ( x & 2 && ckey ) k = VK_COMMODORE_RETURN; else
		if ( k == 0 )
		for ( int i = 0; i < 8; i++ )
		{
			for ( a = 0; a < 8; a++ )
			{
				if ( ( ( matrix[ a ] >> i ) & 1 ) == 0 )
				{
					k = keyTable[ i * 8 + a ];
					break;
				}
			}
		}

		/*char bb[64];
		sprintf( bb, "%d  %c   ", k, k );
		printC64( 0, 10, bb, 1, 0, 0, 39 );*/

		extern u32 handleKey( int k );
		u32 cmd = 0;
		if ( k == lastKey )
			repKey ++;
		if ( k && ( k != lastKey || repKey > 4 ) )
		{
			if ( showTimings && fadeToTimings == 0 )
			{
				if ( k == 'B' || k == '?' ) 
				{ 
					int newTimingValues[ 21 ];
					transferTimingValues( newTimingValues );
					extern void temporaryTimingsUpdate( int *newTimingValues );
					temporaryTimingsUpdate( newTimingValues );
					fadeToTimings = 128 + 10;
				}
				if ( k == 'P' ) return SAVE_CONFIG;

				const int step = 1;
				if ( k == 'A' ) reu.TIMING_TRIGGER_DMA = max( 0, reu.TIMING_TRIGGER_DMA - step );
				if ( k == 'Q' ) reu.TIMING_TRIGGER_DMA += step;
				if ( k == 'S' ) reu.TIMING_DATA_HOLD = max( 0, reu.TIMING_DATA_HOLD - step );
				if ( k == 'W' ) reu.TIMING_DATA_HOLD += step;
				if ( k == 'D' ) reu.TIMING_ENABLE_ADDRLATCH = max( 0, reu.TIMING_ENABLE_ADDRLATCH - step );
				if ( k == 'E' ) reu.TIMING_ENABLE_ADDRLATCH += step;
				if ( k == 'F' ) reu.TIMING_OFFSET_CBTD = max( 0, reu.TIMING_OFFSET_CBTD - step );
				if ( k == 'R' ) reu.TIMING_OFFSET_CBTD += step;
			#ifdef OLD_BUS_PROTOCOL
				if ( k == 'G' ) reu.TIMING_READ_BA_WRITING = max( 0, reu.TIMING_READ_BA_WRITING - step );
				if ( k == 'T' ) reu.TIMING_READ_BA_WRITING += step;
			#else
				if ( k == 'G' ) reu.WAIT_CYCLE_WRITEDATA = max( 0, reu.WAIT_CYCLE_WRITEDATA - step );
				if ( k == 'T' ) reu.WAIT_CYCLE_WRITEDATA += step;
			#endif
				if ( k == 'H' ) reu.TIMING_BA_SIGNAL_AVAIL = max( 0, reu.TIMING_BA_SIGNAL_AVAIL - step );
				if ( k == 'Y' ) reu.TIMING_BA_SIGNAL_AVAIL += step;
				if ( k == 'J' ) reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = max( 0, reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING - step );
				if ( k == 'U' ) reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING += step;
			
			#ifdef OLD_BUS_PROTOCOL
				if ( k == 'K' ) reu.TIMING_ENABLE_DATA_WRITING = max( 0, reu.TIMING_ENABLE_DATA_WRITING - step );
				if ( k == 'I' ) reu.TIMING_ENABLE_DATA_WRITING += step;
				if ( k == 'L' ) reu.TIMING_RW_BEFORE_ADDR = max( 0, reu.TIMING_RW_BEFORE_ADDR - step );
				if ( k == 'O' ) reu.TIMING_RW_BEFORE_ADDR += step;
			#endif

				WAIT_FOR_SIGNALS = reu.WAIT_FOR_SIGNALS;
				WAIT_CYCLE_MULTIPLEXER = reu.WAIT_CYCLE_MULTIPLEXER;
				WAIT_CYCLE_READ = reu.WAIT_CYCLE_READ;
				WAIT_CYCLE_WRITEDATA = reu.WAIT_CYCLE_WRITEDATA;
				WAIT_CYCLE_READ_VIC2 = reu.WAIT_CYCLE_READ_VIC2;
				WAIT_CYCLE_WRITEDATA_VIC2 = reu.WAIT_CYCLE_WRITEDATA_VIC2;
				WAIT_CYCLE_MULTIPLEXER_VIC2 = reu.WAIT_CYCLE_MULTIPLEXER_VIC2;
				WAIT_TRIGGER_DMA = reu.WAIT_TRIGGER_DMA;
				WAIT_RELEASE_DMA = reu.WAIT_RELEASE_DMA;
				TIMING_OFFSET_CBTD = reu.TIMING_OFFSET_CBTD;
				TIMING_DATA_HOLD = reu.TIMING_DATA_HOLD;
				TIMING_TRIGGER_DMA = reu.TIMING_TRIGGER_DMA;
				TIMING_ENABLE_ADDRLATCH = reu.TIMING_ENABLE_ADDRLATCH;
				TIMING_READ_BA_WRITING = reu.TIMING_READ_BA_WRITING;
				TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING;
				TIMING_ENABLE_DATA_WRITING = reu.TIMING_ENABLE_DATA_WRITING;
				TIMING_BA_SIGNAL_AVAIL = reu.TIMING_BA_SIGNAL_AVAIL;
				TIMING_RW_BEFORE_ADDR = reu.TIMING_RW_BEFORE_ADDR;
				reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING_MINUS_RW_BEFORE_ADDR = TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING - TIMING_RW_BEFORE_ADDR;

				k = 0;
			}

			if ( ( k == 'H' || showHelp ) && fadeToHelp == 0 )
			{
				if ( showHelp )
					fadeToHelp = 128 + 10; else
					fadeToHelp = 10;
				k = 0;	
			}

			if ( k == '?' && fadeToTimings == 0 )
			{
				if ( showTimings )
					fadeToTimings = 128 + 10; else
					fadeToTimings = 10;
				k = 0;
			}

			if ( imageNameEdit )
			{
				if ( k != lastKey )
				{
					if ( k == VK_ESC )
					{
						imageNameEdit = 0;
					} else
					if ( ( ( k >= 'A' && k <= 'Z' ) || ( k >= '0' && k <= '9' ) ) && imageNameStrLength < 20 )
					{
						imageNameStr[ imageNameStrLength++ ] = k;
						imageNameStr[ imageNameStrLength ] = 0;
					} else
					if ( k == VK_DELETE && imageNameStrLength )
					{
						imageNameStr[ --imageNameStrLength ] = 0; 
					} else
					if ( k == VK_RETURN )
					{
						imageNameEdit = 0;
						return SAVE_IMAGE;
					}
					repKey = 0;
					lastKey = k;
				}

				k = -1;
				//continue;
				goto test;
			}


			#ifdef DEBUG_REBOOT_RPI_ON_R
			if ( k == 'R' )
				return RUN_REBOOT;
			#endif

			if ( k == 'N' && reu.isModified == meType + 1 )
			{
				if ( radImageSelectedFile[ 0 ] == '_' || radImageSelectedFile[ 0 ] == 0 )
					imageNameStr[ 0 ] = 0; else
					strcpy( imageNameStr, radImageSelectedPrint );
				imageNameStrLength = strlen( imageNameStr );
				imageNameEdit = 1;
			}

			if ( k == 'X' )
			{
				memset( &statusMsg[ 80 ], 32, 40 );

				radLaunchGEORAM = false;
				radLaunchPRG	= false;
				radLaunchPRG_NORUN_128 = false;
				radLaunchVSF = false;

				return RUN_MEMEXP + meType + 1;
			} 
			if ( k == 'U' )
			{
				unmarkAllFiles();
				radImageSelectedFile[ 0 ] = 0;
				radImageSelectedName[ 0 ] = 0;
				radImageSelectedPrint[ 0 ] = 0;
				strcpy( radImageSelectedPrint, "_____________________" );
				radLoadREUImage = false;
				radLoadGeoImage = false;
				radLaunchVSF = false;
				reu.isModified  = 0;
			}

			if ( k == 'T' ) 
			{
				meType = ( meType + 1 ) % 3; 
				radImageSelectedFile[ 0 ] = 0;
				radImageSelectedName[ 0 ] = 0;
				radImageSelectedPrint[ 0 ] = 0;
				strcpy( radImageSelectedPrint, "_____________________" );
				radLoadREUImage = false;
				radLoadGeoImage = false;
				radLaunchVSF = false;
				reu.isModified  = 0;
			} else
			if ( k == '+' || k == '-' ) 
			{
				int d = (k == '+') ? 1 : -1;
				if ( meType == 0 ) 
					meSize0 = max( 0, min( 7, meSize0 + d ) ); else  
				if ( meType == 1 ) 
					meSize1 = max( 0, min( 3, meSize1 + d ) ); 

				radImageSelectedFile[0] = 0;
				radImageSelectedName[0] = 0;
				radImageSelectedPrint[0] = 0;
				strcpy(radImageSelectedPrint, "_____________________");
				radLoadREUImage = false;
				radLoadGeoImage = false;
				radLaunchVSF = false;
				reu.isModified = 0;
			} else
				cmd = handleKey( k );

			if ( cmd == REUMENU_SELECT_FILE_REU || cmd == REUMENU_PLAY_NUVIE_REU )
			{
				if ( dirSelectedFileSize >= 128 * 1024 && dirSelectedFileSize <= 16384 * 1024 )
				{
					reu.isModified = 0;

					meType = 0;
					u32 s = 128 * 1024; 
					meSize0 = 0;
					while ( s < dirSelectedFileSize && meSize0 < 7 ) { s <<= 1; meSize0 ++; }
					strncpy( radImageSelectedFile, dirSelectedFile, 1023 );
					strncpy( radImageSelectedName, dirSelectedName, 1023 );
					memset( radImageSelectedPrint, 0, 22 );
					strncpy( radImageSelectedPrint, dirSelectedName, 21 );
					removeFileExt( radImageSelectedPrint );
					radLoadREUImage = true;

					char tmp[ 40 ];

					if ( cmd == REUMENU_PLAY_NUVIE_REU )
					{
						strncpy( radLaunchPRGFile, DEFAULT_NUVIE_PLAYER, 1023 );

					#ifdef STATUS_MESSAGES
						sprintf( tmp, "%s (NUVIE %dM)", radImageSelectedPrint, s / 1024 / 1024 );
						setStatusMessage( &statusMsg[ 0 ], tmp );
					#endif
						radLaunchPRG = true;
						radLaunchPRG_NORUN_128 = false;
						return RUN_MEMEXP + meType + 1;
					} else
					{
					#ifdef STATUS_MESSAGES
						sprintf( tmp, "%s (REU %dK)", radImageSelectedPrint, s / 1024 );
						setStatusMessage( &statusMsg[ 0 ], tmp );
					#endif
					}
				}
			}
			if ( cmd == REUMENU_SELECT_FILE_VSF )
			{
				reu.isModified = 0;
				old_meType = meType;
				meType = 3;

				// guess REU size
				u32 guessedSize = dirSelectedFileSize - ( 64 + 120 ) * 1024;
				u32 s = 128 * 1024; 
				meSize0 = 0;
				while ( s < guessedSize && meSize0 < 7 ) { s <<= 1; meSize0 ++; }

				strncpy( radImageSelectedFile, dirSelectedFile, 1023 );
				strncpy( radImageSelectedName, dirSelectedName, 1023 );
				memset( radImageSelectedPrint, 0, 22 );
				// if name of VSF should be printed under "Image":
				//strncpy( radImageSelectedPrint, dirSelectedName, 21 );
				//removeFileExt( radImageSelectedPrint );

				char tmp[ 40 ];

				#ifdef STATUS_MESSAGES
				sprintf( tmp, "%s (VSF %dK)", radImageSelectedPrint, dirSelectedFileSize / 1024 );
				setStatusMessage( &statusMsg[ 0 ], tmp );
				#endif

				radLaunchPRG = false;
				radLaunchPRG_NORUN_128 = false;
				radLaunchGEORAM = false;
				radLaunchVSF = true;
				return RUN_MEMEXP + meType + 1;

			}
			if ( cmd == REUMENU_SELECT_FILE_GEO || cmd == REUMENU_START_GEORAM )
			{
				if ( dirSelectedFileSize >= 512 * 1024 && dirSelectedFileSize <= 4096 * 1024 )
				{
					reu.isModified = 0;
					meType = 1;
					u32 s = 512 * 1024; 
					meSize1 = 0;
					while ( s < dirSelectedFileSize && meSize1 < 3 ) { s <<= 1; meSize1 ++; }
					strncpy( radImageSelectedFile, dirSelectedFile, 1023 );
					strncpy( radImageSelectedName, dirSelectedName, 1023 );
					memset( radImageSelectedPrint, 0, 22 );
					strncpy( radImageSelectedPrint, dirSelectedName, 10 );
					removeFileExt( radImageSelectedPrint );
					radLoadGeoImage = true;
					radLaunchGEORAM = false;

				#ifdef STATUS_MESSAGES
					char tmp[ 40 ];
					sprintf( tmp, "%s (GEORAM %dK)", radImageSelectedPrint, s / 1024 );
					setStatusMessage( &statusMsg[ 0 ], tmp );
				#endif

					if ( cmd == REUMENU_START_GEORAM )
					{
						radLaunchPRG = false;
						radLaunchPRG_NORUN_128 = false;
						radLaunchGEORAM = true;
						radLaunchVSF = false;
						return RUN_MEMEXP + meType + 1;
					}
				}
			}
			if ( cmd == REUMENU_SELECT_FILE_PRG )
			{
				if ( dirSelectedFileSize > 2 && dirSelectedFileSize <= 65536 + 2 )
				{
					char tmp1[ 22 ];
					memset( tmp1, 0, 22 );
					strncpy( tmp1, dirSelectedName, 20 );

				#ifdef STATUS_MESSAGES
					char tmp[ 40 ];
					sprintf( tmp, "%s (%2.1fK)", tmp1, (float)dirSelectedFileSize / 1024.0f );
					setStatusMessage( &statusMsg[ 80 ], tmp );

					sprintf( tmp, "%s (%2.1fK, $0000)", tmp1, (float)dirSelectedFileSize / 1024.0f );
					setStatusMessage( &statusMsg[ 120 ], tmp );

					tmp[ 0 ] = 0;
					setStatusMessage( &statusMsg[ 0 ], tmp );
				#endif

					strncpy( radLaunchPRGFile, dirSelectedFile, 1023 );
					radLaunchPRG = true;
					radLaunchPRG_NORUN_128 = !(k == VK_COMMODORE_RETURN);
					radLaunchVSF = false;
					return RUN_MEMEXP + meType + 1;
				}
			}

			if ( k != lastKey )
			{
				lastKey = k;
				repKey = 0;
			}
			BUS_RESYNC
		} else
		{
			if ( !k ) lastKey = -1;
		}
	} else
	{
		SPOKE( 0xdc00, 255 );
		lastKey = -1;
	}
test:

	u8 fadeBetween = 0;
	#define FADE_TO_OTHER_SCREEN( f, s )									\
		if ( f ) {															\
			f --;															\
			if ( f < 128 ) { /* fading from browser to other screen */		\
				if ( f > 5 ) 												\
					fadeBetween = 11 - f; else								\
					fadeBetween = f; 										\
				if ( f == 5 ) s = 1;										\
			} else {														\
				if ( f > 128 + 5 ) 											\
					fadeBetween = 128 + 11 - f; else						\
					fadeBetween = f - 128; 									\
				if ( f == 128 + 5 ) s = 0;									\
				if ( f == 128 ) f = 0;										\
		}	}

	FADE_TO_OTHER_SCREEN( fadeToHelp, showHelp )
	FADE_TO_OTHER_SCREEN( fadeToTimings, showTimings )

	u8 curFade = min( 5, max( fadeBetween, fade ) );
	if ( showTimings )	
		printTimingsScreen( curFade ); else
	if ( showHelp )
		printHelpScreen( curFade ); else
		printBrowser( curFade );

	const u32 oo = 1;
	const u32 px = 4, vx = 15, kx = 26;
	printC64( px, 3+oo, "Memory Expansion", 1, 0, 0, 39 );
	printC64( px, 4+oo, "Type",13, 0, 0, 39 );

	printC64( px, 5+oo, "Size", meType == 2 ? 5 : 13, 0, 0, 39 );

	if ( meType == 0 )
	{
		printC64( vx, 4+oo, "REU   ", 3, 0, 0, 39 );
		printC64( vx, 5+oo, meSizeStr[ meSize0 + 1 ], 3, 0, 0, 39 );
	} else
	if ( meType == 1 )
	{
		printC64( vx, 4+oo, "GeoRAM", 3, 0, 0, 39 );
		printC64( vx, 5+oo, meSizeStr[ meSize1 + 3 ], 3, 0, 0, 39 );
	} else
	if ( meType == 2 )
	{
		printC64( vx, 4+oo, "none  ", 3, 0, 0, 39 );
		printC64( vx, 5+oo, "      ", 3, 0, 0, 39 );
	}
	if ( meType == 3 )
	{
		printC64( vx, 4+oo, "(VSF) ", 3, 0, 0, 39 );
		printC64( vx, 5+oo, "      ", 3, 0, 0, 39 );
	}

	if ( meType == 2 )
	{
		printC64( px, 6+oo, "Image:                ", 5, 0, 0, 39 ); 
		printC64( px, 7+oo, "_____________________", 5, 0, 0, 39 );
	} else
	{
		if ( reu.isModified == meType + 1 )
		{
			printC64( px, 6+oo, "Image (modified):     ", 13, 0, 0, 39 );
			printC64( px + 7, 6+oo, "modified", 18, 0, 0, 39 );
		} else
			printC64( px, 6+oo, "Image:                ", 13, 0, 0, 39 ); 
		printC64( px, 7+oo, "_____________________", 3, 0, 0, 39 );
	}

	if ( imageNameEdit )
	{
		printC64( px, 7+oo, imageNameStr, 3, 0, 0, 39 );
		printC64( px + imageNameStrLength, 7+oo, "_", 3, 128, 0, 39 );
		printC64( kx, 6+oo, "(\x1f cancel,", 15, 0, 0, 39 );
		printC64( kx, 7+oo, " RET save) ", 15, 0, 0, 39 );
		printC64( kx+1, 6+oo, "\x1f", 19, 0, 0, 39 );
		printC64( kx+1, 7+oo, "RET", 20, 0, 0, 39 );

		printC64( kx + 1, 4+oo, "T", 15, 0, 0, 39 );
		printC64( kx, 5+oo, "(+/-)", 15, 0, 0, 39 );
	} else
	{
		printC64( kx, 4+oo, "(T/EXIT)", 15, 0, 0, 39 );
		printC64( kx+1, 4+oo, "T", 19, 0, 0, 39 );
		printC64( kx+4, 4+oo, "X", 20, 0, 0, 39 );
		printC64( kx, 5+oo, "(+/-)", meType == 2 ? 11 : 15, 0, 0, 39 );
		printC64( px, 7+oo, radImageSelectedPrint, meType == 2 ? 14 : 3, 0, 0, 39 );
		if ( radImageSelectedFile[ 0 ] != 0 || reu.isModified == meType + 1 )
			printC64( kx, 6+oo, "(Unmount)  ", meType == 2 ? 11 : 15, 0, 0, 39 ); else
			printC64( kx, 6+oo, "           ", meType == 2 ? 11 : 15, 0, 0, 39 );
		if ( meType != 2 )
		{
			printC64( kx + 1, 5+oo, "+", 21, 0, 0, 39 );
			printC64( kx + 3, 5+oo, "-", 22, 0, 0, 39 );
			if ( radImageSelectedFile[ 0 ] != 0 || reu.isModified == meType + 1 )
				printC64( kx+1, 6+oo, "U", 23, 0, 0, 39 );
		}
		printC64( kx, 7+oo, "           ", 15, 0, 0, 39 );
		if ( reu.isModified == meType + 1 )
		{
			printC64( kx - 1, 7+oo, " (Name&Save)", 15, 0, 0, 39 );
			printC64( kx + 1, 7+oo, "N", 24, 0, 0, 39 );
		}
	}
	screenUpdated = true;

	return 0;
}

#include "logo.h"
//u8 logo[ 320 * 21 ];
u8 font_logo[ 0x1000 ];
static u32 actLED_RegOffset, actLED_RegMask;

u16 sdLen = 0;
u8 spriteData[ 512 ];

u32 handleOneRasterLine( int fade1024, u8 fadeText = 1 )
{
	static u32 srCopy = 0, chCopy = 0, lgfCopy = 0, sprCopy = 0, fntCopy = 0;

	CACHE_PRELOADL2STRM( (u8 *)&c64ColorRAM[ srCopy ] );
	CACHE_PRELOADL2STRM( (u8 *)&c64ScreenRAM[ srCopy ] );
	CACHE_PRELOADL2STRM( (u8 *)&font_bin[ 64 * 8 + chCopy ] );

	static u32 rasterCount = 0;
	u32 v = ( (rasterCount++) >> 10 ) & 31;
	u16 ledActivityBrightness;
	if ( v < 16 )
		ledActivityBrightness = v; else
		ledActivityBrightness = 31 - v;

	static int bla = 0;
	bla ++; bla &= 31;
	if ( bla < ledActivityBrightness )
		{ write32( ARM_GPIO_GPSET0+ actLED_RegOffset, actLED_RegMask ); } else
		{ write32( ARM_GPIO_GPCLR0+ actLED_RegOffset, actLED_RegMask );	}

	int fade = fade1024 >> 8;

	u8 y;

	static u16 addr = 0;

#ifdef PLAY_MUSIC
	s16 raw = wavMemory[ wavPosition ];
	if ( fade1024 )
	{
		u8 sc = max( 0, 1024 - fade1024 );
		raw = ( (int)raw - 128 ) * sc / 1024 + 128;
	}

	register volatile u8 s;
	if ( supportDAC )
		s = raw; else
	if ( SIDType )
		s = mahoneyLUT[ raw ]; else
		s = raw >> 4; // 4-bit digi playing

	wavPosition ++;
	if ( wavPosition >= nWAVSamples )
		wavPosition = 0;

	CACHE_PRELOADL1STRM( &wavMemory[ wavPosition ] );
	CACHE_PRELOADL1STRM( &wavMemory[ wavPosition + 64 ] );

#else
	s16 raw = 128;
	u8 s = 0;
#endif

	BUS_RESYNC

	u16 curRasterLine;
	do {
		SPEEK( 0xd012, y );
		curRasterLine = y;
	} while ( curRasterLine == lastRasterLine );
	lastRasterLine = curRasterLine;

	SPEEK( 0xd011, y );
	if ( y & 128 ) curRasterLine += 256;

	if ( CPU_RESET )
		return RESET_DETECTED;

	bool badline = (curRasterLine & 7) == 3;

	if ( curRasterLine < 51 || curRasterLine > 250 ) 
		badline = false;

#ifdef PLAY_MUSIC
	SPOKE( 0xd418, s );
#endif

	if ( curRasterLine == rasterCommands[ curRasterCommand ][ 0 ] )
	{
		if ( curRasterCommand == 0 )
			frameCount ++;

		register volatile u8 x;

		switch ( rasterCommands[ curRasterCommand ][ 1 ] )
		{
		case 0: 
			x = fadeTabStep[ rasterCommands[ curRasterCommand ][ 2 ] ][ fade ];
			SPOKE( 0xd020, x );
			break;
		case 1:
			x = rasterCommands[ curRasterCommand ][ 2 ];
			SPOKE( 0xd018, x );
			break;
		default:
			break;
		};

		curRasterCommand ++;
		curRasterCommand %= nRasterCommands;
	} else
	{
		static u8 nthFrame = 0;
		static u8 c1, c2;
		register volatile u8 cc1, cc2;

		if ( curRasterLine == 34 )
		{
			c1 = fadeTabStep[ 15 ][ fade ];
			c2 = fadeTabStep[ 11 ][ fade ];
			SPOKE( 0xd015, 0b111111 );
		} else
		if ( curRasterLine >= 35 && curRasterLine < 38 )
		{
			u8 i = curRasterLine - 35;
			if ( fade1024 && fadeText )
			{
				cc1 = fadeTabStep[ c1 ][ fade ];
				cc2 = fadeTabStep[ c2 ][ fade ];
				SPOKE( 0xd027 + i, cc1 );
				SPOKE( 0xd02a + i, cc2 );
				SPOKE( SCREEN1 + 1024 - 8 + i, i );
				SPOKE( SCREEN1 + 1024 - 8 + i+3, i+3 );
			} else
			{
				cc1 = c1; cc2 = c2;
				SPOKE( 0xd027 + i, cc1 );
				SPOKE( 0xd02a + i, cc2 );
				SPOKE( SCREEN1 + 1024 - 8 + i, i );
				SPOKE( SCREEN1 + 1024 - 8 + i+3, i+3 );
			}
		} else
		if ( fade1024 && fadeText )
		{
			register volatile u8 x;
			//SPEEK( 0xd800 + addr, x );
			//SPOKE( 0xd800 + addr, fadeTab[ x & 15 ] );
			x = fadeTabStep[ c64ColorRAM[ addr ] ][ fade ];
			SPOKE( 0xd800 + addr, x );
			addr += 3;
			addr %= 1000;
		} else
		if ( curRasterLine == keyScanRasterLine && !fade1024 && ( ++nthFrame % 3 ) == 0 )
		{
			nthFrame = 0;
			u32 r = readKeyRenderMenu( fade );

			if ( r ) return r;
		} else
		{
			if ( !badline )
			{
				u32 bytesToCopyScreen = 4;
				u32 bytesToCopyOszi = 4;

				if ( screenUpdated && !fade1024 )
				{
					bytesToCopyScreen = min( bytesToCopyScreen, 1000 - srCopy );

					register volatile u32 d1 = *(u32*)&c64ColorRAM[ srCopy ];
					register volatile u32 d2 = *(u32*)&c64ScreenRAM[ srCopy ];

					register volatile u32 dx, cx = 0;
					register volatile u16 addr;
					if ( ( curRasterLine % 3 ) == 0 )
					{
						dx = *(u32*)&font_logo[ lgfCopy ]; 
						addr = CHARSET2 + lgfCopy;
						lgfCopy += 4;
						lgfCopy %= 1600;
					} else
					if ( ( curRasterLine % 3 ) == 1 )
					{
						dx = *(u32*)&spriteData[ sprCopy ]; 
						addr = 0x4000 + sprCopy;
						sprCopy += 4;
						sprCopy %= sdLen;
					} else
					{
						dx = *(u32*)&font_bin[ fntCopy + 2048 ];
						addr = CHARSET + 2048 + fntCopy;
						fntCopy += 4;
						fntCopy &= 2047;
					}

					BUS_RESYNC
					for ( register int i = 0; i < 4; i++ )
					{
						POKE( 0xd800 + srCopy + i, d1 & 255 ); d1 >>= 8;
						POKE( SCREEN1 + srCopy + i, d2 & 255 ); d2 >>= 8;
						if ( !refreshGraphic )
						{
							u8 t;
							PEEK( addr + i, t ); cx |= (u32)t << ( i * 8 );
						} else
						{
							POKE( addr + i, dx & 255 ); dx >>= 8;
						}
					}

					// TODO 
					if ( !refreshGraphic && cx != dx ) refreshGraphic = 1;

					srCopy += bytesToCopyScreen;
					if ( srCopy >= 1000 ) 
						srCopy = 0;
				} else
					bytesToCopyOszi = 8;


				bytesToCopyOszi = min(1280 - chCopy, bytesToCopyOszi);

				register volatile u32 d1 = *(u32*)&font_bin[ 64 * 8 + chCopy ];

				BUS_RESYNC
				for ( register int i = 0; i < 4; i++ )
				{
					POKE( CHARSET + 64 * 8 + chCopy + i, d1 & 255 ); d1 >>= 8;
				}
				// BUS_RESYNC
				// SMEMCPY( CHARSET + 64 * 8 + chCopy, (u8*)&font_bin[ 64 * 8 + chCopy ], bytesToCopyOszi );
				chCopy += bytesToCopyOszi;
				if ( chCopy >= 1280 ) chCopy = 0;
			} 
		}

		#if 1
		u16 oy = oszi[ osziPos ];
		oszi[ osziPos ] = raw >> 3;
		u16 ox = osziPos;
		osziPos ++; osziPos %= 320;

		// which char to modify?
		// (ox/8) + (oy/8) * 40
		u16 ox8 = ox / 8;
		u16 o = ox8*8 + (oy/8) * 320  // which char?
				+ (oy & 7);			  // which row inside char
		o += 64*8;
		u8 xc = 1 << (7-(ox & 7));

		font_bin[ o ] &= ~xc;
//		font_bin[ o ] |= xc;

		o = ox8*8 + ((raw>>3)/8) * 320  // which char?
				+ ((raw>>3) & 7);		// which row inside char
		o += 64*8;
		if ( ( ( ox8 == 0 || ox8 == 39 ) && ( xc & 0x11 ) ) ||
				( ( ox8 == 1 || ox8 == 38 ) && ( xc & 0x55 ) ) ||
				( ( ox8 == 2 || ox8 == 37 ) && ( xc & 0xdb ) ) ||
				( ox8 > 2 && ox8 < 37 ) )
//				font_bin[ o ] &= ~xc;
				font_bin[ o ] |= xc;

		if ( ox >= 319 - 72 )
		for ( int j = 0; j < 5; j++ )
			//for ( int i = 0; i < 72; i++ )
			{
				u16 oy = 32 - 5 + j;
				u16 i = ox - ( 319 - 72 );
				u16 ox8 = ox / 8;
				u16 o = ox8 * 8 + ( oy / 8 ) * 320  // which char?
					+ ( oy & 7 );			  // which row inside char
				o += 64 * 8;
				u8 xc = 1 << ( 7 - ( ox & 7 ) );
				if ( logo[ i + j * 320 + 76 ] )
					font_bin[ o ] |= xc;

			}
		#endif
		BUS_RESYNC
	}

	return 0;
}

void initHijack()
{
	extern CLogger	*logger;
	u32 size;

	initMenu();

	static const char DRIVE[] = "SD:";

	//readFile( logger, (char*)DRIVE, ( char* )"SD:RAD/font.bin", &font_bin[ 0 ], &size );
	//memcpy( font_bin + 2048 + 94 * 8, font_bin + 2048 + 233 * 8, 8 );

	//readFile( logger, (char*)DRIVE, ( char* )"SD:RAD/logo.raw", &logo[ 0 ], &size );

	#ifdef PLAY_MUSIC
	wavMemory = new u8[ 8192 * 1024 ];
	readFile( logger, (char*)DRIVE, ( char* )"SD:RAD/music.wav", wavMemory, &size );
	convertWAV2RAW_inplace( wavMemory );
	wavPosition = 0;
	#endif

	checkForRPiZero();

	u32 actLED_Info  = CMachineInfo::Get()->GetActLEDInfo() & ACTLED_PIN_MASK;
	actLED_RegOffset = ( actLED_Info / 32 ) * 4;
	actLED_RegMask   = 1 << ( actLED_Info & 31 );
}

void fadeScreen()
{
	u32 addr = 0;

	for ( u32 j = 0; j < 12; j++ )
	{
		u8 x;
		for ( u32 i = 0; i < 25; i++ )
		{
			do {
				SPEEK( 0xd012, x );
			} while ( x != 250 );
		}

		SPEEK( 0xd020, x );
		SPOKE( 0xd020, fadeTab[ x & 15 ] );
		SPEEK( 0xd021, x );
		SPOKE( 0xd021, fadeTab[ x & 15 ] );

		for ( u32 i = 0; i < 334; i++ )
		{
			SPEEK( 0xd800 + addr, x );
			SPOKE( 0xd800 + addr, fadeTab[ x & 15 ] );
			addr += 3;
			addr %= 1000;
		}
	}
}

int hijackC64( bool alreadyInDMA )
{
	register u32 g2;
	u8 x;

restartHijacking:

	if ( !alreadyInDMA )
		waitAndHijackMenu( g2 );

	SPOKE( 0xdc0d, 0x7f );
	SPOKE( 0xd01a, 0x01 );
	SPOKE( 0xd011, 0x1b );
	SPOKE( 0xd012, 0x10 );
	SPEEK( 0xdc0d, x );
	SPEEK( 0xdd0d, x );
	SPEEK( 0xd019, x );

	checkForC128();
	checkForNTSC();

	justBooted = 0;

	BUS_RESYNC
	SPOKE( 0xD418, 0 );

#ifdef PLAY_MUSIC
	wavPosition = 0;
#endif

	if ( meType == 3 )
		meType = old_meType;

	radLaunchPRG = false;
	radLaunchPRGFile[ 0 ] = 0; 

	static const char DRIVE[] = "SD:";

	extern void scanDirectoriesRAD( char *DRIVE );
	scanDirectoriesRAD( (char*)DRIVE );

#ifdef STATUS_MESSAGES
	if ( radImageSelectedFile[ 0 ] == '_' || radImageSelectedFile[ 0 ] == 0 )
		setStatusMessage( &statusMsg[ 0 ], " " );

	setStatusMessage( &statusMsg[ 80 ], " " );
#endif

	memset( oszi, 0, 320 );
	memset( c64ScreenRAM, 32, 1024 );
	memset( c64ColorRAM, 2, 1024 );

	for ( int i = 0; i < 4*40; i++ )
	{
		c64ScreenRAM[ osziPosY * 40 + i ] = i + 64;
		c64ColorRAM[ osziPosY * 40 + i ] = (i/40) == 0 || (i/40) == 3 ? 12 : 15;
	}

	printBrowser( 0 );

	CACHE_PRELOAD_DATA_CACHE( &oszi[ 0 ], 320, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( &font_bin[ 0 ], 4096, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( &c64ScreenRAM[ 0 ], 1024, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( &c64ColorRAM[ 0 ], 1024, CACHE_PRELOADL2KEEP )
	FORCE_READ_LINEAR32a( &oszi, 320, 320 * 8 );
	FORCE_READ_LINEAR32a( &font_bin, 4096, 4096 * 8 );
	FORCE_READ_LINEAR32a( &c64ScreenRAM, 1024, 1024 * 8 );
	FORCE_READ_LINEAR32a( &c64ColorRAM, 1024, 1024 * 8 );


	for ( int i = 0; i < 4 * 40 * 8; i++ )
		font_bin[ 64*8+i ] = 255*0;

	BUS_RESYNC

	fadeScreen();

	for ( u32 i = 0; i < 1000; i++ )
	{
		SPOKE( 0xd800 + i, 0 );
		SPOKE( SCREEN1 + i, 32 );

		// also clear original screen, not to be fooled by "READY." after reboot
		SPOKE( 0x0400 + i, 32 );
	}

	// initialize CIA2
	SPOKE( 0xdd02, 0x3f );
	SPOKE( 0xdd0d, 0x7f );
	SPOKE( 0xdd03, 0x06 );
	SPOKE( 0xdd01, 0x06 );

	PEEK( 0xdd00, x );
	x |= 4;
	SPOKE( 0xdd00, x );

	const u8 vic[] = { 
		0, 0, 0, 0, 0, 0, 0, 0, 
		0, 0, 0, 0, 0, 0, 0, 0, 
		0, 0x1B-0x10, 0, 0, 0, 0, 8, 0, 
		0x14*0+8, 0, 0, 0, 0, 0, 0, 0,
		0*14, 6*0, 1, 2, 3, 4, 0, 1, 
		2, 3, 4, 5, 6, 7
	};

	for ( int j = 0; j < 46; j++ )
		SPOKE( 0xd000 + j, vic[ j ] );

	SPOKE( 0xdd00, 0b11000000 | ( ( SCREEN1 >> 14 ) ^ 0x03 ) );
	SPOKE( 0xd018, PAGE1_LOWERCASE );

	// init SID
	BUS_RESYNC
	for ( int i = 0; i < 32; i++ )
		SPOKE( 0xd400 + i, 0 );


	static u8 detectSIDOnce = 1;

	if ( detectSIDOnce )
	{
		detectSIDOnce = 0;

		SIDKickVersion[ 0 ] = 0;
		supportDAC = hasSIDKick = 0;

		int j = 0; 
		while ( j++ < 16 && SIDType == 0 )
		{
			SPOKE( 0xd41f, 0xff );
			for ( int i = 0; i < 16 + 16; i++ )
			{
				SPOKE( 0xd41e, 224 + i );
				SPEEK( 0xd41d, *(u8*)&SIDKickVersion[ i ] );
			}

			// SIDKick (Teensy) ?
			if ( SIDKickVersion[ 0 ] == 0x53 &&
				SIDKickVersion[ 1 ] == 0x49 &&
				SIDKickVersion[ 2 ] == 0x44 &&
				SIDKickVersion[ 3 ] == 0x4b &&
				SIDKickVersion[ 4 ] == 0x09 &&
				SIDKickVersion[ 5 ] == 0x03 &&
				SIDKickVersion[ 6 ] == 0x0b )
			{
				// found SIDKick!
				SIDKickVersion[ 16 ] = 0; 
				hasSIDKick = 1;

				// let's see if it's version 0.21 (supporting direct DAC)
				const unsigned char VERSION_STR_ext[10] = { 0x53, 0x49, 0x44, 0x4b, 0x09, 0x03, 0x0b, 0x00, 0, 21 };

				supportDAC = 1;
				// check for signature
				for ( int i = 0; i < 8; i++ )
					if ( SIDKickVersion[ i + 20 ] != VERSION_STR_ext[ i ] )
						supportDAC = 0;
				
				if ( supportDAC )
				{
					int version = SIDKickVersion[ 20 + 8 ] * 100 + SIDKickVersion[ 20 + 9 ];
					if ( version < 21 )
						supportDAC = 0;
				}

				if ( supportDAC && SIDKickVersion[ 20 + 10 ] == 0 )
					supportDAC = 0;

				break;
			} else
			// SIDKick pico ?
			if ( SIDKickVersion[ 0 ] == 0x53 &&
				SIDKickVersion[ 1 ] == 0x4b &&
				SIDKickVersion[ 2 ] == 0x10 &&
				SIDKickVersion[ 3 ] == 0x09 &&
				SIDKickVersion[ 4 ] == 0x03 &&
				SIDKickVersion[ 5 ] == 0x0f )
			{
				// found SIDKick pico!
				SIDKickVersion[ 16 ] = 0; 
				hasSIDKick = 2;

				// let's see if it's version 0.21 (supporting direct DAC)
				//const unsigned char VERSION_STR_ext[10] = { 0x53, 0x49, 0x44, 0x4b, 0x09, 0x03, 0x0b, 0x00, 0, 21 };

				supportDAC = 0;
				if ( SIDKickVersion[ 30 ] )
					supportDAC = 2;

				break;
			} else
				SIDKickVersion[ 0 ] = 0;



			bool badline = false;

			do {
				u8 y;
				PEEK( 0xd012, y );
				u16 curRasterLine = y;
				do
				{
					PEEK( 0xd012, y );
				} while ( y == curRasterLine );

				badline = ( curRasterLine & 7 ) == 3;
			} while ( badline );

			u8 a1 = detectSID();
			u8 a2 = detectSID();
			u8 a3 = detectSID();

			if ( a1 == a2 && a2 == a3 )
				SIDType = a1; else		// detection succesful: 6581 or 8580
				SIDType = 0;			// no success => maybe SwinSID
		}
	}

	u16 addr = 0x4000;
	sdLen = 0;
	for ( int i = 0; i < 6; i++ )
	{
		u8 c = 2 + (i / 3);
		int ox = (i%3) * 24;

		for ( int y = 0; y < 21; y++ )
		{
			int x = ox;
			for ( int b = 0; b < 3; b++ )
			{
				u8 v = 0;
				for ( int a = 0; a < 8; a++ ) v |= logo[ (x++) + y * 320 ] == c ? (1<<(7-a)) : 0; 
				SPOKE( addr, v );
				spriteData[ sdLen ++ ] = v;
				addr ++;				
			}
		}
		SPOKE( addr, 0 );
		spriteData[ sdLen ++ ] = 0;
		addr ++;
	}

	for ( int i = 0; i < 40 * 4; i++ )
	{
		c64ScreenRAM[ i ] = i;
		c64ColorRAM[ i ] = 1;
	}

	addr = 0x7800;
	for ( int i = 0; i < 1024; i++ )
		SPOKE( addr + i, 0 );

	memset( font_logo, 0, 0x1000 );

	for ( int i = 0; i < 72; i++ )
	for ( int j = 0; j < 21; j++ )
	{
		int ox = i + 124, oy = j + 3+3-2;

		if ( logo[ i + j * 320 ] == 1 )
		{
			u16 ox8 = ox / 8;
			u16 o = ox8*8 + (oy/8) * 320  // which char?
					+ (oy & 7);			  // which row inside char
			u8 xc = 1 << (7-(ox & 7));

			font_logo[ o ] |= xc;
		}
	}
	
	CACHE_PRELOAD_DATA_CACHE( &font_logo[ 0 ], 4096, CACHE_PRELOADL2KEEP )
	FORCE_READ_LINEAR32a( &font_logo, 4096, 4096 * 8 );
	BUS_RESYNC
	SMEMCPY( CHARSET2, &font_logo[0], 0x1000*0+1600 );

	CACHE_PRELOAD_DATA_CACHE( &font_bin[ 0 ], 4096, CACHE_PRELOADL2KEEP )
	FORCE_READ_LINEAR32a( &font_bin, 4096, 4096 * 8 );
	BUS_RESYNC
	SMEMCPY( CHARSET, &font_bin[0], 0x1000 );


	SPOKE( 0xdc03, 0 );		// port b ddr (input)
	SPOKE( 0xdc02, 0xff );	// port a ddr (output)
	SPOKE( 0xd016, 8 );
	SPOKE( 0xd021, 0 );
	SPOKE( 0xd011, 0x1b );
	SPOKE( 0xdd00, 0b11000000 | ((SCREEN1 >> 14) ^ 0x03) );
	// sprites
	u32 o = 56+92;
	SPOKE( 0xd000, o+0 );  SPOKE( 0xd001, 53+1 );
	SPOKE( 0xd002, o+24 ); SPOKE( 0xd003, 53+1 );
	SPOKE( 0xd004, o+48 ); SPOKE( 0xd005, 53+1 );
	//o += 72;
	SPOKE( 0xd006, o+0 );  SPOKE( 0xd007, 53+1 );
	SPOKE( 0xd008, o+24 ); SPOKE( 0xd009, 53+1 );
	SPOKE( 0xd00a, o+48 ); SPOKE( 0xd00b, 53+1 );
	SPOKE( 0xd010, 0 );
	SPOKE( 0xd01c, 0 );
	// u8 c1 = fadeTabStep[ 15 ][ 0 ];
	// u8 c2 = fadeTabStep[ 11 ][ 0 ];
	// c1 = c2 = 0;
	// keep sprites black for now (will be colored on-the-fly)
	for ( int i = 0; i < 3; i++ )
	{
		SPOKE( 0xd027 + i, 0 );
		SPOKE( 0xd02a + i, 0 );
	}
	SPOKE( 0xd015, 0b111111 );
	for ( int i = 0; i < 6; i++ )
		SPOKE( SCREEN1 + 1024 - 8 + i, i );


	BUS_RESYNC
	POKE( 0xd011, 0x1b );

	lastRasterLine = 1234;

	screenUpdated = true;

	imageNameEdit = 0;
	imageNameStr[ 0 ] = 0;
	imageNameStrLength = 0;

	extern CLogger	*logger;
	char imgFileName[ 512 ];
	u32 imgSize;

	// init SID
	BUS_RESYNC
	for ( int i = 0; i < 32; i++ )
		SPOKE( 0xd400 + i, 0 );

	#ifdef PLAY_MUSIC
	BUS_RESYNC
	if ( SIDType == 0 && !supportDAC )
	{
		SPOKE( 0xd405, 0 );
		SPOKE( 0xd406, 0xff );
		SPOKE( 0xd40d, 0xff );
		SPOKE( 0xd414, 0xff );
		SPOKE( 0xd404, 0x49 );
		SPOKE( 0xd40b, 0x49 );
		SPOKE( 0xd412, 0x49 );
		SPOKE( 0xd40c, 0 );
		SPOKE( 0xd413, 0 );
		SPOKE( 0xd415, 0 );
		SPOKE( 0xd416, 0x10 );
		SPOKE( 0xd417, 0xf7 );
	} else
	{
		// use SIDKick's pure DAC mode if supported
		if ( supportDAC == 1 )
		{
			SPOKE( 0xd41f, 0xfc );
		} else
		if ( supportDAC == 2 ) // SKpico
		{
			u8 x;
			do {
				SPEEK( 0xd41d, x );
			} while ( x != 0x4c );
			SPOKE( 0xd41f, 0xfc );
		} else
		{
			// Mahoney's technique
			SPOKE( 0xd405, 0x0f );
			SPOKE( 0xd40c, 0x0f );
			SPOKE( 0xd413, 0x0f );
			SPOKE( 0xd406, 0xff );
			SPOKE( 0xd40d, 0xff );
			SPOKE( 0xd414, 0xff );
			SPOKE( 0xd404, 0x49 );
			SPOKE( 0xd40b, 0x49 );
			SPOKE( 0xd412, 0x49 );
			SPOKE( 0xd415, 0xff );
			SPOKE( 0xd416, 0xff );
			SPOKE( 0xd417, 0x03 );
		}
	}

	mahoneyLUT = ( SIDType == (6581 & 255) ) ? lookup6581 : lookup8580;
	#endif


	CACHE_PRELOAD_DATA_CACHE( (u8*)&font_bin[ 0 ], 4096, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( (u8*)&oszi[ 0 ], 320, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( (u8*)&c64ScreenRAM[ 0 ], 1024, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( (u8*)&c64ColorRAM[ 0 ], 1024, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( (u8*)&font_bin[ 0 ], 256, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( (u8*)rasterCommands, 256, CACHE_PRELOADL2KEEP )

	CACHE_PRELOAD_DATA_CACHE( (u8*)fadeTabStep, 128, CACHE_PRELOADL2KEEP )
	CACHE_PRELOAD_DATA_CACHE( (u8*)colorCycle, 512, CACHE_PRELOADL2KEEP )


	while ( 1 )
	{
		u32 r = handleOneRasterLine( 0 );

		u8 t;
		SPEEK( 0xd020, t );

		int i;

		switch ( ( r & RUN_FLAGS ) )
		{
		case RESET_DETECTED:
			SET_GPIO(bLATCH_A_OE | bIRQ_OUT | bOE_Dx | bRW_OUT |bDMA_OUT);
			INP_GPIO(RW_OUT);
			INP_GPIO(IRQ_OUT);
			do {
				g2 = read32(ARM_GPIO_GPLEV0);
			} while (CPU_RESET);
			DELAY(1 << 27);
			alreadyInDMA = false;
			goto restartHijacking;
			break; 
		#ifdef DEBUG_REBOOT_RPI_ON_R
		case RUN_REBOOT:
			r = RUN_REBOOT;
		#endif
		case RUN_MEMEXP:
			for ( i = 4; i < 1024 * 6; i ++ )
				handleOneRasterLine( i >> 2 );
			return r;
		case SAVE_IMAGE:
			// fade out
			for ( i = 4; i < 1024 * 6; i ++ )
				handleOneRasterLine( i >> 2, 1 );

			if ( meType == 0 ) // REU
			{
				sprintf( imgFileName, "SD:REU/%s.reu", imageNameStr );
				extern u8 *mempoolPtr;
				imgSize = ( 128 << meSize0 ) * 1024;
				writeFile( logger, DRIVE, imgFileName, mempoolPtr, imgSize );
			} else
			if ( meType == 1 ) // GeoRAM
			{
				sprintf( imgFileName, "SD:GEORAM/%s.georam", imageNameStr );
				extern u8 *mempoolPtr;
				imgSize = ( 512 << meSize1 ) * 1024;
				writeFile( logger, DRIVE, imgFileName, mempoolPtr, imgSize );
			} 
				
			reu.isModified  = false;
			scanDirectoriesRAD( (char*)DRIVE );

			// fade in 
			for ( i = 1024 * 6; i >= 0; i -- )
				handleOneRasterLine( i >> 2, 1 );
			break;
		case SAVE_CONFIG:
			// fade out
			for ( i = 4; i < 1024 * 6; i ++ )
				handleOneRasterLine( i >> 2, 1 );

			int timingValues[ 21 ];
			transferTimingValues( timingValues );

			extern int changeTimingsInConfig( CLogger *logger, const char *DRIVE, const char *FILENAME, int *newTimingValues );
			changeTimingsInConfig( logger, DRIVE, FILENAME_CONFIG, timingValues );

			// fade in 
			for ( i = 1024 * 6; i >= 0; i -- )
				handleOneRasterLine( i >> 2, 1 );
			break;
		default:
			break;
		}
	}
	return 0;
}

struct WAVHEADER
{
	u8  riff[ 4 ];
	u32 filesize;
	u8  wave[ 4 ];
	u8  fmtChunkMarker[ 4 ];
	u32 fmtLength;
	u32 fmtType;
	u32 nChannels;
	u32 sampleRate;
	u32 byteRate;
	u32 blockAlign;
	u32 bpp;
	u8  dataChunkHeader[ 4 ];
	u32 dataSize;
};

static struct WAVHEADER header;

static u8 buffer4[ 4 ];
static u8 buffer2[ 2 ];

static void convertWAV2RAW_inplace( u8 *_data )
{
	u8 *data = _data;
	u8 *rawOut = data;

	#define FREAD( dst, s ) { memcpy( (u8*)dst, data, s ); data += s; }

	FREAD( header.riff, sizeof( header.riff ) );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.filesize = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	FREAD( header.wave, sizeof( header.wave ) );

	FREAD( header.fmtChunkMarker, sizeof( header.fmtChunkMarker ) );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.fmtLength = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	FREAD( buffer2, sizeof( buffer2 ) );
	header.fmtType = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( buffer2, sizeof( buffer2 ) );
	header.nChannels = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.sampleRate = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	// ... = header.sampleRate;

	FREAD( buffer4, sizeof( buffer4 ) );
	header.byteRate = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	FREAD( buffer2, sizeof( buffer2 ) );

	header.blockAlign = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( buffer2, sizeof( buffer2 ) );
	header.bpp = buffer2[ 0 ] | ( buffer2[ 1 ] << 8 );

	FREAD( header.dataChunkHeader, sizeof( header.dataChunkHeader ) );

	FREAD( buffer4, sizeof( buffer4 ) );
	header.dataSize = buffer4[ 0 ] | ( buffer4[ 1 ] << 8 ) | ( buffer4[ 2 ] << 16 ) | ( buffer4[ 3 ] << 24 );

	long num_samples = ( 8 * header.dataSize ) / ( header.nChannels * header.bpp );

	long size_of_each_sample = ( header.nChannels * header.bpp ) / 8;

	// duration in secs: (float)header.filesize / header.byteRate;

#ifdef PLAY_MUSIC
	nWAVSamples = 0;
#endif

	if ( header.fmtType == 1 ) // PCM
	{
		char data_buffer[ size_of_each_sample ];

		long bytes_in_each_channel = ( size_of_each_sample / header.nChannels );

		if ( ( bytes_in_each_channel  * header.nChannels ) == size_of_each_sample ) // size if correct?
		{
			for ( u32 i = 1; i <= num_samples; i++ )
			{
				FREAD( data_buffer, sizeof( data_buffer ) );

				unsigned int  xnChannels = 0;
				int data_in_channel = 0;
				int offset = 0; // move the offset for every iteration in the loop below

				for ( xnChannels = 0; xnChannels < header.nChannels; xnChannels++ )
				{
					if ( bytes_in_each_channel == 4 )
					{
						data_in_channel = ( data_buffer[ offset ] & 0x00ff ) | ( ( data_buffer[ offset + 1 ] & 0x00ff ) << 8 ) | ( ( data_buffer[ offset + 2 ] & 0x00ff ) << 16 ) | ( data_buffer[ offset + 3 ] << 24 );
						data_in_channel += 2147483648;
						data_in_channel >>= 24;
					} else
					if ( bytes_in_each_channel == 2 )
					{
						data_in_channel = ( data_buffer[ offset ] & 0x00ff ) | ( data_buffer[ offset + 1 ] << 8 );
						data_in_channel += 32768;
						data_in_channel >>= 8;
					} else
					if ( bytes_in_each_channel == 1 )
					{
						data_in_channel = data_buffer[ offset ] & 0x00ff; // 8 bit unsigned
					}

					if ( xnChannels == 0 )
					{
						*rawOut = (u8)data_in_channel;
						rawOut ++;
						#ifdef PLAY_MUSIC
						nWAVSamples ++;
						#endif
					}

					// if stereo => mix with channel #0
					if ( xnChannels == 1 )
					{
						u16 t = *( rawOut - 1 );
						t += (u16)data_in_channel;
						*( rawOut - 1 ) = (u8)( t >> 1 );
					}

					offset += bytes_in_each_channel;
				}
			}
		}
	}
}

//
//
// VSF (Vice-Snapshot) Injection 
//
//

int substr( char *str, char *strsearch )
{
    char *t = strstr( str, strsearch );
    return (u64)t - (u64)str;
}

bool substr( char *str, int ofs, char *strsearch )
{
    char *t = strstr( str + ofs, strsearch );
    if ( t != NULL ) return true;
    return false;
}

u8 *getVSFModule( u8 *vsf, int vsfSize, char *modulename )
{
    int i = 0x3a, modLen;
    bool found = false;
    u8 *p = NULL;
    do {
        modLen = (int)vsf[ i + 0x12 ] + ( (int)vsf[ i + 0x13 ] << 8 ) +
                 ( (int)vsf[ i + 0x14 ] << 16 ) + ( (int)vsf[ i + 0x15 ] << 24 );
        
        if ( substr( (char*)vsf, i, modulename ) )
        {
            found = true;
            p = &vsf[ i ];
        } else
        {
            i += modLen;
        }
    } while ( !found && ( i < vsfSize ) );
    
    return p;
}

#include "C64Side/ultimax_vsf.h"

const int patchOffset = 9;

static int nRasterNOPs = 0;
static u8 code[ 256 ];
static int vsfRasterLine, vsfRasterCycle;
static u8 *vsfREU = NULL;

void __attribute__ ((noinline)) doInjectionVSF( u8 *vsf, u32 vsfSize )
{
	u8 *cpu, *mem, *cia1, *cia2, *sid, *vic;

	INP_GPIO( GAME_OUT );
	SET_GPIO( bGAME_OUT );

	bool isC128Snapshot = false;

	mem = getVSFModule( vsf, vsfSize, (char *)"C64MEM" );

	if ( mem != NULL )
	{
		mem += VSF_SIZE_MODULE_HEADER;
	} else
	{
		mem = getVSFModule( vsf, vsfSize, (char *)"C128MEM" ) + VSF_SIZE_MODULE_HEADER + 11;	
		if ( mem == NULL )
			return;

		if ( mem[ 0 ] == 0 && mem[ 1 ] == 0 ) // catch Vice bug, and write often-correct port ddr/bits
		{
			mem[ 0 ] = 0x2f;
			mem[ 1 ] = 0x75;
		}

		isC128Snapshot = true;
	}

	if ( isC128Snapshot )
	{
		//mem = getVSFModule( vsf, vsfSize, (char *)"C128MEM" ) + VSF_SIZE_MODULE_HEADER + 11;
		for ( int i = 2; i < 65536; i++ )
			SPOKE_NG( i, mem[ i ] );
	} else
	{
		// offset memory dump = 26 = VSF_SIZE_MODULE_HEADER + 4
		// offset cpu port 0 and 1 = 22 and 23 = VSF_SIZE_MODULE_HEADER
		// EXROM / GAME = 24 and 25 = VSF_SIZE_MODULE_HEADER + 2
		//	mem = getVSFModule( vsf, vsfSize, (char *)"C64MEM" ) + VSF_SIZE_MODULE_HEADER;
		u8 *ppp = mem + 4;
		for ( int i = 2; i < 65536; i++ )
			SPOKE_NG( i, ppp[ i ] );

	}

	// Ultimax mode => write to IO
	OUT_GPIO( GAME_OUT );
	CLR_GPIO( bGAME_OUT );

	cia1 = getVSFModule( vsf, vsfSize, (char *)"CIA1" ) + VSF_SIZE_MODULE_HEADER;

	SPOKE_NG( 0xdc0d, 0x1f );
	SPOKE_NG( 0xdc0e, 0x00 );
	SPOKE_NG( 0xdc0f, 0x00 );

	for ( int i = 0; i < 13; i++ )
		SPOKE_NG( 0xdc00 + i, cia1[ i ] );

	SPOKE_NG( 0xdc0d, cia1[ 13 ] | 0x80 );

	SPOKE_NG( 0xdc0e, 0x10 );
	SPOKE_NG( 0xdc0f, 0x10 );

	cia2 = getVSFModule( vsf, vsfSize, (char *)"CIA2" ) + VSF_SIZE_MODULE_HEADER;

	SPOKE_NG( 0xdd0d, 0x1f );
	SPOKE_NG( 0xdd0e, 0x00 );
	SPOKE_NG( 0xdd0f, 0x00 );

	for ( int i = 0; i < 13; i++ )
		SPOKE_NG( 0xdd00 + i, cia2[ i ] );

	SPOKE_NG( 0xdd0d, cia2[ 13 ] | 0x80 );

	SPOKE_NG( 0xdd0e, 0x10 );
	SPOKE_NG( 0xdd0f, 0x10 );

	sid  = getVSFModule( vsf, vsfSize, (char *)"SIDEXTENDED" ) + VSF_SIZE_MODULE_HEADER;
	for ( int i = 0; i < 0x20; i++ )
		SPOKE_NG( 0xd400 + i, sid[ i ] );

	vic  = getVSFModule( vsf, vsfSize, (char *)"VIC-II" ) + VSF_SIZE_MODULE_HEADER;

	if ( isC128Snapshot ) // Vice uses the old VIC-II emulation (not the "SC" one)
	{
		for ( int i = 0; i < 0x2f; i++ )
			SPOKE_NG( 0xd000 + i, vic[ i + 1119 ] );

		for ( int i = 0; i < 0x400; i++ )
			SPOKE_NG( 0xd800 + i, vic[ i + 43 ] );
	} else
	{
		for ( int i = 0; i < 0x2f; i++ )
			SPOKE_NG( 0xd000 + i, vic[ i + 1 ] );

		for ( int i = 0; i < 0x400; i++ )
			SPOKE_NG( 0xd800 + i, vic[ i + 761 ] );
	}

	// 
	// create reanimation code for execution on C64-CPU
	//
	u8 *c = &code[ patchOffset ];

	// CIAs
	#define CODE_POKE( A, D ) { \
		*(c ++) = 0xa9; *(c ++) = (D); \
		*(c ++) = 0x8d; *(c ++) = (A) & 255; *(c ++) = ( (A) >> 8 ); }

	CODE_POKE( 0xdc0e, cia1[ 14 ] );
	CODE_POKE( 0xdc0f, cia1[ 15 ] );
	CODE_POKE( 0xdd0e, cia2[ 14 ] );
	CODE_POKE( 0xdd0f, cia2[ 15 ] );

	for ( int i = 0; i < 4; i++ )
		CODE_POKE( 0xdc04 + i, cia1[ 16 + i ] );
	for ( int i = 0; i < 4; i++ )
		CODE_POKE( 0xdd04 + i, cia2[ 16 + i ] );

	// CPU ports
	if ( isC128Snapshot )
	{
		CODE_POKE( 1, mem[ 1 ] );
		CODE_POKE( 0, mem[ 0 ] );
	} else
	{
		CODE_POKE( 1, mem[ 0 ] );
		CODE_POKE( 0, mem[ 1 ] );
	}

	// wait for raster
	int line = vic[ 0x49 ];
	if ( vic[ 0x48 ] & 128 ) line += 256;

	int rastercycle = ( (int)vic[ 0x40 ] ) + ( (int)vic[ 0x44 ] << 8 );

	vsfRasterLine = line;
	vsfRasterCycle = rastercycle;

	nRasterNOPs = ( ( rastercycle & 63 ) / 2 ) + 21;

	// isNTSC == 0 => PAL: 312 rasterlines, 63 cycles
	// isNTSC == 1 => NTSC: 262 (0..261) rasterlines, 64 cycles, 6567R56A
	// isNTSC == 2 => NTSC: 263 (0..262) rasterlines, 65 cycles, 6567R8

	int maxRasterLines;
	switch ( isNTSC )
	{
		default:
		case 0: maxRasterLines = 312; break;
		case 1: maxRasterLines = 262; break;
		case 2: maxRasterLines = 263; break;
	}
	line --; if ( line < 0 ) line = maxRasterLines - 1;
	if ( line >= (maxRasterLines - 1) ) line = maxRasterLines - 1;

	/*
	wait for rasterline:	line < 256		line >= 256
	.C:1000  2C 11 D0    	BIT $D011
	.C:1003  10 FB       	BPL $1000		BMI (0x30)
	.C:1005  2C 11 D0    	BIT $D011
	.C:1008  30 FB       	BMI $1005		BPL (0x10)
	.C:100a  A2 20       	LDX #$20
	.C:100c  EC 12 D0    	CPX $D012
	.C:100f  D0 FB       	BNE $100C
	*/

	*( c++ ) = 0x2c; *( c++ ) = 0x11; *( c++ ) = 0xd0;
	if ( line < 256 )
	{
		*( c++ ) = 0x10; *( c++ ) = 0xfb;
		*( c++ ) = 0x2c; *( c++ ) = 0x11; *( c++ ) = 0xd0;
		*( c++ ) = 0x30; *( c++ ) = 0xfb;
	} else
	{
		*( c++ ) = 0x30; *( c++ ) = 0xfb;
		*( c++ ) = 0x2c; *( c++ ) = 0x11; *( c++ ) = 0xd0;
		*( c++ ) = 0x10; *( c++ ) = 0xfb;
	}
	*( c++ ) = 0xa2; *( c++ ) = ( line & 255 );
	*( c++ ) = 0xec; *( c++ ) = 0x12; *( c++ ) = 0xd0;
	*( c++ ) = 0xd0; *( c++ ) = 0xfb;

	for ( int i = 0; i < nRasterNOPs; i++ )
		*( c++ ) = 0xea;	

	// CPU reanimation
	cpu = getVSFModule( vsf, vsfSize, (char *)"MAINCPU" ) + VSF_SIZE_MODULE_HEADER;
	*(c ++) = 0xa2; *(c ++) = cpu[ 11 ];     					// LDX #sp
	*(c ++) = 0x9a;                       						// TXS
	*(c ++) = 0xa9; *(c ++) = cpu[ 14 ];       					// LDA #flags
	*(c ++) = 0x48;                       						// PHA
	*(c ++) = 0xa9; *(c ++) = cpu[ 8 ];        					// LDA #...
	*(c ++) = 0xa2; *(c ++) = cpu[ 9 ];        					// LDX #...
	*(c ++) = 0xa0; *(c ++) = cpu[ 10 ];       					// LDY #...
	*(c ++) = 0x28;                       						// PLP
	*(c ++) = 0x4c; *(c ++) = cpu[ 12 ]; *(c ++) = cpu[ 13 ]; 	// JMP addr

	vsfREU = getVSFModule( vsf, vsfSize, (char *)"REU1764" );

	if ( vsfREU )
	{
		SyncDataAndInstructionCache();
		extern void warmCache();
		warmCache();

		CACHE_PRELOAD_INSTRUCTION_CACHE( (void*)reuUsingPolling, 1024 * 7 );
		FORCE_READ_LINEARa( (void*)reuUsingPolling, 1024 * 7, 65536 );

		reuUsingPolling( 1 );
	}
}


void resetAndInjectVSF( u8 *vsf, u32 vsfSize )
{
	register u32 g2, g3;


restartInjection:
	SET_GPIO( bLATCH_A_OE | bIRQ_OUT | bOE_Dx | bRW_OUT );
	INP_GPIO( RW_OUT );
	INP_GPIO( IRQ_OUT );
	OUT_GPIO( RESET_OUT );
	OUT_GPIO( GAME_OUT );
	CLR_GPIO( bRESET_OUT | bGAME_OUT | bDMA_OUT );

	memcpy( code, ultimax_vsf, 256 );

	nRasterNOPs = 0;

	int oneTimeOffset = patchOffset - 1;

	int cycleCounter = 0;

restartUltimaxCartridge:
	CACHE_PRELOAD_DATA_CACHE( &code[ 0 ], 256, CACHE_PRELOADL2KEEP )
	FORCE_READ_LINEAR32a( &code, 256, 256 * 32 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( &&vsfCRTCFG, 4096 );
	CACHE_PRELOAD_INSTRUCTION_CACHE( &&vsfCRTCFG, 4096 );

	DELAY( 1 << 20 );

vsfCRTCFG:
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER
	WAIT_FOR_VIC_HALFCYCLE
	SET_GPIO( bRESET_OUT | bDMA_OUT );
	INP_GPIO( RESET_OUT );
	CLR_GPIO( bGAME_OUT );


	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER						
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS+ TIMING_OFFSET_CBTD );
		g2 = read32( ARM_GPIO_GPLEV0 );

		SET_GPIO( bMPLEX_SEL );
		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER );
		
		g3 = read32( ARM_GPIO_GPLEV0 );
		CLR_GPIO( bMPLEX_SEL );

		u8 addr = ADDRESS0to7;
		u8 access = 0;

		if ( ADDRESS_FFxx && CPU_READS_FROM_BUS )
		{
			u8 D = code[ addr ];

			register u32 DD = ( ( D ) & 255 ) << D0;
			write32( ARM_GPIO_GPCLR0, ( D_FLAG & ( ~DD ) ) | bOE_Dx | bDIR_Dx );
			write32( ARM_GPIO_GPSET0, DD );
			SET_BANK2_OUTPUT
			WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ );
			SET_GPIO( bOE_Dx | bDIR_Dx );

			access = 1;
		}

		WAIT_FOR_VIC_HALFCYCLE
		cycleCounter ++;

		if ( access && addr == oneTimeOffset )
		{
			oneTimeOffset = 0x100000;

			// note: $00/$01 are configured for full RAM access if no cartridge is active,
			//       access to IO is done using the Ultimax mode (GAME low)
			CLR_GPIO( bDMA_OUT );
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			
			doInjectionVSF( vsf, vsfSize );

			goto restartUltimaxCartridge;
		}

		// # instructions
		// CPU reanimation		16
		// wait raster 			17		
		// nRasterNOPs			n
		// CODE_POKE			14 * 5
		if ( access && addr == patchOffset + 16 + 14 * 5 + 17 + nRasterNOPs - 1 )
		{
			SET_GPIO( bDMA_OUT );
			INP_GPIO( GAME_OUT );
			SET_GPIO( bGAME_OUT );

			if ( vsfREU )
				reuUsingPolling( 2 );

			return;
		}

		if ( cycleCounter > 1000000 )
			goto restartInjection;
	}
}

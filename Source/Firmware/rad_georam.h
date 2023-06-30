/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
        - GeoRAM emulation (based on my Sidekick64 emulation)
 Copyright (c) 2022 Carsten Dachsbacher <frenetic@dachsbacher.de>

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
// size of GeoRAM/NeoRAM in Kilobytes
#define MAX_GEORAM_SIZE 4096
u32 geoSizeKB = 4096;

typedef struct
{
	// GeoRAM registers
	// $dffe : selection of 256 Byte-window in 16 Kb-block
	// $dfff : selection of 16 Kb-nlock
	u8  reg[ 2 ];	

	// for rebooting the RPi
	u64 c64CycleCount;
	u32 resetCounter;
	u32 nBytesRead, stage;

	u8 *RAM AA;

	u32 saveRAM, releaseDMA;
	u8 isModified;

	u8 padding[ 52 - 10 + 17 ];
} __attribute__((packed)) GEOSTATE;

volatile static GEOSTATE geo AAA;

// geoRAM memory pool 
extern u8* mempoolPtr;
static u8  *geoRAM_Pool = (u8*)mempoolPtr;

// u8* to current window
#define GEORAM_WINDOW (&geo.RAM[ ( geo.reg[ 1 ] * 16384 ) + ( geo.reg[ 0 ] * 256 ) ])

// geoRAM helper routines
static void geoRAM_Init()
{
	geo.reg[ 0 ] = geo.reg[ 1 ] = 0;
	geo.RAM = &geoRAM_Pool[0]; //(u8*)( ( (u64)&geoRAM_Pool[0] + 128 ) & ~127 );
	memset( geo.RAM, 0, geoSizeKB * 1024 );

	geo.c64CycleCount = 0;
	geo.resetCounter = 0;
	geo.nBytesRead = 0;
	geo.stage = 0;
	geo.saveRAM = 0;
	geo.releaseDMA = 0;
	geo.isModified = 0;
}

__attribute__( ( always_inline ) ) inline u8 geoRAM_IO2_Read( u32 A )
{
    if ( A < 2 ) return geo.reg[ A & 1 ];
	return 0;
}

__attribute__( ( always_inline ) ) inline void geoRAM_IO2_Write( u32 A, u8 D )
{
	if ( ( A & 1 ) == 1 )
		geo.reg[ 1 ] = D & ( ( geoSizeKB / 16 ) - 1 ); else
		geo.reg[ 0 ] = D & 63;
}


void geoRAMUsingPolling()
{
	register u32 D, g2, g3;

	CLR_GPIO( bMPLEX_SEL );
	WAIT_FOR_CPU_HALFCYCLE
	BEGIN_CYCLE_COUNTER						
	WAIT_FOR_VIC_HALFCYCLE

	while ( 1 )
	{
		WAIT_FOR_CPU_HALFCYCLE
		RESTART_CYCLE_COUNTER						
		WAIT_UP_TO_CYCLE( WAIT_FOR_SIGNALS+TIMING_OFFSET_CBTD );	
		g2 = read32( ARM_GPIO_GPLEV0 );			
	
		SET_GPIO( bMPLEX_SEL );

		// let's use this time to preload the cache (L1-cache, streaming)
		CACHE_PRELOADL2STRM( GEORAM_WINDOW[ 0 ] );
		CACHE_PRELOADL2STRM( GEORAM_WINDOW[ 64 ] );
		CACHE_PRELOADL2STRM( GEORAM_WINDOW[ 128 ] );
		CACHE_PRELOADL2STRM( GEORAM_WINDOW[ 192 ] );

		WAIT_UP_TO_CYCLE( WAIT_CYCLE_MULTIPLEXER ); 
		g3 = read32( ARM_GPIO_GPLEV0 );			
		CLR_GPIO( bMPLEX_SEL );

		//CACHE_PRELOADL1STRM( &GEORAM_WINDOW[ GET_IO12_ADDRESS ] );
		//payload = GEORAM_WINDOW[ GET_IO12_ADDRESS ];

		if ( IO1_OR_IO2_ACCESS )
		{
			if ( CPU_READS_FROM_BUS )	// CPU reads from memory page or register
			{
				if ( IO1_ACCESS )	
					// GeoRAM read from memory page
					D = GEORAM_WINDOW[ GET_IO12_ADDRESS ]; else
					// GeoRAM read register (IO2_ACCESS)
					D = geoRAM_IO2_Read( GET_IO12_ADDRESS );

				// write D0..D7 to bus
				PUT_DATA_ON_BUS( D )
			} else
			// CPU writes to memory page or register // CPU_WRITES_TO_BUS is always true here
			{
				// read D0..D7 from bus
				GET_DATA_FROM_BUS( D )

				if ( IO1_ACCESS )	
				{
					// GeoRAM write to memory page
					GEORAM_WINDOW[ GET_IO12_ADDRESS ] = D; 
					geo.isModified = 2;
				} else
				{
					// GeoRAM write register (IO2_ACCESS)
					geoRAM_IO2_Write( GET_IO12_ADDRESS, D );
					CACHE_PRELOADL1STRM( GEORAM_WINDOW[ 0 ] );
					CACHE_PRELOADL1STRM( GEORAM_WINDOW[ 64 ] );
					CACHE_PRELOADL1STRM( GEORAM_WINDOW[ 128 ] );
					CACHE_PRELOADL1STRM( GEORAM_WINDOW[ 192 ] );
					FORCE_READ_LINEAR32_SKIP( GEORAM_WINDOW, 256 );
				}
			}
		}

		if ( BUTTON_PRESSED ) 
			return;

		WAIT_FOR_VIC_HALFCYCLE
		RESET_CPU_CYCLE_COUNTER					
	}
}
 
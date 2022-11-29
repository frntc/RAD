/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
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
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/util.h>
#include "lowlevel_arm64.h"
#include "config.h"
#include "helpers.h"
#include "linux/kernel.h"

u32 radStartup = 0, radStartupSize = 0;

int atoi( char* str )
{
	int res = 0;
	for ( int i = 0; str[ i ] != '\0' && str[ i ] != 10 && str[ i ] != 13; i ++ )
		if ( str[ i ] >= '0' && str[ i ] <= '9' )
			res = res * 10 + str[ i ] - '0';
	return res;
}

char *cfgPos;
char curLine[ 2048 ];

int getNextLine()
{
	memset( curLine, 0, 2048 );

	int sp = 0, ep = 0;
	while ( cfgPos[ ep ] != 0 && cfgPos[ ep ] != '\n' ) ep++;

	while ( sp < ep && ( cfgPos[ sp ] == ' ' || cfgPos[ sp ] == 9 ) ) sp++;

	strncpy( curLine, &cfgPos[ sp ], ep - sp - 1 );

	cfgPos = &cfgPos[ ep + 1 ];

	return ep - sp;
}

int timingValues[ TIMING_NAMES ] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

char cfg[ 65536 ];

int readConfig( CLogger *logger, const char *DRIVE, const char *FILENAME )
{
	u32 cfgBytes;
	memset( cfg, 0, 65536 );

	if ( !readFile( logger, DRIVE, FILENAME, (u8*)cfg, &cfgBytes ) )
		return 0;

	cfgPos = cfg;

	while ( *cfgPos != 0 )
	{
		if ( getNextLine() && curLine[ 0 ] )
		{
			char *rest = NULL;
			char *ptr = strtok_r( curLine, " \t", &rest );

			if ( ptr )
			{
				if ( strcmp( ptr, "STARTUP" ) == 0 )
				{
					ptr = strtok_r( NULL, " \t", &rest );
					if ( strcmp( ptr, "NORMAL" ) == 0 ) radStartup = radStartupSize = 0;
					if ( strcmp( ptr, "MENU" ) == 0 ) radStartup = 3; 

					if ( strcmp( ptr, "REU128K" ) == 0 ) { radStartup = 1; radStartupSize = 0; }
					if ( strcmp( ptr, "REU256K" ) == 0 ) { radStartup = 1; radStartupSize = 1; }
					if ( strcmp( ptr, "REU512K" ) == 0 ) { radStartup = 1; radStartupSize = 2; }
					if ( strcmp( ptr, "REU1M" ) == 0 )   { radStartup = 1; radStartupSize = 3; }
					if ( strcmp( ptr, "REU2M" ) == 0 )   { radStartup = 1; radStartupSize = 4; }
					if ( strcmp( ptr, "REU4M" ) == 0 )   { radStartup = 1; radStartupSize = 5; }
					if ( strcmp( ptr, "REU8M" ) == 0 )   { radStartup = 1; radStartupSize = 6; }
					if ( strcmp( ptr, "REU16M" ) == 0 )  { radStartup = 1; radStartupSize = 7; }

					if ( strcmp( ptr, "GEO512K" ) == 0 ) { radStartup = 2; radStartupSize = 0; }
					if ( strcmp( ptr, "GEO1M" ) == 0 )   { radStartup = 2; radStartupSize = 1; }
					if ( strcmp( ptr, "GEO2M" ) == 0 )   { radStartup = 2; radStartupSize = 2; }
					if ( strcmp( ptr, "GEO4M" ) == 0 )   { radStartup = 2; radStartupSize = 3; }
				}

				for ( int i = 0; i < TIMING_NAMES; i++ )
					if ( strcmp( ptr, timingNames[ i ] ) == 0 && ( ptr = strtok_r( NULL, "\"", &rest ) ) )
					{
						timingValues[ i ] = atoi( ptr );
						while ( *ptr == '\t' || *ptr == ' ' ) ptr++;
					#ifdef DEBUG_OUT
						logger->Write( "RaspiMenu", LogNotice, "  %s >%d< (%s)", timingNames[ i ], timingValues[ i ], ptr );
					#endif
						break;
					}
			}
		}
	}

	if ( timingValues[ 0 ] ) WAIT_FOR_SIGNALS = timingValues[ 0 ];
	if ( timingValues[ 1 ] ) WAIT_CYCLE_READ = timingValues[ 1 ];
	if ( timingValues[ 2 ] ) WAIT_CYCLE_WRITEDATA = timingValues[ 2 ];
	if ( timingValues[ 3 ] ) WAIT_CYCLE_READ_BADLINE = timingValues[ 3 ];
	if ( timingValues[ 4 ] ) WAIT_CYCLE_READ_VIC2 = timingValues[ 4 ];
	if ( timingValues[ 5 ] ) WAIT_CYCLE_WRITEDATA_VIC2 = timingValues[ 5 ];
	if ( timingValues[ 6 ] ) WAIT_CYCLE_MULTIPLEXER = timingValues[ 6 ];
	if ( timingValues[ 7 ] ) WAIT_CYCLE_MULTIPLEXER_VIC2 = timingValues[ 7 ];
	if ( timingValues[ 8 ] ) WAIT_TRIGGER_DMA = timingValues[ 8 ];
	if ( timingValues[ 9 ] ) WAIT_RELEASE_DMA = timingValues[ 9 ];

	if ( timingValues[ 10 ] ) TIMING_OFFSET_CBTD = timingValues[ 10 ];
	if ( timingValues[ 11 ] ) TIMING_DATA_HOLD = timingValues[ 11 ];
	if ( timingValues[ 12 ] ) TIMING_TRIGGER_DMA = timingValues[ 12 ];
	if ( timingValues[ 13 ] ) TIMING_ENABLE_ADDRLATCH = timingValues[ 13 ];
	if ( timingValues[ 14 ] ) TIMING_READ_BA_WRITING = timingValues[ 14 ];
	if ( timingValues[ 15 ] ) TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = timingValues[ 15 ];
	if ( timingValues[ 16 ] ) TIMING_ENABLE_DATA_WRITING = timingValues[ 16 ];
	if ( timingValues[ 17 ] ) TIMING_BA_SIGNAL_AVAIL = timingValues[ 17 ];

	if ( timingValues[ 18 ] ) CACHING_L1_WINDOW_KB = timingValues[ 18 ];
	if ( timingValues[ 19 ] ) CACHING_L2_OFFSET_KB = timingValues[ 19 ];
	if ( timingValues[ 20 ] ) CACHING_L2_PRELOADS_PER_CYCLE = timingValues[ 20 ];

	return 1;
}


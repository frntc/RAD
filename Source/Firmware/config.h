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
#ifndef _config_h
#define _config_h

extern u32 radStartup, radStartupSize;

#define TIMING_NAMES 21
const char timingNames[TIMING_NAMES][32] = {
	"WAIT_FOR_SIGNALS", 
	"WAIT_CYCLE_READ", 
	"WAIT_CYCLE_WRITEDATA", 
	"WAIT_CYCLE_READ_BADLINE", 
	"WAIT_CYCLE_READ_VIC2", 
	"WAIT_CYCLE_WRITEDATA_VIC2", 
	"WAIT_CYCLE_MULTIPLEXER", 
	"WAIT_CYCLE_MULTIPLEXER_VIC2", 
	"WAIT_TRIGGER_DMA", 
	"WAIT_RELEASE_DMA",
	"WAIT_OFFSET_CBTD",
	"WAIT_DATA_HOLD",
	"WAIT_TRIGGER_DMA",
	"WAIT_ENABLE_ADDRLATCH",
	"WAIT_READ_BA_WRITING",
	"WAIT_ENABLE_RW_ADDRLATCH",
	"WAIT_ENABLE_DATA_WRITING", 
	"WAIT_BA_SIGNAL_AVAIL",
	"CACHING_L1_WINDOW_KB",
	"CACHING_L2_OFFSET_KB",
	"CACHING_L2_PRELOADS_PER_CYCLE"
};

extern int readConfig( CLogger *logger, const char *DRIVE, const char *FILENAME );

#endif

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
#ifndef _dirscan_h
#define _dirscan_h

#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/util.h>
#include "helpers.h"

#include "c64screen.h"

#define D64_GET_HEADER	( 1 << 24 )
#define D64_GET_DIR		  ( 1 << 25 )
#define D64_GET_FILE	  ( 1 << 26 )
#define D64_COUNT_FILES ( 1 << 27 )

#define DISPLAY_LINES 11

typedef struct
{
	u8	name[ 256 ];
	u32 f, parent, next, level, size;
} DIRENTRY;

// file type requires 3 bits
#define SHIFT_TYPE		16

#define DIR_FILE_MARKED	(1<<20)
#define DIR_KERNAL_FILE	(1<<21)
#define DIR_SID_FILE	(1<<22)
#define DIR_LISTALL 	(1<<23)
#define DIR_SCANNED 	(1<<24)
#define DIR_UNROLLED	(1<<25)
#define DIR_D64_FILE	(1<<26)
#define DIR_CRT_FILE	(1<<27)
#define DIR_PRG_FILE	(1<<28)
#define DIR_DIRECTORY	(1<<29)
#define DIR_FILE_IN_D64	(1<<30)
#define DIR_BIN_FILE	(1<<31)

#define ITEM_SELECTED 128

#define MAX_DIR_ENTRIES		16384
extern DIRENTRY dir[ MAX_DIR_ENTRIES ];
extern s32 nDirEntries;

extern void unmarkAllFiles();

#define REUMENU_SELECT_FILE_REU	(1<<24)
#define REUMENU_SELECT_FILE_GEO	(2<<24)
#define REUMENU_SELECT_FILE_PRG	(3<<24)
#define REUMENU_PLAY_NUVIE_REU  (4<<24)
#define REUMENU_START_GEORAM	(5<<24)
#define REUMENU_CREATE_IMAGE	(1<<26)

extern char dirSelectedFile[ 1024 ];
extern char dirSelectedName[ 1024 ];
extern u32 dirSelectedFileSize;


#endif

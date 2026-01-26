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

#define REUDIR_REUIMAGE		0x01
#define REUDIR_GEOIMAGE		0x02
#define REUDIR_PRG			  0x04
#define REUDIR_DIRECTORY	0x08
#define REUDIR_DUMMYNEW		0x10
#define REUDIR_TOPARENT		0x20
#define REUDIR_VSFIMAGE		0x40

#define REUDIR_D64			0x80
#define REUDIR_ZIP			0x100
#define REUDIR_SEQ			0x200

#define BROWSER_NUM_CATEGORIES	3
#define BROWSER_NUM_LINES		10

#define ITEM_SELECTED 128

#define MAX_DIR_ENTRIES		16384
extern DIRENTRY dir[ MAX_DIR_ENTRIES ];
extern s32 nDirEntries;
extern int nFilesAllCategories;

#define REUMENU_SELECT_FILE_REU	(1<<24)
#define REUMENU_SELECT_FILE_GEO	(2<<24)
#define REUMENU_SELECT_FILE_PRG	(3<<24)
#define REUMENU_PLAY_NUVIE_REU  (4<<24)
#define REUMENU_START_GEORAM	  (5<<24)
#define REUMENU_SELECT_FILE_VSF	(6<<24)
#define REUMENU_CREATE_IMAGE	  (1<<26)

extern char dirSelectedFile[ 1024 ];
extern char dirSelectedName[ 1024 ];
extern u32 dirSelectedFileSize;

#define REUDIR_MARKSYNC		(1<<23)
#define IECSYNC_NOT_SYNCED	0x01

#define REUDIR_FILEOP_DELETE  0x01
#define REUDIR_FILEOP_RENAME  0x02

typedef struct
{
	u8  path[ 1024 ];
	u8  filename[ 256 ];
	u8	name[ 256 ];
	u32 f, size, first, last, parent;
  u32 fileOp;
  u8  rename[ 256 ];
} REUDIRENTRY;

#endif

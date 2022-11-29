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
#include "dirscan.h"
#include "linux/kernel.h"
#include <circle/util.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

extern CLogger *logger;

#define REUDIR_PRINT_MAXNAMECHARS	22
#define REUDIR_PRINT_SIZEPOS		25

#define REUDIR_REUIMAGE		0x01
#define REUDIR_GEOIMAGE		0x02
#define REUDIR_PRG			0x04
#define REUDIR_DIRECTORY	0x08
#define REUDIR_DUMMYNEW		0x10
#define REUDIR_TOPARENT		0x20

u32 rdSectionFirst, rdSectionLast;

typedef struct
{
	u8  path[ 1024 ];
	u8  filename[ 256 ];
	u8	name[ 64 ];
	u32 f, size, first, last, parent;
} REUDIRENTRY;



int compareEntriesREU( const void *e1, const void *e2 )
{
	REUDIRENTRY *a = (REUDIRENTRY*)e1;
	REUDIRENTRY *b = (REUDIRENTRY*)e2;

	if ( (a->f & REUDIR_TOPARENT) ) return -1;
	if ( (b->f & REUDIR_TOPARENT) ) return 1;

	if ( (a->f & REUDIR_DIRECTORY) && !(b->f & REUDIR_DIRECTORY) )
		return -1;
	if ( (b->f & REUDIR_DIRECTORY) && !(a->f & REUDIR_DIRECTORY) )
		return 1;

	if ( (a->f & REUDIR_DUMMYNEW) ) return -1;
	if ( (b->f & REUDIR_DUMMYNEW) ) return 1;

	return strcasecmp( (char*)a->filename, (char*)b->filename );
}

void quicksortREU( REUDIRENTRY *begin, REUDIRENTRY *end )
{
	REUDIRENTRY *ptr = begin, *split = begin + 1;
	if ( end - begin < 1 ) return;
	while ( ++ptr <= end ) {
		if ( compareEntriesREU( ptr, begin ) < 0 ) {
			REUDIRENTRY tmp = *ptr; *ptr = *split; *split = tmp;
			++split;
		}
	}
	REUDIRENTRY tmp = *begin; *begin = *( split - 1 ); *( split - 1 ) = tmp;
	quicksortREU( begin, split - 1 );
	quicksortREU( split, end );
}


REUDIRENTRY sort[ 2048 ];

static unsigned char toupper( unsigned char c )
{
	if ( c >= 'a' && c <= 'z' )
		return c + 'A' - 'a';
	return c;
}

void makeFormattedName( REUDIRENTRY *d )
{
	char filename[ 1024 ], fn_up[ 1024 ];
	memset( filename, 0, 1024 );
	strncpy( filename, (char*)d->filename, 1023 );
	int i = 0;
	while ( i < 1024 && filename[ i ] != 0 )
	{
		fn_up[ i ] = toupper( filename[ i ] );
		i ++;
	}
	fn_up[ i ] = 0;

	if ( strstr( (char*)fn_up, ".PRG" ) || 
		 strstr( (char*)fn_up, ".REU" ) )
		filename[ strlen( filename ) - 4 ] = 0;
	if ( strstr( (char*)fn_up, ".GEORAM" ) )
		filename[ strlen( filename ) - 7 ] = 0;

	char name[ 64 ];
	memset( name, 0, 64 );

	if ( !( d->f & REUDIR_DIRECTORY ) && !( d->f & REUDIR_DUMMYNEW ) )
	{
		strncpy( name, filename, REUDIR_PRINT_MAXNAMECHARS ); 
	} else
	{
		strncpy( name, fn_up, REUDIR_PRINT_MAXNAMECHARS ); 
		for ( int i = strlen( name ); i < REUDIR_PRINT_SIZEPOS + 5; i++ )
			name[ i ] = ' ';
	}

	char fs[ 8 ];
	fs[ 0 ] = 0;

	if ( !( d->f & REUDIR_DIRECTORY ) && !( d->f & REUDIR_DUMMYNEW ) && !( d->f & REUDIR_TOPARENT ) )
	{
		if ( d->size < 1000 )
		{
			sprintf( fs, "%4db", d->size ); 
		} else
		if ( d->size / 1024 > 999 )
			sprintf( fs, "%4dm", (int)( d->size / (1024 * 1024) ) ); else
			sprintf( fs, "%4dk", d->size / 1024 );

		for ( int i = strlen( name ); i < REUDIR_PRINT_SIZEPOS; i++ )
			name[ i ] = ' ';
	}


	sprintf( (char*)d->name, "%s%s", name, fs );
}


bool ListDirectoryContents( const char *sDir, REUDIRENTRY *d, u32 *n, u32 *nElementsThisLevel, u32 parent, u32 level, bool addNewImageEntry )
{
	char sPath[ 2048 ];

	u32 sortCur = 0;

	sprintf( sPath, "%s", sDir );

	int nAdditionalEntries = 0;

	if ( level > 0 )
	{
		sprintf( (char*)sort[ sortCur ].path, "%s", sDir );
		sprintf( (char*)sort[ sortCur ].filename, ".. " );
		sort[ sortCur ].f = REUDIR_TOPARENT;
		sort[ sortCur ].size = 0;
		sortCur ++;
		nAdditionalEntries ++;
	}

	if ( addNewImageEntry )
	{
		sprintf( (char*)sort[ sortCur ].path, "%s", sDir );
		sprintf( (char*)sort[ sortCur ].filename, "__NEW IMAGE__" );
		sort[ sortCur ].f = REUDIR_DUMMYNEW;
		sort[ sortCur ].size = 0;
		sortCur ++;
		nAdditionalEntries ++;
	}

	DIR dir;
	FILINFO FileInfo;

	FRESULT res = f_findfirst( &dir, &FileInfo, sPath, "*" );

	if ( res != FR_OK )
		logger->Write( "read directory", LogNotice, "error opening dir" );


	while ( res == FR_OK && FileInfo.fname[ 0 ] )
	{
		if ( strcmp( FileInfo.fname, "." ) != 0 && strcmp( FileInfo.fname, ".." ) != 0 )
		{
			sprintf( sPath, "%s\\%s", sDir, FileInfo.fname );

			// file or folder?
			if ( ( FileInfo.fattrib & ( AM_DIR ) ) )
			{
				strcpy( (char*)sort[sortCur].path, sDir );
				strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
				sort[ sortCur++ ].f = REUDIR_DIRECTORY;
				nAdditionalEntries ++;
			} else
			{
				if ( strstr( FileInfo.fname, ".reu" ) > 0 || strstr( FileInfo.fname, ".REU" ) > 0 )
				{
					strcpy( (char*)sort[sortCur].path, sDir );
					strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
					sort[ sortCur ].size = FileInfo.fsize;
					sort[ sortCur++ ].f = REUDIR_REUIMAGE;
					nAdditionalEntries ++;
				}
				if ( strstr( FileInfo.fname, ".georam" ) > 0 || strstr( FileInfo.fname, ".GEORAM" ) > 0 )
				{
					strcpy( (char*)sort[sortCur].path, sDir );
					strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
					sort[ sortCur ].size = FileInfo.fsize;
					sort[ sortCur++ ].f = REUDIR_GEOIMAGE;
					nAdditionalEntries ++;
				}
				if ( strstr( FileInfo.fname, ".prg" ) > 0 || strstr( FileInfo.fname, ".PRG" ) > 0 )
				{
					strcpy( (char*)sort[sortCur].path, sDir );
					strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
					sort[ sortCur ].size = FileInfo.fsize;
					sort[ sortCur++ ].f = REUDIR_PRG;
					nAdditionalEntries ++;
				}
			}
		}

		res = f_findnext( &dir, &FileInfo );
	};

	f_closedir( &dir );

	if ( !nAdditionalEntries )
		return true;

	quicksortREU( &sort[ 0 ], &sort[ sortCur - 1 ] );

	*nElementsThisLevel = nAdditionalEntries;

	memcpy( &d[ *n ], sort, sizeof( REUDIRENTRY ) * nAdditionalEntries );

	int prevOffset = *n;
	*n += nAdditionalEntries;

	for ( int idx = 0; idx < nAdditionalEntries; idx++ )
	{
		int i = prevOffset + idx;
		d[ i ].parent = parent;
		d[ i ].first = d[ i ].last = 0;

		makeFormattedName( &d[ i ] );
		
		if ( d[ i ].f & REUDIR_DIRECTORY )
		{
			char path[ 1024 ];
			sprintf( path, "%s\\%s", d[ i ].path, d[ i ].filename );
			d[ i ].first = *n;
			u32 nElementsThisLevel;
			ListDirectoryContents( path, d, n, &nElementsThisLevel, *n, level + 1, addNewImageEntry );
			d[ i ].last = d[ i ].first + nElementsThisLevel - 1;
		}
	}

	return true;
}





#define BROWSER_NUM_CATEGORIES	3
#define BROWSER_NUM_LINES		10

REUDIRENTRY filesAll[ 1024 ];

struct BROWSESTATE
{
	int first, last, curPos, scrollPos;
};

int curCategory;
REUDIRENTRY *filePtrCat[ BROWSER_NUM_CATEGORIES ];
int curPositionCat[ BROWSER_NUM_CATEGORIES ];
int curLevelCat[ BROWSER_NUM_CATEGORIES ];
BROWSESTATE dirFirstLastCat[ BROWSER_NUM_CATEGORIES ][ 32 ];
u32 nTotalElements[ BROWSER_NUM_CATEGORIES ];

int nFilesAllCategories = 0;

int curPosition = 0;
int curLevel = 0;
BROWSESTATE dirFirstLast[ 32 ];
REUDIRENTRY *files;

#define SAVE_CATEGORY( c )											\
	curPositionCat[ c ] = curPosition;								\
	curLevelCat[ c ] = curLevel;									\
	for ( int i = 0; i < 32; i++ ) 									\
		dirFirstLastCat[ c ][ i ] = dirFirstLast[ i ];	

#define LOAD_CATEGORY( c )											\
	curPosition = curPositionCat[ c ];								\
	curLevel = curLevelCat[ c ];									\
	for ( int i = 0; i < 32; i++ )									\
		dirFirstLast[ i ] = dirFirstLastCat[ c ][ i ];				\
	files = filePtrCat[ c ];

static u8 firstTimeScanning = 1;

u32 dirSelectedFileSize = 0;
char dirSelectedFile[ 1024 ];
char dirSelectedName[ 1024 ];

int prevPositionCat[ BROWSER_NUM_CATEGORIES ];
int prevLevelCat[ BROWSER_NUM_CATEGORIES ];
BROWSESTATE prevDirFirstLastCat[ BROWSER_NUM_CATEGORIES ][ 32 ];
u32 nTotalElementsPrev[ BROWSER_NUM_CATEGORIES ];

u8 selectedCategory = 0;
char dirSelectedFilePRG[ 1024 ];
char dirSelectedFileREU[ 1024 ];
char dirSelectedFileGEO[ 1024 ];

int findFile( u8 cat, char *search )
{
	if ( search[ 0 ] == 0 )
		return -1;

	curCategory = cat;

	// start with last file in root directory
	LOAD_CATEGORY( curCategory );

	curPosition = dirFirstLast[ curLevel ].first;
	int prevPos;
	do {
		prevPos = curPosition;
		if ( curPosition < dirFirstLast[ curLevel ].last - 1 )
			if ( ++ curPosition >= dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos + BROWSER_NUM_LINES )
				dirFirstLast[ curLevel ].scrollPos ++;
	} while ( curPosition != prevPos );

	while ( true )
	{
		REUDIRENTRY *e = &files[ curPosition ];

		if ( files[ curPosition ].f & REUDIR_TOPARENT )
		{
			if ( curLevel == 0 )
				return -1;

			// go directory up
			curPosition = dirFirstLast[ curLevel-- ].curPos;
			
			if ( curPosition > dirFirstLast[ curLevel ].first )
				curPosition --; else
				return -1; // this was the last file
		} else
		if ( ( e->f & REUDIR_DIRECTORY ) && ( e->last > e->first ) )
		{
			// go into directory
			curLevel ++;
			dirFirstLast[ curLevel ].first  = e->first;
			dirFirstLast[ curLevel ].last   = e->last + 1;
			dirFirstLast[ curLevel ].curPos = curPosition;

			curPosition = e->first;
			int prevPos;
			do {
				prevPos = curPosition;
				if ( curPosition < dirFirstLast[ curLevel ].last - 1 )
					if ( ++ curPosition >= dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos + BROWSER_NUM_LINES )
						dirFirstLast[ curLevel ].scrollPos ++;
			} while ( curPosition != prevPos );

		} else
		{
			char tmp[ 1024 ];
			sprintf( tmp, "%s/%s", (const char*)e->path, (const char*)e->filename );

			if ( strcmp( tmp, search ) == 0 )
			{
				SAVE_CATEGORY( curCategory );
				return curPosition;
			}

			if ( curPosition > dirFirstLast[ curLevel ].first )
			{
				if ( -- curPosition < dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos )
					dirFirstLast[ curLevel ].scrollPos --;
			}
		}
	}
}


void scanDirectoriesRAD( char *DRIVE )
{
	FATFS m_FileSystem;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
		logger->Write( "RaspiMenu", LogPanic, "Cannot mount drive: %s", DRIVE );

	u32 n = 0, nElementsLevel0 = 0, tmp;

	memset( curLevelCat, 0, sizeof( int ) * BROWSER_NUM_CATEGORIES );
	memset( curPositionCat, 0, sizeof( int ) * BROWSER_NUM_CATEGORIES );
	memset( dirFirstLastCat, 0, sizeof( BROWSESTATE ) * 32 * BROWSER_NUM_CATEGORIES );

	const char *scanDirs[ BROWSER_NUM_CATEGORIES ] = {
		"SD:RAD_PRG",
		"SD:REU",
		"SD:GEORAM"
	};

	const bool bAddNewImage[ BROWSER_NUM_CATEGORIES ] = {
		false, false, false
	};

	for ( int c = 0; c < BROWSER_NUM_CATEGORIES; c ++ )
	{
		tmp = nElementsLevel0 = 0;
		filePtrCat[ c ] = &filesAll[ n ];
		ListDirectoryContents( (const char*)scanDirs[ c ], &filesAll[ n ], &tmp, &nElementsLevel0, 0xffffffff, 0, bAddNewImage[ c ] );
		n += tmp;
		dirFirstLastCat[ c ][ 0 ].last = nElementsLevel0;
		nTotalElements[ c ] = nElementsLevel0;
	}

	nFilesAllCategories = n;

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RaspiMenu", LogPanic, "Cannot unmount drive: %s", DRIVE );

	curCategory = 0;

	if ( firstTimeScanning )
	{
		dirSelectedFileSize = 0;
		dirSelectedFile[ 0 ] = 
		dirSelectedName[ 0 ] = 
		dirSelectedFilePRG[ 0 ] = 0;
		dirSelectedFileREU[ 0 ] = 0;
		dirSelectedFileGEO[ 0 ] = 0;
		firstTimeScanning = 0;
	} else
	{
		if ( nTotalElementsPrev[ 0 ] == nTotalElements[ 0 ] &&
			 nTotalElementsPrev[ 1 ] == nTotalElements[ 1 ] &&
			 nTotalElementsPrev[ 2 ] == nTotalElements[ 2 ] )
		{
			// presumably no new files on SD
			memcpy( curPositionCat, prevPositionCat, sizeof( int ) * BROWSER_NUM_CATEGORIES);
			memcpy( curLevelCat, prevLevelCat, sizeof( int ) * BROWSER_NUM_CATEGORIES);
			memcpy( dirFirstLastCat, prevDirFirstLastCat, sizeof( BROWSESTATE ) * BROWSER_NUM_CATEGORIES * 32 );
		} else
		{
			findFile( 0, dirSelectedFilePRG );
			findFile( 1, dirSelectedFileREU );
			findFile( 2, dirSelectedFileGEO );
		}
		curCategory = selectedCategory;
	}

	LOAD_CATEGORY( curCategory );
}

void saveCurrentCursor()
{
	selectedCategory = curCategory;

	REUDIRENTRY *e = &files[ curPosition ];
	if ( e->f & REUDIR_REUIMAGE ||
 		 e->f & REUDIR_GEOIMAGE ||
		 e->f & REUDIR_PRG  )
	{
		sprintf( dirSelectedFile, "%s/%s", (const char*)files[ curPosition ].path, (const char*)files[ curPosition ].filename );

		if ( e->f & REUDIR_REUIMAGE )
			strncpy( dirSelectedFileREU, dirSelectedFile, 1023 ); else
		if ( e->f & REUDIR_GEOIMAGE )
			strncpy( dirSelectedFileGEO, dirSelectedFile, 1023 ); else
			strncpy( dirSelectedFilePRG, dirSelectedFile, 1023 );
	}

	SAVE_CATEGORY( curCategory );

	memcpy( prevPositionCat, curPositionCat, sizeof( int ) * BROWSER_NUM_CATEGORIES);
	memcpy( prevLevelCat, curLevelCat, sizeof( int ) * BROWSER_NUM_CATEGORIES);
	memcpy( prevDirFirstLastCat, dirFirstLastCat, sizeof( BROWSESTATE ) * BROWSER_NUM_CATEGORIES * 32 );
	memcpy( nTotalElementsPrev, nTotalElements, sizeof( u32 ) * BROWSER_NUM_CATEGORIES );
}

void unmarkAllFiles()
{
	for ( int i = 0; i < nFilesAllCategories; i++ )
		files[ i ].f &= ~DIR_FILE_MARKED;
}

u32 handleKey( int k )
{
	if ( k == VK_F1 || k == VK_F3 )
	{
		for ( int i = 0; i < 10; i++ )
			handleKey( k == VK_F1 ? VK_UP : VK_DOWN );
		return 0;
	} else
	if ( k == VK_HOME )
	{
		dirFirstLast[ curLevel ].scrollPos = 0;
		curPosition = dirFirstLast[ curLevel ].first;
	} else
	if ( ( k == VK_LEFT && curCategory > 0 ) || 
		 ( k == VK_RIGHT && curCategory < BROWSER_NUM_CATEGORIES - 1 ) )
	{
		SAVE_CATEGORY( curCategory );
		curCategory += (k == VK_LEFT) ? -1 : 1;
		LOAD_CATEGORY( curCategory );
		saveCurrentCursor();
	} else
	if ( ( k == VK_RETURN && files[ curPosition ].f & REUDIR_TOPARENT && curLevel > 0 ) ||
		 ( k == VK_DELETE && curLevel > 0 ) )
	{
		curPosition = dirFirstLast[ curLevel-- ].curPos;
		saveCurrentCursor();
	} else
	if ( k == VK_RETURN || k == VK_SHIFT_RETURN || k == VK_COMMODORE_RETURN )
	{
		REUDIRENTRY *e = &files[ curPosition ];
		if ( ( e->f & REUDIR_DIRECTORY ) && ( e->last > e->first ) )
		{
			curLevel ++;
			dirFirstLast[ curLevel ].first  = e->first;
			dirFirstLast[ curLevel ].last   = e->last + 1;
			dirFirstLast[ curLevel ].curPos = curPosition;
			curPosition = e->first;
			saveCurrentCursor();
		} else
		if ( e->f & REUDIR_REUIMAGE ||
			 e->f & REUDIR_GEOIMAGE ||
			 e->f & REUDIR_PRG  )
		{
			//printf( "SELECT: %s/%s\n", files[ curPosition ].path, files[ curPosition ].filename );
			sprintf( dirSelectedFile, "%s/%s", (const char*)files[ curPosition ].path, (const char*)files[ curPosition ].filename );
			strncpy( dirSelectedName, (const char*)files[ curPosition ].filename, 511 );
			dirSelectedFileSize = files[ curPosition ].size;

			saveCurrentCursor();

			// double RETURN -> start
			if ( e->f & DIR_FILE_MARKED && k == VK_RETURN )
				k = VK_SHIFT_RETURN;

			if ( k == VK_RETURN || k == VK_COMMODORE_RETURN )
			{
				unmarkAllFiles();
				e->f |= DIR_FILE_MARKED;

				if ( e->f & REUDIR_REUIMAGE )
					return REUMENU_SELECT_FILE_REU; else
				if ( e->f & REUDIR_GEOIMAGE )
					return REUMENU_SELECT_FILE_GEO; else
					return REUMENU_SELECT_FILE_PRG;
			} else
			{
				if (e->f & REUDIR_REUIMAGE)
					return REUMENU_PLAY_NUVIE_REU; else
				if (e->f & REUDIR_GEOIMAGE)
					return REUMENU_START_GEORAM;
			}
		} else
		if ( e->f & REUDIR_DUMMYNEW )
		{
			//printf( "CREATE NEW IMAGE\n" );
			return REUMENU_CREATE_IMAGE;
		}
	}

	if ( k == VK_UP && curPosition > dirFirstLast[ curLevel ].first )
	{
		if ( -- curPosition < dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos )
			dirFirstLast[ curLevel ].scrollPos --;
	}
	if ( k == VK_DOWN && curPosition < dirFirstLast[ curLevel ].last - 1 )
	{
		if ( ++ curPosition >= dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos + BROWSER_NUM_LINES )
			dirFirstLast[ curLevel ].scrollPos ++;
	}

	saveCurrentCursor();

	return 0;
}

extern void printC64( u32 x, u32 y, const char *t, u8 color, u8 flag, u32 convert, u32 maxL );

extern unsigned char fadeTabStep[ 16 ][ 6 ];

void printBrowser( int fade )
{
	int curScrollPos = dirFirstLast[ curLevel ].scrollPos;

	int from = dirFirstLast[ curLevel ].first + curScrollPos;
	int to   = dirFirstLast[ curLevel ].last;

	to = min( to, to + curScrollPos );
	if ( to - from > BROWSER_NUM_LINES ) to = BROWSER_NUM_LINES + from;

	int xp = 4;
	int yp = 9+1;

	u8 c = fadeTabStep[ 1 ][ fade ];
	printC64( xp, yp, "PRG", c, (curCategory==0)?0x80:0, 0, 4 );
	printC64( xp+4, yp, "REU", c, (curCategory==1)?0x80:0, 0, 4 );
	printC64( xp+8, yp, "GEORAM", c, (curCategory==2)?0x80:0, 0, 6 );

	yp ++;

	extern u8 c64ScreenRAM[ 1024 * 4 ]; 
	extern u8 c64ColorRAM[ 1024 * 4 ]; 

	memset( &c64ScreenRAM[ yp * 40 ], 32, 40 * BROWSER_NUM_LINES );
	memset( &c64ColorRAM[ yp * 40 ], 0, 40 * BROWSER_NUM_LINES );

	for ( int i = from; i < to; i++ )
	{
		u8 color = files[ i ].f & REUDIR_DIRECTORY ? fadeTabStep[ 3 ][ fade ] : fadeTabStep[ 15 ][ fade ];
		if ( i == curPosition ) 
			color = fadeTabStep[ 13 ][ fade ]; 

		if ( files[ i ].f & DIR_FILE_MARKED )
			color = 16 + 1 + ( fade << 5 );

		printC64( xp, yp ++, (const char*)files[ i ].name, color, i == curPosition ? 0x80 : 0, 0, 39 );
	}

	// scroll bar
	yp = 10+1;

	int nDirEntries = max( 1, dirFirstLast[ curLevel ].last - dirFirstLast[ curLevel ].first );
	int t = ( from - dirFirstLast[ curLevel ].first ) * BROWSER_NUM_LINES / nDirEntries;
	int b = ( to - dirFirstLast[ curLevel ].first  ) * BROWSER_NUM_LINES / nDirEntries;

	// bar on the right
	u8 color = fadeTabStep[ 12 ][ fade ];
	for ( int i = 0; i < BROWSER_NUM_LINES; i++ )
	{
		char c = 95;
		if ( t <= i && i <= b )
			c = 96 + 128 - 64;
		c64ScreenRAM[ 35 + ( i + yp ) * 40 ] = c;
		c64ColorRAM[ 35 + ( i + yp ) * 40 ]  = color;
	}
	/*
	for ( int i = 0; i < 256; i++ )
	{
		c64ScreenRAM[ (i%20) + ( (i/20) + yp ) * 40 ] = i;
		c64ColorRAM[ (i%20) + ( ( i / 20 ) + yp ) * 40 ] = color;
	}
	*/
}
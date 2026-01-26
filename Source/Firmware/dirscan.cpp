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
#include "dirscan.h"
#include "linux/kernel.h"
#include <circle/util.h>

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "rad_iecdevice.h"

extern CLogger *logger;

#define REUDIR_PRINT_MAXNAMECHARS	22
#define REUDIR_PRINT_SIZEPOS		25

u32 nFileOpsPending = 0, foRenaming = 0;
REUDIRENTRY *pFileToRename = NULL;

u32 rdSectionFirst, rdSectionLast;

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

	if ( /*strstr( (char*)fn_up, ".D64" ) || */
		 strstr( (char*)fn_up, ".PRG" ) || 
		 strstr( (char*)fn_up, ".REU" ) )
		filename[ strlen( filename ) - 4 ] = 0;
	if ( strstr( (char*)fn_up, ".VSF" ) )
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

	nFileOpsPending = 0;

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

			sort[ sortCur ].rename[ 0 ] = 0;
			sort[ sortCur ].fileOp = 0;
			
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
				if ( strstr( FileInfo.fname, ".vsf" ) > 0 || strstr( FileInfo.fname, ".VSF" ) > 0 )
				{
					strcpy( (char*)sort[sortCur].path, sDir );
					strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
					sort[ sortCur ].size = FileInfo.fsize;
					sort[ sortCur++ ].f = REUDIR_VSFIMAGE;
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

				if ( strstr( FileInfo.fname, ".seq" ) > 0 || strstr( FileInfo.fname, ".SEQ" ) > 0 )
				{
					strcpy( (char*)sort[sortCur].path, sDir );
					strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
					sort[ sortCur ].size = FileInfo.fsize;
					sort[ sortCur++ ].f = REUDIR_SEQ;
					nAdditionalEntries ++;
				}
				//D64,D71,D81,G64,G71
				//ZIP
				if ( strstr( FileInfo.fname, ".d64" ) > 0 || strstr( FileInfo.fname, ".D64" ) > 0 ||
					 strstr( FileInfo.fname, ".d71" ) > 0 || strstr( FileInfo.fname, ".D71" ) > 0 ||
					 strstr( FileInfo.fname, ".d81" ) > 0 || strstr( FileInfo.fname, ".D81" ) > 0 ||
					 strstr( FileInfo.fname, ".g64" ) > 0 || strstr( FileInfo.fname, ".G64" ) > 0 ||
					 strstr( FileInfo.fname, ".g71" ) > 0 || strstr( FileInfo.fname, ".G71" ) > 0 )
				{
					strcpy( (char*)sort[sortCur].path, sDir );
					strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
					sort[ sortCur ].size = FileInfo.fsize;
					sort[ sortCur++ ].f = REUDIR_D64;
					nAdditionalEntries ++;
				}

				if ( strstr( FileInfo.fname, ".zip" ) > 0 || strstr( FileInfo.fname, ".ZIP" ) > 0 )
				{
					strcpy( (char*)sort[sortCur].path, sDir );
					strcpy( (char*)sort[sortCur].filename, FileInfo.fname );
					sort[ sortCur ].size = FileInfo.fsize;
					sort[ sortCur++ ].f = REUDIR_ZIP;
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




REUDIRENTRY filesAll[ 8192 ];

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
		 e->f & REUDIR_VSFIMAGE ||
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
		filesAll[ i ].f &= ~(u32)DIR_FILE_MARKED;
}


void omitAllPendingFileoperations()
{
	if ( nFileOpsPending == 0 ) return;
	for ( int i = 0; i < nFilesAllCategories; i++ )
	{
		files[ i ].fileOp = 0;
	}
	nFileOpsPending = 0;
}

void applyAllPendingFileoperations( const char *DRIVE )
{
	if ( nFileOpsPending == 0 ) return;

	extern u32 handleOneRasterLine( int fade1024, u8 fadeText );

	for ( u32 i = 312; i < 312 * 10; i ++ )
		handleOneRasterLine( 0x10000000 | (i * 256 / 312 / 2), 1 );

	extern void POKE_FILL( u16 a, u16 n, u8 v );
	POKE_FILL( 0x6400, 1000, 32 );
	POKE_FILL( 0xD800, 1000, 0 );

	nFileOpsPending = 0;

	FATFS m_FileSystem;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
		logger->Write( "RaspiMenu", LogPanic, "Cannot mount drive: %s", DRIVE );

	char curSelectedFile[ 1024 ];
	curSelectedFile[ 0 ] = 0;

	int myCurCategory = curCategory;
	int myScrollPos = dirFirstLast[ curLevel ].scrollPos;

	while ( files[ curPosition ].fileOp & REUDIR_FILEOP_DELETE )
	{
		if ( curPosition > dirFirstLast[ curLevel ].first + 1 )
		{
			if ( -- curPosition < dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos )
				dirFirstLast[ curLevel ].scrollPos --;
			sprintf( curSelectedFile, "%s/%s", (const char*)files[ curPosition ].path, (const char*)files[ curPosition ].filename );
		} else
		if ( curPosition < dirFirstLast[ curLevel ].last - 1 )
		{
			if ( dirFirstLast[ curLevel ].scrollPos > 0 )
				dirFirstLast[ curLevel ].scrollPos --;
			if ( ++ curPosition >= dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos + BROWSER_NUM_LINES )
				dirFirstLast[ curLevel ].scrollPos ++;
		
			sprintf( curSelectedFile, "%s/%s", (const char*)files[ curPosition ].path, (const char*)files[ curPosition ].filename );
		} 
	}

	for ( int i = 0; i < nFilesAllCategories; i++ )
	{
		if ( files[ i ].fileOp & REUDIR_FILEOP_DELETE )
		{
			char fn[ 2048 ];
			sprintf( fn, "%s/%s", files[ i ].path, files[ i ].filename );
			f_unlink( fn );
		} else
		if ( files[ i ].fileOp & REUDIR_FILEOP_RENAME )
		{
			char oldName[ 2048 ], newName[ 2048 ];
			sprintf( oldName, "%s/%s", files[ i ].path, files[ i ].filename );
			sprintf( newName, "%s/%s", files[ i ].path, files[ i ].rename );
			f_rename( oldName, newName );
		}
	}

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RaspiMenu", LogPanic, "Cannot unmount drive: %s", DRIVE );

	firstTimeScanning = 1;
	scanDirectoriesRAD( (char*)DRIVE );
	if ( curSelectedFile[ 0 ] )
	{
		findFile( myCurCategory, curSelectedFile );
		dirFirstLast[ curLevel ].scrollPos = 0;
		for ( int i = 0; i < myScrollPos; i++ )
		{
			if ( dirFirstLast[ curLevel ].first + dirFirstLast[ curLevel ].scrollPos + BROWSER_NUM_LINES < dirFirstLast[ curLevel ].last )
				dirFirstLast[ curLevel ].scrollPos ++;
		}
	}

	for ( int i = curLevel - 1; i >= 0; i-- )
	{
		dirFirstLast[ i ].scrollPos = dirFirstLast[ i ].first;
	}
	/*if ( curLevel > 0 )
	{
		// this fakes "go on directory up, and go to directory again"
		curPosition = dirFirstLast[ curLevel-- ].curPos;
		saveCurrentCursor();
		u32 handleKey( int k );
		handleKey( VK_RETURN );
	}*/

	extern u32 readKeyRenderMenu( int fade );
	readKeyRenderMenu( 0 );
	extern u8 c64ScreenRAM[ 1024 * 4 ];
	extern void POKE_MEMCPY( u16 a, u16 n, u8 *src );
	POKE_MEMCPY( 0x6400, 1000, c64ScreenRAM );

	for ( s32 i = 312 * 10; i >= 0; i -- )
		handleOneRasterLine( 0x10000000 | (i * 256 / 312 / 2), 1 ); 
}


int showWarningMessage = 0, showWarningTimeout = 0, showWarningPosition = 0;
char warningMessage[ 41 ];

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
			dirFirstLast[ curLevel ].scrollPos = 0;
			saveCurrentCursor();
		} else
		if ( e->f & REUDIR_REUIMAGE ||
			 e->f & REUDIR_VSFIMAGE ||
			 e->f & REUDIR_GEOIMAGE ||
			 e->f & REUDIR_PRG  )
		{
			sprintf( dirSelectedFile, "%s/%s", (const char*)files[ curPosition ].path, (const char*)files[ curPosition ].filename );
			strncpy( dirSelectedName, (const char*)files[ curPosition ].filename, 511 );
			dirSelectedFileSize = files[ curPosition ].size;

			saveCurrentCursor();

			// double RETURN -> start
			if ( (e->f & DIR_FILE_MARKED) && (k == VK_RETURN) )
				k = VK_SHIFT_RETURN;

			if ( k == VK_RETURN || k == VK_COMMODORE_RETURN )
			{
				unmarkAllFiles();
				e->f |= DIR_FILE_MARKED;

				if ( e->f & REUDIR_REUIMAGE )
					return REUMENU_SELECT_FILE_REU; else
				if ( e->f & REUDIR_VSFIMAGE )
				{
					k = VK_SHIFT_RETURN;
					return REUMENU_SELECT_FILE_VSF; 
				} else
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
			return REUMENU_CREATE_IMAGE;
		}
	} else
	if ( ( k == 'D' || k == 'd' ) && !( files[ curPosition ].f & REUDIR_MARKSYNC ) )  // mark file for deletion
	{
		REUDIRENTRY *e = &files[ curPosition ];

		if ( e->f & ( REUDIR_D64 | REUDIR_PRG | REUDIR_REUIMAGE |  REUDIR_REUIMAGE | REUDIR_VSFIMAGE ) )
		{
			if ( e->fileOp & REUDIR_FILEOP_DELETE )
			{
				if ( nFileOpsPending ) nFileOpsPending --;
				e->fileOp &= ~REUDIR_FILEOP_DELETE;
			} else
			{
				nFileOpsPending ++;
				e->fileOp |= REUDIR_FILEOP_DELETE;
			}
		}
	} else
	if ( ( k == 'R' || k == 'r' ) && !( files[ curPosition ].f & REUDIR_MARKSYNC ) )  // renaming
	{
		REUDIRENTRY *e = &files[ curPosition ];

		if ( e->f & ( REUDIR_D64 | REUDIR_PRG | REUDIR_REUIMAGE |  REUDIR_REUIMAGE | REUDIR_VSFIMAGE ) )
		{
			if ( !( e->fileOp & REUDIR_FILEOP_RENAME ) )
			{
				nFileOpsPending ++;
				memcpy( e->rename, e->filename, 64 );
				e->fileOp |= REUDIR_FILEOP_RENAME;
			}

			foRenaming = 1;
			pFileToRename = e;
		}
	} else
	if ( k == 'S' || k == 's' ) // mark file for sync (if filetype is PRG, SEQ, Dxx, ...)
	{
		#ifdef DEBUG_OUT_IECDEVICE
		logger->Write( "[mark files for sync]", LogNotice, " " );
		#endif
		REUDIRENTRY *e = &files[ curPosition ];
		if ( e->f & REUDIR_D64 ||
			 e->f & REUDIR_PRG )
		{
			if ( !(e->f & REUDIR_MARKSYNC) )
			{
				int idx = indexOfSyncFile_FileNameSize( syncRemoveFiles, nRemoveFiles, (char*)e->filename, e->size );
				if ( idx != -1 )
				{
					// file exists on IECDevice and we marked it for deletion. Now we undo this!
					removeSyncFile( syncRemoveFiles, &nRemoveFiles, (char*)e->path, (char*)e->filename, (char*)e->name, e->size );
					e->f |= REUDIR_MARKSYNC;
				} else
				{
					// we avoid name clashes as on iecdevice files will be identified by "name" only
					int idx1 = indexOfSyncFile_FileNameOnly( syncFileOnDevice, nSyncFileOnDevice, (char*)e->filename );
					int idx2 = indexOfSyncFile_FileNameOnly( syncFileChanges, nSyncFileChanges, (char*)e->filename );

					if ( idx1 != -1 || idx2 != -1 )
					{
						// a file with the same filename is already on the IECDevice or to be synced
						// todo: display warning
						showWarningMessage = 1;
						showWarningTimeout = 0;
						showWarningPosition = -1;
						sprintf( warningMessage, "filename already used on IECBuddy" );
					} else
					{
						// 2 or more files on SD-card (from different subdirectories may have the same filename)
						addSyncFile( syncFileChanges, &nSyncFileChanges, (char*)e->path, (char*)e->filename, (char*)e->name, e->size, 0 );
						e->f |= REUDIR_MARKSYNC;
					}
				}
			} else
			{
				e->f &= ~REUDIR_MARKSYNC;
				// remove from list of files to be synced
				int idxA = indexOfSyncFile_FileNameSize( syncFileOnDevice, nSyncFileOnDevice, (char*)e->filename, e->size );
				if ( idxA != -1 )
				{
					// file is on the IECDevice and we mark the file to deletion
					addSyncFile( syncRemoveFiles, &nRemoveFiles, (char*)e->path, (char*)e->filename, (char*)e->name, e->size, 0 );
				} else
				{
					// file is not yet on IECDevice, it has just been added to the list for syncing, and we'll remove it from there
					removeSyncFile( syncFileChanges, &nSyncFileChanges, (char*)e->path, (char*)e->filename, (char*)e->name, e->size );
				}
			}
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

void fadeBG_FG( int curtime, int totaltime, int fadesteps, int *fBG, int *fFG )
{
	if ( curtime < fadesteps )
	{
		// fade away normal text
		*fBG = curtime;	// => 0 .. fadesteps - 1
		*fFG = -1;
	} else
	if ( curtime < 2 * fadesteps - 1 ) // fadesteps .. 2 * fadesteps - 2
	{
		// fade in error message
		*fFG = 2 * fadesteps - 2 - curtime; 
		*fBG = -1;
	} else
	if ( curtime < totaltime - 2 * fadesteps )
	{
		*fFG = 0;
		*fBG = -1;
	} else
	if ( curtime < totaltime - fadesteps )
	{
		// fade out error message
		*fFG = curtime - (totaltime - 2 * fadesteps) + 1;
		*fBG = -1;
	} else
	if ( curtime < totaltime - 1 )  // curTime [totaltime-fadesteps .. totalTime - 1]
	{
		*fBG = totaltime - curtime - 1;
		*fFG = -1;
	} else
	{
		*fBG = -1;
		*fFG = -1;
	}	
}

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
	printC64( xp, yp, "              ", 0, 0, 0, 14 );
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

		if ( files[ i ].fileOp & REUDIR_FILEOP_DELETE )
			color = 10; else
		if ( files[ i ].fileOp & REUDIR_FILEOP_RENAME )
			color = 14; 

		u8 printName[ 64 ];
		if ( files[ i ].f & REUDIR_MARKSYNC )
		{
			memcpy( printName, files[ i ].name, 64 );
			printC64( xp-1, yp, (const char*)"\x5b", color, 0 /*i == curPosition ? 0x80 : 0*/, 0, 39 );
			printC64( xp, yp, (const char*)printName, color, i == curPosition ? 0x80 : 0, 0, 39 );
		} else
		{
			memcpy( printName, files[ i ].name, 64 );
			printC64( xp, yp, (const char*)printName, color, i == curPosition ? 0x80 : 0, 0, 39 );
		}

		// icons
		if ( files[ i ].f & (REUDIR_VSFIMAGE) )
			printC64( xp + 23, yp, (const char*)"\x5e", color, i == curPosition ? 0x80 : 0, 0, 39 ); else
		if ( files[ i ].f & (REUDIR_PRG) )
			printC64( xp + 23, yp, (const char*)"\x5d", color, i == curPosition ? 0x80 : 0, 0, 39 ); else
		if ( files[ i ].f & (REUDIR_D64) )
			printC64( xp + 23, yp, (const char*)"\x5c", color, i == curPosition ? 0x80 : 0, 0, 39 ); 
		if ( files[ i ].f & (REUDIR_REUIMAGE|REUDIR_GEOIMAGE) )
			printC64( xp + 23, yp, (const char*)"\x60", color, i == curPosition ? 0x80 : 0, 0, 39 ); 

		yp ++;
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

	if ( showWarningMessage )
	{
		int bFade = max( 0, min( 6, min( showWarningTimeout, 50 - showWarningTimeout ) ) );

		if ( bFade > 0 ) 
		{
			for ( int y = 17; y < 21; y++ )
			{
				int f = max( 0, bFade - ( 20 - y ) * 2 );
				for ( int i = 0; i < 40; i++ )
				{
					c64ColorRAM[ y * 40 + i ] = fadeTabStep[ c64ColorRAM[ y * 40 + i ] ][ f ];
				}
			}
		}


		if ( bFade >= 3 )
		{
			int fFade = 5 - max( 0, min( 5, min( showWarningTimeout, 50 - showWarningTimeout ) - 3 ) );
			int c = fadeTabStep[ 10 ][ fFade ];
			printC64( 0, 19, (const char*)"________________________________________", c, 0, 0, 40 );
			printC64( 3, 19+1, (const char*)warningMessage, c, 0, 0, 39 );
		}

		showWarningTimeout ++;
		if ( showWarningTimeout > 50 )
			showWarningMessage = 0;
	}
}
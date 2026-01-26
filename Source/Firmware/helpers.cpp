/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
        - this file contains some code already used in Sidekick64
 Copyright (c) 2019-2026 Carsten Dachsbacher <frenetic@dachsbacher.de>

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

#include "helpers.h"
#include <circle/util.h>

unsigned char toupper( unsigned char c )
{
	if ( c >= 'a' && c <= 'z' )
		return c + 'A' - 'a';
	return c;
}

char *strupr( unsigned char *s )
{
	unsigned char *p;

  	for ( p = s; *p; p++ )
    	*p = toupper(*p);

  	return (char *)s;
}

void strupr( char *d, char *s )
{
	strcpy( d, s );
	strupr( (unsigned char*)d );
}

void makeFileStructure( const char *DRIVE )
{
	FATFS m_FileSystem;

	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK ) return;

	f_mkdir( "RAD_PRINT" );
	f_chdir( "RAD_PRG" );
	f_mkdir( "IECBuddy" );

	if ( f_mount( 0, DRIVE, 0 ) != FR_OK ) return;
}

// file reading
int readFile( CLogger *logger, const char *DRIVE, const char *FILENAME, u8 *data, u32 *size )
{
	FATFS m_FileSystem;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot mount drive: %s", DRIVE );

	// get filesize
	FILINFO info;
	u32 result = f_stat( FILENAME, &info );
	u32 filesize = (u32)info.fsize;

	// open file
	FIL file;
	result = f_open( &file, FILENAME, FA_READ | FA_OPEN_EXISTING );
	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogNotice, "Cannot open file: %s", FILENAME );

		if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
			logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );

		return 0;
	}

	*size = filesize;

	// read data in one big chunk
	u32 nBytesRead;
	result = f_read( &file, data, filesize, &nBytesRead );

	if ( result != FR_OK )
		logger->Write( "RAD", LogError, "Read error" );

	if ( f_close( &file ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot close file" );

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );
	
	return 1;
}

int getFileSize( CLogger *logger, const char *DRIVE, const char *FILENAME, u32 *size )
{
	FATFS m_FileSystem;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot mount drive: %s", DRIVE );

	// get filesize
	FILINFO info;
	u32 result = f_stat( FILENAME, &info );

	if ( result != FR_OK )
		return 0;

	*size = (u32)info.fsize;

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );
	
	return 1;
}

// file writing
int writeFile( CLogger *logger, const char *DRIVE, const char *FILENAME, u8 *data, u32 size )
{
	FATFS m_FileSystem;

	// mount file system
	if ( f_mount( &m_FileSystem, DRIVE, 1 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot mount drive: %s", DRIVE );

	// open file
	FIL file;
	u32 result = f_open( &file, FILENAME, FA_WRITE | FA_CREATE_ALWAYS );
	if ( result != FR_OK )
	{
		logger->Write( "RAD", LogNotice, "Cannot open file: %s", FILENAME );
		return 0;
	}

	// write data in one big chunk
	u32 nBytesWritten;
	result = f_write( &file, data, size, &nBytesWritten );

	if ( result != FR_OK )
		logger->Write( "RAD", LogError, "Read error" );

	if ( f_close( &file ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot close file" );

	// unmount file system
	if ( f_mount( 0, DRIVE, 0 ) != FR_OK )
		logger->Write( "RAD", LogPanic, "Cannot unmount drive: %s", DRIVE );
	
	return 1;
}


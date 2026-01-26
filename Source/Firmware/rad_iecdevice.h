/*

  {_______            {_          {______
		{__          {_ __               {__
		{__         {_  {__               {__
	 {__           {__   {__               {__
 {______          {__     {__              {__
	   {__       {__       {__            {__
		 {_________         {______________		Expansion Unit

 RAD IECDevice Interfacing
 Copyright (c) 2022-2025 Carsten Dachsbacher <frenetic@dachsbacher.de> and David Hansel

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
#ifndef _rad_iecdevice_h
#define _rad_iecdevice_h

#include <circle/devicenameservice.h>
#include <circle/usb/usbhcidevice.h>
#include <circle/usb/usbserialch341.h>
#include <circle/usb/usbserialft231x.h>
#include <circle/usb/usbserialpl2303.h>
#include <circle/usb/usbserialcdc.h>
#include <SDCard/emmc.h>
#include <fatfs/ff.h>
#include <circle/util.h>
#include "helpers.h"
#include "dirscan.h"

typedef int32_t  StatusType;
typedef uint32_t CommandType;

typedef struct
{
	u8  path[ 1024 ], 		// path on SD-card
		filename[ 256 ],	// filename (both SD and IECDevice)
		name[ 256 ];          // formatted name + filesize
	u32	size, flags;
} IECSYNCFILE;

#define SHOW_IEC_FILE_DELETE    (1 << 8)
#define SHOW_IEC_FILE_NEW       (1 << 9)
#define SHOW_IEC_FILE_MODIFIED  (1 << 10)

#define MAX_SYNC_FILES	512

//#define DEBUG_OUT_IECDEVICE

extern int iecDevDriveLetter;

extern void initSerialOverUSB_IECDevice( CInterruptSystem *_pInterrupt, CTimer *_pTimer, CDeviceNameService *pDeviceNameService, bool onlyInitUSB );

extern bool send_data( uint32_t length, const uint8_t *buffer );
extern bool recv_data( uint32_t length, uint8_t *buffer );
extern StatusType sendDriveCommand( const char *cmd );
extern StatusType readDir( IECSYNCFILE *fl, int nMaxFL, int &nFiles, int &bytesFree );
extern void toPETSCII( const char *s, char *d );
extern void fromPETSCII( const char *s, char *d );
extern StatusType getFile( const char *fname, uint8_t *data, uint32_t *len );
extern StatusType getDriveStatus( char *drivestatus );
extern StatusType putFile( const char *fname, const char *data, u32 length );
extern StatusType setConfigValue( const char *key, const char *value );
extern StatusType getConfigValue( const char *key, char *value );
extern StatusType clearConfig();
extern void readSyncFile( const char *syncfile, IECSYNCFILE *syncData, u32 *nSyncData );
extern void writeSyncFile( const char *syncfile, IECSYNCFILE *syncData, u32 *nSyncData );
extern void loadSyncDataFromSD();
extern void saveSyncDataToSD();


typedef int32_t  StatusType;

extern s32 indexOfSyncFile_FileNameSize( IECSYNCFILE *list, u32 nFiles, const char *name, u32 size );
extern s32 indexOfSyncFile_FileNameOnly( IECSYNCFILE *list, u32 nFiles, const char *filename );
extern s32 addSyncFile( IECSYNCFILE *list, u32 *nFiles, const char *path, const char *filename, const char *name, u32 filesize, u32 flags );
extern s32 removeSyncFile( IECSYNCFILE *list, u32 *nFiles, const char *path, const char *filename, const char *name, u32 filesize );
extern s32 removeSyncFile_FileNameOnly( IECSYNCFILE *list, u32 *nFiles, const char *name, u32 filesize );
extern StatusType readDir( IECSYNCFILE *fl, int nMaxFL, int &nFiles, int &bytesFree );

extern StatusType getFile( const char *fname, uint8_t *data, uint32_t *len );
extern StatusType putFile( const char *fname, const char *data, u32 length );
extern StatusType sendDriveCommand( const char *cmd );

extern void getIECUpdateStatistics();


extern int IECDevicePresent;

extern u32 nSyncFileOnDevice;
extern IECSYNCFILE	syncFileOnDevice[ MAX_SYNC_FILES ];

extern u32 nSyncFileChanges;
extern IECSYNCFILE	syncFileChanges[ MAX_SYNC_FILES ];

extern u32 nRemoveFiles;
extern IECSYNCFILE	syncRemoveFiles[ MAX_SYNC_FILES ];

extern IECSYNCFILE iecFiles[ MAX_SYNC_FILES ];
extern int iecBytesFree, iecNumFiles;

extern u32 nSyncFileFavorites;
extern IECSYNCFILE syncFileFavorites[ MAX_SYNC_FILES ];

extern void markSyncFilesRAD(); // IECSYNCFILE *syncFile, u32 nSyncFile );

extern void unmarkAllFiles();


#endif

/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RAD IECDevice Interfacing
 Copyright (c) 2022-2026 Carsten Dachsbacher <frenetic@dachsbacher.de> and David Hansel

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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rad_iecdevice.h"
#include "protocol.h"

extern CLogger *logger;

static const char DRIVE[] = "SD:";

static const char LOGOFILE[]     = "SD:RAD/iecdevice.gif";
static const char SYNCFILE[]     = "SD:RAD/iecdevice.sync";
static const char SYNCFILE_CFG[] = "SD:RAD/iecdevice.cfg";
static const char SYNCFILE_RM[]  = "SD:RAD/iecdevice.rm";
static const char SYNCFILE_CH[]  = "SD:RAD/iecdevice.ch";
static const char SYNCFILE_FAV[] = "SD:RAD/iecdevice.fav";

int iecDevDriveLetter = 8;

u8 mempoolIECDev[ 2048 * 1024 ] = {0};

int toupperString( char *dst, int maxLength, char *src )
{
	int i = 0;
	while ( i < maxLength && src[ i ] != 0 )
	{
		dst[ i ] = toupper( src[ i ] );
		i ++;
	}
	dst[ i ] = 0;
	return i;
}

StatusType deleteFile( const char *fname );
StatusType deleteHiddenFiles( int keepLogo = 1 );
StatusType existsFileIECDevice( const char *searchname, bool *found );

void updateIECDeviceLogo( bool immediate = false )
{
	u32 fileSize;

	bool found = false;
	existsFileIECDevice( (const char*)"$DEFAULT.GIF$", &found );

	if ( !found && getFileSize( logger, DRIVE, LOGOFILE, &fileSize ) ) // if logo exists on SD but not on IECDevice
	{
#ifdef DEBUG_OUT_IECDEVICE
		logger->Write( "[LOGO]", LogNotice, "uploading new logo" );
#endif

		readFile( logger, DRIVE, LOGOFILE, mempoolIECDev, &fileSize );

		if ( found ) deleteFile( "$DEFAULT.GIF$" );

		putFile( "$DEFAULT.GIF$", (char*)mempoolIECDev, fileSize );

		if ( immediate )
		{
			StatusType sendGIF(const uint8_t *buffer, uint32_t nBytes, int32_t x, int32_t y);
			sendGIF( mempoolIECDev, fileSize, 0, 0 );
		}
	}
}


#define SERIAL_DEVICE CUSBSerialDevice

CInterruptSystem	*pInterrupt;
CTimer				*pTimer;
CDeviceNameService  *pDeviceNameService;
CUSBHCIDevice     	*m_USBHCI;
SERIAL_DEVICE	    *m_pUSBSerial;

void initSerialOverUSB_IECDevice( CInterruptSystem *_pInterrupt, CTimer *_pTimer, CDeviceNameService *_pDeviceNameService, bool onlyInitUSB )
{
	// init serial-over-USB for IECDevice
	pInterrupt = _pInterrupt;
	pTimer = _pTimer;
	pDeviceNameService = _pDeviceNameService;

	IECDevicePresent = 0;

	m_USBHCI = new CUSBHCIDevice( pInterrupt, pTimer, FALSE );
	if ( !m_USBHCI->Initialize() )
	{
		logger->Write( "[USB]", LogNotice, "error initializing CUSBHCIDevice" );
	}

	m_pUSBSerial = (SERIAL_DEVICE *)pDeviceNameService->GetDevice( "utty1", FALSE );
	if ( m_pUSBSerial != 0 )
	{
		#ifdef DEBUG_OUT_IECDEVICE
		logger->Write( "[USB]", LogNotice, "USB device detected" );
		#endif

		IECDevicePresent = 1;

		if ( onlyInitUSB ) return;

		sendDriveCommand( "CD:..");

		loadSyncDataFromSD();

		char favList[ 10 * 32 + 16 ];
		memset( favList, 0, 10 * 32 + 1 );

		//readDir( iecFiles, MAX_SYNC_FILES, iecNumFiles, iecBytesFree );

		char t[ 8 ];
		sprintf( t, "X%d", iecDevDriveLetter );
		sendDriveCommand( t );
		
		extern void makeFileStructure( const char *DRIVE );
		makeFileStructure( DRIVE );

		updateIECDeviceLogo();
	}
}

bool send_data( uint32_t length, const uint8_t *buffer )
{
	int bytes2send = length;
	const uint8_t *b = buffer;

	while ( bytes2send > 0 )
	{
		int nsend = min( 2048, bytes2send );
		int32_t n = m_pUSBSerial->Write( (const char *)b, nsend );
		b += nsend;
		bytes2send -= nsend;

		if ( n < 0 )
		{
			#ifdef DEBUG_OUT_IECDEVICE
			logger->Write( "[IEC-send]", LogNotice, "problem with sending" );
			#endif
		}
	}

	return true;
}

#define RECV_BUF_SIZE 65536
u8 recvBuffer[ RECV_BUF_SIZE ];
u32 recvSize = 0;

bool recv_data( uint32_t length, uint8_t *buffer )
{
	u8 temp[ 65536 ];
	int n = -1;

	while ( recvSize < length )
	{
		// need more data!
		n = m_pUSBSerial->Read( (char *)temp, 65536 );

		if ( n > 0 )
		{
			memcpy( &recvBuffer[ recvSize ], temp, n );
			recvSize += n;
		}
	}

	// todo: maybe try (once) again to read from serial in case the message is fragmented?

	memcpy( buffer, recvBuffer, length );

	recvSize -= length;
	for ( u32 i = 0; i < recvSize; i++ )
		recvBuffer[ i ] = recvBuffer[ i + length ];

	return true;
}

IECSYNCFILE tempFile[ 64 ];

StatusType readDir( IECSYNCFILE *fl, int nMaxFL, int &nFiles, int &bytesFree )
{
	StatusType status = ST_OK;
	uint32_t available;

#ifdef DEBUG_OUT_IECDEVICE
	logger->Write( "[IEC]", LogNotice, "read dir" );
#endif

	if ( !send_command( CMD_DIR ) )
		status = ST_COM_ERROR;

	if ( status == ST_OK )
		status = recv_status();

	if ( status == ST_OK )
		if ( !recv_uint( available ) )
			status = ST_COM_ERROR;

	nFiles = 0;

	if ( status == ST_OK )
	{
		bool ok = true;

		uint32_t flags, size;
		u8 *name = (u8 *)mempoolIECDev;

		while ( ( ok = recv_uint( flags ) ) && flags != 0xFFFFFFFF )
		{
			/*if ( ok ) ok = */recv_uint( size );
			/*if ( ok ) ok = */recv_string( name );

			if ( name[ 0 ] == '$' && name[ strlen( (char *)name ) - 1 ] == '$' )
			{
#ifdef DEBUG_OUT_IECDEVICE
				char tmp[ 128 ];
				sprintf( tmp, "%7u %02X %s (HIDDEN)", size, flags, name );
				logger->Write( "[IEC]", LogNotice, tmp );
#endif
			}

			if ( nFiles < nMaxFL && !( name[ 0 ] == '$' && name[ strlen( (char *)name ) - 1 ] == '$' ) )
			{
				memset( fl[ nFiles ].name, 0, 256 );
				memset( fl[ nFiles ].filename, 0, 256 );
				memset( fl[ nFiles ].path, 0, 1024 );
				snprintf( (char *)fl[ nFiles ].name, 256, (char *)name );
				snprintf( (char *)fl[ nFiles ].filename, 256, (char *)name );
				fl[ nFiles ].size = size;
				fl[ nFiles ].flags = flags;
#ifdef DEBUG_OUT_IECDEVICE
				char tmp[ 128 ];
				sprintf( tmp, "%7u %02X %s", size, flags, name );
				logger->Write( "[IEC]", LogNotice, tmp );
#endif
				nFiles ++;
			}
		}

		bytesFree = available;
#ifdef DEBUG_OUT_IECDEVICE
		char tmp[ 128 ];
		sprintf( tmp, "%7u bytes free.\n", available );
		logger->Write( "[IEC]", LogNotice, tmp );
#endif
		if ( !ok ) status = ST_COM_ERROR;
	}

	return status;
}




StatusType existsFileIECDevice( const char *searchname, bool *found )
{
	StatusType status = ST_OK;
	uint32_t available;

	if ( !send_command( CMD_DIR ) )
		status = ST_COM_ERROR;

	if ( status == ST_OK )
		status = recv_status();

	if ( status == ST_OK )
		if ( !recv_uint( available ) )
			status = ST_COM_ERROR;

	*found = false;

	if ( status == ST_OK )
	{
		bool ok = true;

		uint32_t flags, size;
		u8		name[ 1024 ];

		while ( ( ok = recv_uint( flags ) ) && flags != 0xFFFFFFFF )
		{
			recv_uint( size );
			recv_string( name );

			if ( strcmp( searchname, (char*)name ) == 0 )
			{
				*found = true;
			}
		}

		if ( !ok ) status = ST_COM_ERROR;
	}

	return status;
}

StatusType deleteHiddenFiles( int keepLogo )
{
	StatusType status = ST_OK;
	uint32_t available;

	if ( !send_command( CMD_DIR ) )
		status = ST_COM_ERROR;

	if ( status == ST_OK )
		status = recv_status();

	if ( status == ST_OK )
		if ( !recv_uint( available ) )
			status = ST_COM_ERROR;

	int nFiles = 0;
	IECSYNCFILE *fl = (IECSYNCFILE *)mempoolIECDev;

	if ( status == ST_OK )
	{
		bool ok = true;

		uint32_t flags, size;
		u8		name[ 1024 ];

		while ( ( ok = recv_uint( flags ) ) && flags != 0xFFFFFFFF )
		{
			recv_uint( size );
			recv_string( name );

			if ( name[ 0 ] == '$' && name[ strlen( (char*)name ) - 1 ] == '$' )
			{
#ifdef DEBUG_OUT_IECDEVICE
				char tmp[ 1024 ];
				sprintf( tmp, "%7u %02X %s (HIDDEN)", size, flags, name );
				logger->Write( "[IEC]", LogNotice, tmp );
#endif

				if ( keepLogo && strcmp( (char*)name, "$DEFAULT.GIF$" ) == 0 )
				{
					// keep this file
				} else
				{
					memset( fl[ nFiles ].name, 0, 256 );
					memset( fl[ nFiles ].filename, 0, 256 );
					memset( fl[ nFiles ].path, 0, 1024 );
					snprintf( (char*)fl[ nFiles ].filename, 256, (char*)name );
					fl[ nFiles ].size = size;
					fl[ nFiles ].flags = flags;
					nFiles ++;
				}
			}
		}

		if ( !ok ) status = ST_COM_ERROR;
	}

	if ( status == ST_OK )
	{
		for ( int i = 0; i < nFiles; i++ )
		{
			status = deleteFile( (char*)fl[ i ].filename );

			if ( status != ST_OK )
				return status;
		}
	}
	return status;
}


#define SHOW_LOWERCASE 0

void toPETSCII( const char *s, char *d )
{
	for ( size_t i = 0; i < strlen( s ); i++ )
	{
		char c = s[ i ];

		if ( c >= 65 && c <= 90 && SHOW_LOWERCASE )
			c += 32; else
		if ( c >= 97 && c <= 122 )
			c -= 32;

		d[ i ] = c;
	}
	d[ strlen( s ) ] = 0;
}


uint8_t fromPETSCII( uint8_t c )
{
	if ( c == 0xFF )
		c = '~'; else 
	if ( c >= 192 )
		c -= 96;

	if ( c >= 65 && c <= 90 )
		c += 32; else 
	if ( c >= 97 && c <= 122 )
		c -= 32;

	return c;
}

void fromPETSCII( const char *s, char *d )
{
	for ( size_t i = 0; i < strlen( s ); i++ )
		*(uint8_t *)&d[ i ] = fromPETSCII( *(const uint8_t *)&s[ i ] );
}




StatusType getFile( const char *fname, uint8_t *data, uint32_t *len )
{
	char fnamePETSCII[ 256 ];
	StatusType status = ST_OK;

	u32 dPos = 0;
	uint32_t length = 0;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_GETFILE ) )
			status = ST_COM_ERROR;

	// send file name
	if ( status == ST_OK )
	{
		//logger->Write( "[IEC]", LogNotice, "sending name" );
		toPETSCII( fname, fnamePETSCII );
		//logger->Write( "[IEC]", LogNotice, fnamePETSCII );
		if ( !send_string( (const u8 *)fnamePETSCII ) )
			status = ST_COM_ERROR;
	}

	// receive file length
	if ( status == ST_OK )
	{
		//logger->Write( "[IEC]", LogNotice, "receiving length" );
		if ( !recv_uint( length ) )
		{
			status = ST_COM_ERROR;
		}
	}


	*len = length;
	// receive status
	if ( status == ST_OK )
		status = recv_status();

	// receive data
	uint8_t buf[ 1024 ];
	while ( status == ST_OK && length > 0 )
	{
		uint32_t n = min( 1024u, length );

		// receive data block
		if ( !recv_data( n, buf ) )
			status = ST_COM_ERROR;

		// compute and receive checksum
		if ( status == ST_OK )
		{
			uint8_t checksum1 = 0, checksum2 = 0;
			for ( uint32_t j = 0; j < n; j++ ) checksum1 ^= buf[ j ];

			if ( !recv_data( 1, &checksum2 ) )
				status = ST_COM_ERROR;
			else if ( checksum1 != checksum2 )
				status = ST_CHECKSUM_ERROR;
		}

		// receive transmitter status
		if ( status != ST_COM_ERROR && recv_status() == ST_OK )
		{
			// save data block
			if ( status == ST_OK )
			{
				//char tmp[256];
				//sprintf( tmp, "%d", dPos );
				//logger->Write( "[IEC]", LogNotice, tmp );

			//if( fwrite(buf, 1, n, file) != n )
				//status = ST_WRITE_ERROR;
				memcpy( &data[ dPos ], buf, n );
				dPos += n;
			}

			// send our status
			if ( !send_status( status ) )
				status = ST_COM_ERROR;
		}

		length -= n;
	}

	return status;
}

StatusType deleteFile( const char *fname )
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_DELETE_FILE ) )
			status = ST_COM_ERROR;

	char fnamePETSCII[ 256 ];
	toPETSCII( fname, fnamePETSCII );

	// send file name
	if ( status == ST_OK )
		if ( !send_string( (const u8 *)fnamePETSCII ) )
			status = ST_COM_ERROR;

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	return status;
}

StatusType reboot()
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_REBOOT ) )
			status = ST_COM_ERROR;

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	return status;
}


StatusType getDriveStatus( char *drivestatus )
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_DRIVESTATUS ) )
			status = ST_COM_ERROR;

	if ( status == ST_OK )
		if ( !recv_string( (u8 *)drivestatus ) )
			status = ST_COM_ERROR;

	return status;
}

StatusType sendDriveCommand( const char *cmd )
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_DRIVECMD ) )
			status = ST_COM_ERROR;

	// send command string
	if ( status == ST_OK )
	{
		char tmp[ 1024 ];
		toPETSCII( cmd, tmp );
#ifdef DEBUG_OUT_IECDEVICE
		logger->Write( "[drive command]", LogNotice, tmp );
#endif
		if ( !send_string( (u8 *)tmp ) )
			status = ST_COM_ERROR;
	}

	// receive status
	if ( status == ST_OK )
		status = recv_status();

#ifdef DEBUG_OUT_IECDEVICE
	if ( status == ST_OK )
		logger->Write( "[drive command]", LogNotice, "status OK" );
#endif

	return status;
}


StatusType putFile( const char *fname, const char *data, u32 length )
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_PUTFILE ) )
			status = ST_COM_ERROR;

	// send file name
	if ( status == ST_OK )
	{
		char tmp[ 1024 ];
		toPETSCII( fname, tmp );
		if ( !send_string( (u8 *)tmp ) )
			status = ST_COM_ERROR;
	}

	// send file length
	if ( status == ST_OK )
		if ( !send_uint( length ) )
			status = ST_COM_ERROR;

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	// send data
	uint8_t buf[ 1024 ];
	u32 ofs = 0;
	while ( status == ST_OK && length > 0 )
	{
		uint32_t n = min( 1024u, length );
		u32 i = n;
		memcpy( buf, &data[ ofs ], n );

		// receiver expects a full block so we send it even if read failed
		if ( !send_data( n, buf ) ) status = ST_COM_ERROR;

		// compute and send checksum for data block
		uint8_t checksum = 0;
		for ( uint32_t j = 0; j < i; j++ ) checksum ^= buf[ j ];
		if ( status == ST_OK )
			if ( !send_data( 1, &checksum ) )
				status = ST_COM_ERROR;

		// check if we successfully read the data
		if ( i != n ) status = ST_READ_ERROR;

		// send status
		if ( status != ST_COM_ERROR )
			if ( !send_status( status ) )
				status = ST_COM_ERROR;

		// receive status
		if ( status == ST_OK )
			status = recv_status();

		ofs += i;
		length -= i;
	}

	return status;
}





StatusType setConfigValue( const char *key, const char *value )
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_SET_CONFIG_VAL ) )
			status = ST_COM_ERROR;

	// send key and value
	if ( status == ST_OK )
		if ( !send_string( (const u8 *)key ) || !send_string( (const u8 *)value ) )
			status = ST_COM_ERROR;

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	return status;
}

StatusType setConfigValueArray( const char *key, const char *value, int lenghth )
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_SET_CONFIG_VAL ) )
			status = ST_COM_ERROR;

	// send key and value
	if ( status == ST_OK )
		if ( !send_string( (const u8 *)key ) || !send_string_length( (const u8 *)value, lenghth ) )
			status = ST_COM_ERROR;

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	return status;
}

StatusType getConfigValue( const char *key, char *value )
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_GET_CONFIG_VAL ) )
			status = ST_COM_ERROR;

	// send key
	if ( status == ST_OK )
		if ( !send_string( (const u8 *)key ) )
			status = ST_COM_ERROR;

	char tempValue[ 65536 ];

	// receive value
	int len = 0;
	if ( status == ST_OK )
	{
		len = recv_string( (u8 *)tempValue );
		if ( !len )
			status = ST_COM_ERROR;
	}
	//   sprintf( tempValue, "blub" );

	if ( status == ST_OK )
	{
#ifdef DEBUG_OUT_IECDEVICE
		char tempString[ 14096 + 4 ];
		sprintf( tempString, "Value of config '%s' is '%s' (length=%d)\n", key, tempValue, len );
		logger->Write( "[CFG]", LogNotice, (char *)tempString );
#endif
		sprintf( tempValue, "bla" );
		tempValue[ len + 1 ] = 0;
		strcpy( value, tempValue );
	} else
		value[ 0 ] = 0;

	return status;
}


StatusType clearConfig()
{
	StatusType status = ST_OK;

	// send command
	if ( status == ST_OK )
		if ( !send_command( CMD_CLEAR_CONFIG ) )
			status = ST_COM_ERROR;

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	return status;
}


StatusType sendBitmap( const uint8_t *buffer, uint32_t x, uint32_t y, uint32_t w, uint32_t h )
{
	uint32_t nbytes = w * h * 2;
	StatusType status = ST_OK;

	if ( !send_command( CMD_SHOW_BITMAP ) )
		status = ST_COM_ERROR;

	// send position/size
	if ( status == ST_OK )
		if ( !send_uint( x ) || !send_uint( y ) || !send_uint( w ) || !send_uint( h ) )
			status = ST_COM_ERROR;

	// send image data
	if ( status == ST_OK )
	{
		const uint8_t *ptr = buffer;
		while ( nbytes > 0 && ( status = recv_status() ) == ST_OK )
		{
			uint32_t n = min( 1024u, nbytes );
			if ( send_data( n, ptr ) )
			{
				ptr += n;
				nbytes -= n;
			} else
				status = ST_COM_ERROR;
		}
	}

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	return status;
}


StatusType sendGIF( const uint8_t *buffer, uint32_t nBytes, int32_t x, int32_t y )
{
	StatusType status = ST_OK;

	// send command
	if ( !send_command( CMD_SHOW_GIF ) )
		status = ST_COM_ERROR;

	// send position
	if ( status == ST_OK )
		if ( !send_sint( x ) || !send_sint( y ) || !send_uint( nBytes ) )
			status = ST_COM_ERROR;

	// send image data
	if ( status == ST_OK )
	{
		const uint8_t *ptr = buffer;
		while ( nBytes > 0 && status == ST_OK && ( status = recv_status() ) == ST_OK )
		{
			uint32_t n = min( 1024u, nBytes );
			if ( send_data( n, ptr ) )
			{
				ptr += n;
				nBytes -= n;
			} else
				status = ST_COM_ERROR;
		}
	}

	// receive status
	if ( status == ST_OK )
		status = recv_status();

	return status;
}

#define PRINTER_DRIVER_ASCII	0
#define PRINTER_DRIVER_NL10		1
#define PRINTER_DRIVER_NL10_PDF	2

#include "Printer/drv-nl10.h"

// ------------ Printer data file reader class

class PrinterDataFile
{
	public:
	PrinterDataFile( uint8_t *data, uint32_t length );
	~PrinterDataFile();

	bool    IsNewDataBlock() { bool b = m_newBlock; m_newBlock = false; return b; }
	uint8_t GetChannel() { return m_channel; }
	int     GetNextByte();

	private:
	void ReadHeader();

	uint8_t *m_data;
	uint32_t m_length, m_pos;
	uint8_t m_zeroCount;
	uint8_t m_channel;
	bool    m_newBlock;
};


PrinterDataFile::PrinterDataFile( uint8_t *data, uint32_t length ) //string fname);
{
	m_data = data;
	m_length = length;
	m_pos = 0;
	m_channel = 0xFF;
	m_newBlock = false;
	m_zeroCount = 0;
	ReadHeader();
}

PrinterDataFile::~PrinterDataFile()
{
}

int PrinterDataFile::GetNextByte()
{
	uint8_t data;

	if ( m_zeroCount > 0 )
	{
		m_zeroCount--;
		return 0;
	}
	//else if( fread(&data, 1, 1, m_file)==1 )
	else if ( m_pos < m_length ) //fread(&data, 1, 1, m_file)==1 )
	{
		data = m_data[ m_pos ++ ];
		if ( data == 0 )
		{
			//fread(&m_zeroCount, 1, 1, m_file);
			m_zeroCount = m_data[ m_pos ++ ];

			if ( m_zeroCount == 0 )
			{
				ReadHeader();
				return GetNextByte();
			} else
			{
				m_zeroCount--;
				return 0;
			}
		} else
			return data;
	}

	return -1;
}

void PrinterDataFile::ReadHeader()
{
	uint8_t buffer[ 4 ];
	if ( m_pos + 4 <= m_length )
	{
		memcpy( buffer, &m_data[ m_pos ], 4 );
		m_pos += 4;
		if ( buffer[ 0 ] == 'P' && buffer[ 1 ] == 'D' && buffer[ 2 ] == 'B' )
		{
			m_newBlock = true;
			if ( buffer[ 3 ] >= '0' && buffer[ 3 ] <= '9' )
				m_channel = buffer[ 3 ] - '0';
			else if ( buffer[ 3 ] >= 'A' && buffer[ 3 ] <= 'F' )
				m_channel = buffer[ 3 ] - 'A' + 10;
			else
				m_pos = m_length;
		} else
			m_pos = m_length;

	} else
		m_pos = m_length;
}

#define PRINTER_DRIVER_ASCII	0
#define PRINTER_DRIVER_NL10		1
#define PRINTER_DRIVER_NL10_PDF	2

#define PRINTER_OUTPUT_MAX_SIZE	( 32 * 1024 * 1024 )
char		printOutputFilename[ 1024 ];
uint8_t 	printOutputFile[ PRINTER_OUTPUT_MAX_SIZE ];
uint32_t 	printOutputPos, printOutputSize;

char *strdup( const char *s )
{
	char *t = (char *)malloc( strlen( s ) + 1 );
	strcpy( t, s );
	return t;
}

int print_fopen_write( const char *fn )
{
	strcpy( printOutputFilename, fn );
	printOutputPos = printOutputSize = 0;
	return 1;
}

int print_fclose()
{
	char tempString[ 256 ];

	extern CLogger *logger;

	sprintf( tempString, "SD:RAD_PRINT/%s", printOutputFilename );
	writeFile( logger, DRIVE, tempString, printOutputFile, printOutputSize );

	printOutputPos = printOutputSize = 0;
	return 1;
}

int	print_fwrite( unsigned char *d, int n, int s )
{
	if ( printOutputPos + n * s >= PRINTER_OUTPUT_MAX_SIZE )
		return 0;

	memcpy( &printOutputFile[ printOutputPos ], d, n * s );

	printOutputPos += n * s;

	if ( printOutputPos > printOutputSize ) printOutputSize = printOutputPos;

	return n * s;
}

int print_fprintf_( const char *format, va_list args )
{
	char temp[ 4096 ];
	vsnprintf( temp, 4096, format, args );
	int l = strlen( temp );
	print_fwrite( (unsigned char *)temp, 1, strlen( temp ) );
	return l;
}

int print_fprintf( const char *format, ... )
{
	va_list args;
	va_start( args, format );
	int l = print_fprintf_( format, args );
	va_end( args );
	return l;
}

int print_ftell()
{
	return printOutputPos;
}

int print_fseek( int ofs, int origin )
{
	if ( origin == 1 )
		printOutputPos = printOutputSize; else
		if ( origin == 0 )
			printOutputPos = ofs;
	return printOutputPos;
}


void print_data( uint8_t *pdata, uint32_t len, char *filename )
{
	int outputformat;
	char outputfile[ 256 ];

	sprintf( outputfile, "%s.txt", filename );

	// printer driver PRINTER_DRIVER_ASCII
	const char *codes1[ 32 ] =
	{ "", "($1)","($2)","($3)","($4)","(WHT)","($6)","($7)",
	 "(DISH)","(ENSH)","","($11)","($12)","\n","(SWLC)","($15)",
	 "($16)","(DOWN)","(RVS)","(HOME)","(DEL)","($21)","($22)","($23)",
	 "($24)","($25)","($26)","(ESC)","(RED)","(RGHT)","(GRN)","(BLU)" };

	const char *codes2[ 32 ] =
	{ "($128)","(ORNG)","($130)","($131)","($132)","(F1)","(F3)","(F5)",
	 "(F7)","(F2)","(F4)","(F6)","(F8)","(SHRT)","(SWUC)","($143)",
	 "(BLK)","(UP)","(OFF)","(CLR)","(INST)","(BRN)","(LRED)","(GRY1)",
	 "(GRY2)","(LGRN)","(LBLU)","(GRY3)","(PUR)","(LEFT)","(YEL)","(CYN)" };

	int data;
	PrinterDataFile f( pdata, len );//datafile);
	print_fopen_write( outputfile );

	while ( ( data = f.GetNextByte() ) >= 0 )
	{
		if ( data >= 0 && data < 32 )
			print_fprintf( "%s", codes1[ data ] );
		else if ( data >= 128 && data < 128 + 32 )
			print_fprintf( "%s", codes2[ data - 128 ] );
		else
			print_fprintf( "%c", (char)fromPETSCII( data ) );
	}

	print_fclose();


	// printer driver PRINTER_DRIVER_NL10_PDF and BMP
	for ( int i = 0; i < 2; i++ )
	{
		if ( i == 0 )
		{
			sprintf( outputfile, "%s.pdf", filename );
			outputformat = OUTPUT_FORMAT_PDF;
		} else
		{
			sprintf( outputfile, "%s", filename ); // index and type will be added later
			outputformat = OUTPUT_FORMAT_BMP;
		}

		drv_nl10_init( outputfile, outputformat );

		int data;
		PrinterDataFile f( pdata, len );
		while ( ( data = f.GetNextByte() ) >= 0 )
		{
			if ( f.IsNewDataBlock() )
				drv_nl10_open( f.GetChannel() );

			drv_nl10_putc( data );
		}

		drv_nl10_formfeed();
		drv_nl10_close();
		drv_nl10_shutdown();
	}
}


void print_data_raw( uint8_t *pdata, uint32_t len )
{
	char outputfile[ 256 ];

	outputfile[ 0 ] = 0;

	drv_nl10_init( outputfile, OUTPUT_FORMAT_RAW );

	int data;
	PrinterDataFile f( pdata, len );//datafile);
	while ( ( data = f.GetNextByte() ) >= 0 )
	{
		if ( f.IsNewDataBlock() )
			drv_nl10_open( f.GetChannel() );

		drv_nl10_putc( data );
	}

	drv_nl10_formfeed();
	drv_nl10_close();
	drv_nl10_shutdown();
}






void readSyncFile( const char *syncfile, IECSYNCFILE *syncData, u32 *nSyncData )
{
	u32 fileSize;

	if ( getFileSize( logger, DRIVE, syncfile, &fileSize ) ) // if sync data is on SD
	{
		readFile( logger, DRIVE, syncfile, mempoolIECDev, &fileSize );

		*nSyncData = *(u32 *)mempoolIECDev;

		u8 *p = mempoolIECDev + sizeof( u32 );

		for ( u32 i = 0; i < *nSyncData; i++ )
		{
			memcpy( syncData[ i ].path, p, 1024 ); p += 1024;
			memcpy( syncData[ i ].filename, p, 256 ); p += 256;
			memcpy( syncData[ i ].name, p, 256 ); p += 256;
			syncData[ i ].size = *(u32 *)p; p += sizeof( u32 );
			syncData[ i ].flags = *(u32 *)p; p += sizeof( u32 );
		}
	} else
	{
		*nSyncData = 0;
	}
}

void writeSyncFile( const char *syncfile, IECSYNCFILE *syncData, u32 *nSyncData )
{
	u8 *p = mempoolIECDev;
	*(u32 *)p = *nSyncData; p += sizeof( u32 );

	u32 size = sizeof( u32 );
	for ( u32 i = 0; i < *nSyncData; i++ )
	{
		memcpy( p, syncData[ i ].path, 1024 ); p += 1024;
		memcpy( p, syncData[ i ].filename, 256 ); p += 256;
		memcpy( p, syncData[ i ].name, 256 ); p += 256;
		*(u32 *)p = syncData[ i ].size; p += sizeof( u32 );
		*(u32 *)p = syncData[ i ].flags; p += sizeof( u32 );
		size += 1024 + 256 + 256 + 2 * sizeof( u32 );
	}
	writeFile( logger, DRIVE, syncfile, mempoolIECDev, size );
}

void loadSyncDataFromSD()
{
	u32 fileSize;

	if ( getFileSize( logger, DRIVE, SYNCFILE_CFG, &fileSize ) ) // if sync data is on SD
	{
		readFile( logger, DRIVE, SYNCFILE_CFG, mempoolIECDev, &fileSize );
		u8 *d = (u8 *)mempoolIECDev;
		iecDevDriveLetter = *d;
	} else
	{
		iecDevDriveLetter = 8;
	}

	readSyncFile( SYNCFILE, syncFileOnDevice, &nSyncFileOnDevice );
	readSyncFile( SYNCFILE_RM, syncRemoveFiles, &nRemoveFiles );
	readSyncFile( SYNCFILE_CH, syncFileChanges, &nSyncFileChanges );
	readSyncFile( SYNCFILE_FAV, syncFileFavorites, &nSyncFileFavorites );


#ifdef DEBUG_OUT_IECDEVICE
	char tempString[ 2048 ];
	logger->Write( "----------------------", LogNotice, "" );
	logger->Write( "Files currently in syncFileOnDevice-list", LogNotice, "" );
	for ( u32 i = 0; i < nSyncFileOnDevice; i++ )
	{
		sprintf( (char *)tempString, "%s (%d bytes)", syncFileOnDevice[ i ].filename, syncFileOnDevice[ i ].size );
		logger->Write( "", LogNotice, (char *)tempString );
	}
	logger->Write( "----------------------", LogNotice, "" );
#endif
}

void saveSyncDataToSD()
{
#ifdef DEBUG_OUT_IECDEVICE
	logger->Write( "Saving SyncData to SD", LogNotice, "" );
#endif

	u8 *d = (u8 *)mempoolIECDev;
	*d = (u8)iecDevDriveLetter;
	writeFile( logger, DRIVE, SYNCFILE_CFG, mempoolIECDev, 1 );

	writeSyncFile( SYNCFILE, syncFileOnDevice, &nSyncFileOnDevice );
	writeSyncFile( SYNCFILE_RM, syncRemoveFiles, &nRemoveFiles );
	writeSyncFile( SYNCFILE_CH, syncFileChanges, &nSyncFileChanges );
	writeSyncFile( SYNCFILE_FAV, syncFileFavorites, &nSyncFileFavorites );
}


int IECDevicePresent = 0;
int curIECPosition = 0, curIECScrollPos = 0;
u32 nSyncFile = 0;
IECSYNCFILE	syncFile[ MAX_SYNC_FILES ];

// A) this list holds the files which _according to our history_ are stored on the IECDevice
u32 nSyncFileOnDevice = 0;
IECSYNCFILE	syncFileOnDevice[ MAX_SYNC_FILES ];

// B) this list contains the changes the user makes in the file-menus (which will later be applied to the IECDevice)
u32 nSyncFileChanges = 0;
IECSYNCFILE	syncFileChanges[ MAX_SYNC_FILES ];

// C) this list contains the files to be removed from IECDevice
u32 nRemoveFiles = 0;
IECSYNCFILE	syncRemoveFiles[ MAX_SYNC_FILES ];

// D) this list contains the list of favorites
u32 nSyncFileFavorites = 0;
IECSYNCFILE	syncFileFavorites[ MAX_SYNC_FILES ];

IECSYNCFILE iecFiles[ MAX_SYNC_FILES ];
int iecBytesFree = 0, iecNumFiles = 0;


s32 addSyncFile( IECSYNCFILE *list, u32 *nFiles, const char *path, const char *filename, const char *name, u32 filesize, u32 flags )
{
	u32 i = *nFiles;

	if ( i >= MAX_SYNC_FILES ) return -1;

	memcpy( list[ i ].path, path, 1024 );
	memcpy( list[ i ].filename, filename, 256 );
	memcpy( list[ i ].name, name, 256 );
	list[ i ].size = filesize;
	list[ i ].flags = flags;

	*nFiles = i + 1;

#ifdef DEBUG_OUT_IECDEVICE
	char tempString[ 256 ];
	sprintf( (char *)tempString, "adding '%s'", filename );
	logger->Write( "[addsync]", LogNotice, (char *)tempString );
#endif

	return i;
}


// return index of file in list, or -1 if not contained
s32 indexOfSyncFile( IECSYNCFILE *list, u32 nFiles, const char *path, const char *filename, const char *name, u32 size )
{
	for ( u32 i = 0; i < nFiles; i++ )
	{
		if ( list[ i ].size == size &&
			 strcmp( strupr( list[ i ].filename ), strupr( (u8 *)filename ) ) == 0 &&
			 strcmp( strupr( list[ i ].path ), strupr( (u8 *)path ) ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

// return index of file in list, or -1 if not contained
s32 indexOfSyncFile_FileNameSize( IECSYNCFILE *list, u32 nFiles, const char *filename, u32 size )
{
	for ( u32 i = 0; i < nFiles; i++ )
	{
		if ( list[ i ].size == size &&
			 strcmp( strupr( list[ i ].filename ), strupr( (u8 *)filename ) ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

s32 indexOfSyncFile_FileNameOnly( IECSYNCFILE *list, u32 nFiles, const char *filename )
{
	for ( u32 i = 0; i < nFiles; i++ )
	{
		if ( strcmp( strupr( list[ i ].filename ), strupr( (u8 *)filename ) ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

s32 removeSyncFile_FileNameOnly( IECSYNCFILE *list, u32 *nFiles, const char *filename, u32 filesize )
{
	s32 idx = indexOfSyncFile_FileNameSize( list, *nFiles, filename, filesize );

#ifdef DEBUG_OUT_IECDEVICE
	char tempString[ 256 ];
	sprintf( (char *)tempString, "looking for '%s'", filename );
	logger->Write( "[removesync]", LogNotice, (char *)tempString );
#endif

	if ( idx < 0 ) return -1;

#ifdef DEBUG_OUT_IECDEVICE
	sprintf( (char *)tempString, "found at index '%d'", idx );
	logger->Write( "[removesync]", LogNotice, (char *)tempString );
#endif

	memcpy( list[ idx ].path, list[ *nFiles - 1 ].path, 1024 );
	memcpy( list[ idx ].filename, list[ *nFiles - 1 ].filename, 256 );
	memcpy( list[ idx ].name, list[ *nFiles - 1 ].name, 256 );
	list[ idx ].size = list[ *nFiles - 1 ].size;
	list[ idx ].flags = list[ *nFiles - 1 ].flags;

	*nFiles = *nFiles - 1;

	return 1;
}

s32 removeSyncFile_Idx( IECSYNCFILE *list, u32 *nFiles, u32 idx )
{
	memcpy( list[ idx ].path, list[ *nFiles - 1 ].path, 1024 );
	memcpy( list[ idx ].filename, list[ *nFiles - 1 ].filename, 256 );
	memcpy( list[ idx ].name, list[ *nFiles - 1 ].name, 256 );
	list[ idx ].size = list[ *nFiles - 1 ].size;
	list[ idx ].flags = list[ *nFiles - 1 ].flags;

	*nFiles = *nFiles - 1;

	return 1;
}

s32 removeSyncFile_Idx_OrderPreserving( IECSYNCFILE *list, u32 *nFiles, u32 idx )
{
	for ( u32 i = idx; i < *nFiles - 1; i++ )
	{
		memcpy( list[ i ].path, list[ i + 1 ].path, 1024 );
		memcpy( list[ i ].filename, list[ i + 1 ].filename, 256 );
		memcpy( list[ i ].name, list[ i + 1 ].name, 256 );
		list[ i ].size = list[ i + 1 ].size;
		list[ i ].flags = list[ i + 1 ].flags;
	}

	*nFiles = *nFiles - 1;

	return 1;
}

s32 removeSyncFile( IECSYNCFILE *list, u32 *nFiles, const char *path, const char *filename, const char *name, u32 filesize )
{
	s32 idx = indexOfSyncFile( list, *nFiles, path, filename, name, filesize );

	if ( idx < 0 ) return -1;

	memcpy( list[ idx ].path, list[ *nFiles - 1 ].path, 1024 );
	memcpy( list[ idx ].filename, list[ *nFiles - 1 ].filename, 256 );
	memcpy( list[ idx ].name, list[ *nFiles - 1 ].name, 256 );
	list[ idx ].size = list[ *nFiles - 1 ].size;
	list[ idx ].flags = list[ *nFiles - 1 ].flags;

	*nFiles = *nFiles - 1;

	return 1;
}

s32 addSyncFile( REUDIRENTRY *f )
{
	u32 i = nSyncFile;

	if ( i >= MAX_SYNC_FILES ) return -1;

	memcpy( syncFile[ i ].path, f->path, 1024 );
	memcpy( syncFile[ i ].filename, f->filename, 256 );
	memcpy( syncFile[ i ].name, f->name, 256 );
	syncFile[ i ].size = f->size;
	syncFile[ i ].flags = IECSYNC_NOT_SYNCED;

	nSyncFile ++;

	return i;
}

s32 indexOfSyncFile( const char *path, const char *filename, u32 size )
{
	for ( u32 i = 0; i < nSyncFile; i++ )
	{
		if ( syncFile[ i ].size == size &&
			 strcmp( strupr( syncFile[ i ].filename ), strupr( (u8 *)filename ) ) == 0 &&
			 strcmp( strupr( syncFile[ i ].path ), strupr( (u8 *)path ) ) == 0 )
		{
			return i;
		}
	}
	return -1;
}

s32 removeSyncFile( REUDIRENTRY *f )
{
	s32 idx = indexOfSyncFile( (const char *)f->path, (const char *)f->filename, f->size );

	if ( idx < 0 ) return -1;

	memcpy( syncFile[ idx ].path, syncFile[ nSyncFile - 1 ].path, 1024 );
	memcpy( syncFile[ idx ].filename, syncFile[ nSyncFile - 1 ].filename, 256 );
	memcpy( syncFile[ idx ].name, syncFile[ nSyncFile - 1 ].name, 256 );
	syncFile[ idx ].size = syncFile[ nSyncFile - 1 ].size;
	syncFile[ idx ].flags = syncFile[ nSyncFile - 1 ].flags;

	nSyncFile --;

	return 1;
}


s32 indexOfSyncFileTest( IECSYNCFILE *list, u32 nFiles, const char *path, const char *filename, const char *name, u32 size )
{
	for ( u32 i = 0; i < nFiles; i++ )
	{
		if ( //list[ i ].size == size &&
			 strcmp( strupr( list[ i ].filename ), strupr( (u8 *)filename ) ) == 0 /*&&
			 strcmp( (const char*)list[ i ].name, name ) == 0 &&
			 strcmp( (const char*)list[ i ].path, path ) == 0 */ )
		{
			return i;
		}
	}
	return -1;
}

void markSyncFilesRAD()
{
#ifdef DEBUG_OUT_IECDEVICE
	logger->Write( "[SD]", LogNotice, "markSyncFilesRAD" );
#endif
	for ( int i = 0; i < nFilesAllCategories; i++ )
	{
		extern REUDIRENTRY *files;
		REUDIRENTRY *f = &files[ i ];

		if ( f->f & REUDIR_D64 ||
			 f->f & REUDIR_ZIP ||
			 f->f & REUDIR_SEQ ||
			 f->f & REUDIR_PRG )
		{
			f->f &= ~REUDIR_MARKSYNC;

			int idx = indexOfSyncFile_FileNameSize( syncFileOnDevice, nSyncFileOnDevice, (char *)f->filename, f->size );

			if ( idx == -1 )
				idx = indexOfSyncFile_FileNameSize( syncFileChanges, nSyncFileChanges, (char *)f->filename, f->size );

			if ( idx != -1 )
			{
				int idx_rm = indexOfSyncFile_FileNameSize( syncRemoveFiles, nRemoveFiles, (char *)f->filename, f->size );
				if ( idx_rm == -1 )
					f->f |= REUDIR_MARKSYNC;
			}
		}
	}
}



u8 tempIEC[ 1024 * 1024 ];
char tempString[ 2048 ];
u32 tempFilesize;

u32 filesToCopyFromIEC = 0, filesToUpdateFromIEC = 0, filesToDeleteOnIEC = 0;
s32 spaceNeededOnIEC = 0;
int transferIECPossible = 1;

#define PRINTDATAFILE "$PRINTDATA$"

#define HAS_PRINT_NONE			0
#define HAS_PRINT_NEEDFILENAME	1
#define HAS_PRINT_DELETEONIEC	2
#define HAS_PRINT_PROCESS		3

static int hasPrintIECDevice = HAS_PRINT_NONE;
static int showPrintPreview = 0;
static int updatePrintPreview = 0;
static int previewGenerated = 0;


int getPrint()
{
	hasPrintIECDevice = HAS_PRINT_NONE;
	previewGenerated = 0;

	if ( !IECDevicePresent )
		return 0;

	StatusType status;
	status = getFile( (char *)PRINTDATAFILE, &tempIEC[ 0 ], &tempFilesize );

	if ( status == ST_OK )
	{
		hasPrintIECDevice = HAS_PRINT_NEEDFILENAME;
		showPrintPreview = 0;
	}

	return hasPrintIECDevice;
}

static char printNameStr[ 60 ] = { 0 };
static u8 printNameStrLength = 0;

void getIECUpdateStatistics()
{
	EnableIRQs();

	printNameStrLength = 0;
	memset( printNameStr, 0, 32 );

	filesToCopyFromIEC = filesToUpdateFromIEC = filesToDeleteOnIEC = 0;

	spaceNeededOnIEC = 0;

	readDir( iecFiles, MAX_SYNC_FILES, iecNumFiles, iecBytesFree );


	for ( int i = 0; i < iecNumFiles; i++ )
	{
		IECSYNCFILE *f = &iecFiles[ i ];

		for ( u32 j = 0; j < nRemoveFiles; j++ )
		{
			IECSYNCFILE *r = &syncRemoveFiles[ j ];

			if ( strcmp( (char *)f->filename, (char *)r->filename ) == 0 )
			{
				filesToDeleteOnIEC ++;
				spaceNeededOnIEC -= r->size;
				f->flags |= SHOW_IEC_FILE_DELETE;
				goto nextIECFile;
			}
		}

		if ( f->filename[ 0 ] == '$' && f->filename[ strlen( (char *)f->filename ) - 1 ] == '$' )
			continue;

		char tmpFN[ 256 ], filename[ 256 ];
		strupr( tmpFN, (char *)f->filename );
		sprintf( filename, "%s", tmpFN );

		// 1a files on IECDevice but not in that list? 
		if ( indexOfSyncFile_FileNameSize( syncFileOnDevice, nSyncFileOnDevice, (char *)filename, f->size ) == -1 )
		{
			filesToCopyFromIEC ++;
			f->flags |= SHOW_IEC_FILE_NEW;
		} else
			// 1b file modified on IECDevice: ask whether we rename+copy or overwrite it on SD-card (or do nothing)
			if ( f->flags == 1 /* todo: f has been modified */ )
			{
				filesToUpdateFromIEC ++;
				f->flags |= SHOW_IEC_FILE_MODIFIED;
			}
	nextIECFile:;
	}

	for ( u32 i = 0; i < nSyncFileChanges; i++ )
	{
		spaceNeededOnIEC -= syncFileChanges[ i ].size;
	}

	if ( spaceNeededOnIEC + 16384 < iecBytesFree )
		transferIECPossible = 1; else
		transferIECPossible = 0;

#ifdef DEBUG_OUT_IECDEVICE
	extern CLogger *logger;
	sprintf( (char *)tempString, "files to delete on IECDevice: %d", filesToDeleteOnIEC );
	logger->Write( "[Query]", LogNotice, (char *)tempString );
	sprintf( (char *)tempString, "files to copy from IECDevice: %d", filesToCopyFromIEC );
	logger->Write( "[Query]", LogNotice, (char *)tempString );
	sprintf( (char *)tempString, "files to update from IECDevice: %d", filesToUpdateFromIEC );
	logger->Write( "[Query]", LogNotice, (char *)tempString );
	sprintf( (char *)tempString, "files to copy to IECDevice: %d", nSyncFileChanges );
	logger->Write( "[Query]", LogNotice, (char *)tempString );

	logger->Write( "[FAV]", LogNotice, "---- list of favorites ----" );
	for ( u32 i = 0; i < nSyncFileFavorites; i++ )
		logger->Write( "[FAV]", LogNotice, (char *)syncFileFavorites[ i ].filename );
#endif
}


void updateIECFavorites( IECSYNCFILE *syncFav, u32 *nSyncFav, IECSYNCFILE *syncIEC, u32 nSyncIEC, int filePos, int favPos )
{
	// syncFav/nSyncFav stores the ordered list of currect favorites
	// syncIEC/nSyncIEC is the list of files from which we choose favorites (typically those on the IEC device)

	// filePos is the position in syncIEC, favPos is the position where this file should be located in syncFav
	// if favPos is negative, then we remove position |favPos| from the list

	if ( filePos >= (int)nSyncIEC ) return;

	// 1. if favPos is >= 100, then we remove position favPos-100 from the list
	if ( favPos >= 100 )
	{
		int rmPos = favPos - 100;
		for ( int i = rmPos; i < (int)*nSyncFav; i++ )
			syncFav[ i ] = syncFav[ i + 1 ];
		*nSyncFav = *nSyncFav - 1;
		return;
	}

	// 0. check if this file is a disk image
	char *fn = (char *)syncIEC[ filePos ].filename;

	if ( strstr( fn, ".d64" ) > 0 || strstr( fn, ".D64" ) > 0 ||
		 strstr( fn, ".d71" ) > 0 || strstr( fn, ".D71" ) > 0 ||
		 strstr( fn, ".d81" ) > 0 || strstr( fn, ".D81" ) > 0 ||
		 strstr( fn, ".g64" ) > 0 || strstr( fn, ".G64" ) > 0 ||
		 strstr( fn, ".g71" ) > 0 || strstr( fn, ".G71" ) > 0 )
	{
		// ok, it's a disk image
	} else
		return;


	// 1. check is file is already in syncFav
	int idx = indexOfSyncFile_FileNameSize( syncFav, *nSyncFav, fn, syncIEC[ filePos ].size );

	if ( favPos >= (int)*nSyncFav && idx != -1 ) return;
	/*if ( favPos >= *nSyncFav )
	{
		if ( idx != - 1 )
			favPos = *nSyncFav; else
			return;
	}*/

	if ( idx == -1 )
	{
		if ( favPos >= (int)*nSyncFav )
			favPos = (int)*nSyncFav;
		// not in fav list => add and possibly fix indices of other files
		//if ( favPos < *nSyncFav )
		{
			for ( int i = *nSyncFav; i >= favPos + 1; i-- )
				syncFav[ i ] = syncFav[ i - 1 ];

			strcpy( (char *)syncFav[ favPos ].filename, fn );
			syncFav[ favPos ].size = syncIEC[ filePos ].size;

			*nSyncFav = *nSyncFav + 1;
		}
	} else
	{
		if ( idx == favPos ) return;

		if ( favPos < idx ) // move to front
		{

			IECSYNCFILE tmp;
			strcpy( (char *)tmp.filename, (char *)syncFav[ idx ].filename );
			tmp.size = syncFav[ idx ].size;

			// move files from [ favPos + 1; idx - 1 ] one to the right
			for ( int i = idx; i >= favPos + 1; i-- )
				syncFav[ i ] = syncFav[ i - 1 ];

			// copy info to syncFav[ favPos]
			strcpy( (char *)syncFav[ favPos ].filename, (char *)tmp.filename );
			syncFav[ favPos ].size = tmp.size;

		} else
		{
			// save info about syncFav[ idx ]
			IECSYNCFILE tmp;
			strcpy( (char *)tmp.filename, (char *)syncFav[ idx ].filename );
			tmp.size = syncFav[ idx ].size;

			// move files from [ favPos + 1; idx - 1 ] one to the right
			for ( int i = idx; i < favPos; i++ )
				syncFav[ i ] = syncFav[ i + 1 ];

			// copy info to syncFav[ favPos]
			strcpy( (char *)syncFav[ favPos ].filename, (char *)tmp.filename );
			syncFav[ favPos ].size = tmp.size;
		}
	}
	if ( *nSyncFav >= 10 ) *nSyncFav = 10;
}

//
//
// the code below uses a lot of methods from rad_hijack, but I wanted the latter to remain focused
// this is not nice at all, but ok for now
//
//

extern void SPOKE( u16 a, u8 v );
extern void POKE_FILL( u16 a, u16 n, u8 v );
extern void SPEEK( u16 a, u8 &v );
extern void POKE_MEMCPY( u16 a, u16 n, u8 *src );

#define SCREEN1				0x6400

extern u8 showIECDevice;
extern bool screenUpdated;

extern u8 fadeTabStep[ 16 ][ 6 ];
extern u8 c64ScreenRAM[ 1024 * 4 ];
extern u8 c64ColorRAM[ 1024 * 4 ];

void waitRetrace()
{
	uint8_t x, y; uint16_t t;
	do
	{
		SPEEK( 0xd012, y );
		SPEEK( 0xd011, x );
		t = ( (uint16_t)x & 128 ) << 1;
		t |= y;
	} while ( t != 0 );
}

static u8 tempScreenRAM[ 1000 ], tempColorRAM[ 1000 ];

#define SCREEN1				0x6400
#define CHARSET				0x6800
#define CHARSET2			0x7800
#define PAGE1_LOWERCASE		(((SCREEN1 >> 6) & 0xF0) | (((CHARSET+0x800) >> 10) & 0x0E))

extern u8 font_logo[ 0x1000 ];

#define FADE_SCREEN_AWAY_NO_IRQ										\
		memcpy( tempScreenRAM, c64ScreenRAM, 1000 );				\
		memcpy( tempColorRAM, c64ColorRAM, 1000 );					\
		u32 handleOneRasterLine( int fade1024, u8 fadeText );		\
		for ( u32 i = 312; i < 312 * 12; i ++ )						\
			handleOneRasterLine( 0x10000000 | (i * 256 / 312 / 2), 1 );	\
		POKE_FILL( SCREEN1, 1000, 32 );								\
		SPOKE( 0xd018, PAGE1_LOWERCASE ); 

#define FADE_SCREEN_AWAY 											\
		FADE_SCREEN_AWAY_NO_IRQ										\
		EnableIRQs();										

#define FADE_SCREEN_BACK {												\
		DisableIRQs();													\
		memcpy( c64ScreenRAM, tempScreenRAM, 1000 );					\
		memcpy( c64ColorRAM, tempColorRAM, 1000 );						\
		printIECDeviceScreen( iecFiles, iecNumFiles, iecBytesFree, 0 ); \
		for ( int i = 0; i < 4*40; i++ ) c64ColorRAM[ i ] = 8; \
		extern void setCharsForLogoAndOscilloscope(); \
		setCharsForLogoAndOscilloscope(); \
		POKE_MEMCPY( CHARSET2, 1600, font_logo ); \
		screenUpdated = true;											\
		for ( u32 i = 0; i < 1000; i++ ) {								\
			SPOKE( 0xd800 + i, 0 ); 									\
			SPOKE( SCREEN1 + i, c64ScreenRAM[ i ] ); }					\
		uint8_t x, y; int t;											\
		do {															\
			SPEEK( 0xd012, y );											\
			SPEEK( 0xd011, x );											\
			t = ( (int)x & 128 ) << 1; t |= y;							\
		} while ( t != 260 );											\
		u32 handleOneRasterLine( int fade1024, u8 fadeText );			\
		for ( s32 i = 312 * 12; i >= 0; i -- )							\
			handleOneRasterLine( 0x10000000 | (i * 256 / 312 / 2), 1 ); }

void renderProgressBar( int progress, int total, int fade = 0 )
{
	DisableIRQs();

	if ( total == 0 ) { total = 1; progress = 0; }


	int xp = 4;
	int yp = 15;

	printC64( 0, 0, "...transfer ongoing...", 15, 0, 0, 22 );

	u8 color = fadeTabStep[ 15 ][ fade ];

	for ( int i = 0; i < 22; i++ )
	{
		u32 ofs = i + 9 + 13 * 40;

		SPOKE( SCREEN1 + ofs, c64ScreenRAM[ i ] );
		SPOKE( SCREEN1 + ofs, c64ScreenRAM[ i ] );
		SPOKE( 0xd800 + ofs, color );
	}

	int t = max( 0, progress * ( 40 - 2 * xp ) / total );

	for ( int i = 0; i < ( 40 - 2 * xp ); i++ )
	{
		u32 ofs = i + xp + yp * 40;

		u8 c = 95;
		if ( i <= t )
			c = 96 + 128 - 64;

		SPOKE( SCREEN1 + ofs, c );
		SPOKE( 0xd800 + ofs, color );
	}

	EnableIRQs();
}

static int favListIndex[ MAX_SYNC_FILES ];

const char favDelimiter = '|';
const char favFiller = '^';

void updateFavoritesOnIECDevice()
{
	char allFileNames[ 10 * 32 + 1 ];

	memset( allFileNames, 0, 10 * 32 );

	int nFav = nSyncFileFavorites <= 10 ? nSyncFileFavorites : 10;
	for ( int i = 0; i < nFav; i++ )
	{
		sprintf( &allFileNames[ i * 32 ], "%s%c", (char *)syncFileFavorites[ i ].filename, favDelimiter );
	}
	for ( u32 i = nFav; i < 10; i ++ )
	{
		allFileNames[ i * 32 + 0 ] = favDelimiter;
		allFileNames[ i * 32 + 1 ] = 0;
	}

	allFileNames[ 10 * 32 ] = 0;

#ifdef DEBUG_OUT_IECDEVICE
	char tempString[ 2560 ];
	extern CLogger *logger;
	sprintf( tempString, "nFavs: %d", nFav );
	logger->Write( "[FAVGEN]", LogNotice, tempString );
#endif

	char favPrint[ 321 ];
	for ( int i = 0; i < 10 * 32; i ++ )
	{
		if ( allFileNames[ i ] == 0 )
			favPrint[ i ] = favFiller; else
			favPrint[ i ] = allFileNames[ i ];
	}
	favPrint[ 320 ] = 0;

#ifdef DEBUG_OUT_IECDEVICE
	sprintf( tempString, "length of fav string: %d", (int)strlen( favPrint ) );
	logger->Write( "[FAVGEN]", LogNotice, tempString );

	sprintf( tempString, "fav string: '%s'", favPrint );
	logger->Write( "[FAVGEN]", LogNotice, tempString );
#endif


	extern StatusType setConfigValue( const char *key, const char *value );
	setConfigValueArray( "favlist", favPrint, 320 );
}

//
// this function is used to remove all files from the favorites-list which do no longer exist on the IECDevice
//
void sanitizeFavorites( IECSYNCFILE *syncFav, u32 *nSyncFav, IECSYNCFILE *syncIEC, u32 nSyncIEC )
{
	// attention: this only works with order-preserving removal from file list
	for ( u32 i = 0; i < *nSyncFav; i++ )
	{
		char *fn = (char *)syncFav[ i ].filename;
		int idx = indexOfSyncFile_FileNameSize( syncIEC, nSyncIEC, fn, syncFav[ i ].size );

		if ( idx == -1 )
		{
			extern s32 removeSyncFile_Idx_OrderPreserving( IECSYNCFILE * list, u32 * nFiles, u32 idx );
			removeSyncFile_Idx_OrderPreserving( syncFav, nSyncFav, i );
		}
	}
}


static int iecDeviceFavoritesNeedUpdate = 1;
extern int iecDevDriveLetter;
static int askAreYouSure = 0, askQuestion = 0;
static int answerAreYouSure = 0;
static int askX = -1, askY = -1;
static int lastKey = -1, repKey = 0;

extern int rasterLineDelayCounter;

#define takeNoKey rasterLineDelayCounter

extern int noUpdatesRasterLine;

static int rseed = 0;

#define RAND_MAX_32 ((1U << 31) - 1)

static int myrand()
{
	return ( rseed = ( rseed * 214013 + 2531011 ) & RAND_MAX_32 ) >> 16;
}

#define MAX_PREVIEW_X 512
#define MAX_PREVIEW_Y 793

static uint8_t previewImage[ 2432 * 3172 ];
static int previewPage = 0;

void copyPrintPreviewPage( int p )
{
	memset( previewImage, 0, 2432 * 3172 );

	// +1, +2

	const int gsc = 5;
	const int gsc2 = 4;
	for ( int y = 2; y < 3172; y += gsc2 )
	{
		for ( int x = 1; x < 2432; x += gsc )
		{
			//			int r = myrand() | ( myrand() << 16 );
			//			if ( x * x + y * y < (myrand() % (1500*1500)) )
			//			if ( x * x + y * y < (r%(1500*1500)) )
			//			int src = ((3171-y - p * 3172)*2432/8) + (x/8);

						// cross
			int src = ( ( ( 3172 - 1 ) - y + p * 3172 ) * 2432 / 8 ) + ( x / 8 );
			int bit = 7 - ( x & 7 );
			if ( printOutputFile[ src ] & ( 1 << bit ) )
				previewImage[ ( x / gsc ) + ( y / gsc2 ) * 512 ] = 1;

			// small dot on the right
			int x_ = x + 2;
			src = ( ( ( 3172 - 1 ) - y + p * 3172 ) * 2432 / 8 ) + ( x_ / 8 );
			bit = 7 - ( x_ & 7 );
			if ( printOutputFile[ src ] & ( 1 << bit ) )
				previewImage[ ( x / gsc ) + ( y / gsc2 ) * 512 ] |= 2;

			// small dot to the left
			if ( x > 3 )
			{
				int x_ = x - 3;
				src = ( ( ( 3172 - 1 ) - y + p * 3172 ) * 2432 / 8 ) + ( x_ / 8 );
				bit = 7 - ( x_ & 7 );
				if ( printOutputFile[ src ] & ( 1 << bit ) )
					previewImage[ ( x / gsc ) + ( y / gsc2 ) * 512 ] |= 4;
			}
		}
	}
}

void generatePrintPreview()
{
	previewGenerated = 1;

	previewPage = 0;
	printOutputPos = printOutputSize = 0;

	print_data_raw( tempIEC, tempFilesize );

	copyPrintPreviewPage( 0 );
}

extern int fastRefresh;

static int ofsX = 0 << 16, ofsY = 0 << 16, scaleX = 1 << 16, scaleY = 1 << 16, exceedToNextPage = 0, exceedToPrevPage = 0;
static int lastMovement = -1;

void setupPrintPreview()
{
	noUpdatesRasterLine = 1;
	fastRefresh = 1;

	ofsX = 0 << 16;
	ofsY = 0 << 16;
	scaleX = 1 << 16, scaleY = 1 << 16;
	lastMovement = -1;
	exceedToNextPage = exceedToPrevPage = 0;

	SPOKE( 0xd015, 0 );

	SPOKE( 0xd011, 0x3b );
	SPOKE( 0xd018, 0x08 + 0x10 ); // 0x10 -> Screen RAM bei 1 * 1024 in der VIC Bank
	SPOKE( 0xd016, 0x08 );

#define GFX				0x0400
	SPOKE( 0xdd00, 0b11000000 | ( ( GFX >> 14 ) ^ 0x03 ) );

	const uint8_t c = 0;
	SPOKE( 0xd020, c );
	SPOKE( 0xd021, c );
	POKE_FILL( 0x0400, 1024, 0 );
}

void closePrintPreview()
{
	noUpdatesRasterLine = 0;
	fastRefresh = 0;

	SPOKE( 0xd020, 0 );
	SPOKE( 0xd021, 0 );

	extern void prepareScreenAndSprites();
	prepareScreenAndSprites();

#define SCREEN1				0x6400
	SPOKE( 0xdd00, 0b11000000 | ( ( SCREEN1 >> 14 ) ^ 0x03 ) );

	uint8_t x;
	SPEEK( 0xdd00, x );
	x |= 4;
	SPOKE( 0xdd00, x );

#define CHARSET				0x6800
#define PAGE1_LOWERCASE		(((SCREEN1 >> 6) & 0xF0) | (((CHARSET+0x800) >> 10) & 0x0E))
	SPOKE( 0xd018, PAGE1_LOWERCASE );
}

static int printPreviewFadeIn = 0;

const uint8_t ditherMatrix4x4_line[ 4 * 4 ] = {
	 0,  4,  2,  6,
	 8, 12, 10, 14,
	 3,  7,  1,  5,
	11, 15,  9, 13 };

u8 dot[ 32 * 32 ];
uint8_t buf[ 8000 ];

void printIECDeviceScreen( IECSYNCFILE *syncFile, u32 nSyncFile, u32 nBytesFree, int fade )
{
	char str[ 16 ];

	if ( showPrintPreview )
	{
		if ( showPrintPreview == 2 )
		{
			setupPrintPreview();
			showPrintPreview = 1;
			printPreviewFadeIn = 6 * 2;
			updatePrintPreview = 1;
		}

		int dither = ( min( 64, 1 + max( exceedToNextPage, exceedToPrevPage ) ) * 5 ) >> 5;
		static int lastDither = -1;

		if ( printPreviewFadeIn || dither != lastDither )
		{
			lastDither = dither;

			waitRetrace();
			if ( printPreviewFadeIn ) printPreviewFadeIn --;

			uint8_t c2;
			if ( dither )
				c2 = fadeTabStep[ 15 ][ 5 - dither ]; else
				c2 = 0;

			uint8_t c = fadeTabStep[ 15 ][ printPreviewFadeIn / 2 ];
			SPOKE( 0xd020, c );
			SPOKE( 0xd021, c );
			c |= c2 << 4;
			POKE_FILL( 0x0400, 1024, c );
		}

#define SET_PIXEL( x, y ) { int ofs = ( (x) & ~7 ) + ( (y) / 8 ) * 320 + ( (y) & 7 ); buf[ ofs ] |= 128 >> ( (x) & 7 ); }
		{
			updatePrintPreview = 0;

			int dotSize = 0;
			memset( dot, 0, 32 * 32 );
			// create dot image if necessary
			if ( scaleX < 65536 / 3 )
			{
				int s = 65536 / scaleX + 1; // 4 or larger
				if ( s > 31 ) s = 31;
				dotSize = s;
				int sqr = ( ( s / 2 ) * ( s / 2 ) );
				for ( int y = 0; y < s; y++ )
					for ( int x = 0; x < s; x++ )
					{
						int a = x - s / 2;
						int b = y - s / 2;
						int d = a * a + b * b;
						if ( d < sqr )
							dot[ x + y * 32 ] = 1;
					}
			}

			memset( buf, 0, 8000 );
			int posY = ofsY;
			for ( int y = 0; y < 200; y++ )
			{
				int posX = ofsX;
				for ( int x = 0; x < 320; x++ )
				{
					int ofs = ( x & ~7 ) + ( y / 8 ) * 320 + ( y & 7 );

					uint8_t v = 0;

					int px = posX;
					int py = posY;

					// minification
					if ( scaleX > 65536 )
					{
						px += ( ( (uint64_t)scaleX * ( (uint64_t)myrand() - 2 * 32768 ) ) >> 17 );
						py += ( ( (uint64_t)scaleY * ( (uint64_t)myrand() - 2 * 32768 ) ) >> 17 );
						if ( py > ( 792 << 16 ) ) py = ( 792 << 16 );
						if ( py < 0 ) py = 0;
					}

					int src = ( px >> 16 ) + ( py >> 16 ) * 512;
					//int src = ( posX >> 16 ) + ( posY >> 16 ) * 512;
					v = previewImage[ src ];

					// magnification (at least x4) => create dot
					if ( scaleX < 65536 / 3 && v )
					{
						if ( v == 1 )
						{
							int fx = ( ( px * dotSize ) >> 16 ) % dotSize;
							int fy = ( ( py * dotSize ) >> 16 ) % dotSize;
							v = dot[ fx + fy * 32 ];
						} else
						{
							uint8_t v_ = v;
							v = 0;
							if ( v_ & 2 ) // dot shifted right
							{
								int fx = ( ( px * dotSize ) >> 16 ) % dotSize;
								int fy = ( ( py * dotSize ) >> 16 ) % dotSize;
								fx -= dotSize / 2;
								if ( fx >= 0 )
									v |= dot[ fx + fy * 32 ];
							}
							if ( v_ & 4 ) // dot shifted left
							{
								int fx = ( ( px * dotSize ) >> 16 ) % dotSize;
								int fy = ( ( py * dotSize ) >> 16 ) % dotSize;
								fx += dotSize / 2;
								if ( fx < dotSize )
									v |= dot[ fx + fy * 32 ];
							}
						}
					} else
					{
						if ( scaleX < 65536 && ( v != 1 ) )
						{
							int fx = ( ( px * 2 ) >> 16 ) % 2;
							if ( ( fx == 1 && ( v & 2 ) ) || ( fx == 0 && ( v & 4 ) ) ) {} else v = 0;
						} else
						{
							//if ( v == 1 || ( v & 6 ) == 6 ) {} else v = 0;
						}
					}

					int dither = max( exceedToNextPage, exceedToPrevPage ) >> 1;
					if ( dither )
					{
						int x_ = x & 7, y_ = y & 7;
						if ( dither > ditherMatrix4x4_line[ x_ + y_ * 4 ] ) v = 0;
					}

					if ( v )
						buf[ ofs ] |= 128 >> ( x & 7 );

					posX += scaleX;
				}
				posY += scaleY;
			}

			//#define MAX_PREVIEW_X 512
			//#define MAX_PREVIEW_Y 793

			// generate overview image
#define DRAW_BOX( x, y, xs, ys )	\
				for ( int i = x; i <= x + xs; i++ ) {					\
					SET_PIXEL( i, y );									\
					SET_PIXEL( i, y + ys ); }							\
				for ( int i = y; i <= y + ys; i++ ) {					\
					SET_PIXEL( x, i );									\
					SET_PIXEL( x + xs, i ); }							\

			DRAW_BOX( 303, 175, 16, 24 );

			int a, b, c, d;
			a = ( ofsX >> 21 );
			if ( a < 0 ) a = 0;
			b = ( ofsY >> 21 );
			if ( b < 0 ) b = 0;
			c = ( ( ofsX + ( 320 + 5 ) * scaleX ) >> 21 ) - a;
			d = ( ( ofsY + ( 200 + 5 ) * scaleY ) >> 21 ) - b;
			a += 303; b += 175;

			DRAW_BOX( a, b, c, d );

			// page count
			extern int writtenPages;

			if ( writtenPages > 1 )
			{
				DRAW_BOX( 303, 171, 16, 2 )
					int l = max( 0, 16 * previewPage / ( writtenPages - 1 ) - 2 );
				if ( l > 0 )
				{
					DRAW_BOX( 304, 172, l, 1 )
				}
			}

			POKE_MEMCPY( 0x2000, 8000, buf );
			SPOKE( 0xd015, 0 );
		}
		return;
	}

	const u8 e1 = fadeTabStep[ 3 ][ fade ];

	const u8 c5 = fadeTabStep[ 15 ][ fade ];
	const u8 c6 = fadeTabStep[ 11 ][ fade ];

	const u8 c_del = fadeTabStep[ 2 ][ fade ];
	const u8 c_mod = fadeTabStep[ 6 ][ fade ];
	const u8 c_new = fadeTabStep[ 5 ][ fade ];

	const u8 c_del_b = fadeTabStep[ 10 ][ fade ];
	const u8 c_mod_b = fadeTabStep[ 14 ][ fade ];
	const u8 c_new_b = fadeTabStep[ 13 ][ fade ];


	extern int curIECScrollPos;//dirFirstLast[ curLevel ].scrollPos;

	int from = curIECScrollPos;
	int to = nSyncFile;

	int iecBrowserNumLines = BROWSER_NUM_LINES + 1;

	if ( to - from > iecBrowserNumLines ) to = iecBrowserNumLines + from;

	int xp = 4;
	int yp = 9 + 1 - 6;

	u8 c = fadeTabStep[ 1 ][ fade ];
	printC64( xp, yp, "IECBuddy", c, 0, 0, 16 );

	char tempString[ 40 ];

	int xo = 0;
	sprintf( tempString, " D:%02d (+/-), ", iecDevDriveLetter );
	printC64( xp + xo + 8, yp, tempString, c6, 0, 0, 29 );


	xo += strlen( tempString );
	xo -= 6;

	printC64( xp + xo + 9 - 1, yp, "+", 25, 0, 0, 29 );
	printC64( xp + xo + 11 - 1, yp, "-", 26, 0, 0, 29 );

	xo += 6;
	sprintf( tempString, "%dk free", nBytesFree / 1024 );
	printC64( xp + xo + 9 - 1, yp, tempString, c6, 0, 0, 29 );

	//int xo = strlen( tempString );

	yp ++;

	extern u8 c64ScreenRAM[ 1024 * 4 ];
	extern u8 c64ColorRAM[ 1024 * 4 ];

	memset( &c64ScreenRAM[ yp * 40 ], 32, 40 * 17 );
	memset( &c64ColorRAM[ yp * 40 ], 0, 40 * 17 );

	if ( hasPrintIECDevice == HAS_PRINT_NEEDFILENAME )
	{
		printC64( xp, yp + 2, "Save printer-file?", c, 0, 0, 18 );
		printC64( xp, yp + 4, "Filename: ____________________", e1, 0, 0, 30 );

		printC64( xp + 10, yp + 4, printNameStr, e1, 0, 0, 20 );
		printC64( xp + 10 + printNameStrLength, yp + 4, "_", e1, 128, 0, 39 );

		printC64( xp, yp + 6, "(\x1f delete, RET save, F7 preview)", 15, 0, 0, 39 );
		printC64( xp + 1, yp + 6, "\x1f", 19, 0, 0, 39 );
		printC64( xp + 11, yp + 6, "RET", 20, 0, 0, 39 );
		printC64( xp + 21, yp + 6, "F7", 21, 0, 0, 39 );
		//						   1234567890123456789012345678901234567890
		printC64( xp, yp + 8, "preview ctrl: CRSR, +/-/SPACE,", 12, 0, 0, 39 );
		printC64( xp, yp + 9, "              F1/F3 (select page)", 12, 0, 0, 39 );

		askX = 4; askY = 16;
		if ( askAreYouSure && askX >= 0 && askY >= 0 )
			printC64( askX, askY, "Are you sure? (Y/N)              ", 18, 0, 0, 33 ); else
			printC64( askX, askY, "                   ", c_del_b, 0, 0, 19 );

		return;
	}



	if ( iecDeviceFavoritesNeedUpdate )
	{
		for ( int i = 0; i < MAX_SYNC_FILES; i++ )
			favListIndex[ i ] = -1;

		// todo: from-to-limit is not enough once we're using the optimization below
		for ( int i = from; i < to; i++ )
		{
			int idx = indexOfSyncFile_FileNameSize( syncFileFavorites, nSyncFileFavorites, (char *)syncFile[ i ].filename, syncFile[ i ].size );
			if ( idx != - 1 )
				favListIndex[ i ] = idx;
		}

		// todo: use this for optimization
		//iecDeviceFavoritesNeedUpdate = 0;
	}

	u8 printName[ 64 ];
	for ( int i = from; i < to; i++ )
	{
		u8 color = c5;

		extern int curIECPosition;

		if ( syncFile[ i ].flags & SHOW_IEC_FILE_DELETE ) color = c_del_b;
		if ( syncFile[ i ].flags & SHOW_IEC_FILE_NEW ) color = c_new_b;
		if ( syncFile[ i ].flags & SHOW_IEC_FILE_MODIFIED ) color = c_mod_b;

		memcpy( printName, syncFile[ i ].name, 64 );

		printC64( xp, yp ++, (const char *)printName, color, i == curIECPosition ? 0x80 : 0, 0, 39 );


		if ( favListIndex[ i ] == - 1 )
			sprintf( (char *)printName, " " ); else
			sprintf( (char *)printName, "%2d", favListIndex[ i ] + 1 );
		printC64( xp - 2, yp - 1, (const char *)printName, 11, /*i == curIECPosition ? 0x80 : */0, 0, 39 );
	}

	// scroll bar
	yp = 4 + 1;

	int nDirEntries = max( 1, nSyncFile );
	int t = (from)* iecBrowserNumLines / nDirEntries;
	int b = ( to - 1 ) * iecBrowserNumLines / nDirEntries;

	if ( from > 0 ) t = max( 1, t );

	// bar on the right
	u8 color = fadeTabStep[ 12 ][ fade ];
	for ( int i = 0; i < iecBrowserNumLines; i++ )
	{
		char c = 95;
		if ( t <= i && i <= b )
			c = 96 + 128 - 64;
		c64ScreenRAM[ 35 + ( i + yp ) * 40 ] = c;
		c64ColorRAM[ 35 + ( i + yp ) * 40 ] = color;
	}

	yp = 4 + iecBrowserNumLines + 2;
	printC64( xp, yp, "Transfer, \x1f back, Wipe all, Init", c5, 0, 0, 29 + 3 );
	printC64( xp, yp, "T", 19, 0, 0, 1 );
	printC64( xp + 10, yp, "\x1f", 20, 0, 0, 1 );
	printC64( xp + 18, yp, "W", 21, 0, 0, 1 );
	printC64( xp + 28, yp, "I", 22, 0, 0, 1 );

	// print question here
	if ( askAreYouSure && askX >= 0 && askY >= 0 )
	{
		if ( askQuestion == 2 )
			printC64( askX, askY, "Delete all on IECBuddy? (Y/N)", 18, 0, 0, 36 );  else
			printC64( askX, askY, "Initialize/reset IECBuddy? (Y/N)", 18, 0, 0, 36 );
		return;
	}

	yp += 3;
	if ( !transferIECPossible )
	{
		printC64( xp, yp - 3, "Transfer,", c6, 0, 0, 29 );
		printC64( xp, yp, "not enough space, unsync files!", 18, 0, 0, 35 );
	} else
	{
		printC64( xp, yp, "Quick: 1..0 set, Q remove", c5, 0, 0, 29 );
		printC64( xp + 7, yp, "1", 26, 0, 0, 29 );
		printC64( xp + 8, yp, ".", 25, 0, 0, 29 );
		printC64( xp + 9, yp, ".", 25, 0, 0, 29 );
		printC64( xp + 10, yp, "0", 24, 0, 0, 29 );
		printC64( xp + 17, yp, "Q", 23, 0, 0, 29 );
	}

	yp = 4 + iecBrowserNumLines + 4;

	yp --;
	printC64( xp, yp ++, "copy from/to IEC         : ", c6, 0, 0, 27 );
	printC64( xp, yp ++, "update from/delete on IEC: ", c6, 0, 0, 27 );

	yp -= 2;
	xp = 27 + 4;

	sprintf( (char *)str, "%d", filesToCopyFromIEC );
	printC64( xp, yp, str, c_new, 0, 0, 11 );
	xo = strlen( str );
	printC64( xp + xo, yp, "/", c6, 0, 0, 11 );
	sprintf( (char *)str, "%d  ", nSyncFileChanges );
	printC64( xp + xo + 1, yp ++, str, c_new, 0, 0, 11 );


	sprintf( (char *)str, "%d ", filesToUpdateFromIEC );
	printC64( xp, yp, str, c_mod, 0, 0, 11 );
	xo = strlen( str );
	printC64( xp + xo - 1, yp, "/", c6, 0, 0, 11 );
	sprintf( (char *)str, "%d  ", filesToDeleteOnIEC );
	printC64( xp + xo, yp, str, c_del, 0, 0, 11 );
}

u32 handleKeyIECDeviceScreen( int k )
{
	extern int curIECPosition, curIECScrollPos;
	char tempString[ 256 ];

	if ( lastKey == k && takeNoKey )
	{
		k = 0;
	} else
		takeNoKey = 0;


#ifdef DEBUG_OUT_IECDEVICE
	if ( k != 0 )
	{
		extern CLogger	*logger;
		sprintf( (char*)tempString, "key '%c' = %d", k, k );
		logger->Write( "[KEY]", LogNotice, (char*)tempString );
	}
#endif

	if ( askAreYouSure )
	{
		if ( askAreYouSure == 1 )
		{
			askAreYouSure = 2;
			lastKey = -1;
		} else
		if ( k == 'Y' )
		{
			askAreYouSure = 0;
			answerAreYouSure = 1;
			takeNoKey = 0; // debouncing of keys
		} else
		if ( k == 'N' || k == VK_ESC )
		{
			askAreYouSure = 0;
			answerAreYouSure = 2;
			if ( k == VK_ESC )
				takeNoKey = 750; // debouncing of keys
		}
		lastKey = k; k = 0; repKey = 0;
		//return 0;
	}

	if ( hasPrintIECDevice == HAS_PRINT_NEEDFILENAME && !showPrintPreview )
	{
		if ( k == lastKey )
			repKey ++;

#if 1
		if ( answerAreYouSure == 1 ) // answer was 'yes'
		{
			hasPrintIECDevice = HAS_PRINT_DELETEONIEC;
			FADE_SCREEN_AWAY

			deleteFile( PRINTDATAFILE );

			FADE_SCREEN_BACK

			hasPrintIECDevice = HAS_PRINT_NONE;
			answerAreYouSure = 0;
			askX = askY = -1;
			return 0;
		}
		if ( answerAreYouSure == 2 ) // answer was 'no'
		{
			k = -1;
			answerAreYouSure = 0;
			askX = askY = -1;
			return 0;
		}

		if ( k && ( k != lastKey || repKey > 4 ) )
		{
			if ( k == VK_ESC )
			{
				askQuestion = 1;
				askAreYouSure = 1;
				askX = 4; askY = 13;
				answerAreYouSure = 0;
				lastKey = k;
				takeNoKey = 2000; // debouncing of keys
				return 0;
			} else
			if ( ( ( k >= 'A' && k <= 'Z' ) || ( k >= '0' && k <= '9' ) ) && printNameStrLength < 20 )
			{
				printNameStr[ printNameStrLength++ ] = k;
				printNameStr[ printNameStrLength ] = 0;
			} else
			if ( k == VK_DELETE && printNameStrLength )
			{
				printNameStr[ --printNameStrLength ] = 0; 
			} else
			if ( k == VK_RETURN )
			{
				hasPrintIECDevice = HAS_PRINT_PROCESS;

				FADE_SCREEN_AWAY

				print_data( tempIEC, tempFilesize, printNameStr );

				deleteFile( PRINTDATAFILE );

				hasPrintIECDevice = HAS_PRINT_NONE;

				FADE_SCREEN_BACK
			} else
			if ( k == VK_F7 )
			{
				FADE_SCREEN_AWAY_NO_IRQ

				if ( !previewGenerated )
				{
					generatePrintPreview();
				}

				if ( !showPrintPreview )
				{
					showPrintPreview = 2;
				} 
				//lastKey = k;
				takeNoKey = 750; // debouncing of keys
				//return 0;
			} 
			repKey = 0;
			lastKey = k;
		}
#else
		hasPrintIECDevice = HAS_PRINT_NONE;
#endif
		k = -1;
		return 0;
	}

	const int iecBrowserNumLines = BROWSER_NUM_LINES + 1;
	extern int iecDevDriveLetter;

	if ( askQuestion == 2 && answerAreYouSure ) // handle Y/N for "scratch all"
	{
		if ( answerAreYouSure == 1 ) // Yes
		{
			FADE_SCREEN_AWAY

			sendDriveCommand( "CD:..");
			sendDriveCommand( "S:*");
			deleteHiddenFiles( 1 );

			nSyncFileOnDevice = 0;
			filesToCopyFromIEC = filesToUpdateFromIEC = filesToDeleteOnIEC = 0;
			nRemoveFiles = 0;
			nSyncFileFavorites = 0;
			for ( u32 i = 0; i < MAX_SYNC_FILES; i++ )
			{
				syncFileFavorites[ i ].filename[ 0 ] = 0;
			}
			updateFavoritesOnIECDevice();

			readDir( iecFiles, MAX_SYNC_FILES, iecNumFiles, iecBytesFree );

			FADE_SCREEN_BACK
		}
		askAreYouSure = askQuestion = answerAreYouSure = 0;
		askX = askY = -1;
		k = 0;
	} 

	if ( askQuestion == 3 && answerAreYouSure ) // handle Y/N for "initialization/factory reset"
	{
		if ( answerAreYouSure == 1 ) // Yes
		{
			FADE_SCREEN_AWAY

			setConfigValue( "fsformat", "1" );

			nSyncFileOnDevice = 0;
			filesToCopyFromIEC = filesToUpdateFromIEC = filesToDeleteOnIEC = 0;
			nRemoveFiles = 0;
			nSyncFileFavorites = 0;
			for ( u32 i = 0; i < MAX_SYNC_FILES; i++ )
			{
				syncFileFavorites[ i ].filename[ 0 ] = 0;
			}

			extern void saveSyncDataToSD();
			saveSyncDataToSD();

			{
				reboot();

				delete m_USBHCI;

				for ( int i = 0; i < 50 * 1; i++ )
					waitRetrace();

				initSerialOverUSB_IECDevice(pInterrupt, pTimer, pDeviceNameService, true );
				sendDriveCommand( "CFG:ROTATE=180" );
				updateIECDeviceLogo( true );

			}

			readDir( iecFiles, MAX_SYNC_FILES, iecNumFiles, iecBytesFree );

			FADE_SCREEN_BACK
		}
		askAreYouSure = askQuestion = answerAreYouSure = 0;
		askX = askY = -1;
		k = 0;
	} 

	if ( showPrintPreview )
	{
		int maxPosX = ( (MAX_PREVIEW_X-1) << 16 ) - 320 * scaleX;
		int maxPosY = ( (MAX_PREVIEW_Y-1) << 16 ) - 200 * scaleY;

		static int moveInSameDirection = 0;
		static int speed = 0;
		const int dSpeed = 256;

		if ( k == ' ' )
		{
			scaleX = 1 << 16, scaleY = 1 << 16;
			lastMovement = -1;
		}

		int didMove = 0;

		if ( k == 'A' || k == VK_LEFT )
		{
			if ( lastMovement == 1 )
			{
				moveInSameDirection ++; 
				speed += dSpeed;
			} else
			{
				moveInSameDirection = 0;
				speed = 0;
			}
			ofsX -= (uint64_t)scaleX * (uint64_t)( 4096 + speed ) >> 12;
			lastMovement = 1;
			updatePrintPreview = 1;
			didMove = 1;
		} else
		if ( k == 'D' || k == VK_RIGHT )
		{
			if ( lastMovement == 3 )
			{
				moveInSameDirection ++; 
				speed += dSpeed;
			} else
			{
				moveInSameDirection = 0;
				speed = 0;
			}
			speed = 4096 + moveInSameDirection * moveInSameDirection;
			ofsX += (uint64_t)scaleX * (uint64_t)( 4096 + speed ) >> 12;
			lastMovement = 3;
			updatePrintPreview = 1;
			didMove = 1;
		} else
		if ( k == 'W' || k == VK_UP )
		{
			if ( lastMovement == 2 )
			{
				moveInSameDirection ++; 
				speed += dSpeed;
			} else
			{
				moveInSameDirection = 0;
				speed = 0;
			}
			speed = 4096 + moveInSameDirection * moveInSameDirection;
			ofsY -= (uint64_t)scaleY * (uint64_t)( 4096 + speed ) >> 12;
			lastMovement = 2;
			updatePrintPreview = 1;
			didMove = 1;
		} else
		if ( k == 'S' || k == VK_DOWN )
		{
			if ( lastMovement == 4 )
			{
				moveInSameDirection ++; 
				speed += dSpeed;
			} else
			{
				moveInSameDirection = 0;
				speed = 0;
			}
			speed = 4096 + moveInSameDirection * moveInSameDirection;
			ofsY += (uint64_t)scaleY * (uint64_t)( 4096 + speed ) >> 12;
			lastMovement = 4;
			updatePrintPreview = 1;
			didMove = 1;
		} else
		{
			if ( lastMovement >= 1 )
			{
				lastMovement = -1;
				updatePrintPreview = 1;
			}
		}

		if ( !didMove )
		{
				lastMovement = -1;
				updatePrintPreview = 1;

		}

		if ( k == VK_HOME )
		{
			ofsX = ofsY = 0;
		} else
		if ( k == VK_DELETE )
		{
			ofsX = 0;
			ofsY = maxPosY;
		} else
		if ( k == '+' )
		{
			scaleX = ( (uint64_t)scaleX * 62260L ) >> 16L;
			updatePrintPreview = 1;
		}
		if ( k == '-' )
		{
			scaleX = ( (uint64_t)scaleX * 68985L ) >> 16L;
			updatePrintPreview = 1;
		}

		if ( scaleX > MAX_PREVIEW_X * 65536 / 320 )
		{
			scaleX = MAX_PREVIEW_X * 65536 / 320;
		}
		scaleY = scaleX;

		if ( ofsX < 0 ) ofsX = 0;
		if ( ofsX >= maxPosX ) ofsX = maxPosX;

		if ( ofsY < 0 ) ofsY = 0;
		if ( ofsY >= maxPosY ) ofsY = maxPosY;

		extern int writtenPages;
		if ( (previewPage < writtenPages - 1 ) && ofsY >= maxPosY && lastMovement == 4 && ( k == 'S' || k == VK_DOWN ) ) 
		{
			exceedToNextPage ++;
			extern int writtenPages;
			if ( exceedToNextPage > 31 && previewPage < writtenPages - 1 )
			{
				k = VK_F3; // jump to next page
				ofsY = 0;
				moveInSameDirection = 0;
				speed = 0;
			}
		} else
			exceedToNextPage = 0;

		if ( previewPage && ofsY == 0 && lastMovement == 2 && ( k == 'W' || k == VK_UP ) ) 
		{
			exceedToPrevPage ++;
			if ( exceedToPrevPage > 31 && previewPage > 0 )
			{
				k = VK_F1; // jump to next page
				ofsY = maxPosY;
				moveInSameDirection = 0;
				speed = 0;
			}
		} else
			exceedToPrevPage = 0;

		static int oldDither = -1;
		int dither = max( exceedToNextPage, exceedToPrevPage );
		if ( dither != oldDither )
		{
			oldDither = dither;
			updatePrintPreview = 1;
		}
			

		if ( k == VK_F1 )
		{
			previewPage --;
			if ( previewPage < 0 ) previewPage = 0;
			copyPrintPreviewPage( previewPage );
			lastKey = k;
			takeNoKey = 750; // debouncing of keys
		} else
		if ( k == VK_F3 )
		{
			previewPage ++;
			extern int writtenPages;
			if ( previewPage >= writtenPages ) previewPage = writtenPages - 1;
			copyPrintPreviewPage( previewPage );
			lastKey = k;
			takeNoKey = 750; // debouncing of keys
		} else
		if ( k == VK_F2 )
		{
			if ( previewPage != 0 )
			{
				previewPage = 0;
				copyPrintPreviewPage( previewPage );
			}
			lastKey = k;
			takeNoKey = 750; // debouncing of keys
		} else
		if ( k == VK_F4 )
		{
			extern int writtenPages;
			if ( previewPage != writtenPages - 1 )
			{
				previewPage = writtenPages - 1;
				copyPrintPreviewPage( previewPage );
			}
			lastKey = k;
			takeNoKey = 750; // debouncing of keys
		} else
		
		if ( k == VK_F7 || k == VK_ESC )
		{
			for ( int f = 0; f < 6*2; f++ ) 
			{
				waitRetrace();
				uint8_t c = fadeTabStep[ 15 ][ f/2 ];
				SPOKE( 0xd020, c );
				SPOKE( 0xd021, c );
				POKE_FILL( 0x0400, 1024, c );
			}

			lastKey = k;
			takeNoKey = 750; // debouncing of keys
			showPrintPreview = 0;
			closePrintPreview();
			FADE_SCREEN_BACK
		}

		k = 0;
	} else
	if ( k == VK_F1 || k == VK_F3 )
	{
		for ( int i = 0; i < 10; i++ )
			handleKeyIECDeviceScreen( k == VK_F1 ? VK_UP : VK_DOWN );
		return 0;
	} else
	if ( k == VK_HOME )
	{
		curIECScrollPos = 0;
	} else
	if ( k == VK_UP && curIECPosition > 0 )
	{
		if ( -- curIECPosition < curIECScrollPos )
			curIECScrollPos --;
	} else
	if ( k == VK_DOWN && curIECPosition < iecNumFiles - 1 )
	{
		if ( ++ curIECPosition >= curIECScrollPos + iecBrowserNumLines )
			curIECScrollPos ++;
	} else
	if ( k == '+' )
	{
		iecDevDriveLetter = min( iecDevDriveLetter + 1, 15 );
	} else
	if ( k == '-' )
	{
		iecDevDriveLetter = max( iecDevDriveLetter - 1, 8 );
	} else
	if ( k == 'W' || k == 'w' )
	{
		askAreYouSure = 1;
		askQuestion = 2;
		answerAreYouSure = 0;
		askX = 4; askY = 19;
	} else
	if ( k == 'I' || k == 'i' )
	{
		askAreYouSure = 1;
		askQuestion = 3;
		answerAreYouSure = 0;
		askX = 4; askY = 19;
	} else
	if ( k == 'Q' || k == 'q' )
	{
		if ( favListIndex[ curIECPosition ] != -1 )
			updateIECFavorites( syncFileFavorites, &nSyncFileFavorites, iecFiles, iecNumFiles, curIECPosition, 100 + favListIndex[ curIECPosition ] );
	} else
	if ( k >= '0' && k <= '9' )
	{
		int setFav;
		if ( k == '0' ) setFav = 9; else setFav = (int)k - (int)'1';

		updateIECFavorites( syncFileFavorites, &nSyncFileFavorites, iecFiles, iecNumFiles, curIECPosition, setFav );
	} else
	if ( k == 'D' || k == 'd' || k == VK_ESC )
	{
		noUpdatesRasterLine = 0;
		showIECDevice = 0;
		screenUpdated = false;
		u32 handleOneRasterLine( int fade1024, u8 fadeText );
		for ( u32 i = 312; i < 312 * 10; i ++ )
			handleOneRasterLine( 0x10000000 | (i * 256 / 312 / 2), 1 );

		POKE_FILL( SCREEN1 + 4 * 40, 1000 - 4 * 40, 32 );

		POKE_FILL( 0xd800, 1000, 0 );

		static const char DRIVE[] = "SD:";
		extern void saveSyncDataToSD();
		saveSyncDataToSD();

		EnableIRQs();
		
		if ( IECDevicePresent )
		{
			sanitizeFavorites( syncFileFavorites, &nSyncFileFavorites, iecFiles, iecNumFiles );
			updateFavoritesOnIECDevice();

			char t[ 8 ];
			sprintf( t, "X%d", iecDevDriveLetter );
			sendDriveCommand( t );
		}

		extern void scanDirectoriesRAD( char *DRIVE );
		scanDirectoriesRAD( (char*)DRIVE );

		if ( IECDevicePresent )
			markSyncFilesRAD();// syncFileOnDevice, nSyncFileOnDevice );
		
		screenUpdated = true;

		DisableIRQs();

		memset( &c64ScreenRAM[ 5 * 40 ], 32, 40 * 17 );
		memset( &c64ColorRAM[ 5 * 40 ], 0, 40 * 17 );

		u32 readKeyRenderMenu( int fade );
		readKeyRenderMenu( 0 );

		for ( u32 i = 0; i < 1000; i++ )
		{
			SPOKE( SCREEN1 + i, c64ScreenRAM[ i ] );
		}

		// fade in 
		//for ( s32 i = 1024 * 6; i >= 0; i -- )
		//	handleOneRasterLine( i >> 2, 1 );
		uint8_t x, y; int t;											\
		do {															\
			SPEEK( 0xd012, y );											\
			SPEEK( 0xd011, x );											\
			t = ( (int)x & 128 ) << 1; t |= y;							\
		} while ( t != 260 );											\

		// fade in 
		//for ( i = 1024 * 6; i >= 0; i -- )
		//	handleOneRasterLine( i >> 2, 1 );
		for ( s32 i = 312 * 10; i >= 0; i -- )
			handleOneRasterLine( 0x10000000 | (i * 256 / 312 / 2), 1 ); 

		//showBrowser = 1;
		//fadeToIECDevice = 128 + 10;
	} else
	if ( ( k == 'T' || k == 't' ) && ( nRemoveFiles + filesToCopyFromIEC + nSyncFileChanges + filesToUpdateFromIEC ) > 0 && transferIECPossible )
	{
		int nTotalTransfers = nRemoveFiles + nSyncFileChanges + filesToCopyFromIEC + filesToUpdateFromIEC;
		int transferProgress = 0;

		FADE_SCREEN_AWAY

		// it would be nice to get all user input before we fade out the screen!
		// for now, simply ignore that

		static const char DRIVE[] = "SD:";
		char path[1024], filename[256], name[256], sdFile[ 1024 + 256 ], sdFile2[ 1024 + 256 ], extension[ 8 ], temp[ 1024 + 256 ];
			u32 filesize;

		extern CLogger	*logger;

		// 2. remove files from IECDevice which are no longer synced

		readDir( iecFiles, MAX_SYNC_FILES, iecNumFiles, iecBytesFree );

		renderProgressBar( transferProgress, nTotalTransfers );

		for ( u32 i = 0; i < nRemoveFiles; i++ )
		{
			IECSYNCFILE *f = &syncRemoveFiles[ i ];

			// delete file f on IECDevice
			sprintf( (char*)tempString, "S:%s", f->filename );
			sendDriveCommand( (char*)tempString );

			char fnNoExt[ 256 ];
			int l = toupperString( fnNoExt, 256, (char*)f->filename );

			// strip all extensions
			if ( strstr( fnNoExt + l - 4, ".PRG" ) ||
			     strstr( fnNoExt + l - 4, ".D64" ) ||
				 strstr( fnNoExt + l - 4, ".D71" ) ||
				 strstr( fnNoExt + l - 4, ".D81" ) ||
				 strstr( fnNoExt + l - 4, ".G64" ) ||
				 strstr( fnNoExt + l - 4, ".G71" ) ) 
				 fnNoExt[ l - 4 ] = 0;

			char fnHiddenGIF[ 256 + 2 + 2 ];
			sprintf( fnHiddenGIF, "$%s.GIF$", fnNoExt );
			deleteFile( (char*)fnHiddenGIF );

#ifdef DEBUG_OUT_IECDEVICE
			sprintf( (char*)tempString, "delete '%s' on IECBuddy", f->filename );
			logger->Write( "[SYNC2]", LogNotice, (char*)tempString );
#endif

			// also remove from our own list
			removeSyncFile( syncFileOnDevice, &nSyncFileOnDevice, (char*)f->path, (char*)f->filename, (char*)f->name, f->size );

			renderProgressBar( ++transferProgress, nTotalTransfers );
		}

		if ( nRemoveFiles ) // rereading directory necessary?
			readDir( iecFiles, MAX_SYNC_FILES, iecNumFiles, iecBytesFree );

		nRemoveFiles = 0;

		// 1. compare our state of files on device with the actual IECDevice content
		//
		// => 1a files on IECDevice but not in that list: copy to SD
		// => 1b file modified on IECDevice: ask whether we rename+copy or overwrite it on SD-card (or do nothing)
		// => 1c optional: files no longer on IECDevice but in list: ask user if they should be deleted on SD-card
		// => 1d optional (only happens when IECDevice is shared between instances): files in list, but not on IECDevice: upload SD->IECDevice

#ifdef DEBUG_OUT_IECDEVICE
		logger->Write( "----------------------", LogNotice, "" );
		logger->Write( "Files currently in syncFileOnDevice-list", LogNotice, "" );
		for ( u32 i = 0; i < nSyncFileOnDevice; i++ )
		{
			sprintf( (char*)tempString, "%s (%d bytes)", syncFileOnDevice[ i ].filename, syncFileOnDevice[ i ].size );
			logger->Write( "", LogNotice, (char*)tempString );
		}
		logger->Write( "----------------------", LogNotice, "" );
#endif

		for ( int i = 0; i < iecNumFiles; i++ )
		{
			IECSYNCFILE *f = &iecFiles[ i ];

			if ( f->filename[ 0 ] == '$' /*&& f->filename[ strlen( (char*)f->filename ) - 1 ] == '$'*/ ) 
				continue;

			char tmpFN[ 256 ];
			strupr( tmpFN, (char*)f->filename );
			//if ( strstr( tmpFN, ".PRG" ) == NULL && strstr( tmpFN, ".D64" ) == NULL )
			//	sprintf( filename, "%s.PRG", tmpFN ); else
				sprintf( filename, "%s", tmpFN );

			// 1a files on IECDevice but not in that list? 
			bool fileAlreadySync = ( indexOfSyncFile_FileNameSize( syncFileOnDevice, nSyncFileOnDevice, (char*)filename, f->size ) != -1 ) ? true : false;

			if ( f->flags == 1 || !fileAlreadySync )
			{
				// todo: hallucinate target directory and filename
				sprintf( path, "SD:RAD_PRG/IECBuddy");

				sprintf( sdFile, "%s/%s", path, filename );

				// check if this file already exists on the SD-card?

				// sdFile has .prg or .d64 extension: separate path+name from extension to add a counter inbetween if necessary
				strcpy( extension, &sdFile[ strlen( sdFile ) - 4 ] );
				strcpy( sdFile2, sdFile );
				sdFile2[ strlen( sdFile2 ) - 4 ] = 0;

				strcpy( temp, sdFile );

				u32 fileSize, counter = 0;

				if ( f->flags != 1 ) // if not an updated file, then we create a new filename
				{
					while( getFileSize( logger, DRIVE, temp, &fileSize ) && counter < 100 ) // if file 'temp' already exists
					{
						sprintf( temp, "%s%02d%s", sdFile2, counter, extension );
						counter ++;
					}
				} else
				{
#ifdef DEBUG_OUT_IECDEVICE
					sprintf( (char*)tempString, "'%s' (%d bytes) has been modified", f->filename, f->size );
					logger->Write( "[SYNC1b]", LogNotice, (char*)tempString );
#endif
				}

				if ( counter < 100 )
				{
					strcpy( sdFile, temp );

					// copy f onto SD-card
					getFile( (char*)f->filename, &tempIEC[ 0 ], &filesize );

					// todo todo check res
					
					writeFile( logger, DRIVE, sdFile, tempIEC, f->size );

#ifdef DEBUG_OUT_IECDEVICE
					sprintf( (char*)tempString, "copied IECDevice '%s' (%d bytes) => SD '%s'", f->filename, f->size, sdFile );
					logger->Write( "[SYNC1a]", LogNotice, (char*)tempString );
#endif

					// add to syncFileOnDevice
					//if ( f->flags != 1 ) // if not an updated file, then we add it to the list
					if ( !fileAlreadySync )
					{
#ifdef DEBUG_OUT_IECDEVICE
						sprintf( (char*)tempString, "adding '%s' to syncfilelist", filename );
						logger->Write( "[SYNC1a]", LogNotice, (char*)tempString );
#endif
						addSyncFile( syncFileOnDevice, &nSyncFileOnDevice, path, filename, name, f->size, 0 );
					}
				} else
				{
#ifdef DEBUG_OUT_IECDEVICE
					logger->Write( "[SYNC1a]", LogNotice, "renaming exceeded 100 attemps" );	
#endif
				}
	
				renderProgressBar( ++transferProgress, nTotalTransfers );

			} /*else
			// 1b file modified on IECDevice: ask whether we rename+copy or overwrite it on SD-card (or do nothing)
			if ( f->flags == 1 / todo: f has been modified / )
			{
				// todo: ask user whether to rename or overwrite
				sprintf( (char*)tempString, "'%s' (%d bytes) has been modified, need to do something about it", f->filename, f->size );
				logger->Write( "[SYNC1b]", LogNotice, (char*)tempString );
			}*/
		}

		// => 1c optional: files no longer on IECDevice but in list: ask user if they should be deleted on SD-card
		// => 1d optional (only happens when IECDevice is shared between instances): files in list, but not on IECDevice: upload SD->IECDevice
		// both cases are not distinguishable
		/*for ( int i = 0; i < nSyncFileOnDevice; i++ )
		{
			StatusType res;
			IECSYNCFILE *f = &syncFileOnDevice[ i ];
			
			for ( int j = 0; j < iecNumFiles; j++ )
			{
				IECSYNCFILE *g = &iecFiles[ j ];
	
				// is this iec-device-file also in our list? (todo: what if user deletes files on SD-card which we think we still have!?)
				if ( strcmp( (char*)f->filename, (char*)g->filename ) == 0 )
				{
					goto nextFile;
				}
			}

			// 1d: upload "f" to IECDevice
			sprintf( sdFile, "%s/%s", f->path, f->filename );
			readFile( logger, DRIVE, sdFile, tempIEC, &filesize );

			res = putFile( (char*)f->filename, (char*)&tempIEC, filesize );

			sprintf( (char*)tempString, "copied SD '%s' => IECDevice '%s' (%d bytes)", sdFile, f->filename, filesize );
			logger->Write( "[SYNC1d]", LogNotice, (char*)tempString );

		nextFile:;
		}*/


		// 3. upload files from SD->IECDevice

		for ( u32 i = 0; i < nSyncFileChanges; i++ )
		{
			IECSYNCFILE *f = &syncFileChanges[ i ];

			// todo: strip .prg extension (but keep .d64)
			u32 filesize;

			sprintf( sdFile, "%s/%s", f->path, f->filename );
			readFile( logger, DRIVE, sdFile, tempIEC, &filesize );

			char fnNoExt[ 256 ];
			int l = toupperString( fnNoExt, 256, (char*)f->filename );
			//if ( strstr( fnNoExt + l - 4, ".PRG" ) ) fnNoExt[ l - 4 ] = 0;

			/*StatusType res = */putFile( (char*)fnNoExt, (char*)&tempIEC, filesize );
			// TODO GIF

#ifdef DEBUG_OUT_IECDEVICE
			sprintf( (char*)tempString, "copied SD '%s' => IECDevice '%s' (%d bytes)", sdFile, f->filename, filesize );
			logger->Write( "[SYNC3]", LogNotice, (char*)tempString );
#endif

			addSyncFile( syncFileOnDevice, &nSyncFileOnDevice, (char*)f->path, (char*)f->filename, (char*)f->name, f->size, 0 );

			// strip all extensions
			if ( strstr( fnNoExt + l - 4, ".D64" ) ||
				 strstr( fnNoExt + l - 4, ".D71" ) ||
				 strstr( fnNoExt + l - 4, ".D81" ) ||
				 strstr( fnNoExt + l - 4, ".G64" ) ||
				 strstr( fnNoExt + l - 4, ".G71" ) ) 
				 fnNoExt[ l - 4 ] = 0;

			sprintf( sdFile, "%s/%s.gif", f->path, fnNoExt );

#ifdef DEBUG_OUT_IECDEVICE
			sprintf( (char*)tempString, "sdfile '%s'", sdFile );
			logger->Write( "[SYNCG]", LogNotice, (char*)tempString );
#endif

			u32 fileSize;
			if ( getFileSize( logger, DRIVE, sdFile, &fileSize ) ) // if sync data is on SD
			{
				readFile( logger, DRIVE, sdFile, tempIEC, &fileSize );

				char fnHiddenGIF[ 256 + 2 + 2 ];
				
				sprintf( fnHiddenGIF, "$%s.GIF$", fnNoExt );
				deleteFile( (char*)fnHiddenGIF );

				putFile( (char*)fnHiddenGIF, (char*)&tempIEC, fileSize );

#ifdef DEBUG_OUT_IECDEVICE
				sprintf( (char*)tempString, "gif '%s' (size %d)", fnHiddenGIF, fileSize );
				logger->Write( "[SYNCG]", LogNotice, (char*)tempString );
#endif				
			}



#ifdef DEBUG_OUT_IECDEVICE
			logger->Write( "[SYNC]", LogNotice, "Sync File On Device" );
			for ( u32 i = 0; i < nSyncFileOnDevice; i++ )
			{
				logger->Write( "[SYNC]", LogNotice, (char*)syncFileOnDevice[ i ].path );
				logger->Write( "[SYNC]", LogNotice, (char*)syncFileOnDevice[ i ].filename );
			}
#endif

			renderProgressBar( ++transferProgress, nTotalTransfers );
		}
		nSyncFileChanges = 0;

		readDir( iecFiles, MAX_SYNC_FILES, iecNumFiles, iecBytesFree );

		char t2[ 1024 ];

		for ( u32 j = 0; j < nSyncFileOnDevice; j++ )
		{
			IECSYNCFILE *r = &syncFileOnDevice[ j ];

			bool found = false;

			for ( int i = 0; i < iecNumFiles; i++ )
			{
				IECSYNCFILE *f = &iecFiles[ i ];

				// add '.PRG' if f->filename does not have an extension
				char tmpFN[ 256 ], filename[ 256 ];
				strupr( tmpFN, (char*)f->filename );
				//if ( strstr( tmpFN, ".PRG" ) == NULL && strstr( tmpFN, ".D64" ) == NULL )
				//	sprintf( filename, "%s.PRG", tmpFN ); else
					sprintf( filename, "%s", tmpFN );


				//strupr( t1, (char*)f->filename );
				strupr( t2, (char*)r->filename );

#ifdef DEBUG_OUT_IECDEVICE
				sprintf( (char*)tempString, "compare '%s' to '%s'", filename, t2 );
				logger->Write( "[compare2]", LogNotice, (char*)tempString );
#endif

				if ( strcmp( filename, t2 ) == 0 )
				{
					found = true;
					goto nextSyncFile;
				}
			}
			nextSyncFile:

			if ( !found )
			{
				strupr( t2, (char*)r->filename );
#ifdef DEBUG_OUT_IECDEVICE
				sprintf( (char*)tempString, "remove file from sync list: '%s'", t2 );
				logger->Write( "[list consistency]", LogNotice, (char*)tempString );
#endif
				removeSyncFile_FileNameOnly( syncFileOnDevice, &nSyncFileOnDevice, t2, r->size );
			}

			renderProgressBar( ++transferProgress, nTotalTransfers );
		}

		sanitizeFavorites( syncFileFavorites, &nSyncFileFavorites, iecFiles, iecNumFiles );
		updateFavoritesOnIECDevice();

		DisableIRQs();

		for ( int fade = 0; fade < 6 * 2; fade ++ )
		{
			u16 lastRasterLine = 0;
			for ( int wait = 0; wait < 312; wait ++ )
			{
				u16 curRasterLine;
				u8 y;
				do {
					SPEEK( 0xd012, y );
					curRasterLine = y;
				} while ( curRasterLine == lastRasterLine );
				lastRasterLine = curRasterLine;
			}

			renderProgressBar( nTotalTransfers, nTotalTransfers, fade >> 1 );
		}

		filesToCopyFromIEC = filesToUpdateFromIEC = filesToDeleteOnIEC = 0;	

		FADE_SCREEN_BACK
	}

	DisableIRQs();

	return 0;
}


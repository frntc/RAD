#include "linux/kernel.h"

#define CMD_DIR              1
#define CMD_GETFILE          2
#define CMD_PUTFILE          3
#define CMD_DRIVESTATUS      4
#define CMD_DRIVECMD         5
#define CMD_MOUNT            6
#define CMD_UNMOUNT          7
#define CMD_GET_MOUNTED      8
#define CMD_SET_CONFIG_VAL   9
#define CMD_GET_CONFIG_VAL  10
#define CMD_CLEAR_CONFIG    11
#define CMD_DELETE_FILE     12
#define CMD_SHOW_BITMAP     13
#define CMD_SHOW_GIF        14
#define CMD_REBOOT          15
#define CMD_INVALID          0xFFFFFFFF

#define ST_OK                0
#define ST_INVALID_COMMAND   1
#define ST_INVALID_DIR       2
#define ST_INVALID_FILE      3
#define ST_INVALID_LENGTH    4 
#define ST_DRIVE_FULL        5 
#define ST_READ_ERROR        6
#define ST_WRITE_ERROR       7
#define ST_INVALID_DATA      8
#define ST_TIMEOUT           9
#define ST_CHECKSUM_ERROR   10
#define ST_FILE_EXISTS      11
#define ST_FILE_NOT_FOUND   12
#define ST_NOT_MOUNTED      13
#define ST_COM_ERROR        -1

#define FF_MODIFIED         0x00000001

// these need to be defined in the implementation
bool send_data( uint32_t length, const uint8_t *buffer );
bool recv_data( uint32_t length, uint8_t *buffer );


const char *get_status_msg( StatusType status )
{
	switch ( status )
	{
		case ST_OK:               return "OK";
		case ST_INVALID_COMMAND:  return "INVALID_COMMAND";
		case ST_INVALID_DIR:      return "INVALID_DIR";
		case ST_INVALID_FILE:     return "INVALID_FILE";
		case ST_INVALID_LENGTH:   return "INVALID_LENGTH";
		case ST_DRIVE_FULL:       return "DRIVE_FULL";
		case ST_READ_ERROR:       return "READ_ERROR";
		case ST_WRITE_ERROR:      return "WRITE_ERROR";
		case ST_INVALID_DATA:     return "INVALID_DATA";
		case ST_TIMEOUT:          return "TIMEOUT";
		case ST_CHECKSUM_ERROR:   return "ST_CHECKSUM_ERROR";

		case ST_COM_ERROR:        return "COM_ERROR";

		default:
		{
			static char msg[ 20 ];
			sprintf( msg, "[ERROR %i]", status );
			return msg;
		}
	}
}

bool send_sint( int32_t i )
{
	uint8_t data[ 4 ];
	data[ 0 ] = i & 255; i = i / 256;
	data[ 1 ] = i & 255; i = i / 256;
	data[ 2 ] = i & 255; i = i / 256;
	data[ 3 ] = i;
	return send_data( 4, data );
}


bool recv_sint( int32_t &i )
{
	uint8_t data[ 4 ];
	i = 0;
	if ( !recv_data( 4, data ) )
		return false;
	else
	{
		i = data[ 3 ]; i = i * 256;
		i = data[ 2 ]; i = i * 256;
		i = data[ 1 ]; i = i * 256;
		i = data[ 0 ];
		return true;
	}
}


bool send_uint( uint32_t i )
{
	uint8_t data[ 4 ];
	data[ 0 ] = i & 255; i = i / 256;
	data[ 1 ] = i & 255; i = i / 256;
	data[ 2 ] = i & 255; i = i / 256;
	data[ 3 ] = i;
	return send_data( 4, data );
}


bool recv_uint( uint32_t &i )
{
	uint8_t data[ 4 ];
	if ( !recv_data( 4, data ) )
	{
		return false;
	} else
	{
		i = data[ 3 ];
		i = data[ 2 ] + i * 256;
		i = data[ 1 ] + i * 256;
		i = data[ 0 ] + i * 256;
		return true;
	}
}

#define CMD_MAGIC 0xFEEDABCD

bool send_command( CommandType cmd )
{
	return send_uint( CMD_MAGIC ) && send_uint( cmd );
}

CommandType recv_command()
{
	uint32_t d;
	CommandType cmd;
	if ( recv_uint( d ) && d == CMD_MAGIC && recv_uint( cmd ) )
		return cmd;
	else
		return CMD_INVALID;
}


bool send_status( StatusType status )
{
	return send_sint( status );
}


StatusType recv_status()
{
	StatusType status;
	status = 0xff;
	if ( !recv_sint( status ) )
	{
		status = ST_COM_ERROR;
	}
	return status;
}


bool send_string( const u8 *s )
{
	uint32_t len = strlen( (const char *)s );
	return send_uint( len ) && send_data( len, (const uint8_t *)s );
}

bool send_string_length( const u8 *s, int len )
{
	return send_uint( len ) && send_data( len, (const uint8_t *)s );
}


int recv_string( u8 *s )
{
	int res = 0;

	uint32_t len;
	if ( recv_uint( len ) )
	{
		char data[ 65536 ];

		memset( data, 0, 65536 );
		if ( recv_data( len, (uint8_t *)data ) )
		{
			memcpy( s, data, len );
			s[ len ] = 0;
			res = len;
		}
	}

	return res;
}

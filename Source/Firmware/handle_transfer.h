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
#define COMMON_ENTRY_IN_TRANSFER

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"

register u32 g2;

register u16 c_a = reu.addrC64;
register u32 r_a = (u32)reu.addrREU | ( (u32)reu.bank << 16 );

// don't remove any of the (seemingly) redundant reuPrefetchL1 calls in this file!
reuPrefetchL1( r_a );

register s32 l = reu.length ? reu.length : 0x10000;
register s32 length = l;
	
reu.incrC64 = reu.addrREUCtrl & REU_ADDR_FIX_C64 ? 0 : 1;
reu.incrREU = reu.addrREUCtrl & REU_ADDR_FIX_REU ? 0 : 1;

register u32 next_r_a, temp_r_a;
register u8 newStatus = 0, x, y, tmp;

//#define REU_PROTOCOL
#ifdef REU_PROTOCOL
extern REUPROT reuProtocol[ 65536 ];
extern u32 nReuProtocol;
REUPROT *R;
#endif

#ifdef COMMON_ENTRY_IN_TRANSFER

reuPrefetchL1( r_a & ( reu.wrapAroundDRAM - 1 ) );

WAIT_FOR_VIC_HALFCYCLE
RESTART_CYCLE_COUNTER
reuPrefetchL1( r_a & ( reu.wrapAroundDRAM - 1 ) );

WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA ); // 80ns after falling Phi2
CLR_GPIO( bDMA_OUT );
#endif

switch ( reu.command & 3 )
{
case REU_COMMAND_TRANSFER_TO_REU: // stash: c64->reu
#ifndef COMMON_ENTRY_IN_TRANSFER
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

#ifdef REU_PROTOCOL
	R = &reuProtocol[ nReuProtocol ++ ];
	R->cmdREUAddr = ( 0 << 24 ) | r_a;
	R->c64AddrLength = ( c_a << 16 ) | l;
#endif

	WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA ); // 80ns after falling Phi2
	CLR_GPIO( bDMA_OUT );
#endif

	while ( l )
	{
		reuPrefetchW( r_a );
		emuReadByteREU_p1( g2, c_a );
		emuReadByteREU_p2( g2 );
		if ( l < length )
			reuStore( temp_r_a, x );
		l --;
		temp_r_a = r_a; REU_INCREMENT_ADDRESS( r_a ); 
		reuPrefetchW( r_a + 64 ); 
		REU_INCREMENT_C64ADDRESS( c_a );
		emuReadByteREU_p3( g2, x, (l==0) );
	}
	reuStore( temp_r_a, x );


#ifdef REU_PROTOCOL
	R->c64AddrLength = ( c_a << 16 ) | x;
#endif

	reu.isModified = 1;

/*	if ( length == 1 )
		reu.contiguous1ByteWrites ++; else
		reu.contiguous1ByteWrites = 0;*/
	reu.contiguousWrite += length;
	reu.contiguousVerify = 0;
	++l; newStatus = REU_STATUS_END_OF_BLOCK;
	break;

case REU_COMMAND_TRANSFER_TO_C64: // fetch: c64<-reu
#ifndef COMMON_ENTRY_IN_TRANSFER
	reuPrefetchL1( r_a & ( reu.wrapAroundDRAM - 1 ) );
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	reuPrefetchL1( r_a & ( reu.wrapAroundDRAM - 1 ) );

	WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA ); // 80ns after falling Phi2
	CLR_GPIO( bDMA_OUT );
#endif

#if 1
	if ( l == 16 && reu.isSpecial == SPECIAL_NUVIE && ( r_a & 0xffff ) == 0xf0 )
	{
		CACHE_PRELOADL1STRM( &reuMemory[ r_a & ( reu.wrapAroundDRAM - 1 ) ] );
		for ( int i = 0; i < 8; i++ )
		{
			reu.nextREUByte = reuLoad( r_a );
			WAIT_FOR_CPU_HALFCYCLE
			WAIT_FOR_VIC_HALFCYCLE
			RESTART_CYCLE_COUNTER
		}
	} else
#endif
	{
		reu.nextREUByte = reuLoad( r_a );
	}

	while ( l )
	{
		next_r_a = REU_GET_NEXT_ADDRESS( r_a ); 
		reuPrefetchL1( next_r_a );
		l --;
		emuWriteByteREU_p1( g2, c_a, reu.nextREUByte );
		reu.nextREUByte = reuLoad( next_r_a ); r_a = next_r_a; reuPrefetchL1( r_a ); reuPrefetchL1( r_a + 64 ); 
		emuWriteByteREU_p2( g2, (l==0) );

		REU_INCREMENT_C64ADDRESS( c_a );
	}

	reu.contiguousWrite = 0;
	reu.contiguousVerify = 0;
	++l; newStatus = REU_STATUS_END_OF_BLOCK;
	break;

case REU_COMMAND_TRANSFER_SWAP: // swap: c64<->reu
#ifndef COMMON_ENTRY_IN_TRANSFER
	reuPrefetchL1( r_a & ( reu.wrapAroundDRAM - 1 ) );
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

	WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA ); 
	CLR_GPIO( bDMA_OUT );
#endif

	while ( l )
	{
		reuPrefetchL1( r_a );
		emuReadByteREU_p1( g2, c_a );
		emuReadByteREU_p2( g2 );
		{tmp = reuLoad( r_a ); next_r_a = REU_GET_NEXT_ADDRESS( r_a ); reuPrefetchL1( next_r_a );l --; reuPrefetchL1( r_a + 64 ); }
		emuReadByteREU_p3( g2, x, false );

		emuWriteByteREU_p1( g2, c_a, tmp );
		{reuStore( r_a, x ); r_a = next_r_a; REU_INCREMENT_C64ADDRESS( c_a ); }
		emuWriteByteREU_p2( g2, (l==0) );

	}

	reu.isModified = 1;
	reu.contiguousWrite = 0;
	++l; newStatus = REU_STATUS_END_OF_BLOCK;
	break;

case REU_COMMAND_TRANSFER_VERIFY: // verify: c64-?-reu
#ifndef COMMON_ENTRY_IN_TRANSFER
	reuPrefetchL1( r_a & ( reu.wrapAroundDRAM - 1 ) );
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER

	WAIT_UP_TO_CYCLE( TIMING_TRIGGER_DMA );
	CLR_GPIO( bDMA_OUT );
#endif

	reu.nextREUByte = reuLoad( r_a );

	while ( l )
	{
		reuPrefetchL1( r_a );
		emuReadByteREU_p1( g2, c_a );
		emuReadByteREU_p2( g2 );
		l --;
		next_r_a = REU_GET_NEXT_ADDRESS( r_a ); reuPrefetchL1( r_a + 64 );
		y = reuLoad( r_a );
		r_a = next_r_a; REU_INCREMENT_C64ADDRESS( c_a ); 
		emuReadByteREU_p3( g2, x, ( l == 0 ) );
		if ( x != y )
		{
			newStatus |= REU_STATUS_VERIFY_ERROR;

			if ( l > 1 )
			{
				WAIT_FOR_CPU_HALFCYCLE
				WAIT_FOR_VIC_HALFCYCLE
				RESTART_CYCLE_COUNTER
			}
			break;
		}
	}

	if ( l == 0 )
	{
		l++;
		newStatus |= REU_STATUS_END_OF_BLOCK;
	} else
	if ( l == 1 )
	{
		reuPrefetchL1( r_a );
		emuReadByteREU_p1( g2, c_a );
		emuReadByteREU_p2( g2 );
		emuReadByteREU_p3( g2, x, true );

		y = reuLoad( r_a );
		if ( x == y )
			newStatus |= REU_STATUS_END_OF_BLOCK;

	}

	SET_GPIO( bDMA_OUT );

	// this is a hack (sort of detects REU tests) to compensate for one peculiarity in the cache preloading
	if ( reu.contiguousWrite == reu.reuSize && ( /*reu.addrREU |*/ ( (u32)reu.bank << 16 ) ) == 0 )
		newStatus &= ~REU_STATUS_VERIFY_ERROR;

	reu.contiguousWrite = 0;
	reu.contiguousVerify += length;
	break;
}

reuUpdateRegisters( c_a, r_a, l, newStatus );

reu.command = ( reu.command & ~REU_COMMAND_EXECUTE ) | REU_COMMAND_FF00_DISABLED;

#pragma GCC diagnostic pop

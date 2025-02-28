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

//#define DOUBLE_GPIO_WRITES

//#define OLD_BUS_PROTOCOL

#ifdef OLD_BUS_PROTOCOL
#define OLD_BUS_PROTOCOL_READ
#define OLD_BUS_PROTOCOL_WRITE
#endif


typedef struct
{
	u32 cmdREUAddr,
		c64AddrLength;
}REUPROT;

#define DELAY(rounds) \
	for ( int i = 0; i < rounds; i++ ) { \
		asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" );	asm volatile( "nop" ); }

#define CPU_RESET			(!(g2&bRESET_OUT)) 
#define BUTTON_PRESSED		(!(g2&bBUTTON))

#define IO2_ACCESS			(!(g2 & bIO2))
#define IO_ADDRESS			(( ( g3 >> 10 ) & 15 ) | ( ( g3 & 15 ) << 4 ))

#define IO1_ACCESS			(!(g2 & bIO1))
#define IO1_OR_IO2_ACCESS	(IO1_ACCESS||IO2_ACCESS)
#define GET_IO12_ADDRESS	IO_ADDRESS

#define ADDRESS0to7			IO_ADDRESS
#define ADDRESS_FFxx		(!(g2 & bTriggerFF00))

#define CPU_READS_FROM_BUS	(g2 & bRW_OUT)
#define CPU_WRITES_TO_BUS	(!(g2 & bRW_OUT))

#define CPU_IRQ_LOW			(!(g2 & bIRQ_OUT))
#define CPU_NMI_LOW			(!(g2 & bNMI))

#define VIC_HALF_CYCLE		(!(g2 & bPHI))
#define CPU_HALF_CYCLE		((g2 & bPHI))
#define VIC_BA				(!(g2 & bBA))
#define VIC_EARLY_BA		(!(g2_earlyBA & bBA))
#define BUSAVAIL( g ) 		( (g) & bBA )

#define PUT_DATA_ON_BUS( D ) {											\
	register u32 DD = ( (D) & 255 ) << D0;								\
	CLR_GPIO( (D_FLAG & ( ~DD )) | bOE_Dx | bDIR_Dx );					\
	SET_GPIO( DD );														\
	SET_BANK2_OUTPUT													\
	WAIT_UP_TO_CYCLE( WAIT_CYCLE_READ+TIMING_OFFSET_CBTD );				\
	SET_GPIO( bOE_Dx | bDIR_Dx ); }

#define GET_DATA_FROM_BUS( D ) {										\
	SET_BANK2_INPUT														\
	SET_GPIO( bDIR_Dx );												\
	CLR_GPIO( bOE_Dx );													\
	WAIT_UP_TO_CYCLE( WAIT_CYCLE_WRITEDATA+TIMING_OFFSET_CBTD );		\
	D = ( read32( ARM_GPIO_GPLEV0 ) >> D0 ) & 255;						\
	SET_GPIO( bOE_Dx );													\
	SET_BANK2_OUTPUT }

__attribute__( ( always_inline ) ) inline u8 countBits( u16 x )
{
	u32 t;
	asm volatile( "dup v0.2d, %0" : : "r" ( x ) );
	asm volatile( "cnt v0.16b, v0.16b" : : );
	asm volatile( "fmov %0, v0.d[1]" : "=r" ( t ) );
	return *(u8*)&t;
}


__attribute__( ( always_inline ) ) inline u8 flipByte( u8 x )
{
	u32 t;
	asm volatile( "rbit %w0, %w1" : "=r" ( t ) : "r" ( x ) );	// flip all bits in 32-bit-DWORD
	asm volatile( "rev  %w0, %w1" : "=r" ( t ) : "r" ( t ) );	// reverse 4 bytes in 32-bit-DWORD
	return *(u8*)&t;
}


#define DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER( releaseDMA )								\
	SET_GPIO( bLATCH_A_OE | bGAME_OUT | bOE_Dx | bRW_OUT | (releaseDMA ? bDMA_OUT : 0) );	\
	INP_GPIO_RW();																			\
	SET_BANK2_OUTPUT 																		

#define DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER_NG( releaseDMA )								\
	SET_GPIO( bLATCH_A_OE | bOE_Dx | bRW_OUT | (releaseDMA ? bDMA_OUT : 0) );	\
	INP_GPIO_RW();																			\
	SET_BANK2_OUTPUT 																		
	

#define ENABLE_D07_FOR_WRITING		\
	CLR_GPIO( bOE_Dx | bDIR_Dx );	\
	SET_BANK2_OUTPUT 

#define WAIT_FOR_CPU_HALFCYCLE {do { g2 = read32( ARM_GPIO_GPLEV0 ); } while ( VIC_HALF_CYCLE );}
#define WAIT_FOR_VIC_HALFCYCLE {do { g2 = read32( ARM_GPIO_GPLEV0 ); } while ( CPU_HALF_CYCLE ); }

#define emuWAIT_FOR_VIC_HALFCYCLE {				\
	do { 										\
		g2 = read32( ARM_GPIO_GPLEV0 );			\
	} while ( !(g2 & bPHI) );					\
	do { 										\
		g2 = read32( ARM_GPIO_GPLEV0 );			\
	} while ( (g2 & bPHI) );	}


#define emuWAIT_FOR_CPU_HALFCYCLE {				\
	do { 										\
		g2 = read32( ARM_GPIO_GPLEV0 );			\
	} while ( (g2 & bPHI) );					\
	do {										\
		g2 = read32( ARM_GPIO_GPLEV0 );			\
	} while ( !(g2 & bPHI) );	}						


#define HANDLE_BUS_AVAILABLE_READ \
	WAIT_UP_TO_CYCLE( reu.TIMING_BA_SIGNAL_AVAIL );			\
	g2 = read32( ARM_GPIO_GPLEV0 );							\
	if ( VIC_BA ) {											\
		do {												\
			WAIT_FOR_CPU_HALFCYCLE							\
			WAIT_FOR_VIC_HALFCYCLE							\
			RESTART_CYCLE_COUNTER							\
			WAIT_UP_TO_CYCLE( reu.TIMING_BA_SIGNAL_AVAIL );	\
			g2 = read32( ARM_GPIO_GPLEV0 );					\
		} while ( !(g2 & bBA) );							\
	}								

#define HANDLE_BUS_AVAILABLE \
	WAIT_UP_TO_CYCLE( reu.TIMING_READ_BA_WRITING );			\
	/*g2 = read32( ARM_GPIO_GPLEV0 );						*/	\
	if ( VIC_BA ) {											\
		do {												\
			WAIT_FOR_CPU_HALFCYCLE							\
			WAIT_FOR_VIC_HALFCYCLE							\
			RESTART_CYCLE_COUNTER							\
			WAIT_UP_TO_CYCLE( reu.TIMING_READ_BA_WRITING );	\
			g2 = read32( ARM_GPIO_GPLEV0 );					\
		} while ( !(g2 & bBA) );							\
	}								

#if 0
#define HANDLE_BUS_AVAILABLE \
	WAIT_UP_TO_CYCLE( reu.TIMING_READ_BA_WRITING );			\
	/*g2 = read32( ARM_GPIO_GPLEV0 );*/						\
	while ( !( g2 & bBA ) )	{								\
		WAIT_FOR_CPU_HALFCYCLE								\
		WAIT_FOR_VIC_HALFCYCLE								\
		RESTART_CYCLE_COUNTER								\
		WAIT_UP_TO_CYCLE( reu.TIMING_READ_BA_WRITING );		\
		g2 = read32( ARM_GPIO_GPLEV0 ); }
#endif


#define DMA_READBYTE_P1( addr )	{										\
	register u32 A asm ("r3" ) = addr;									\
	register u32 DD asm ("r4" );										\
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );			\
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );								\
																		\
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD | bRW_OUT | bDIR_Dx );			\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );								\
																		\
	CLR_GPIO( bLATCH_A8 );												\
																		\
	SET_GPIO( DD );														\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	CLR_GPIO( bLATCH_A0 );												\
																		\
	SET_BANK2_INPUT														\
	CLR_GPIO( bOE_Dx );													\
	OUT_GPIO_RW(); 														\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_ADDRLATCH );					\
																		\
	g2 = read32( ARM_GPIO_GPLEV0 );										\
	if ( VIC_BA ) {														\
		do {															\
			WAIT_FOR_CPU_HALFCYCLE										\
			WAIT_FOR_VIC_HALFCYCLE										\
			RESTART_CYCLE_COUNTER										\
			WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_ADDRLATCH );			\
			g2 = read32( ARM_GPIO_GPLEV0 );								\
		} while ( !(g2 & bBA) );										\
	}																	\
																		\
	CLR_GPIO( bLATCH_A_OE );											}


#define DMA_READBYTE_P2() {												\
	WAIT_FOR_CPU_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////


#define DMA_READBYTE_P1_CPU( addr )	{									\
	register u32 A asm ("r3" ) = addr;									\
	register u32 DD asm ("r4" );										\
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );			\
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );								\
																		\
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD | bRW_OUT | bDIR_Dx );			\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );								\
																		\
	CLR_GPIO( bLATCH_A8 );												\
																		\
	SET_GPIO( DD );														\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	CLR_GPIO( bLATCH_A0 );												\
																		\
	SET_BANK2_INPUT														\
	CLR_GPIO( bOE_Dx );													\
	OUT_GPIO_RW(); 														\
																		\
	readStalled = 0;													\
	WAIT_UP_TO_CYCLE( reu.TIMING_BA_SIGNAL_AVAIL );						\
	g2 = read32( ARM_GPIO_GPLEV0 );										\
	if ( VIC_BA ) {														\
		readStalled = 1;												\
		SET_GPIO( bOE_Dx ); SET_BANK2_OUTPUT							\
		return;															\
	}																	\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_ADDRLATCH );					\
																		\
	CLR_GPIO( bLATCH_A_OE );											}

#define DMA_READBYTE_P2_IO( pullGAMEforIO ) {							\
	WAIT_FOR_CPU_HALFCYCLE												\
	if ( pullGAMEforIO ) CLR_GPIO( bGAME_OUT ); 						\
	RESTART_CYCLE_COUNTER												}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#define DMA_READBYTE_P3( x, releaseDMA ) {								\
	WAIT_UP_TO_CYCLE( WAIT_CYCLE_WRITEDATA + 20 );						\
	g2 = read32( ARM_GPIO_GPLEV0 );										\
	x = (u8)( ( g2 >> D0 ) & 255 );										\
	WAIT_FOR_VIC_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												\
	DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER( releaseDMA );				}	

#define DMA_READBYTE_P3_NG( x, releaseDMA ) {							\
	WAIT_UP_TO_CYCLE( WAIT_CYCLE_WRITEDATA + 20 );						\
	g2 = read32( ARM_GPIO_GPLEV0 );										\
	x = (u8)( ( g2 >> D0 ) & 255 );										\
	WAIT_FOR_VIC_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												\
	DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER_NG( releaseDMA );			}	



#define DMA_WRITEBYTE_P1x( addr, data )	{								\
	register u32 A = addr, DD;											\
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );			\
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );								\
																		\
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD );								\
	CLR_GPIO( bDMA_OUT | ( D_FLAG & ( ~DD ) ) );						\
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );								\
																		\
	CLR_GPIO( bLATCH_A8 );												\
	SET_GPIO( DD | bRW_OUT );											\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	CLR_GPIO( bLATCH_A0 | bDIR_Dx );									\
	OUT_GPIO( RW_OUT );													\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING + 0 );	\
																		\
	if ( VIC_BA ) {														\
		do {															\
			WAIT_FOR_CPU_HALFCYCLE										\
			WAIT_FOR_VIC_HALFCYCLE										\
			RESTART_CYCLE_COUNTER										\
			WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING );		\
			g2 = read32( ARM_GPIO_GPLEV0 );								\
		} while ( !(g2 & bBA) );										\
	}																	\
																		\
	CLR_GPIO( bLATCH_A_OE );											\
																		\
	DD = (data) << D0;													\
	SET_GPIO( DD );														\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	WAIT_FOR_CPU_HALFCYCLE												\
	CLR_GPIO( bLATCH_A_OE | bOE_Dx | bRW_OUT ); 						}







#define DMA_WRITEBYTE_P1( addr, data )	{								\
	register u32 A = addr, DD;											\
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );			\
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );								\
																		\
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD );								\
	CLR_GPIO( bDMA_OUT | ( D_FLAG & ( ~DD ) ) );						\
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );								\
																		\
	CLR_GPIO( bLATCH_A8 );												\
	SET_GPIO( DD | bRW_OUT );											\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	CLR_GPIO( bLATCH_A0 | bDIR_Dx );									\
	OUT_GPIO( RW_OUT );													\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING + 0 );	\
																		\
	if ( VIC_BA ) {														\
		do {															\
			WAIT_FOR_CPU_HALFCYCLE										\
			WAIT_FOR_VIC_HALFCYCLE										\
			RESTART_CYCLE_COUNTER										\
			WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING );		\
			g2 = read32( ARM_GPIO_GPLEV0 );								\
		} while ( !(g2 & bBA) );										\
	}																	\
																		\
	CLR_GPIO( bLATCH_A_OE );											\
																		\
	DD = (data) << D0;													\
	SET_GPIO( DD );														\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	WAIT_FOR_CPU_HALFCYCLE												\
	CLR_GPIO( bLATCH_A_OE | bOE_Dx | bRW_OUT ); 						}







#define DMA_WRITEBYTE_P1_EARLY_BA( addr, data )	{						\
	register u32 A = addr, DD;											\
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );			\
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );								\
																		\
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD );								\
	CLR_GPIO( bDMA_OUT | ( D_FLAG & ( ~DD ) ) );						\
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );								\
																		\
	CLR_GPIO( bLATCH_A8 );												\
	SET_GPIO( DD | bRW_OUT );											\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	CLR_GPIO( bLATCH_A0 | bDIR_Dx );									\
	OUT_GPIO( RW_OUT );													\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING + 0 );	\
																		\
	if ( VIC_EARLY_BA ) {												\
		do {															\
			WAIT_FOR_CPU_HALFCYCLE										\
	RESTART_CYCLE_COUNTER												\
	WAIT_UP_TO_CYCLE( 600 + 0 );										\
	g2_earlyBA = g2;													\
			WAIT_FOR_VIC_HALFCYCLE										\
			RESTART_CYCLE_COUNTER										\
			WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING );		\
			g2 = read32( ARM_GPIO_GPLEV0 );								\
		} while ( !(g2 & bBA) );										\
	}																	\
	CLR_GPIO( bLATCH_A_OE );											\
																		\
	DD = (data) << D0;													\
	SET_GPIO( DD );														\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	WAIT_FOR_CPU_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												\
																		\
	CLR_GPIO( bLATCH_A_OE | bOE_Dx | bRW_OUT ); 						\
	WAIT_UP_TO_CYCLE( 600 + 0 );										\
	g2_earlyBA = g2;													}






#define DMA_WRITEBYTE_P1_IO( addr, data, pullGAMEforIO )	{			\
	register u32 A = addr, DD;											\
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );			\
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );								\
																		\
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD );								\
	CLR_GPIO( bDMA_OUT | ( D_FLAG & ( ~DD ) ) );						\
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );								\
																		\
	CLR_GPIO( bLATCH_A8 );												\
	SET_GPIO( DD | bRW_OUT );											\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	CLR_GPIO( bLATCH_A0 | bDIR_Dx );									\
	OUT_GPIO( RW_OUT );													\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING + 0 );	\
																		\
	CLR_GPIO( bLATCH_A_OE );											\
																		\
	DD = (data) << D0;													\
	SET_GPIO( DD );														\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	WAIT_FOR_CPU_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												\
	CLR_GPIO( bOE_Dx | bRW_OUT | ( pullGAMEforIO ? bGAME_OUT : 0 ) );										}




#define DMA_WRITEBYTE_P1_IO_CHECK_BA( addr, data, pullGAMEforIO )	{	\
	register u32 A = addr, DD;											\
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );			\
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );								\
																		\
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD );								\
	CLR_GPIO( bDMA_OUT | ( D_FLAG & ( ~DD ) ) );						\
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );								\
																		\
	CLR_GPIO( bLATCH_A8 );												\
	SET_GPIO( DD | bRW_OUT );											\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	CLR_GPIO( bLATCH_A0 | bDIR_Dx );									\
	OUT_GPIO( RW_OUT );													\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_BA_SIGNAL_AVAIL );						\
	g2 = read32( ARM_GPIO_GPLEV0 );										\
	if ( VIC_BA ) {														\
		do {															\
			WAIT_FOR_CPU_HALFCYCLE										\
			WAIT_FOR_VIC_HALFCYCLE										\
			RESTART_CYCLE_COUNTER										\
			WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING );		\
			g2 = read32( ARM_GPIO_GPLEV0 );								\
		} while ( !(g2 & bBA) );										\
	}																	\
																		\
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING + 0 );	\
																		\
	CLR_GPIO( bLATCH_A_OE );											\
																		\
	DD = (data) << D0;													\
	SET_GPIO( DD );														\
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );									\
																		\
	WAIT_FOR_CPU_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												\
	CLR_GPIO( bOE_Dx | bRW_OUT | ( pullGAMEforIO ? bGAME_OUT : 0 ) );										}







#define DMA_WRITEBYTE_P2( releaseDMA ) {								\
	WAIT_FOR_VIC_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												\
	WAIT_UP_TO_CYCLE( reu.TIMING_DATA_HOLD );							\
	DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER( releaseDMA ) }			


#define DMA_WRITEBYTE_P2_NG( releaseDMA ) {								\
	WAIT_FOR_VIC_HALFCYCLE												\
	RESTART_CYCLE_COUNTER												\
	WAIT_UP_TO_CYCLE( reu.TIMING_DATA_HOLD );							\
	DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER_NG( releaseDMA ) }			














__attribute__( ( always_inline ) ) inline 
void emuReadByteREU_p1( register u32 &g2, u16 addr )
{ 
#ifdef OLD_BUS_PROTOCOL_READ
	register u32 DD = flipByte( ( addr ) & 255 ) << D0;
	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD | bOE_Dx | bDIR_Dx );
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );
	#ifdef DOUBLE_GPIO_WRITES
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );
	#endif
	SET_GPIO( bRW_OUT );
	DD = flipByte( ( ( addr ) >> 8 ) & 255 ) << D0;									
	CLR_GPIO( bLATCH_A0 );
	#ifdef DOUBLE_GPIO_WRITES
	CLR_GPIO( bLATCH_A0 );
	#endif
	SET_GPIO( DD );												
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );
	#ifdef DOUBLE_GPIO_WRITES
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );
	#endif
	CLR_GPIO( bLATCH_A8 );
	SET_BANK2_INPUT

	CLR_GPIO( bOE_Dx );
	HANDLE_BUS_AVAILABLE_READ

	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_ADDRLATCH );	
	CLR_GPIO( bLATCH_A_OE );
#else
	register u32 A asm ("r3" ) = addr;
	register u32 DD asm ("r4" );
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );	// flip all bits in 32-bit-DWORD
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );

	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD | bRW_OUT | bDIR_Dx );
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );

	CLR_GPIO( bLATCH_A8 );

	SET_GPIO( DD );
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );

	CLR_GPIO( bLATCH_A0 );

	SET_BANK2_INPUT
	CLR_GPIO( bOE_Dx );
	OUT_GPIO_RW(); 

	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_ADDRLATCH );	

	if ( VIC_BA ) {											
		do {												
			WAIT_FOR_CPU_HALFCYCLE							
			WAIT_FOR_VIC_HALFCYCLE							
			RESTART_CYCLE_COUNTER							
			WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_ADDRLATCH );	
			g2 = read32( ARM_GPIO_GPLEV0 );					
		} while ( !(g2 & bBA) );							
	}		

	CLR_GPIO( bLATCH_A_OE );

#endif
}


__attribute__( ( always_inline ) ) inline  
void emuReadByteREU_p2( register u32 &g2 )
{ 
#ifdef OLD_BUS_PROTOCOL_READ
	WAIT_FOR_CPU_HALFCYCLE		
	RESTART_CYCLE_COUNTER								
#else
	WAIT_FOR_CPU_HALFCYCLE		
	RESTART_CYCLE_COUNTER								
#endif
}

__attribute__( ( always_inline ) ) inline  
void emuReadByteREU_p3( register u32 &g2, register u8 &x, bool releaseDMA )
{ 
#ifdef OLD_BUS_PROTOCOL_READ
	WAIT_UP_TO_CYCLE( WAIT_CYCLE_WRITEDATA + 20 );
	g2 = read32( ARM_GPIO_GPLEV0 );
	x = (u8)( ( g2 >> D0 ) & 255 );
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER( releaseDMA );
#else
	WAIT_UP_TO_CYCLE( WAIT_CYCLE_WRITEDATA + 20 );
	g2 = read32( ARM_GPIO_GPLEV0 );
	x = (u8)( ( g2 >> D0 ) & 255 );

	WAIT_FOR_VIC_HALFCYCLE

	RESTART_CYCLE_COUNTER
	DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER( releaseDMA );
#endif
} 


__attribute__( ( always_inline ) ) inline  
void emuWriteByteREU_p1( register u32 &g2, u16 addr, u8 data )
{
#ifdef OLD_BUS_PROTOCOL_WRITE
	register u32 A asm ("r3" ) = addr;
	register u32 DD asm ("r4" );
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );	// flip all bits in 32-bit-DWORD
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );

	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD );
	CLR_GPIO( bDMA_OUT | ( D_FLAG & ( ~DD ) ) );
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );

	CLR_GPIO( bDIR_Dx | bRW_OUT | bLATCH_A8 );
	SET_GPIO( DD );
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );
	DD = data << D0;
	CLR_GPIO( bLATCH_A0 );

	SET_GPIO( DD );
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );

	HANDLE_BUS_AVAILABLE

	WAIT_UP_TO_CYCLE(reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING_MINUS_RW_BEFORE_ADDR );
	OUT_GPIO( RW_OUT );
	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING + 0 );
	CLR_GPIO( bLATCH_A_OE );

	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_DATA_WRITING );
	CLR_GPIO( bOE_Dx );
#else
	register u32 A = addr;
	register u32 DD;
	asm volatile( "rbit %w0, %w1" : "=r" ( A ) : "r" ( A ) );	// flip all bits in 32-bit-DWORD
	DD = ( A & 0x00ff0000 ) << ( D0 - 16 );

	SET_GPIO( bLATCH_A0 | bLATCH_A8 | DD );
	CLR_GPIO( bDMA_OUT | ( D_FLAG & ( ~DD ) ) );
	DD = ( A & 0xff000000 ) >> ( 24 - D0 );

	CLR_GPIO( bLATCH_A8 );
	SET_GPIO( DD | bRW_OUT );
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );
	CLR_GPIO( bLATCH_A0 | bDIR_Dx );
	OUT_GPIO( RW_OUT );

	WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING );

	if ( VIC_BA ) {											
		do {												
			WAIT_FOR_CPU_HALFCYCLE							
			WAIT_FOR_VIC_HALFCYCLE							
			RESTART_CYCLE_COUNTER							
			WAIT_UP_TO_CYCLE( reu.TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING );	
			g2 = read32( ARM_GPIO_GPLEV0 );					
		} while ( !(g2 & bBA) );							
	}								

	CLR_GPIO( bLATCH_A_OE );

	DD = data << D0;
	SET_GPIO( DD );
	CLR_GPIO( ( D_FLAG & ( ~DD ) ) );

	WAIT_FOR_CPU_HALFCYCLE
	CLR_GPIO( bLATCH_A_OE | bOE_Dx | bRW_OUT ); 
#endif
}

__attribute__( ( always_inline ) ) inline  
void emuWriteByteREU_p2( register u32 &g2, bool releaseDMA )
{
	WAIT_FOR_VIC_HALFCYCLE
	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( reu.TIMING_DATA_HOLD );
	DISABLE_ADDRESS_LATCH_AND_BUSTRANSCEIVER( releaseDMA )
}


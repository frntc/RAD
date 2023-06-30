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
#ifndef _lowlevel_arm_h
#define _lowlevel_arm_h

#include <circle/types.h>

// default timings
#define	AUTO_TIMING_RPI3PLUS_C64		1
#define	AUTO_TIMING_RPI3PLUS_C64C128	2
#define	AUTO_TIMING_RPI0_C64			3
#define	AUTO_TIMING_RPI0_C64C128		4

extern void setDefaultTimings( int mode );

extern u32 WAIT_FOR_SIGNALS;
extern u32 WAIT_CYCLE_MULTIPLEXER;
extern u32 WAIT_CYCLE_READ;
extern u32 WAIT_CYCLE_READ_BADLINE;
extern u32 WAIT_CYCLE_READ_VIC2;
extern u32 WAIT_CYCLE_WRITEDATA;
extern u32 WAIT_CYCLE_WRITEDATA_VIC2;
extern u32 WAIT_CYCLE_MULTIPLEXER_VIC2;
extern u32 WAIT_TRIGGER_DMA;
extern u32 WAIT_RELEASE_DMA;

extern u32 TIMING_OFFSET_CBTD;
extern u32 TIMING_DATA_HOLD;
extern u32 TIMING_TRIGGER_DMA;
extern u32 TIMING_ENABLE_ADDRLATCH;
extern u32 TIMING_READ_BA_WRITING;
extern u32 TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING;
extern u32 TIMING_ENABLE_DATA_WRITING;
extern u32 TIMING_BA_SIGNAL_AVAIL;

extern u32 CACHING_L1_WINDOW_KB;
extern u32 CACHING_L2_OFFSET_KB;
extern u32 CACHING_L2_PRELOADS_PER_CYCLE;

extern u32 TIMING_RW_BEFORE_ADDR;

extern u32 modeC128;
extern u32 modeVIC, modePALNTSC;
extern u32 hasSIDKick;


#define PMCCFILTR_NSH_EN_BIT    27
#define PMCNTENSET_C_EN_BIT     31
#define PMCR_LC_EN_BIT          6
#define PMCR_C_RESET_BIT        2
#define PMCR_EN_BIT             0

#define AA __attribute__ ((aligned (64)))
#define AAA __attribute__ ((aligned (128)))

#define BEGIN_CYCLE_COUNTER \
						  		armCycleCounter = 0; \
								asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (armCycleCounter) );

#define RESTART_CYCLE_COUNTER \
								asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (armCycleCounter) );

#define READ_CYCLE_COUNTER( cc ) \
								asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (cc) );

#define WAIT_CYCLES( wc ) { \
								u64 cc1,cc2; \
								asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (cc1) ); \
								do { \
									asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (cc2) ); \
								} while ( (cc2) < (wc+cc1) ); }


#define WAIT_UP_TO_CYCLE( wc ) { \
								u64 cc2; \
								do { \
									asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (cc2) ); \
								} while ( (cc2) < (wc+armCycleCounter) ); }

#define WAIT_UP_TO_CYCLE_AFTER( wc, cc ) { \
								u64 cc2; \
								do { \
									asm volatile( "MRS %0, PMCCNTR_EL0" : "=r" (cc2) ); \
								} while ( (cc2-cc) < (wc) ); }

#define CACHE_PRELOADL1KEEP( ptr )	{ asm volatile ("prfm PLDL1KEEP, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADL1STRM( ptr )	{ asm volatile ("prfm PLDL1STRM, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADL1KEEPW( ptr ) { asm volatile ("prfm PSTL1KEEP, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADL1STRMW( ptr ) { asm volatile ("prfm PSTL1STRM, [%0]" :: "r" (ptr)); }

#define CACHE_PRELOADL2KEEP( ptr )	{ asm volatile ("prfm PLDL2KEEP, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADL2KEEPW( ptr )	{ asm volatile ("prfm PSTL2KEEP, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADL2STRM( ptr )	{ asm volatile ("prfm PLDL2STRM, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADL2STRMW( ptr )	{ asm volatile ("prfm PSTL2STRM, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADI( ptr )		{ asm volatile ("prfm PLIL1STRM, [%0]" :: "r" (ptr)); }
#define CACHE_PRELOADIKEEP( ptr )	{ asm volatile ("prfm PLIL1KEEP, [%0]" :: "r" (ptr)); }

#define CACHE_PRELOAD_INSTRUCTION_CACHE( p, size )			\
	{ u8 *ptr = (u8*)( p );									\
	for ( register u32 i = 0; i < (size+63) / 64; i++ )	{	\
		CACHE_PRELOADIKEEP( ptr );							\
		ptr += 64;											\
	} }

#define CACHE_PRELOAD_DATA_CACHE( p, size, FUNC )			\
	{ u8 *ptr = (u8*)( p );									\
	for ( register u32 i = 0; i < (size+63) / 64; i++ )	{	\
		FUNC( ptr );										\
		ptr += 64;											\
	} }

#define FORCE_READ_LINEAR( p, size ) {						\
		__attribute__((unused)) volatile u8 forceRead;		\
		for ( register u32 i = 0; i < size; i++ )			\
			forceRead = ((u8*)p)[ i ];						\
	}

#define FORCE_READ_LINEARa( p, size, acc ) {				\
		__attribute__((unused)) volatile u8 forceRead;		\
		for ( register u32 i = 0; i < acc; i++ )			\
			forceRead = ((u8*)p)[ i % size ];				\
	}

#define ADDR_LINEAR2CACHE_T(l) ((((l)&255)<<5)|(((l)>>8)&31))

#define FORCE_READ_LINEAR_CACHE( p, size, rounds ) {		\
		__attribute__((unused)) volatile u8 forceRead;		\
		for ( register u32 i = 0; i < size*rounds; i++ )	\
			forceRead = ((u8*)p)[ ADDR_LINEAR2CACHE_T(i%size) ]; \
	}

#define FORCE_READ_LINEAR32_SKIP( p, size ) {				\
		__attribute__((unused)) volatile u32 forceRead;		\
		for ( register u32 i = 0; i < size/4; i+=2 )		\
			forceRead = ((u32*)p)[ i ];						\
	}

#define FORCE_READ_LINEAR64( p, size ) {					\
		__attribute__((unused)) volatile u64 forceRead;		\
		for ( register u32 i = 0; i < size/8; i++ )			\
			forceRead = ((u64*)p)[ i ];						\
	}

#define FORCE_READ_LINEAR32( p, size ) {					\
		__attribute__((unused)) volatile u32 forceRead;		\
		for ( register u32 i = 0; i < size/4; i++ )			\
			forceRead = ((u32*)p)[ i ];						\
	}

#define FORCE_READ_LINEAR32a( p, size, acc ) {				\
		__attribute__((unused)) volatile u32 forceRead;		\
		for ( register u32 i = 0; i < acc; i++ )			\
			forceRead = ((u32*)p)[ i % (size/4) ];			\
	}

#define FORCE_READ_RANDOM( p, size, acc ) {					\
	__attribute__((unused)) volatile u32 forceRead;			\
	u32 seed = 123456789;	u32 *ptr32 = (u32*)p;			\
	for ( register u32 i = 0; i < acc; i++ ) {				\
		seed = (1103515245*seed+12345) & ((u32)(1<<31)-1 );	\
		forceRead = ptr32[ seed % ( size / 4 ) ];			\
	} }

#define _LDNP_2x32( addr, val1, val2 ) {						\
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (val1), "=r" (val2) : "r" (addr) : "memory" ); }

#define _LDNP_1x32( addr, val ) { u32 tmp;					\
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (val), "=r" (tmp) : "r" (addr) : "memory" ); }

#define _LDNP_1x16( addr, val ) { u32 tmp1, tmp2;					\
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (tmp1), "=r" (tmp2) : "r" (addr) : "memory" ); val = tmp1 & 65535; }

#define _LDNP_1x8( addr, val ) { u32 tmp1, tmp2;					\
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (tmp1), "=r" (tmp2) : "r" (addr) : "memory" ); val = tmp1 & 255; }

#define SET_GPIO( set )	write32( ARM_GPIO_GPSET0, (set) );
#define CLR_GPIO( clr )	write32( ARM_GPIO_GPCLR0, (clr) );

#define	SETCLR_GPIO( set, clr )	{SET_GPIO( set )	CLR_GPIO( clr )}

extern void initCycleCounter();

#define RESET_CPU_CYCLE_COUNTER \
	asm volatile( "msr PMCR_EL0, %0" : : "r" ( ( 1 << PMCR_LC_EN_BIT ) | ( 1 << PMCR_C_RESET_BIT ) | ( 1 << PMCR_EN_BIT ) ) ); 

extern __attribute__( ( always_inline ) ) inline void LDNP_2x32( unsigned long addr, u32 &val1, u32 &val2 );
extern __attribute__( ( always_inline ) ) inline u32 LDNP_1x32( unsigned long addr );
extern __attribute__( ( always_inline ) ) inline u16 LDNP_1x16( unsigned long addr );

__attribute__( ( always_inline ) ) inline u8 LDNP_1x8( void *addr )
{
	u32 val1, val2;
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (val1), "=r" (val2) : "r" ((unsigned long)addr) : "memory");
	return val1 & 255;
}


#endif

 
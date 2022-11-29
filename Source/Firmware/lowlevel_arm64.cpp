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
#include "lowlevel_arm64.h"


u32 WAIT_FOR_SIGNALS = 80;
u32 WAIT_CYCLE_MULTIPLEXER = 235;
u32 WAIT_CYCLE_READ = 470;
u32 WAIT_CYCLE_WRITEDATA = 470;
u32 WAIT_CYCLE_READ_BADLINE = 385;		// not needed in RAD
u32 WAIT_CYCLE_READ_VIC2 = 428;			// not needed in RAD
u32 WAIT_CYCLE_WRITEDATA_VIC2 = 505;	// not needed in RAD
u32 WAIT_CYCLE_MULTIPLEXER_VIC2 = 265;	// not needed in RAD
u32 WAIT_TRIGGER_DMA = 0;
u32 WAIT_RELEASE_DMA = 600;				// not needed in RAD

u32 TIMING_OFFSET_CBTD = 40;
u32 TIMING_DATA_HOLD = 0;
u32 TIMING_TRIGGER_DMA = 0;
u32 TIMING_ENABLE_ADDRLATCH = 535;
u32 TIMING_READ_BA_WRITING = 256;
u32 TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = 467;
u32 TIMING_ENABLE_DATA_WRITING = 675;
u32 TIMING_BA_SIGNAL_AVAIL = 261;

u32 CACHING_L1_WINDOW_KB = 0;
u32 CACHING_L2_OFFSET_KB = 0;
u32 CACHING_L2_PRELOADS_PER_CYCLE = 0;

// initialize what we need for the performance counters
void initCycleCounter()
{
	unsigned long rControl;
	unsigned long rFilter;
	unsigned long rEnableSet;

	asm volatile( "mrs %0, PMCCFILTR_EL0" : "=r" ( rFilter ) );
	asm volatile( "mrs %0, PMCR_EL0" : "=r" ( rControl ) );

	// enable PMU filter to count cycles
	rFilter |= ( 1 << PMCCFILTR_NSH_EN_BIT );
	asm volatile( "msr PMCCFILTR_EL0, %0" : : "r" ( rFilter ) );
	asm volatile( "mrs %0, PMCCFILTR_EL0" : "=r" ( rFilter ) );

	// enable cycle count register
	asm volatile( "mrs %0, PMCNTENSET_EL0" : "=r" ( rEnableSet ) );
	rEnableSet |= ( 1 << PMCNTENSET_C_EN_BIT );
	asm volatile( "msr PMCNTENSET_EL0, %0" : : "r" ( rEnableSet ) );
	asm volatile( "mrs %0, PMCNTENSET_EL0" : "=r" ( rEnableSet ) );

	// enable long cycle counter and reset it
	rControl = ( 1 << PMCR_LC_EN_BIT ) | ( 1 << PMCR_C_RESET_BIT ) | ( 1 << PMCR_EN_BIT );
	asm volatile( "msr PMCR_EL0, %0" : : "r" ( rControl ) );
	asm volatile( "mrs %0, PMCR_EL0" : "=r" ( rControl ) );
}

void setDefaultTimings( int mode )
{
	switch ( mode )
	{
	case AUTO_TIMING_RPI3PLUS_C64C128:
		WAIT_FOR_SIGNALS = 60;
		WAIT_CYCLE_MULTIPLEXER = 220;
		WAIT_CYCLE_READ = 475;
		WAIT_CYCLE_WRITEDATA = 470;
		WAIT_CYCLE_READ_BADLINE = 400;
		WAIT_CYCLE_READ_VIC2 = 445;
		WAIT_CYCLE_WRITEDATA_VIC2 = 505;
		WAIT_CYCLE_MULTIPLEXER_VIC2 = 265;
		WAIT_TRIGGER_DMA = 600;
		WAIT_RELEASE_DMA = 600;

		TIMING_OFFSET_CBTD = 10;
		TIMING_DATA_HOLD = 22;
		TIMING_TRIGGER_DMA = 112 * 0;
		TIMING_ENABLE_ADDRLATCH = 500;
		TIMING_READ_BA_WRITING = 325;
		TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING = 425;
		TIMING_ENABLE_DATA_WRITING = 700;
		TIMING_BA_SIGNAL_AVAIL = 280;

		break;
	}
}

__attribute__( ( always_inline ) ) inline void LDNP_2x32( unsigned long addr, u32 &val1, u32 &val2 )
{
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (val1), "=r" (val2) : "r" (addr) : "memory");
}

__attribute__( ( always_inline ) ) inline u32 LDNP_1x32( unsigned long addr )
{
	u32 val1, val2;
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (val1), "=r" (val2) : "r" (addr) : "memory");
	return val1;
}

__attribute__( ( always_inline ) ) inline u16 LDNP_1x16( unsigned long addr )
{
	u32 val1, val2;
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (val1), "=r" (val2) : "r" (addr) : "memory");
	return val1 & 65535;
}

/*__attribute__( ( always_inline ) ) inline u8 LDNP_1x8( void *addr )
{
	u32 val1, val2;
    __asm__ __volatile__("ldnp %0, %1, [%2]\n\t" : "=r" (val1), "=r" (val2) : "r" ((unsigned long)addr) : "memory");
	return val1 & 255;
}*/

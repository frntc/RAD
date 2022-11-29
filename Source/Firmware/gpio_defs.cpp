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
#include <circle/bcm2835.h>
#include <circle/gpiopin.h>
#include <circle/memio.h>
#include "gpio_defs.h"
#include "lowlevel_arm64.h"

void INP_GPIO( int pin )
{
	unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( pin / 10 ) * 4;
	unsigned nShift = ( pin % 10 ) * 3;

	u32 nValue = read32 ( nSelReg );
	nValue &= ~( 7 << nShift );
	write32 ( nSelReg, nValue );
}

void OUT_GPIO( int pin )
{
	unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( pin / 10 ) * 4;
	unsigned nShift = ( pin % 10 ) * 3;

	u32 nValue = read32 ( nSelReg );
	nValue &= ~( 7 << nShift );
	nValue |= 1 << nShift;
	write32 ( nSelReg, nValue );
}

void INP_GPIO_RW()
{
	unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( RW_OUT / 10 ) * 4;
	unsigned nShift = ( RW_OUT % 10 ) * 3;

	u32 nValue = read32( nSelReg );
	nValue &= ~( 7 << nShift );
	write32( nSelReg, nValue );
}

void OUT_GPIO_RW()
{
	const unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( RW_OUT / 10 ) * 4;
	const unsigned nShift = ( RW_OUT % 10 ) * 3;

	u32 nValue = read32( nSelReg );
	nValue &= ~( 7 << nShift );
	nValue |= 1 << nShift;
	write32( nSelReg, nValue );
}

void INP_GPIO_RESET()
{
	unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( RESET_OUT / 10 ) * 4;
	unsigned nShift = ( RESET_OUT % 10 ) * 3;

	u32 nValue = read32( nSelReg );
	nValue &= ~( 7 << nShift );
	write32( nSelReg, nValue );
}

void OUT_GPIO_RESET()
{
	const unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( RESET_OUT / 10 ) * 4;
	const unsigned nShift = ( RESET_OUT % 10 ) * 3;

	u32 nValue = read32( nSelReg );
	nValue &= ~( 7 << nShift );
	nValue |= 1 << nShift;
	write32( nSelReg, nValue );
}

void INP_GPIO_IRQ()
{
	unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( IRQ_OUT / 10 ) * 4;
	unsigned nShift = ( IRQ_OUT % 10 ) * 3;

	u32 nValue = read32( nSelReg );
	nValue &= ~( 7 << nShift );
	write32( nSelReg, nValue );
}

void OUT_GPIO_IRQ()
{
	const unsigned nSelReg = ARM_GPIO_GPFSEL0 + ( IRQ_OUT / 10 ) * 4;
	const unsigned nShift = ( IRQ_OUT % 10 ) * 3;

	u32 nValue = read32( nSelReg );
	nValue &= ~( 7 << nShift );
	nValue |= 1 << nShift;
	write32( nSelReg, nValue );
}


#define PI_PUD_OFF  0
#define PI_PUD_DOWN 1
#define PI_PUD_UP   2

#define GPPUD     37
#define GPPUDCLK0 38

#define ARM_GPIO_GPPUD		(ARM_GPIO_BASE + 0x94)
#define ARM_GPIO_GPPUDCLK0	(ARM_GPIO_BASE + 0x98)

#define PI_BANK (gpio>>5)
#define PI_BIT  (1<<(gpio&0x1F))

static void PULLUPDOWN_GPIO( u32 gpio, u32 pud )
{
	write32( ARM_GPIO_GPPUD, pud );

	u64 armCycleCounter = 0;
	BEGIN_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( 20000 )

	write32( ARM_GPIO_GPPUDCLK0 + PI_BANK, PI_BIT );

	RESTART_CYCLE_COUNTER
	WAIT_UP_TO_CYCLE( 20000 )

	write32( ARM_GPIO_GPPUD, 0 );
	write32( ARM_GPIO_GPPUDCLK0 + PI_BANK, 0 );
}

void gpioInit()
{
	// CBTD3861 in/outputs
	// RW, PHI2, CS etc.
	INP_GPIO( PHI2 );		

	INP_GPIO( RW_OUT );
	OUT_GPIO( RW_OUT );
	write32( ARM_GPIO_GPSET0, bRW_OUT ); 
	INP_GPIO( RW_OUT );

	INP_GPIO( DMA_OUT );
	OUT_GPIO( DMA_OUT );
	write32( ARM_GPIO_GPSET0, bDMA_OUT ); 

	INP_GPIO( RESET_OUT );
	OUT_GPIO( RESET_OUT );
	write32( ARM_GPIO_GPSET0, bRESET_OUT ); 
	INP_GPIO( RESET_OUT );

	INP_GPIO( GAME_OUT );
	OUT_GPIO( GAME_OUT );
	write32( ARM_GPIO_GPSET0, bGAME_OUT ); 
	//INP_GPIO( GAME_OUT );

	INP_GPIO( IRQ_OUT );
	OUT_GPIO( IRQ_OUT );
	write32( ARM_GPIO_GPSET0, bIRQ_OUT ); 
	INP_GPIO( IRQ_OUT );

	OUT_GPIO( MPLEX_SEL );
	write32( ARM_GPIO_GPCLR0, bMPLEX_SEL ); 
	for ( int i = 0; i < 4; i++ )
	{
		INP_GPIO( 0 + i );
		INP_GPIO( 10 + i );
	}

	// 245: output disabled, direction: towards bus
	INP_GPIO( OE_Dx );
	OUT_GPIO( OE_Dx );
	write32( ARM_GPIO_GPSET0, 1 << OE_Dx );

	INP_GPIO( DIR_Dx );
	OUT_GPIO( DIR_Dx );
	write32( ARM_GPIO_GPCLR0, 1 << DIR_Dx );
	SET_BANK2_OUTPUT 

	// Latches: output disabled, transparent mode (LATCH_A0, LATCH_A8 high)
	INP_GPIO( LATCH_A_OE );
	OUT_GPIO( LATCH_A_OE );
	write32( ARM_GPIO_GPSET0, 1 << LATCH_A_OE );
	
	INP_GPIO( LATCH_A0 );
	OUT_GPIO( LATCH_A0 );
	write32( ARM_GPIO_GPSET0, 1 << LATCH_A0 );

	INP_GPIO( LATCH_A8 );
	OUT_GPIO( LATCH_A8 );
	write32( ARM_GPIO_GPSET0, 1 << LATCH_A8 );

	for ( u32 i = 0; i < 32; i++ )
		PULLUPDOWN_GPIO( i, PI_PUD_OFF );

	PULLUPDOWN_GPIO( LATCH_A0, PI_PUD_UP );
	PULLUPDOWN_GPIO( LATCH_A8, PI_PUD_UP );
	PULLUPDOWN_GPIO( LATCH_A_OE, PI_PUD_UP );
	PULLUPDOWN_GPIO( DIR_Dx, PI_PUD_DOWN );
	PULLUPDOWN_GPIO( OE_Dx, PI_PUD_UP );
	PULLUPDOWN_GPIO( RW_OUT, PI_PUD_UP );

	PULLUPDOWN_GPIO( DMA_OUT, PI_PUD_UP );
}


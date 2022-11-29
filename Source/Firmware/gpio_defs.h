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
#ifndef _gpio_defs_h
#define _gpio_defs_h

// CBTD3861 in/outputs
#define PHI2 		9
#define RW_OUT		18
#define DMA_OUT		19
#define RESET_OUT	8
#define IRQ_OUT		4
#define GAME_OUT	5

#define bPHI		(1<<PHI2)
#define bRW_OUT		(1<<RW_OUT)
#define bDMA_OUT	(1<<DMA_OUT)
#define bRESET_OUT	(1<<RESET_OUT)
#define bIRQ_OUT	(1<<IRQ_OUT)
#define bGAME_OUT	(1<<GAME_OUT)

// LVC257 inputs + selection
#define MPLEX_SEL	7
#define bMPLEX_SEL	(1<<MPLEX_SEL)
#define BA			10
#define bBA			(1<<BA)
#define IO2			12
#define bIO2		(1<<IO2)
#define triggerFF00 13
#define bTriggerFF00 (1<<triggerFF00)

#define NMI			0
#define bNMI		(1<<NMI)
//#define IRQ			1
//#define bIRQ		(1<<IRQ)
#define IO1			2
#define bIO1		(1<<IO1)
#define BUTTON		3
#define bBUTTON		(1<<BUTTON)
#define ROMH		1
#define bROMH		(1<<ROMH)

#define A0_IN		10
#define A1_IN		11
#define A2_IN		12
#define A3_IN		13
#define A4_IN		0
#define A5_IN		1
#define A6_IN		2
#define A7_IN		3

// latches and bus driver
#define DIR_Dx		6
#define OE_Dx		14
#define LATCH_A0	15
#define LATCH_A8	16
#define LATCH_A_OE	17

// data lines direction (is NOT the same as RW on the expansion port)
#define bDIR_Dx		(1<<DIR_Dx)

// data lines output enable
#define bOE_Dx		(1<<OE_Dx)
#define bLATCH_A0	(1<<LATCH_A0)
#define bLATCH_A8	(1<<LATCH_A8)
#define bLATCH_A_OE	(1<<LATCH_A_OE)

// important: all data pins D0-D7 are assumed to be in GPIO-bank #2 (for faster read/write toggle)
#define D0	20
#define D1	21
#define D2	22
#define D3	23
#define D4	24
#define D5	25
#define D6	26
#define D7	27
#define D_FLAG ( (1<<D0)|(1<<D1)|(1<<D2)|(1<<D3)|(1<<D4)|(1<<D5)|(1<<D6)|(1<<D7) )

#define A0	D0
#define A1	D1
#define A2	D2
#define A3	D3
#define A4	D4
#define A5	D5	 
#define A6	D6	 
#define A7	D7	 
#define A8  D0
#define A9  D1
#define A10 D2
#define A11 D3
#define A12 D4
#define A13 D5
#define A14 D6
#define A15 D7

#define ARM_GPIO_GPFSEL2	(ARM_GPIO_BASE + 0x08) 

// set bank 2 GPIOs to input (D0-D7)
#define SET_BANK2_INPUT { \
		const unsigned int b1 = ~( ( 7 << 0 ) | ( 7 << 3 ) | ( 7 << 6 ) | ( 7 << 9 ) | ( 7 << 12 ) | ( 7 << 15 ) | ( 7 << 18 ) | ( 7 << 21 ) ); \
		u32 t2 = read32( ARM_GPIO_GPFSEL2 ) & b1; \
		write32( ARM_GPIO_GPFSEL2, t2 ); }

// set bank 2 GPIOs to output (D0-D7)
#define SET_BANK2_OUTPUT { \
			const unsigned int b1 = ~( ( 7 << 0 ) | ( 7 << 3 ) | ( 7 << 6 ) | ( 7 << 9 ) | ( 7 << 12 ) | ( 7 << 15 ) | ( 7 << 18 ) | ( 7 << 21 ) ); \
			const unsigned int b2 = ( 1 << 0 ) | ( 1 << 3 ) | ( 1 << 6 ) | ( 1 << 9 ) | ( 1 << 12 ) | ( 1 << 15 ) | ( 1 << 18 ) | ( 1 << 21 ); \
			register u32 t2 = read32( ARM_GPIO_GPFSEL2 ) & b1; \
			write32( ARM_GPIO_GPFSEL2, t2 | b2 ); }

void gpioInit();

extern void INP_GPIO( int pin );
extern void OUT_GPIO( int pin );

extern void INP_GPIO_RW();
extern void OUT_GPIO_RW();
extern void INP_GPIO_IRQ();
extern void OUT_GPIO_IRQ();
extern void INP_GPIO_RESET();
extern void OUT_GPIO_RESET();


#endif

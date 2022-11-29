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
 Copyright (c) 2019-2022 Carsten Dachsbacher <frenetic@dachsbacher.de>

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

#ifndef _helpers_h
#define _helpers_h

#include <SDCard/emmc.h>
#include <fatfs/ff.h>

extern int readFile( CLogger *logger, const char *DRIVE, const char *FILENAME, u8 *data, u32 *size );
extern int getFileSize( CLogger *logger, const char *DRIVE, const char *FILENAME, u32 *size );
extern int writeFile( CLogger *logger, const char *DRIVE, const char *FILENAME, u8 *data, u32 size );

#define ROMH_ACCESS			(!(g2 & bROMH))
#define CPU_RESET			(!(g2&bRESET_OUT)) 

#define STANDARD_SETUP_TIMER_INTERRUPT_CYCLECOUNTER_GPIO										\
	boolean bOK = TRUE;																			\
	m_CPUThrottle.SetSpeed( CPUSpeedMaximum );													\
	if ( bOK ) bOK = m_Screen.Initialize();														\
	if ( bOK ) { 																				\
		CDevice *pTarget = m_DeviceNameService.GetDevice( m_Options.GetLogDevice(), FALSE );	\
		if ( pTarget == 0 )	pTarget = &m_Screen;												\
		bOK = m_Logger.Initialize( pTarget ); 													\
	}																							\
	if ( bOK ) bOK = m_Interrupt.Initialize(); 													\
	if ( bOK ) bOK = m_Timer.Initialize();														\
	/* initialize ARM cycle counters (for accurate timing) */ 									\
	initCycleCounter(); 																		\
	/* initialize GPIOs */ 																		\
	gpioInit(); 																				

#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#endif

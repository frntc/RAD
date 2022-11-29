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
#ifndef _RAD_H
#define _RAD_H

#include <circle/startup.h>
#include <circle/bcm2835.h>
#include <circle/memio.h>
#include <circle/memory.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/screen.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/sched/scheduler.h>
#include <circle/types.h>
#include <circle/gpioclock.h>
#include <circle/gpiopin.h>
#include <circle/gpiopinfiq.h>
#include <circle/gpiomanager.h>
#include <circle/util.h>

#include "lowlevel_arm64.h"
#include "gpio_defs.h"
#include "helpers.h"

CLogger	*logger;

class CRAD
{
public:
	CRAD( void )
		//: m_CPUThrottle(CPUSpeedLow),
		: m_CPUThrottle(CPUSpeedMaximum),
		m_Screen( m_Options.GetWidth(), m_Options.GetHeight() ),
		m_Timer( &m_Interrupt ),
		m_Logger( 5/*m_Options.GetLogLevel()*/, &m_Timer ),
		m_EMMC( &m_Interrupt, &m_Timer, 0 )
	{
	}

	~CRAD( void )
	{
	}

	boolean Initialize( void )
	{
		#ifndef COMPILE_MENU
		logger = &m_Logger;
		#endif
		STANDARD_SETUP_TIMER_INTERRUPT_CYCLECOUNTER_GPIO
		return bOK;
	}

	void Run( void );

private:
	static void FIQHandler( void *pParam );

	CMemorySystem		m_Memory;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CCPUThrottle		m_CPUThrottle;
	CScreenDevice		m_Screen;
	CInterruptSystem	m_Interrupt;
	CTimer				m_Timer;
	CLogger				m_Logger;
	CScheduler			m_Scheduler;
	CEMMCDevice			m_EMMC;
};

#endif

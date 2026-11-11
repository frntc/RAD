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
#include <circle/serial.h>
#include "tee_device.h"
#include <circle/actled.h>
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
#include "gpu64_multicore.h"
#include "gpu64_ladder.h"
#include "gpu64_3d.h"
#include "gpu64_fb.h"

CLogger	*logger;

class CRAD;
// gpu64: lets the cycle-precise bus-hijack loop (rad_reu.cpp, running inside
// CRAD::Run()) reach CRAD::showTestPattern() when the C64 writes the gpu64
// trigger register -- see IO2_ACCESS handling around GPU64_REG_CMD_HI in rad_reu.cpp.
// Only one CRAD is ever constructed (see main() in rad_main.cpp), so a
// single global set from the constructor is enough.
extern CRAD *g_pRAD;

class CRAD
{
public:
	// gpu64: m_CPUThrottle (CCPUThrottle) removed entirely -- its constructor
	// unconditionally does mailbox property-tag round trips
	// (GetClockRate/GetTemperature) and ends by calling SetSpeed() itself. On
	// this board (RPi 3 Model A+, this exact GPU firmware) that mailbox
	// exchange hangs forever, before ANY of our code -- including the
	// earliest possible serial log line -- can run. Proven by reproducing the
	// identical silent hang in an otherwise fully-working stock Circle sample
	// just by adding this same member+call (mailbox calls for the
	// framebuffer, by contrast, work fine on this board -- it's specific to
	// these particular property tags). CPU speed is left at whatever the GPU
	// firmware defaults to for now; revisit once cycle-accurate bus timing
	// needs to be verified for real.
	CRAD( void )
		: m_HDMIConsole( &m_Gpu64FB ),
		m_TeeLog( &m_Serial, &m_HDMIConsole ),
		m_Timer( &m_Interrupt ),
		m_Logger( 5/*m_Options.GetLogLevel()*/, &m_Timer ),
		m_EMMC( &m_Interrupt, &m_Timer, 0 ),
		// gpu64: multicore feasibility spike (docs/api_design.md,
		// "Architecture: this needs two cores") -- see gpu64_multicore.h.
		// Needs m_Memory, so must come after it both here and in the
		// member declaration order below (C++ constructs members in
		// declaration order, not initializer-list order).
		m_MultiCore( &m_Memory )
	{
		g_pRAD = this;
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
		// gpu64: multicore support -- per Circle's multicore.txt, must be
		// initialized last. This starts cores 1-3 running
		// CGpu64MultiCore::Run(); core 0 is untouched and proceeds into
		// CRAD::Run() -> reuUsingPolling() exactly as before.
		//
		// GPU64_MULTICORE_ENABLED: gated (2026-08-22) to isolate a
		// real-hardware regression -- VIC-II garbage appearing before the
		// RAD menu is even reachable, i.e. before reuUsingPolling() runs at
		// all, when the multicore stress spike was fully enabled.
		//
		// Design review (Opus 5) identified a confound in that first A/B:
		// CMultiCoreSupport::Initialize() itself -- independent of whatever
		// Run() does per core -- permanently enables CSpinLock's real
		// atomic path (previously a no-op, now affecting core 0's own
		// mailbox/GPIO/framebuffer calls too), rewrites interrupt routing,
		// and brings each secondary core up through EnableMMU()/EnableIRQs()
		// before Run() is ever reached. So the original test compared "none
		// of that happens" against "all of that *plus* the stress workload"
		// -- it never isolated whether bring-up alone is the cause. This
		// build calls Initialize() (so all of the above happens) while
		// CGpu64MultiCore::Run() is a no-op on every secondary core (see
		// gpu64_multicore.cpp) -- isolates bring-up-alone from
		// bring-up-plus-workload. See progress_tracker.md for the test
		// procedure and milestone6_3d_design.md for the full writeup.
		//
		// GPU64_3D_ENABLED brings the same machinery up for milestone 6
		// proper: core 1 runs the class 1 render loop (gpu64_3d.h), cores 2
		// and 3 park. The ring buffer has to exist before core 1 starts
		// draining it, hence the Init() ahead of Initialize().
		#ifdef GPU64_3D_ENABLED
		gpu64_3dInit();
		#endif
		#if defined( GPU64_MULTICORE_ENABLED ) || defined( GPU64_LADDER_ENABLED ) \
		 || defined( GPU64_3D_ENABLED )
		bOK = m_MultiCore.Initialize() && bOK;
		#endif
		return bOK;
	}

	void Run( void );

	// gpu64: draws directly into the gpu64 framebuffer,
	// independent of the C64 bus hijack -- milestone 2 "display basic
	// pattern" first cut. Public so the C64-triggered path (gpu64_showTestPattern()
	// in rad_main.cpp, called from the IO2 gpu64 write handler in
	// rad_reu.cpp) can reach it via g_pRAD, not just CRAD::Run() itself.
	void showTestPattern( void );

	// gpu64: milestone 3 default-state screen mirror -- renders a 40x25 text
	// snapshot (screen codes + 4-bit color nibbles + border/background) taken
	// by gpu64_mirrorSnapshot() (rad_reu.cpp) into the same reserved
	// GPU_OUTPUT_BOX showTestPattern() uses; the two are mutually exclusive
	// at any given moment (see gpu64ApiActive in rad_reu.cpp). screen/color
	// are each 1000 bytes (40*25), row-major.
	void showMirror( const u8 *screen, const u8 *color, u8 border, u8 background, u8 d018 );

	// gpu64: the same thing for the RAD menu, which never goes near the C64's
	// screen RAM -- it keeps its 40x25 in Pi RAM (c64ScreenRAM/c64ColorRAM in
	// rad_hijack.cpp) and pushes it to the C64 incrementally. So this takes
	// no bus access at all; it is called from the menu's own raster loop.
	// The one thing it does that showMirror() doesn't is pick the glyph base
	// per row, because the menu runs three charsets in one frame via $D018
	// raster splits (logo rows 0-3, lowercase 4-20, uppercase 21-24).
	void showMenuMirror( const u8 *screen, const u8 *color );

private:
	static void FIQHandler( void *pParam );

	CMemorySystem		m_Memory;
	CKernelOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	// gpu64: milestone 4 -- gpu64's own 320x200x8 framebuffer owns the HDMI
	// display now (CScreenDevice used to, at COLOR16 and full HDMI
	// resolution). Only one framebuffer can own it; see
	// dev/milestone4_2d_api_design.md#display-architecture for the choice
	// and what it costs. Declared before m_HDMIConsole, which points at it.
	CGpu64FrameBuffer	m_Gpu64FB;
	// gpu64: Tier 1 bring-up -- CRAD previously never instantiated a serial
	// device at all, so CLogger's default log target ("tty1", i.e. m_Screen)
	// meant every logger->Write() only ever reached the HDMI screen, never
	// GPIO14/15, regardless of anything wired to the UART pins. Default
	// (polling, no interrupt system) so it can be brought up before
	// m_Interrupt.Initialize() and survives even if interrupts never work.
	CSerialDevice		m_Serial;
	// gpu64: renders logger text directly via SetPixel(), bypassing
	// CScreenDevice's own text console -- required because that console
	// (like m_Serial) silently drops writes once IRQs are disabled, which is
	// true for essentially all of RAD's runtime after boot. See tee_device.h.
	CHDMIConsole		m_HDMIConsole;
	// gpu64: fans logger->Write() out to both m_Serial (Tier 1, unchanged)
	// and m_HDMIConsole (Tier 2 -- visible even once gpioInit() steals the
	// UART pins for cartridge use, and even with IRQs disabled).
	CTeeDevice			m_TeeLog;
	CInterruptSystem	m_Interrupt;
	CTimer				m_Timer;
	CLogger				m_Logger;
	CScheduler			m_Scheduler;
	CEMMCDevice			m_EMMC;
	// gpu64: must come after m_Memory in declaration order (see the
	// constructor's initializer-list comment above).
	CGpu64MultiCore		m_MultiCore;
	// (showTestPattern() declared public above; implementation in
	// rad_main.cpp. An early attempt at a *second* CBcmFrameBuffer at
	// 320x200 alongside CScreenDevice's produced nothing visible -- one
	// framebuffer owns the display, which is exactly why m_Gpu64FB replaces
	// CScreenDevice rather than joining it.)
};

#endif

/*

  {_______            {_          {______
        {__          {_ __               {__
        {__         {_  {__               {__
     {__           {__   {__               {__
 {______          {__     {__              {__
       {__       {__       {__            {__   
         {_________         {______________		Expansion Unit
                
 RADExp - A framework for DMA interfacing with Commodore C64/C128 computers using a Raspberry Pi Zero 2 or 3A+/3B+
        - this REU emulation reproduces the behavior of Vice's emulation (https://sourceforge.net/projects/vice-emu/)
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
#include <circle/startup.h>
#include <circle/memio.h>
#include <circle/memory.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/types.h>
#include <circle/util.h>
#include <circle/bcm2835.h>
#include <circle/gpioclock.h>
#include <circle/gpiopin.h>
#include <circle/gpiopinfiq.h>
#include <circle/gpiomanager.h>

#include "lowlevel_arm64.h"
#include "gpio_defs.h"
#include "helpers.h"

// REU command register $df01
#define REU_COMMAND_TRANSFER_BITMASK	0x03
#define REU_COMMAND_TRANSFER_TO_REU     0x00
#define REU_COMMAND_TRANSFER_TO_C64		0x01
#define REU_COMMAND_TRANSFER_SWAP       0x02
#define REU_COMMAND_TRANSFER_VERIFY     0x03
#define REU_COMMAND_UNUSED_BITS         0x4C
#define REU_COMMAND_FF00_DISABLED		0x10
#define REU_COMMAND_AUTOLOAD            0x20
#define REU_COMMAND_EXECUTE				0x80

// REU interrupt flags $df09
#define REU_INTERRUPT_UNUSED_BITMASK	0x1F
#define REU_INTERRUPT_VERIFY			0x20
#define REU_INTERRUPT_END_BLOCK			0x40
#define REU_INTERRUPTS_ENABLED			0x80

// REU address control register $df0a
#define REU_ADDR_UNUSED_BITS			0x3f
#define REU_ADDR_FIX_REU				0x40
#define REU_ADDR_FIX_C64				0x80

// REU status register $df00
#define REU_STATUS_CHIPVERSION_MASK		0x0F
#define REU_STATUS_256K_CHIPS			0x10
#define REU_STATUS_VERIFY_ERROR			0x20
#define REU_STATUS_END_OF_BLOCK			0x40
#define REU_STATUS_INTERRUPT_PENDING	0x80

// REU bank register $df06
#define REU_BANK_UNUSED_BITS			0xF8

#define BITS_ALL_SET( v, m )			(((v) & (m)) == (m))
#define BITS_ALL_CLR( v, m )			((((v) & (m)) == 0))

#define CACHE_PRELOAD_REU				CACHE_PRELOADL2KEEP
#define CACHE_PRELOAD_REUW				CACHE_PRELOADL2KEEPW

#define SPECIAL_NUVIE		0x01
#define SPECIAL_BLUREU		0x02

#pragma pack(push)
#pragma pack(1)
typedef struct
{
	u32 reuSize;

	u8  status;
	u8  command;
	u16 addrC64;
	u16 addrREU;
	u8  bank;
	u16 length;
	u8  IRQmask;
	u8  addrREUCtrl;
//	15
	u8  padding1[ 5 + 16 ];

	u16 shadow_addrC64;
	u16 shadow_addrREU;
	u16 shadow_length;
	u8  shadow_bank;
	u8  regBankUnused;

	u32 wrapAround;
	u32 wrapAroundDRAM;
	u32 wrapStoring;
//56
	u8  irqTriggered;
	u8  irqCustomTriggerActive;
	u8  reuWaitForFF00;

	u8  preset;
	u8  nextREUByte;

	u8  incrC64, 
		incrREU;
	u8  irqRelease;
	u8  releaseDMA;
	u8  isSpecial, isModified;
	u32 contiguousWrite, contiguousVerify;
	u32 contiguous1ByteWrites;
// 70
	u16 pl, pl2, pl3;
// 76
	// a copy of the timing values (to have all in one L1-block)
	u16 WAIT_FOR_SIGNALS,
		WAIT_CYCLE_MULTIPLEXER,
		WAIT_CYCLE_READ,
		WAIT_CYCLE_WRITEDATA,
		WAIT_CYCLE_READ2,
		WAIT_CYCLE_READ_VIC2,
		WAIT_CYCLE_WRITEDATA_VIC2,
		WAIT_CYCLE_MULTIPLEXER_VIC2,
		WAIT_TRIGGER_DMA,
		WAIT_RELEASE_DMA,
		TIMING_OFFSET_CBTD,
		TIMING_DATA_HOLD,
		TIMING_TRIGGER_DMA,
		TIMING_ENABLE_ADDRLATCH,
		TIMING_READ_BA_WRITING,
		TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING,
		TIMING_ENABLE_DATA_WRITING,
		TIMING_BA_SIGNAL_AVAIL, 
		CACHING_L1_WINDOW_KB,
		CACHING_L2_OFFSET_KB,
		CACHING_L2_PRELOADS_PER_CYCLE,
		TIMING_RW_BEFORE_ADDR,
		TIMING_ENABLE_RWOUT_ADDR_LATCH_WRITING_MINUS_RW_BEFORE_ADDR;
// 122		
	u8 padding[ 6 ];
} __attribute__((packed)) REUSTATE;
#pragma pack(pop)

extern u32 REU_SIZE_KB;
extern u8 *reuMemory;
extern bool reuRunning;

extern REUSTATE reu AAA;

extern void resetREU();
extern void initREU( void *mempool );
extern __attribute__((optimize("align-functions=256"))) void FIQHandlerREU( void *pParam );
extern u8 reuUsingPolling();


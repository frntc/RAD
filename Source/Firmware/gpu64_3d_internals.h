// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- class 1 internals: the core-0-to-core-1 ring buffer,
 the resource arena's bump allocator, and the store-burst yield.

 Private to the gpu64_3d_*.cpp files. The public interface is gpu64_3d.h.
*/
#ifndef _gpu64_3d_internals_h
#define _gpu64_3d_internals_h

#include "gpu64_3d.h"
#include <circle/types.h>

#ifdef GPU64_3D_ENABLED

// The store-burst budget and the yield that enforces it now live in
// gpu64_3d_span.h, so the portable renderer can obey the same rule while
// still compiling for tools/hostsim. See that file for the measurement.
#include "gpu64_3d_span.h"

// --- the command ring ---------------------------------------------------
//
// One-way, core 0 producer, core 1 consumer. 32 bytes an entry so the whole
// 16-byte ARG block travels inline: no second allocation, no pointer into
// storage core 0 would then have to keep alive, and two entries to a cache
// line.
//
// The ARG copy is what makes the ring safe against the C64 overwriting ARG
// for its next command while core 1 is still working on this one. Blob
// *payloads* are a phase 1 problem and go through the arena; the descriptor
// pointing at them fits here.
struct Gpu64_3dCmd
{
	u8	op;
	u8	flags;
	u16	id;			// the staged ID at the moment of dispatch
	u32	aux;			// phase 1: arena offset of a pulled blob
	u32	auxLen;

	// Stage 15a: the readback half of the slot. Core 1's
	// gpu64_3dExecuteRender() (gpu64_3d_class1.cpp) writes these for
	// CLEAR_VIEWPORT/DRAW_MESH/DRAW_NODE; gpu64_3dDispatch() reads them back
	// after its drain wait succeeds, under the same DMB pairing that already
	// protects head/tail (see gpu64_3dWorker(), gpu64_3d_core1.cpp). Carved
	// out of the field this replaces (a u32 pad, never used) so the struct
	// stays 32 bytes.
	u8	err;			// GPU64_ERR_*
	u8	result;			// RESULT byte

	// Stage 16 (Finding 2): which framebuffer page this command draws into.
	// The *input* counterpart of err/result -- core 0 stamps it at push time
	// (gpu64_3dRingPush(), before the barrier that publishes the slot) and
	// core 1 only ever reads it.
	//
	// It exists because core 1 must not call GetDrawPage() itself. The draw
	// page is core 0's state, and between an autonomous frame's push and its
	// execution core 0 can advance it (PrepareFlip(), from the next
	// SCENE_COMMIT) -- so a core-1 read could straddle a page hand-over and
	// come back with two different answers within one render, cleaning the
	// cache for rows on a page it never drew. Reading it once on core 0,
	// with the C64 halted for the dispatch, makes that structurally
	// impossible instead of merely unlikely.
	u8	page;

	u8	pad;			// still unused

	u8	arg[ 16 ];
};

#define GPU64_3D_RING_ENTRIES	256		// 8 KB of ring
#define GPU64_3D_RING_MASK	( GPU64_3D_RING_ENTRIES - 1 )

// gpu64: head and tail each alone on a cache line, and each written by
// exactly one core. This is the false-sharing rule from milestone 6a round
// 10 -- separating the lines was not enough there; what took the noise floor
// to zero was core 0 not *reading* a line core 1 writes. Core 0 still has to
// know where the tail is to detect a full ring, so it keeps a private cached
// copy (headCache/tailCache below) and only re-reads the real one when that
// copy says full. On a ring that is not full, core 0 never touches core 1's
// line at all.
//
// Dispatch runs with the C64 DMA-halted and has no deadline, so a coherence
// miss here is survivable in a way one in the polling loop is not -- but the
// cached-tail form costs nothing, so there is no reason to spend it.
struct Gpu64_3dRing
{
	// written by core 0, read by core 1
	volatile u32	head;
	u32		pad0[ 15 ];

	// written by core 1, read by core 0 (rarely -- see above)
	volatile u32	tail;
	u32		pad1[ 15 ];

	// core-0-private: last tail value core 0 bothered to read.
	u32		tailCache;
	u32		pad2[ 15 ];

	Gpu64_3dCmd	slot[ GPU64_3D_RING_ENTRIES ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64_3dRing gpu64_3dRing;

// Stage 16: a synthetic op, never sent over the wire (every real wire opcode
// is <= 0x42 -- see gpu64_3d.h's table). gpu64_3dDispatch() pushes this onto
// the ring itself, from LOOP_START's and SCENE_COMMIT's own handlers, to ask
// core 1 for one autonomous frame; the C64 never spells it. High bit set so
// a stray match against a real opcode is structurally impossible rather than
// merely unlikely.
#define GPU64_3D_OP_RENDER_SCENE	0x80

// --- phase 0 counters ---------------------------------------------------
//
// The only output phase 0 has. Core 1 writes these and core 0 reads them
// once, from the log path, which is not the hot loop -- so the read is
// allowed here in a way it would not be inside reuUsingPolling().
struct Gpu64_3dWorkerStats
{
	volatile u32	consumed;	// commands drained off the ring
	volatile u32	lastOp;		// most recent opcode core 1 saw
	// Stage 15a: every class 1 opcode besides the three render ones -- core 1
	// still has no handler for them because core 0 still executes them
	// synchronously itself. Expected to climb continuously; not a fault
	// counter (see gpu64_3d_core1.cpp's execute()).
	volatile u32	unknownOp;
	volatile u32	wakeups;	// times the drain loop found work
	u32		pad[ 12 ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64_3dWorkerStats gpu64_3dWorkerStats;

// Core-0-side counters, core-0-private, so they share no line with the above.
struct Gpu64_3dHostStats
{
	u32	pushed;			// commands accepted onto the ring
	u32	rejected;		// QUEUE_FULL
	u32	badOpcode;
	u32	pad[ 13 ];
} __attribute__(( aligned( 64 ) ));

extern Gpu64_3dHostStats gpu64_3dHost;

// Core 0 side, implemented in gpu64_3d_core1.cpp next to the worker it pairs
// with. FALSE means the ring is full, which is GPU64_ERR_QUEUE_FULL.
// nPage is the framebuffer page the command is to draw into -- see
// Gpu64_3dCmd::page above; ignored by the opcodes that draw nothing, which
// pass whatever the draw page happens to be rather than a special value.
boolean gpu64_3dRingPush( u8 op, u8 nPage );

// Core 1 side (Stage 15a): the real CLEAR_VIEWPORT / DRAW_MESH / DRAW_NODE
// execution, called from gpu64_3d_core1.cpp's execute() for those three
// opcodes only. Implemented in gpu64_3d_class1.cpp, next to the session state
// and resource table it needs and cannot export -- this declaration is the
// only crossing. pCmd is the ring slot itself (gpu64_3dWorker() passes
// &gpu64_3dRing.slot[tail]); the function writes pCmd->err and, for
// DRAW_MESH/DRAW_NODE, pCmd->result, and never touches gpu64Regs -- core 0
// stays the sole writer of the C64-visible register file, copying pCmd's
// fields across itself once its drain wait succeeds.
void gpu64_3dExecuteRender( Gpu64_3dCmd *pCmd );

// Core 1 side (Stage 16): renders one whole autonomous-loop frame -- clear,
// every visible OBJECT node under the active camera, in scene order -- for
// GPU64_3D_OP_RENDER_SCENE only. Same contract as gpu64_3dExecuteRender():
// writes pCmd->err and pCmd->result (the page rendered into), never touches
// gpu64Regs. Implemented in gpu64_3d_class1.cpp next to gpu64_3dExecuteRender().
void gpu64_3dExecuteRenderScene( Gpu64_3dCmd *pCmd );

// --- the resource arena -------------------------------------------------
//
// A bump allocator over one static block, per dev/milestone6_3d_design.md's
// Resource lifecycle: explicit free only, no automatic eviction, and the
// whole thing is reclaimed on resetREU(). Phase 0 only brings it up; phase 1
// is what actually allocates out of it.
//
// 32 MB against a C64/REU-side source ceiling of 16 MB, so a program cannot
// fill it from the C64 side in one session even by uploading every byte it
// can address. Static rather than heap: the same reason gpu64_api.cpp stages
// blobs statically -- keeping the memory allocator out of a path core 1
// runs.
#define GPU64_3D_ARENA_BYTES	( 32 * 1024 * 1024 )

// Owned by core 0, and that is a deliberate change from the design doc's
// first sketch, which had core 1 allocating. An upload is a DMA pull off the
// C64 bus, which only core 0 can do; making core 1 allocate would mean core 0
// pulling into a staging buffer and core 1 copying it out again, with a
// hand-off protocol to stop core 0 reusing the staging buffer too early. Core
// 0 allocating and pulling straight into the arena removes the copy and the
// protocol together. There is still no lock, and still for the original
// reason: nothing core 0 could block on.
void *gpu64_3dArenaAlloc( u32 nBytes );
void  gpu64_3dArenaReset( void );
u32   gpu64_3dArenaUsed( void );

#endif	// GPU64_3D_ENABLED

#endif

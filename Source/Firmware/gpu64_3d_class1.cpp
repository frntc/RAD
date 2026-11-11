// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- class 1: session state, the resource table, and the
 opcode dispatcher. Through Stage 14 every class 1 opcode, render ops
 included, ran synchronously on core 0. Stage 15a (dev/gap_filling_plan.md)
 moves the three render ops -- CLEAR_VIEWPORT, DRAW_MESH, DRAW_NODE -- onto
 core 1, via the ring gpu64_3d_core1.cpp owns; everything else here is
 unchanged and still runs on core 0.

 DRAW_MESH is specified to return when the mesh is drawn and to be illegal
 while the render loop runs -- so there is exactly one owner of the
 framebuffer and the z-buffer at any instant. Stage 15a keeps that true by
 having gpu64_3dDispatch() stall on the ring fully draining before it returns
 for a render op: a zero-concurrency window, just relocated from "core 0,
 always" to "core 1, while core 0 waits" -- see gpu64_3dDispatch() below and
 gpu64-multicore-rule-scoped-to-polling-loop (memory) for why that stall is
 legal from core 0 despite the "never poll MMIO from another core" rule.

 gpu64_3dExecuteRender(), below, is the core-1 half of the three render ops --
 called from gpu64_3d_core1.cpp's execute(), it lives here because it needs
 the session state and resource table this file owns and does not export.
 Every other class 1 opcode is still pushed to the ring purely for its own
 counters (gpu64_3d_core1.cpp's unknownOp), then executed synchronously here
 on core 0, exactly as before Stage 15a.

 Nothing here is portable; the pipeline it drives (gpu64_3d_render.h) is, and
 is exercised on a PC by tools/hostsim.
*/
#include "gpu64_3d_internals.h"
#include "gpu64_3d_render.h"
#include "gpu64_3d_scene.h"
#include "gpu64_3d_span.h"
#include "gpu64_api.h"
#include "gpu64_fb.h"
#include "gpu64_vsync.h"
#include "gpu64_apidiag.h"
#include "gpu64_level.h"
#include "lowlevel_arm64.h"
#include <circle/util.h>

#ifdef GPU64_3D_ENABLED

// --- the arena ----------------------------------------------------------

static u8  s_Arena[ GPU64_3D_ARENA_BYTES ] __attribute__(( aligned( 64 ) ));
static u32 s_ArenaUsed;

void *gpu64_3dArenaAlloc( u32 nBytes )
{
	// Every allocation starts on a cache line. Wasteful at the byte level and
	// worth it: a texture whose rows share a line with the end of a mesh is a
	// false-sharing bug waiting for the frame core 1 and core 0 happen to
	// touch both in.
	nBytes = ( nBytes + 63 ) & ~63u;

	if ( nBytes > GPU64_3D_ARENA_BYTES - s_ArenaUsed )
		return 0;					// caller answers OUT_OF_MEMORY

	void *p = s_Arena + s_ArenaUsed;
	s_ArenaUsed += nBytes;
	return p;
}

void gpu64_3dArenaReset( void )
{
	// Bump allocator: reset is the whole free path. Nothing is zeroed --
	// every allocation is written before it is read, and zeroing 32 MB here
	// would be the longest unbroken store burst in the firmware.
	s_ArenaUsed = 0;
}

u32 gpu64_3dArenaUsed( void )
{
	return s_ArenaUsed;
}

static void *arenaAllocFn( void *, u32 nBytes )
{
	return gpu64_3dArenaAlloc( nBytes );
}

// --- session state ------------------------------------------------------

static Gpu64_3dState	s_State;
static Gpu64_3dResource	s_Res[ GPU64_3D_MAX_RESOURCES ];
static Gpu64_3dScratch	s_Scratch;

// The scene graph -- stage 14. Nodes reference meshes by ID; resolving that
// ID to a Gpu64_3dResource stays here rather than in gpu64_3d_scene.cpp, for
// the reason given at the top of gpu64_3d_scene.h.
static Gpu64_3dScene	s_Scene;

// --- Stage 16: the autonomous loop and its shadow copy -------------------
//
// s_Scene/s_State are what GPU64_3D_OP_RENDER_SCENE reads on core 1 -- see
// gpu64_3dExecuteRenderScene() below. Once LOOP_START has queued a frame,
// mutating either directly from a C64 command would race that read exactly
// the way Stage 15b's drain-before-mutate exists to prevent for immediate
// mode. The design doc's answer is a shadow copy: while the loop runs,
// isShadowRedirectable() opcodes write s_ShadowScene/s_ShadowState instead,
// and SCENE_COMMIT is the only thing that ever copies shadow -> live, which
// it does only after gpu64_3dDispatch()'s own unconditional drain (SCENE_
// COMMIT is not itself shadow-redirectable) has already proven the in-flight
// render finished. No new synchronisation primitive: this reuses drain-
// before-mutate for the one write that still needs it, and removes the
// drain for everything else, which is the whole point -- SET_POSITION during
// the loop must not stall behind a frame it can no longer race.
static Gpu64_3dScene	s_ShadowScene;
static Gpu64_3dState	s_ShadowState;

static boolean s_LoopRunning;
static boolean s_CommitSeen;	// a SCENE_COMMIT was accepted since boot
static u32     s_LastCommitUs;	// gpu64_vsyncNow() of the last one

// gpu64 (2026-09-10): the loop stopping is one of the four things that makes
// SCENE_COMMIT answer UNSUPPORTED for the rest of a run, and it is the only
// one a command stream can trigger by accident. Every clear goes through
// here so the cause is recorded with it -- gpu64_apidiag.h.
static void loopStop( u8 cause )
{
	if ( s_LoopRunning )
		gpu64_apiDiagBump( &gpu64ApiDiag.loopStops );
	gpu64ApiDiag.loopStopCause = cause;

	// gpu64 (2026-09-10, bench run 11): freeze the command stream around
	// this stop. Bench run 11 proved a LOOP_STOP opcode executes in runs
	// where the C64 never sends one; (loopStopSeq - loopStopSeqPrev) says
	// whether a command arrived at all -- gpu64_apidiag.h. Latched for
	// every cause, not just the opcode, because a SESSION or SCENE_RESET
	// stop wants the same context if it ever turns up unexplained.
	gpu64ApiDiag.loopStopSeq     = gpu64ApiDiag.dispSeq;
	gpu64ApiDiag.loopStopSeqPrev = gpu64ApiDiag.dispSeqPrev;
	gpu64ApiDiag.loopStopPrevOp  = gpu64ApiDiag.dispOpPrev;

	s_LoopRunning = FALSE;
}

// Bookkeeping for the one frame the autonomous loop may have in flight --
// same shape as s_LastRenderSlot/s_LastRenderPending below, deliberately
// separate from them: those track a C64-visible DRAW_MESH/DRAW_NODE RESULT,
// this tracks STATUS bit4 (GPU64_STATUS_FRAME_READY) and the page number
// that goes with it, which is a different C64-visible contract. s_LoopFrame-
// Target is the ring's *head* value read right after RENDER_SCENE was
// pushed -- i.e. the tail value that means "this slot has been consumed".
// tail is a *masked* ring index, not a monotonic counter, and an unrelated
// ring push can drain past s_LoopFrameTarget without ever being observed
// sitting exactly on it -- so pollLoopFrame() below tests "has tail reached
// or passed target" with modular distance rather than equality. (Contrast
// waitForDrain(), which may test equality: there core 1 stops exactly at the
// head core 0 just published and cannot overshoot it.)
static u32     s_LoopFrameSlot;	// ring slot pushLoopFrame() put RENDER_SCENE in
static u32     s_LoopFrameTarget;
static boolean s_LoopFramePending;

// The z-buffer, sized for the largest viewport SET_VIEWPORT will accept.
// Static rather than out of the arena so a program cannot exhaust resource
// RAM and then find it cannot draw.
static u16 s_Depth[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ] __attribute__(( aligned( 64 ) ));

// Blob staging. An upload is pulled off the bus into one of these and then
// parsed into the arena -- the parse is what turns the wire format into the
// form the rasteriser wants (unpacked faces, precomputed normals), so a
// staging step exists whether or not it is spelled out.
static u8 s_StageA[ 65536 ];
static u8 s_StageB[ 65536 ];

#define sArg	gpu64Regs.arg

static inline u16 argU16( unsigned i )
{
	return (u16)( sArg[ i ] | ( sArg[ i + 1 ] << 8 ) );
}

static inline s16 argS16( unsigned i )
{
	return (s16)argU16( i );
}

static inline s32 argS32( unsigned i )
{
	return (s32)( (u32)sArg[ i ] | ( (u32)sArg[ i + 1 ] << 8 ) |
		       ( (u32)sArg[ i + 2 ] << 16 ) | ( (u32)sArg[ i + 3 ] << 24 ) );
}

static inline void argBlob( unsigned i, u8 *pSpace, u32 *pAddr, u32 *pLen )
{
	*pSpace = sArg[ i ];
	*pAddr  = (u32)sArg[ i + 1 ] | ( (u32)sArg[ i + 2 ] << 8 ) | ( (u32)sArg[ i + 3 ] << 16 );
	*pLen   = (u32)sArg[ i + 4 ] | ( (u32)sArg[ i + 5 ] << 8 );
}

static inline u16 stagedId( void )
{
	return (u16)( gpu64Regs.id[ 0 ] | ( gpu64Regs.id[ 1 ] << 8 ) );
}

// --- the resource table -------------------------------------------------

static Gpu64_3dResource *resFind( u16 nId, u8 nType )
{
	for ( unsigned i = 0; i < GPU64_3D_MAX_RESOURCES; i++ )
		if ( s_Res[ i ].type != GPU64_3D_RES_NONE && s_Res[ i ].id == nId )
			return ( nType == GPU64_3D_RES_NONE || s_Res[ i ].type == nType )
				? &s_Res[ i ] : 0;
	return 0;
}

// Finds the slot for an ID, reusing it if the ID is already live. Re-upload
// to a live ID replaces it, per the design's Resource lifecycle.
//
// The bytes of the old allocation are NOT reclaimed: the arena is a bump
// allocator, so a replaced resource leaks until the session resets. That is a
// deliberate phase-1 limit, not an oversight -- it keeps the allocator out of
// a path core 1 will one day share, and 32 MB absorbs a great many reloads.
// Anything that re-uploads in a loop will exhaust it and get OUT_OF_MEMORY,
// which is at least a truthful error.
static Gpu64_3dResource *resSlot( u16 nId )
{
	Gpu64_3dResource *pFree = 0;

	for ( unsigned i = 0; i < GPU64_3D_MAX_RESOURCES; i++ )
	{
		if ( s_Res[ i ].type != GPU64_3D_RES_NONE && s_Res[ i ].id == nId )
			return &s_Res[ i ];
		if ( s_Res[ i ].type == GPU64_3D_RES_NONE && !pFree )
			pFree = &s_Res[ i ];
	}

	return pFree;
}

// Stage 17: the mesh half of the same idea, for gpu64_3dSceneRender(). The
// portable scene render must not know about s_Res[], so it asks through this.
static const Gpu64_3dMesh *lookupMesh( void *, u16 nId )
{
	const Gpu64_3dResource *pR = resFind( nId, GPU64_3D_RES_MESH );
	return pR ? &pR->mesh : 0;
}

static const Gpu64_3dTexture *lookupTexture( void *, u16 nId )
{
	const Gpu64_3dResource *pR = resFind( nId, GPU64_3D_RES_TEXTURE );
	// A face naming a texture that was never uploaded falls back to flat
	// colour rather than failing the draw: one missing texture should not
	// blank a scene, and the wrong-looking face is a better diagnostic than
	// an error code the C64 sees after the fact.
	return pR ? &pR->tex : 0;
}

// --- Stage 16: shadow redirection ----------------------------------------
//
// Every op*() below that mutates the scene graph or the per-frame render
// state goes through one of these two instead of touching s_Scene/s_State
// directly, so the same handler body works whether or not the loop is
// running -- see the comment above s_ShadowScene/s_ShadowState.
static inline Gpu64_3dScene *sceneTarget( void )
{
	return s_LoopRunning ? &s_ShadowScene : &s_Scene;
}

static inline Gpu64_3dState *stateTargetForEdit( void )
{
	return s_LoopRunning ? &s_ShadowState : &s_State;
}

// Whitelist of opcodes gpu64_3dDispatch() may skip the pre-execute drain for
// while the loop runs -- exactly those whose handler writes only through
// sceneTarget()/stateTargetForEdit() above, never s_Scene/s_State directly,
// and never the resource table (resource ops keep draining unconditionally:
// they are not part of the design doc's per-frame dynamic state, and
// gpu64_3dExecuteRenderScene() reads resources by ID through the same
// s_Res[] this file already serialises against the C64 the ordinary way).
// SCENE_COMMIT and SCENE_RESET are deliberately absent -- both need the
// drain themselves, for different reasons covered at their own call sites.
static boolean isShadowRedirectable( u8 op )
{
	switch ( op )
	{
	case GPU64_3D_OP_SET_VIEWPORT:
	case GPU64_3D_OP_SET_PERSPECTIVE:
	case GPU64_3D_OP_SET_LIGHT:
	case GPU64_3D_OP_BUILD_COLORMAP:
	case GPU64_3D_OP_SET_BACKGROUND:
	case GPU64_3D_OP_CREATE_OBJECT:
	case GPU64_3D_OP_CREATE_CAMERA:
	case GPU64_3D_OP_DESTROY_NODE:
	case GPU64_3D_OP_SET_ACTIVE_CAMERA:
	case GPU64_3D_OP_SET_VISIBLE:
	case GPU64_3D_OP_CREATE_SPRITE:
	case GPU64_3D_OP_CREATE_LIGHT:
	case GPU64_3D_OP_SET_SPRITE:
	case GPU64_3D_OP_SET_POINT_LIGHT:
	case GPU64_3D_OP_SET_POSITION:
	case GPU64_3D_OP_SET_ORIENTATION:
	case GPU64_3D_OP_MOVE_LOCAL:
	case GPU64_3D_OP_MOVE_WORLD:
	case GPU64_3D_OP_ROTATE_LOCAL:
	case GPU64_3D_OP_SET_SCALE:
	case GPU64_3D_OP_GET_TRANSFORM:
	case GPU64_3D_OP_LEVEL_PALETTE:		// colormap via stateTargetForEdit()
	case GPU64_3D_OP_LEVEL_NODE:		// scene via sceneTarget()
	case GPU64_3D_OP_WORLD_TICK:		// scene via sceneTarget(); the rest
						// is CLIP_MOVE's level-file reads
		return TRUE;
	default:
		return FALSE;
	}
}

// The drain gpu64_3dDispatch() skips is the same one either way, but the
// reason differs, so it is a second predicate rather than another arm of the
// list above. An isShadowRedirectable() opcode is exempt because its writes go
// somewhere core 1 is not reading; CLIP_MOVE and LEVEL_ENT are exempt because
// they do not write gpu64 state at all -- they read the level file, which is
// immutable once loaded, and write the caller's own C64 memory. Making a
// movement query wait for a frame to finish would put a whole render on the
// critical path of every step the player takes, which is the opposite of what
// the query is for.
static boolean isDrainExempt( u8 op )
{
	return isShadowRedirectable( op ) ||
	       op == GPU64_3D_OP_CLIP_MOVE ||
	       op == GPU64_3D_OP_LEVEL_ENT;
}

// --- lifecycle ----------------------------------------------------------

// Forward declaration: gpu64_3dReset(), directly below, needs Stage 15b's
// drain-before-mutate defined much further down (it comes after every
// opXxx() this file dispatches to, by design -- see the comment above its
// definition). Everything else in this file still relies on definition
// order rather than a declarations block; this is the one call that reaches
// backward across it. pollLoopFrame() is Stage 16's non-blocking counterpart
// -- same forward-reference reason.
static boolean drainAndFlush( void );
// Milestone 18: disarms a level load in progress. Forward-declared for the
// same reason as drainAndFlush() above -- gpu64_3dReset() and SCENE_RESET both
// have to abandon a half-built level, and the loader itself is defined further
// down with the resource table it uses.
static void levelLoadAbort( void );
// always_inline so gpu64_3dPollLoopFrame() below is self-contained: the
// vsync commit warms exactly one address range for it (gpu64_vsyncWarmCommit()
// in rad_reu.cpp), and a call out to a separate body 2.4 KB away would sit
// outside that window and take the cold miss inside the hold.
static void    pollLoopFrame( void ) __attribute__( ( always_inline ) );
static boolean pushLoopFrame( void );

void gpu64_3dInit( void )
{
	memset( &gpu64_3dRing, 0, sizeof( gpu64_3dRing ) );
	memset( (void *)&gpu64_3dWorkerStats, 0, sizeof( gpu64_3dWorkerStats ) );
	memset( &gpu64_3dHost, 0, sizeof( gpu64_3dHost ) );
	memset( s_Res, 0, sizeof( s_Res ) );
	gpu64_3dArenaReset();
	gpu64_3dStateDefaults( &s_State );
	gpu64_3dSceneReset( &s_Scene );
	gpu64_3dStateDefaults( &s_ShadowState );
	gpu64_3dSceneReset( &s_ShadowScene );
	loopStop( GPU64_LOOPSTOP_INIT );
	s_LoopFramePending = FALSE;
}

boolean gpu64_3dLoopRunning( void )
{
	return s_LoopRunning;
}

#define GPU64_3D_COMMITS_LIVE_US	500000

boolean gpu64_3dCommitsLive( void )
{
	return s_LoopRunning && s_CommitSeen &&
	       gpu64_vsyncNow() - s_LastCommitUs < GPU64_3D_COMMITS_LIVE_US;
}

void gpu64_3dLoopFullReset( void )
{
	loopStop( GPU64_LOOPSTOP_FULL_RESET );
	s_LoopFramePending = FALSE;
}

void gpu64_3dReset( void )
{
	// Stage 15b made this drain load-bearing rather than cosmetic: before
	// 15b every dispatch already drained the ring, so by the time a RESTORE
	// could reach here core 1 was always parked and idle. Now a render can
	// still be queued, or actually mid-execute on core 1, when RESTORE is
	// pressed -- and gpu64_3dExecuteRender() reads and writes exactly the
	// s_Res/s_State/s_Scene/s_Scratch/s_Depth structures this function is
	// about to reset. Draining first makes that impossible instead of
	// merely unlikely. The C64 being halted for this call still rules out a
	// *new* command arriving mid-reset -- that half of the old comment's
	// reasoning stands -- it just no longer covers one core 1 was already
	// working on when the halt began.
	//
	// Best-effort: if core 1 is genuinely wedged (the GPU64_3D_DRAIN_TIMEOUT_US
	// backstop), a RUN/STOP+RESTORE still has to recover the session rather
	// than leave the whole subsystem dead, so the reset proceeds regardless
	// of drainAndFlush()'s result -- there is no worse outcome available
	// from a void function with no caller to report a timeout to.
	drainAndFlush();

	// Only core 0 gets to say a new session's ring starts empty. Redundant
	// with the drain above whenever it succeeded (tail already == head by
	// then), kept as the statement of intent for the timeout case, where it
	// is the only thing that still moves the ring forward.
	gpu64_3dRing.tail = gpu64_3dRing.head;
	gpu64_3dRing.tailCache = gpu64_3dRing.head;

	// Every resource of the session goes with it -- design doc, Resource
	// lifecycle. Without this a RUN/STOP+RESTORE leaks the whole arena and
	// the next program starts against stale IDs.
	memset( s_Res, 0, sizeof( s_Res ) );
	gpu64_3dArenaReset();
	gpu64_3dStateDefaults( &s_State );

	// Every node of the session goes with it too, same rule and same
	// reason as the resource table above: RUN/STOP+RESTORE must not leave
	// the next program looking at a dead program's scene.
	gpu64_3dSceneReset( &s_Scene );

	// Stage 16: a RUN/STOP+RESTORE must not leave the autonomous loop
	// running against a session that no longer exists -- the drain above
	// already proved core 1 is idle (or the timeout backstop already
	// decided to proceed regardless), so it is safe to declare the loop
	// stopped and its shadow copy gone here.
	loopStop( GPU64_LOOPSTOP_SESSION );
	s_LoopFramePending = FALSE;
	gpu64Regs.status &= ~GPU64_STATUS_FRAME_READY;
	gpu64_3dStateDefaults( &s_ShadowState );
	gpu64_3dSceneReset( &s_ShadowScene );

	// A level load in flight must not survive the session that armed it: its
	// parsed tables are still valid (the file buffer is untouched) but the
	// arena it was building into has just been reset under it.
	levelLoadAbort();

	memset( &gpu64_3dHost, 0, sizeof( gpu64_3dHost ) );
}

// --- the draw target ----------------------------------------------------

// The draw page as core 0 sees it right now. Every ring push goes through
// this: it is the one place the draw page is read for a command, so a queued
// render's target is fixed at push time and cannot move under it afterwards.
static u8 currentDrawPage( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	return ( pFB != 0 && pFB->IsInitialized() ) ? pFB->GetDrawPage() : 0;
}

// nPage comes from the ring slot core 0 stamped at push time (Gpu64_3dCmd::page),
// never from GetDrawPage(): this runs on core 1, and the draw page is core 0's
// to advance. See that field's comment for what reading it here used to cost.
static boolean makeTarget( Gpu64_3dTarget *pTarget, u8 nPage )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 || !pFB->IsInitialized() )
		return FALSE;

	pTarget->pPixels = pFB->PageBuffer( nPage );
	pTarget->pitch   = pFB->GetPitch();
	pTarget->pDepth  = s_Depth;
	return pTarget->pPixels != 0;
}

// The VideoCore scans DRAM directly and is not coherent with the ARM, so
// nothing drawn is visible until the rows are cleaned -- same rule every
// class 0 draw op follows. Only the viewport is cleaned: the rest of the page
// belongs to the C64's HUD and was cleaned when the C64 drew it.
//
// Stage 15a: this now runs on core 1 (gpu64_3dExecuteRender(), below), not
// inside a dispatch that holds the C64's bus -- so the CLAUDE.md multicore
// burst cap applies to it for the first time. Before 15a a viewport's worth
// of rows was safe as one unchunked CleanDataCacheRange because core 0 owned
// the whole draw and the C64 was DMA-halted throughout; now it is core 1's
// own store burst, subject to the same 256-byte/7-line yield discipline as
// the rasteriser (gpu64_3d_span.h). One CleanRows() + one GPU64_3D_YIELD()
// per row, same as that header's own comment: "one yield per scanline...
// falls out of the geometry."
//
// Stage 16 (Finding 2): the page is the caller's, taken from the same ring
// slot makeTarget() drew through, so the rows cleaned are always the rows just
// written. This used to re-read GetDrawPage() -- a second, independent read
// that could disagree with the first one across a page hand-over, cleaning the
// new page's rows while the pixels sat unflushed on the old one. That is
// tearing that sticks: the display keeps scanning stale DRAM for a page whose
// cache lines were never written back.
static void cleanViewport( u8 nPage )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	// 2026-09-27: the budget is a runtime value and may now be smaller than
	// a row, so a row is cleaned in budget-sized pieces of the viewport's own
	// columns. At the default 256 that is two pieces of a 320-wide viewport.
	const unsigned nSpan = gpu64_3dSpanBytes;
	const unsigned x1    = s_State.vpX + s_State.vpW;

	for ( u16 y = s_State.vpY; y < s_State.vpY + s_State.vpH; y++ )
	{
		for ( unsigned x = s_State.vpX; x < x1; x += nSpan )
		{
			const unsigned xe = ( x1 - x > nSpan ) ? x + nSpan : x1;
			pFB->CleanRowSpan( nPage, y, x, xe );
			GPU64_3D_YIELD();
		}
	}
}

// --- opcodes ------------------------------------------------------------

static u8 opSetViewport( void )
{
	const u16 x = argU16( 0 ), y = argU16( 2 ), w = argU16( 4 ), h = argU16( 6 );

	if ( w == 0 || h == 0 )
		return GPU64_ERR_BAD_ARGS;
	if ( (u32)x + w > GPU64_3D_SURFACE_W || (u32)y + h > GPU64_3D_SURFACE_H )
		return GPU64_ERR_BAD_ARGS;
	if ( (u32)w * h * 3 > GPU64_3D_BUDGET )
		return GPU64_ERR_OUT_OF_RANGE;

	Gpu64_3dState *pState = stateTargetForEdit();

	pState->vpX = x;
	pState->vpY = y;
	pState->vpW = w;
	pState->vpH = h;

	// The focal length is derived from the fov and the viewport width, so a
	// viewport change re-derives it. Not doing so is a subtle one: the scene
	// keeps rendering and quietly has the wrong field of view.
	pState->focal = gpu64_3dFocalFromFov( pState->fov, w );
	return GPU64_ERR_OK;
}

static u8 opSetPerspective( void )
{
	const u16 fov  = argU16( 0 );
	const s32 near = (s32)(s16)argU16( 2 ) << 8;		// 8.8 -> 16.16
	const s32 far  = (s32)(s16)argU16( 4 ) << 8;

	if ( near <= 0 || far <= near )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_3dState *pState = stateTargetForEdit();

	const s32 focal = gpu64_3dFocalFromFov( fov, pState->vpW );
	if ( focal <= 0 )
		return GPU64_ERR_BAD_ARGS;

	pState->fov   = fov;
	pState->focal = focal;
	pState->nearZ = near;
	pState->farZ  = far;
	return GPU64_ERR_OK;
}

static u8 opSetLight( void )
{
	if ( sArg[ 6 ] > 15 )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_3dState *pState = stateTargetForEdit();

	if ( !gpu64_3dNormalise( pState->lightDir, argS16( 0 ), argS16( 2 ), argS16( 4 ) ) )
		return GPU64_ERR_BAD_ARGS;		// a zero-length direction

	pState->ambient = sArg[ 6 ];
	return GPU64_ERR_OK;
}

static u8 opBuildColormap( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_3dBuildColormap( stateTargetForEdit(), pFB->GetPaletteRGB() );
	return GPU64_ERR_OK;
}

static u8 opUploadMesh( void )
{
	u8 spaceV, spaceF;
	u32 addrV, lenV, addrF, lenF;

	argBlob( 0, &spaceV, &addrV, &lenV );
	argBlob( 6, &spaceF, &addrF, &lenF );

	if ( lenV == 0 || lenF == 0 || lenV > sizeof( s_StageA ) || lenF > sizeof( s_StageB ) )
		return GPU64_ERR_BAD_ARGS;

	u8 res = gpu64_blobRead( spaceV, addrV, lenV, s_StageA );
	if ( res != GPU64_ERR_OK ) return res;

	res = gpu64_blobRead( spaceF, addrF, lenF, s_StageB );
	if ( res != GPU64_ERR_OK ) return res;

	Gpu64_3dResource *pR = resSlot( stagedId() );
	if ( pR == 0 )
		return GPU64_ERR_OUT_OF_MEMORY;		// the table, not the arena

	Gpu64_3dMesh mesh;
	res = gpu64_3dBuildMesh( &mesh, s_StageA, lenV, s_StageB, lenF, arenaAllocFn, 0 );
	if ( res != GPU64_3D_OK )
		return res;				// the slot is untouched on failure

	pR->id   = stagedId();
	pR->type = GPU64_3D_RES_MESH;
	pR->mesh = mesh;

	gpu64Regs.result = (u8)mesh.nFaces;
	return GPU64_ERR_OK;
}

static u8 opUploadTexture( void )
{
	u8 space;
	u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	if ( len == 0 || len > sizeof( s_StageA ) )
		return GPU64_ERR_BAD_ARGS;

	u8 res = gpu64_blobRead( space, addr, len, s_StageA );
	if ( res != GPU64_ERR_OK ) return res;

	Gpu64_3dResource *pR = resSlot( stagedId() );
	if ( pR == 0 )
		return GPU64_ERR_OUT_OF_MEMORY;

	Gpu64_3dTexture tex;
	res = gpu64_3dBuildTexture( &tex, s_StageA, len, sArg[ 6 ], sArg[ 7 ], arenaAllocFn, 0 );
	if ( res != GPU64_3D_OK )
		return res;

	pR->id   = stagedId();
	pR->type = GPU64_3D_RES_TEXTURE;
	pR->tex  = tex;
	return GPU64_ERR_OK;
}

static u8 opFreeResource( void )
{
	Gpu64_3dResource *pR = resFind( stagedId(), GPU64_3D_RES_NONE );
	if ( pR == 0 )
		return GPU64_ERR_BAD_ID;

	// Table slot only -- see resSlot() on why the bytes stay allocated.
	pR->type = GPU64_3D_RES_NONE;
	return GPU64_ERR_OK;
}

// --- milestone 18: the level loader -------------------------------------
//
// LOAD_LEVEL/LEVEL_STEP turn a .g64lev from the REU image's level pack
// (gpu64_level.cpp) into textures, meshes and object nodes. It
// lives here rather than in gpu64_level.cpp because this is the file that
// owns the resource table, the arena allocator and the scene -- the parser
// deliberately knows about none of them.
//
// Why it is sliced. Building E1M1 is 66 textures, 108 meshes and 108 nodes;
// done in one command the C64 would be DMA-halted for the whole of it, and if
// anything in the middle failed the only reading would be one ERRCODE with no
// way to say which of 282 items produced it. One bounded slice per command
// gives both a loading bar and a per-item failure address, and keeps each
// hold in the same range as the 64 KB REU transfers this cartridge already
// does routinely.
//
// Why no SD access here. reuUsingPolling() is running: an EMMC transfer's
// MMIO traffic would wreck core 0's per-C64-cycle bus timing the same way a
// second core spinning on a register does (CLAUDE.md, "Multicore"). The file
// is therefore already in RAM before the polling loop ever starts, and this
// code only ever reads memory.
//
// Why not on core 1. The arena allocator is core-0-owned and unlocked, so a
// core-1 build would race any concurrent UPLOAD_MESH; and a startup-time
// build would not survive anyway, since gpu64_3dReset() wipes s_Res, the
// arena, s_State and s_Scene at every session reset. The load has to be
// C64-triggerable, and it has to be here.

// Bytes of source blob a single LEVEL_STEP will consume before it returns.
// 16 KB is roughly one 128x128 texture with its mip chain, or a dozen meshes;
// at E1M1's 620 KB that is about 40 commands for the whole level.
#define GPU64_LEVEL_SLICE_BYTES		16384

enum Gpu64_LevelPhase
{
	GPU64_LEVELPH_IDLE = 0,
	GPU64_LEVELPH_PALETTE,
	GPU64_LEVELPH_TEXTURES,
	GPU64_LEVELPH_MESHES,
	GPU64_LEVELPH_NODES,
	GPU64_LEVELPH_CAMERA,
	GPU64_LEVELPH_DONE
};

static struct
{
	Gpu64_Level	lev;
	u8		phase;
	u16		index;		// item within the phase
	u16		nMeshBase;	// mesh resource ids are nMeshBase + i
	u16		nNodeBase;	// object node ids are nNodeBase + i
	u16		nCameraId;	// 0 = do not create one
	u32		nTotalItems;	// for the percentage only
	u32		nDoneItems;
}
s_Load;

// Textures are the one namespace the loader cannot choose. A face record's
// texture id is a single byte (Gpu64_3dFaceWire.texid) which the renderer
// passes straight to lookupTexture(), so texture resource id i must equal the
// dense table index i the converter wrote into those bytes. Documented rather
// than parameterised: a mesh base is a free choice, a texture base is not.
static u8 levelBuildTexture( unsigned i, u32 *pBytes )
{
	const u8 *pR = s_Load.lev.pTexTab + i * GPU64_LEVEL_TEX_STRIDE;
	// pR[0..1] is the source BSP miptex index, provenance only -- the
	// resource id is i, per the note above.
	const u8  ws = pR[ 2 ], hs = pR[ 3 ];
	const u32 off = gpu64_levelRd32( pR + 4 ), len = gpu64_levelRd32( pR + 8 );

	const u8 *pPix = gpu64_levelBlob( &s_Load.lev, off, len );
	if ( pPix == 0 )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_3dResource *pSlot = resSlot( (u16)i );
	if ( pSlot == 0 )
		return GPU64_ERR_OUT_OF_MEMORY;

	Gpu64_3dTexture tex;
	const u8 res = gpu64_3dBuildTexture( &tex, pPix, len, ws, hs, arenaAllocFn, 0 );
	if ( res != GPU64_3D_OK )
		return res;

	pSlot->id   = (u16)i;
	pSlot->type = GPU64_3D_RES_TEXTURE;
	pSlot->tex  = tex;
	*pBytes = len;
	return GPU64_ERR_OK;
}

static u8 levelBuildMesh( unsigned i, u32 *pBytes )
{
	const u8 *pR = s_Load.lev.pMeshTab + i * GPU64_LEVEL_MESH_STRIDE;
	const u32 vo = gpu64_levelRd32( pR ),      vl = gpu64_levelRd32( pR + 4 );
	const u32 fo = gpu64_levelRd32( pR + 8 ),  fl = gpu64_levelRd32( pR + 12 );

	const u8 *pV = gpu64_levelBlob( &s_Load.lev, vo, vl );
	const u8 *pF = gpu64_levelBlob( &s_Load.lev, fo, fl );
	if ( pV == 0 || pF == 0 )
		return GPU64_ERR_BAD_ARGS;

	const u16 id = (u16)( s_Load.nMeshBase + i );
	Gpu64_3dResource *pSlot = resSlot( id );
	if ( pSlot == 0 )
		return GPU64_ERR_OUT_OF_MEMORY;

	// Straight from the file, not via s_StageA/s_StageB: the blob is already
	// in RAM and gpu64_3dBuildMesh() only reads it. UPLOAD_MESH stages
	// because its source is REU space, which has to be DMAd across first.
	Gpu64_3dMesh mesh;
	const u8 res = gpu64_3dBuildMesh( &mesh, pV, vl, pF, fl, arenaAllocFn, 0 );
	if ( res != GPU64_3D_OK )
		return res;

	pSlot->id   = id;
	pSlot->type = GPU64_3D_RES_MESH;
	pSlot->mesh = mesh;
	*pBytes = vl + fl;
	return GPU64_ERR_OK;
}

static u8 levelBuildNode( unsigned i )
{
	const u8 *pR = s_Load.lev.pNodeTab + i * GPU64_LEVEL_NODE_STRIDE;
	const u16 nMesh = gpu64_levelRd16( pR );
	if ( nMesh >= s_Load.lev.nMesh )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_3dVec pos;
	pos.x = gpu64_levelRdS32( pR + 2 );
	pos.y = gpu64_levelRdS32( pR + 6 );
	pos.z = gpu64_levelRdS32( pR + 10 );

	const u16 id = (u16)( s_Load.nNodeBase + i );

	// OUT_OF_MEMORY here is a capacity answer the converter needs, not a
	// crash: a level can want more chunks than GPU64_3D_MAX_NODES holds, and
	// the honest report is which node number ran out.
	const u8 res = gpu64_3dSceneCreateObject( &s_Scene, id,
						   (u16)( s_Load.nMeshBase + nMesh ) );
	if ( res != GPU64_3D_OK )
		return res;

	return gpu64_3dSceneSetPosition( &s_Scene, id, &pos );
}

// The camera phase: a camera at the level header's start position, raised
// by its eye height. The converter chose both; gpu64 only places the camera
// where the file says. ARG4-5 = 0 opts out, for a caller that wants to place
// its own camera.
static u8 levelPhaseCamera( void )
{
	if ( s_Load.nCameraId == 0 )
		return GPU64_ERR_OK;

	if ( !s_Load.lev.bHasStart )
		return GPU64_ERR_BAD_ARGS;	// the level has no start position

	Gpu64_3dVec pos;
	pos.x = s_Load.lev.nStartX;
	pos.y = s_Load.lev.nStartY + gpu64_levelEyeOfs( &s_Load.lev );
	pos.z = s_Load.lev.nStartZ;
	const u16 yaw = s_Load.lev.nStartYaw;

	u8 res = gpu64_3dSceneCreateCamera( &s_Scene, s_Load.nCameraId );
	if ( res != GPU64_3D_OK )
		return res;
	res = gpu64_3dSceneSetPosition( &s_Scene, s_Load.nCameraId, &pos );
	if ( res != GPU64_3D_OK )
		return res;
	res = gpu64_3dSceneSetOrientation( &s_Scene, s_Load.nCameraId, yaw, 0, 0 );
	if ( res != GPU64_3D_OK )
		return res;
	return gpu64_3dSceneSetActiveCamera( &s_Scene, s_Load.nCameraId );
}

// The palette the level travels with, installed into the framebuffer and then
// turned into the shade ramp. A level's texels are Quake palette indices, so
// without both of these the scene renders through the C64's sixteen colours
// and looks like noise.
static u8 levelPhasePalette( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;

	const u8 *pPal = gpu64_levelBlob( &s_Load.lev, s_Load.lev.nPalOff, 768 );
	if ( pPal == 0 )
		return GPU64_ERR_BAD_ARGS;	// gpu64_levelParse() already checked

	for ( unsigned i = 0; i < 256; i++ )
		pFB->SetPaletteEntry( (u8)i, pPal[ i * 3 ], pPal[ i * 3 + 1 ], pPal[ i * 3 + 2 ] );

	// SetPaletteEntry() updates the shadow and Circle's copy; only
	// CommitPalette() pushes it to the VideoCore, which is why PAL_SET and
	// PAL_LOAD both call it. Omitting it here cost bench run 33: the
	// colormap was right, the geometry was right, and the display was still
	// showing the boot palette -- the C64 sixteen in 0-15, black in 16-254
	// and white at GPU64_LOG_INK -- so E1M1 rendered as a black screen with
	// two white bars where index 255 happened to land. No host sim can
	// catch this: hostsim, scenesim and levelsim all resolve indices
	// through their own palette array and have no VideoCore to be out of
	// step with.
	pFB->CommitPalette();

	gpu64_3dBuildColormap( &s_State, pFB->GetPaletteRGB() );
	return GPU64_ERR_OK;
}

static void levelLoadAbort( void )
{
	s_Load.phase = GPU64_LEVELPH_IDLE;
}

// The Gpu64_3dVisLookup for a loaded level: its PVS row for the camera's
// leaf, over the world chunks LOAD_LEVEL created. Runs on core 1, reading
// s_Load, which only LOAD_LEVEL/LEVEL_STEP write -- and both refuse while the
// loop is running, so nothing writes it while a frame renders.
static const u8 *levelVisLookup( void *, const Gpu64_3dVec *pEye,
				 u16 *pFirstId, u16 *pnNodes, u16 *pFirstMesh )
{
	if ( s_Load.phase != GPU64_LEVELPH_DONE )
		return 0;

	Gpu64_LevelVec e;
	e.v[ 0 ] = pEye->x;
	e.v[ 1 ] = pEye->y;
	e.v[ 2 ] = pEye->z;

	const u8 *pRow = gpu64_levelVisRow( &s_Load.lev, &e, pnNodes );
	if ( pRow == 0 )
		return 0;

	// World chunk k is node k and its mesh is node 0's plus k -- but node 0's
	// mesh is not mesh 0: the actor catalogue comes first in the file
	// (tools/gen_quakelevel.py), so its index is read, not assumed. Assuming
	// it silently turned the whole cull off.
	*pFirstId   = s_Load.nNodeBase;
	*pFirstMesh = (u16)( s_Load.nMeshBase + gpu64_levelRd16( s_Load.lev.pNodeTab ) );
	return pRow;
}

static u8 opLoadLevel( void )
{
	// Keyed by the gate in execute(), like SCENE_RESET -- this wipes the
	// resource table and the scene.
	if ( s_LoopRunning )
		return GPU64_ERR_BUSY;		// the shadow scene cannot take a whole level

	// ARG6 picks the level slot: the Nth level of the REU image's pack
	// (gpu64_levelPreload). It counts only when ARG7 is its complement under $5A,
	// so a program that never heard of slots -- and leaves whatever an
	// earlier command put in ARG6/7 -- keeps loading slot 0.
	u8 nSlot = 0;
	if ( ( sArg[ 6 ] ^ 0x5A ) == sArg[ 7 ] )
		nSlot = sArg[ 6 ];
	if ( nSlot >= GPU64_LEVEL_SLOTS )
		return GPU64_ERR_BAD_ARGS;
	const u8 *pFile  = gpu64LevelSlot[ nSlot ];
	u32       nBytes = gpu64LevelSlotBytes[ nSlot ];

	if ( nBytes == 0 )
		return GPU64_ERR_BAD_ARGS;	// no such level in the REU image

	memset( &s_Load, 0, sizeof( s_Load ) );

	if ( !gpu64_levelParse( &s_Load.lev, pFile, nBytes ) )
		return GPU64_ERR_BAD_ARGS;

	s_Load.nMeshBase = argU16( 0 );
	s_Load.nNodeBase = argU16( 2 );
	s_Load.nCameraId = argU16( 4 );

	// The mesh ids must not collide with the texture ids, which the face
	// format pins to 0..nTex-1 (see levelBuildTexture). Refusing beats
	// silently overwriting a texture with a mesh in the same slot.
	if ( s_Load.nMeshBase < s_Load.lev.nTex )
		return GPU64_ERR_BAD_ARGS;
	if ( (u32)s_Load.nMeshBase + s_Load.lev.nMesh > 0x10000 )
		return GPU64_ERR_BAD_ARGS;
	if ( (u32)s_Load.nNodeBase + s_Load.lev.nNode > 0x10000 )
		return GPU64_ERR_BAD_ARGS;
	if ( s_Load.nCameraId != 0 &&
	     s_Load.nCameraId >= s_Load.nNodeBase &&
	     s_Load.nCameraId <  s_Load.nNodeBase + s_Load.lev.nNode )
		return GPU64_ERR_BAD_ARGS;	// the camera id is inside the node range

	// A fresh table and a fresh scene, exactly as SCENE_RESET leaves them
	// plus the arena: a second LOAD_LEVEL in one session would otherwise
	// leak the first level's 20-odd MB and then answer OUT_OF_MEMORY.
	gpu64_3dArenaReset();
	memset( s_Res, 0, sizeof( s_Res ) );
	gpu64_3dSceneReset( &s_Scene );
	gpu64_3dSceneReset( &s_ShadowScene );

	// s_State is deliberately NOT reset. The viewport, fov and near/far
	// planes belong to the caller, who has to set them before the load
	// anyway (the view they want exists before the level does) and would
	// otherwise have to discover that LOAD_LEVEL silently undid them. The
	// only state this load writes is the colormap, in the palette phase.

	s_Load.phase       = GPU64_LEVELPH_PALETTE;
	s_Load.nTotalItems = 1 + s_Load.lev.nTex + s_Load.lev.nMesh + s_Load.lev.nNode + 1;

	gpu64Regs.result = 0;
	return GPU64_ERR_OK;
}

static u8 opLevelStep( void )
{
	if ( s_Load.phase == GPU64_LEVELPH_IDLE )
		return GPU64_ERR_BAD_ARGS;	// LEVEL_STEP without a LOAD_LEVEL

	// Refused for the same reason LOAD_LEVEL is, and not left to the
	// dispatcher's drain: a step creates nodes in s_Scene, which is the state
	// GPU64_3D_OP_RENDER_SCENE reads on core 1. The drain proves the frame in
	// flight has finished but says nothing about the next one, and a client
	// that starts the loop halfway through a load should be told so rather
	// than have half a level appear.
	if ( s_LoopRunning )
		return GPU64_ERR_BUSY;

	if ( s_Load.phase == GPU64_LEVELPH_DONE )
	{
		gpu64Regs.result = GPU64_3D_LEVEL_DONE;
		return GPU64_ERR_OK;		// idempotent: a lost RESULT read can retry
	}

	u32 nBudget = GPU64_LEVEL_SLICE_BYTES;

	// At least one item per call even if it is over budget on its own,
	// otherwise a single 128x128 texture would stall the load forever.
	while ( s_Load.phase != GPU64_LEVELPH_DONE )
	{
		u32 nBytes = 0;
		u8  res    = GPU64_ERR_OK;

		switch ( s_Load.phase )
		{
		case GPU64_LEVELPH_PALETTE:
			res    = levelPhasePalette();
			nBytes = 768;
			break;

		case GPU64_LEVELPH_TEXTURES:
			res = levelBuildTexture( s_Load.index, &nBytes );
			break;

		case GPU64_LEVELPH_MESHES:
			res = levelBuildMesh( s_Load.index, &nBytes );
			break;

		case GPU64_LEVELPH_NODES:
			res    = levelBuildNode( s_Load.index );
			nBytes = 64;			// bookkeeping, not data
			break;

		case GPU64_LEVELPH_CAMERA:
			res    = levelPhaseCamera();
			nBytes = 64;
			break;
		}

		if ( res != GPU64_ERR_OK )
		{
			// The failing item's number, so a failure names its address
			// instead of just its kind. The phase stays where it is: a
			// retry re-attempts the same item rather than skipping it.
			gpu64Regs.result = (u8)s_Load.index;
			s_Load.phase = GPU64_LEVELPH_IDLE;
			return res;
		}

		s_Load.nDoneItems++;
		s_Load.index++;

		// Advance the phase when its table is exhausted. The single-item
		// phases fall through on the first increment.
		switch ( s_Load.phase )
		{
		case GPU64_LEVELPH_PALETTE:
			s_Load.phase = GPU64_LEVELPH_TEXTURES;
			s_Load.index = 0;
			break;
		case GPU64_LEVELPH_TEXTURES:
			if ( s_Load.index >= s_Load.lev.nTex )
			{
				s_Load.phase = GPU64_LEVELPH_MESHES;
				s_Load.index = 0;
			}
			break;
		case GPU64_LEVELPH_MESHES:
			if ( s_Load.index >= s_Load.lev.nMesh )
			{
				s_Load.phase = GPU64_LEVELPH_NODES;
				s_Load.index = 0;
			}
			break;
		case GPU64_LEVELPH_NODES:
			if ( s_Load.index >= s_Load.lev.nNode )
			{
				s_Load.phase = GPU64_LEVELPH_CAMERA;
				s_Load.index = 0;
			}
			break;
		case GPU64_LEVELPH_CAMERA:
			s_Load.phase = GPU64_LEVELPH_DONE;
			break;
		}

		if ( nBytes >= nBudget )
			break;
		nBudget -= nBytes;
	}

	if ( s_Load.phase == GPU64_LEVELPH_DONE )
	{
		gpu64Regs.result = GPU64_3D_LEVEL_DONE;
		return GPU64_ERR_OK;
	}

	// Percent, clamped to 99 so that only GPU64_3D_LEVEL_DONE ever means
	// finished -- see the constant's comment.
	u32 pct = s_Load.nTotalItems ? s_Load.nDoneItems * 100 / s_Load.nTotalItems : 0;
	gpu64Regs.result = (u8)( pct > 99 ? 99 : pct );
	return GPU64_ERR_OK;
}

// --- milestone 18: collision -------------------------------------------
//
// CLIP_MOVE is a pure function of the level file and the caller's block, so
// unlike LOAD_LEVEL and LEVEL_STEP it does not refuse while the render loop
// is running: it touches neither s_Scene nor the resource table. It is also
// unkeyed -- see the block layout and the reasoning in gpu64_3d.h.

static inline s32 blkS32( const u8 *p )
{
	return (s32)( (u32)p[ 0 ] | ( (u32)p[ 1 ] << 8 ) |
		      ( (u32)p[ 2 ] << 16 ) | ( (u32)p[ 3 ] << 24 ) );
}

static inline void blkPutS32( u8 *p, s32 v )
{
	p[ 0 ] = (u8)( (u32)v       );
	p[ 1 ] = (u8)( (u32)v >>  8 );
	p[ 2 ] = (u8)( (u32)v >> 16 );
	p[ 3 ] = (u8)( (u32)v >> 24 );
}

static inline void blkPut16( u8 *p, u16 v )
{
	p[ 0 ] = (u8)( v      );
	p[ 1 ] = (u8)( v >> 8 );
}

static inline u8 blkXor( const u8 *p, unsigned n )
{
	u8 x = 0;
	for ( unsigned i = 0; i < n; i++ )
		x = (u8)( x ^ p[ i ] );
	return x;
}

static u8 opClipMove( void )
{
	// The wire's limit and the tracer's are the same number in two headers.
	typedef char moverLimitsAgree[
		GPU64_3D_CLIPMOVE_MAX_MOVERS == GPU64_LEVEL_MAX_MOVERS ? 1 : -1 ];
	( void )sizeof( moverLimitsAgree );

	u8 space;
	u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	if ( len < GPU64_3D_CLIPMOVE_BYTES )
		return GPU64_ERR_BAD_ARGS;
	// Nothing to trace against until the load has finished. BAD_ARGS is what
	// LEVEL_STEP-without-LOAD_LEVEL returns, for the same reason.
	if ( s_Load.phase != GPU64_LEVELPH_DONE )
		return GPU64_ERR_BAD_ARGS;

	u8 blk[ GPU64_3D_CLIPMOVE_IN_BYTES ];
	u8 res = gpu64_blobRead( space, addr, GPU64_3D_CLIPMOVE_IN_BYTES, blk );
	if ( res != GPU64_ERR_OK ) return res;

	if ( blk[ 27 ] != GPU64_3D_CLIPMOVE_MAGIC_IN )
		return GPU64_ERR_BAD_ARGS;

	// The mover list, if there is one. It is a second burst rather than one
	// long read because the records sit past the output half and their count
	// is in the first burst; the block was laid out that way so the Pi's
	// answer never has to move when a caller adds doors.
	unsigned nMovers = blk[ 29 ];
	u8 mvb[ GPU64_3D_CLIPMOVE_MAX_MOVERS * GPU64_3D_CLIPMOVE_MOVER_BYTES ];
	unsigned nMvBytes = nMovers * GPU64_3D_CLIPMOVE_MOVER_BYTES;
	if ( nMovers > GPU64_3D_CLIPMOVE_MAX_MOVERS )
		return GPU64_ERR_BAD_ARGS;
	if ( len < GPU64_3D_CLIPMOVE_MOVER_OFF + nMvBytes )
		return GPU64_ERR_BAD_ARGS;
	if ( nMvBytes != 0 )
	{
		res = gpu64_blobRead( space, addr + GPU64_3D_CLIPMOVE_MOVER_OFF,
				      nMvBytes, mvb );
		if ( res != GPU64_ERR_OK ) return res;
	}

	// Then the checksum, over both bursts. Either it or the magic failing
	// means the DMA lost a byte or the caller never filled the block in, and
	// in both cases the right answer is to write nothing at all and let the
	// C64 retry: a half-trusted position would be a teleport, and a mover
	// record that lost its offset byte would be a door in the wrong place.
	u8 chk = blkXor( blk, 28 ) ^ blk[ 29 ];
	for ( unsigned i = 0; i < nMvBytes; i++ )
		chk ^= mvb[ i ];
	if ( chk != blk[ 28 ] )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_LevelVec start, delta;
	for ( unsigned k = 0; k < 3; k++ )
	{
		start.v[ k ] = blkS32( blk + 0  + k * 4 );
		delta.v[ k ] = blkS32( blk + 12 + k * 4 );
	}

	Gpu64_LevelMover movers[ GPU64_3D_CLIPMOVE_MAX_MOVERS ];
	for ( unsigned i = 0; i < nMovers; i++ )
	{
		const u8 *r = mvb + i * GPU64_3D_CLIPMOVE_MOVER_BYTES;
		movers[ i ].nModel = r[ 0 ];
		for ( unsigned k = 0; k < 3; k++ )
			movers[ i ].ofs.v[ k ] = blkS32( r + 4 + k * 4 );
	}

	Gpu64_LevelMove mv;
	if ( !gpu64_levelMoveEnts( &s_Load.lev, blk[ 26 ], blk[ 24 ], blk[ 25 ],
				   &start, &delta, movers, nMovers, &mv ) )
		return GPU64_ERR_BAD_ARGS;

	u8 out[ GPU64_3D_CLIPMOVE_OUT_BYTES ];
	memset( out, 0, sizeof( out ) );
	for ( unsigned k = 0; k < 3; k++ )
		blkPutS32( out + k * 4, mv.end.v[ k ] );
	out[ 12 ] = mv.nFlags;
	out[ 13 ] = mv.nContents;
	out[ 14 ] = mv.nFraction;
	out[ 15 ] = mv.nBumps;
	out[ 16 ] = GPU64_3D_CLIPMOVE_MAGIC_OUT;
	out[ 17 ] = blkXor( out, 17 );
	// Outside the checksum, which predates them, so they carry their own:
	// the water level and its complement. A client that sees 0/0 is talking
	// to firmware that does not report it.
	out[ 18 ] = mv.nWaterLevel;
	out[ 19 ] = (u8)( 0xff - mv.nWaterLevel );

	res = gpu64_blobWrite( space, addr + GPU64_3D_CLIPMOVE_OUT_OFF,
			       GPU64_3D_CLIPMOVE_OUT_BYTES, out );
	if ( res != GPU64_ERR_OK ) return res;

	// A convenience only. RESULT is a single register read, and a failed
	// register read returns whatever was last on the bus (gpu64-errcode-ff-
	// is-the-bus), so the block's own checksum is the channel to trust.
	gpu64Regs.result = mv.nFlags;
	return GPU64_ERR_OK;
}

// WORLD_TICK: the layout, and why it is one block, are in gpu64_3d.h.
//
// The order below is the contract. Everything that can refuse the block is
// decided before anything is applied: the magic, the counts, the length,
// the checksum, and every query's hull and mover model. Only then are the
// actors applied, and only then are the queries answered -- so a block is
// either wholly refused with the scene untouched, or wholly accepted with a
// per-record refusal mask for the one kind of error that cannot be seen
// before looking at the scene: a node or resource that is not there.
//
// Static buffers rather than stack: the input can be 2.3 KB and the output
// 0.8 KB, and this only ever runs on core 0 from a dispatch.
#define WTICK_IN_MAX	( GPU64_3D_WTICK_HDR_BYTES + \
			  GPU64_3D_WTICK_MAX_ACTORS * GPU64_3D_WTICK_ACTOR_BYTES + \
			  GPU64_3D_WTICK_MAX_TRACES * GPU64_3D_WTICK_TRACE_IN_BYTES + \
			  GPU64_3D_WTICK_MAX_MOVES  * GPU64_3D_WTICK_MOVE_IN_BYTES + \
			  GPU64_3D_WTICK_MAX_MOVERS * GPU64_3D_CLIPMOVE_MOVER_BYTES )
#define WTICK_OUT_MAX	( GPU64_3D_WTICK_OUT_HDR_BYTES + \
			  ( GPU64_3D_WTICK_MAX_TRACES + GPU64_3D_WTICK_MAX_MOVES ) * \
			  GPU64_3D_WTICK_ANSWER_BYTES + GPU64_3D_WTICK_TRAILER_BYTES )

static u8 s_WtIn[ WTICK_IN_MAX ];
static u8 s_WtOut[ WTICK_OUT_MAX ];

// One actor record. FALSE refuses it, having changed nothing: every test is
// made before the first write.
static boolean worldTickActor( Gpu64_3dScene *pS, const u8 *r )
{
	const u16 id    = (u16)( r[ 0 ] | ( r[ 1 ] << 8 ) );
	const u16 res   = (u16)( r[ 2 ] | ( r[ 3 ] << 8 ) );
	const u8  flags = r[ 4 ];

	if ( flags & ~( GPU64_3D_WTICK_VISIBLE | GPU64_3D_WTICK_VIEWSPACE ) )
		return FALSE;

	Gpu64_3dNode *pN = gpu64_3dSceneFind( pS, id, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return FALSE;

	// The resource is the animation frame or the HUD digit, so it has to be
	// the right kind for the node. A wrong one is almost certainly a record
	// aimed at the wrong node, and applying its pose would be worse than
	// leaving the node where it was.
	switch ( pN->type )
	{
	case GPU64_3D_NODE_OBJECT:
		if ( res != 0xffff && resFind( res, GPU64_3D_RES_MESH ) == 0 )
			return FALSE;
		break;
	case GPU64_3D_NODE_SPRITE:
		if ( res != 0xffff && resFind( res, GPU64_3D_RES_TEXTURE ) == 0 )
			return FALSE;
		break;
	case GPU64_3D_NODE_LIGHT:
	case GPU64_3D_NODE_CAMERA:
		if ( res != 0xffff )
			return FALSE;
		break;
	default:
		return FALSE;
	}

	if ( res != 0xffff )
	{
		if ( pN->type == GPU64_3D_NODE_OBJECT )
			pN->meshId = res;
		else
			pN->texId = res;
	}

	pN->visible   = ( flags & GPU64_3D_WTICK_VISIBLE ) ? TRUE : FALSE;
	pN->viewSpace = ( pN->type != GPU64_3D_NODE_CAMERA &&
			  ( flags & GPU64_3D_WTICK_VIEWSPACE ) ) ? 1 : 0;

	Gpu64_3dVec pos;
	pos.x = blkS32( r + 8 );
	pos.y = blkS32( r + 12 );
	pos.z = blkS32( r + 16 );
	gpu64_3dSceneSetPosition( pS, id, &pos );

	const u16 yaw = (u16)( r[ 5 ] << 8 ), pitch = (u16)( r[ 6 ] << 8 ),
		  roll = (u16)( r[ 7 ] << 8 );
	if ( pN->yaw != yaw || pN->pitch != pitch || pN->roll != roll )
		gpu64_3dSceneSetOrientation( pS, id, yaw, pitch, roll );
	return TRUE;
}

static u8 opWorldTick( void )
{
	typedef char moverLimitsAgree[
		GPU64_3D_WTICK_MAX_MOVERS == GPU64_LEVEL_MAX_MOVERS ? 1 : -1 ];
	( void )sizeof( moverLimitsAgree );

	u8 space;
	u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	if ( len < GPU64_3D_WTICK_HDR_BYTES )
		return GPU64_ERR_BAD_ARGS;

	u8 *in = s_WtIn;
	u8 res = gpu64_blobRead( space, addr, GPU64_3D_WTICK_HDR_BYTES, in );
	if ( res != GPU64_ERR_OK ) return res;

	const unsigned nAct = in[ 1 ], nTr = in[ 2 ], nMv = in[ 3 ], nMover = in[ 4 ];
	if ( in[ 0 ] != GPU64_3D_WTICK_MAGIC_IN ||
	     nAct > GPU64_3D_WTICK_MAX_ACTORS || nTr > GPU64_3D_WTICK_MAX_TRACES ||
	     nMv > GPU64_3D_WTICK_MAX_MOVES || nMover > GPU64_3D_WTICK_MAX_MOVERS )
		return GPU64_ERR_BAD_ARGS;

	const unsigned actOff   = GPU64_3D_WTICK_HDR_BYTES;
	const unsigned trOff    = actOff + nAct * GPU64_3D_WTICK_ACTOR_BYTES;
	const unsigned mvOff    = trOff + nTr * GPU64_3D_WTICK_TRACE_IN_BYTES;
	const unsigned moverOff = mvOff + nMv * GPU64_3D_WTICK_MOVE_IN_BYTES;
	const unsigned outOff   = moverOff + nMover * GPU64_3D_CLIPMOVE_MOVER_BYTES;
	const unsigned outLen   = GPU64_3D_WTICK_OUT_HDR_BYTES +
				  ( nTr + nMv ) * GPU64_3D_WTICK_ANSWER_BYTES +
				  GPU64_3D_WTICK_TRAILER_BYTES;
	if ( len < outOff + outLen )
		return GPU64_ERR_BAD_ARGS;

	// Queries need a level; actors do not, so a program without one can
	// still use the block for its scene.
	if ( ( nTr | nMv | nMover ) != 0 && s_Load.phase != GPU64_LEVELPH_DONE )
		return GPU64_ERR_BAD_ARGS;

	if ( outOff > GPU64_3D_WTICK_HDR_BYTES )
	{
		res = gpu64_blobRead( space, addr + GPU64_3D_WTICK_HDR_BYTES,
				      outOff - GPU64_3D_WTICK_HDR_BYTES,
				      in + GPU64_3D_WTICK_HDR_BYTES );
		if ( res != GPU64_ERR_OK ) return res;
	}

	u16 sum = 0;
	for ( unsigned i = 0; i < outOff; i++ )
		if ( i != 6 && i != 7 )
			sum = (u16)( sum + in[ i ] );
	if ( sum != (u16)( in[ 6 ] | ( in[ 7 ] << 8 ) ) )
		return GPU64_ERR_BAD_ARGS;

	// The queries' own arguments, before anything is applied.
	Gpu64_LevelMover movers[ GPU64_3D_WTICK_MAX_MOVERS ];
	for ( unsigned i = 0; i < nMover; i++ )
	{
		const u8 *r = in + moverOff + i * GPU64_3D_CLIPMOVE_MOVER_BYTES;
		movers[ i ].nModel = r[ 0 ];
		if ( r[ 0 ] >= s_Load.lev.nHull )
			return GPU64_ERR_BAD_ARGS;
		for ( unsigned k = 0; k < 3; k++ )
			movers[ i ].ofs.v[ k ] = blkS32( r + 4 + k * 4 );
	}
	for ( unsigned i = 0; i < nTr; i++ )
		if ( in[ trOff + i * GPU64_3D_WTICK_TRACE_IN_BYTES + 24 ] > 2 )
			return GPU64_ERR_BAD_ARGS;
	for ( unsigned i = 0; i < nMv; i++ )
	{
		const u8 h = in[ mvOff + i * GPU64_3D_WTICK_MOVE_IN_BYTES + 24 ];
		if ( h < 1 || h > 2 )
			return GPU64_ERR_BAD_ARGS;
	}

	// The actors.
	Gpu64_3dScene *pS = sceneTarget();
	u32 refused = 0;
	unsigned nApplied = 0;
	for ( unsigned i = 0; i < nAct; i++ )
	{
		if ( worldTickActor( pS, in + actOff + i * GPU64_3D_WTICK_ACTOR_BYTES ) )
			nApplied++;
		else
			refused |= 1u << i;
	}

	// The answers.
	u8 *out = s_WtOut;
	memset( out, 0, outLen );
	out[ 0 ] = GPU64_3D_WTICK_MAGIC_OUT;
	out[ 1 ] = in[ 5 ];
	out[ 2 ] = (u8)nApplied;
	blkPutS32( out + 4, (s32)refused );

	u8 *a = out + GPU64_3D_WTICK_OUT_HDR_BYTES;
	for ( unsigned i = 0; i < nTr; i++, a += GPU64_3D_WTICK_ANSWER_BYTES )
	{
		const u8 *r = in + trOff + i * GPU64_3D_WTICK_TRACE_IN_BYTES;
		Gpu64_LevelVec start, end;
		for ( unsigned k = 0; k < 3; k++ )
		{
			start.v[ k ] = blkS32( r + k * 4 );
			end.v[ k ]   = blkS32( r + 12 + k * 4 );
		}
		Gpu64_LevelTrace tr;
		u8 hit = GPU64_CLIP_CONT_EMPTY;
		if ( !gpu64_levelTraceEnts( &s_Load.lev, r[ 24 ], &start, &end,
					    movers, nMover, &tr, &hit ) )
			return GPU64_ERR_BAD_ARGS;	// checked above; kept as a guard

		for ( unsigned k = 0; k < 3; k++ )
			blkPutS32( a + k * 4, tr.end.v[ k ] );
		const s32 f = tr.nFraction;
		blkPut16( a + 12, (u16)( f >= 0xffff ? 0xffff : f < 0 ? 0 : f ) );
		u8 fl = 0;
		if ( tr.nFraction >= 0x10000 && !tr.bStartSolid && !tr.bAllSolid )
			fl |= GPU64_3D_WTICK_TR_CLEAR;
		if ( tr.bStartSolid ) fl |= GPU64_3D_WTICK_TR_STARTSOLID;
		if ( tr.bAllSolid )   fl |= GPU64_3D_WTICK_TR_ALLSOLID;
		a[ 14 ] = fl;
		a[ 15 ] = hit;
	}

	for ( unsigned i = 0; i < nMv; i++, a += GPU64_3D_WTICK_ANSWER_BYTES )
	{
		const u8 *r = in + mvOff + i * GPU64_3D_WTICK_MOVE_IN_BYTES;
		Gpu64_LevelVec start, delta;
		for ( unsigned k = 0; k < 3; k++ )
		{
			start.v[ k ] = blkS32( r + k * 4 );
			delta.v[ k ] = blkS32( r + 12 + k * 4 );
		}
		Gpu64_LevelMove mv;
		if ( !gpu64_levelMoveEnts( &s_Load.lev, 0, r[ 24 ], r[ 25 ],
					   &start, &delta, movers, nMover, &mv ) )
			return GPU64_ERR_BAD_ARGS;

		for ( unsigned k = 0; k < 3; k++ )
			blkPutS32( a + k * 4, mv.end.v[ k ] );
		a[ 12 ] = mv.nFlags;
		a[ 13 ] = mv.nContents;
		a[ 14 ] = mv.nFraction;
		a[ 15 ] = mv.nBumps;
	}

	u16 osum = 0;
	for ( u8 *p = out; p < a; p++ )
		osum = (u16)( osum + *p );
	blkPut16( a, osum );
	a[ 2 ] = GPU64_3D_WTICK_MAGIC_OUT;
	a[ 3 ] = in[ 5 ];

	res = gpu64_blobWrite( space, addr + outOff, outLen, out );
	if ( res != GPU64_ERR_OK ) return res;

	unsigned nRefused = nAct - nApplied;
	gpu64Regs.result = (u8)( nRefused > 255 ? 255 : nRefused );
	return GPU64_ERR_OK;
}

static u8 opLevelPalette( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;
	if ( s_Load.phase != GPU64_LEVELPH_DONE )
		return GPU64_ERR_BAD_ARGS;

	const u8 *pPal = gpu64_levelBlob( &s_Load.lev, s_Load.lev.nPalOff, 768 );
	if ( pPal == 0 )
		return GPU64_ERR_BAD_ARGS;

	const u8 *pNow = pFB->GetPaletteRGB();
	unsigned nFixed = 0;
	for ( unsigned i = 0; i < 256; i++ )
	{
		const u8 *p = pPal + i * 3;
		const u8 *q = pNow + i * 3;
		if ( p[ 0 ] == q[ 0 ] && p[ 1 ] == q[ 1 ] && p[ 2 ] == q[ 2 ] )
			continue;
		pFB->SetPaletteEntry( (u8)i, p[ 0 ], p[ 1 ], p[ 2 ] );
		nFixed++;
	}

	// The common answer is that nothing was wrong, and then nothing else
	// happens: no mailbox call, no colormap -- a refresh ring can afford it.
	if ( nFixed != 0 )
	{
		pFB->CommitPalette();
		gpu64_3dBuildColormap( stateTargetForEdit(), pFB->GetPaletteRGB() );
	}

	gpu64Regs.result = (u8)( nFixed > 255 ? 255 : nFixed );
	return GPU64_ERR_OK;
}

// LEVEL_PICTURE: see gpu64_3d.h. Parses the preloaded file on every call
// rather than borrowing s_Load.lev, because the title runs before any
// LOAD_LEVEL; a parse is a header walk and costs microseconds.
static u8 opLevelPicture( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return GPU64_ERR_UNSUPPORTED;
	if ( s_LoopRunning )
		return GPU64_ERR_BUSY;
	const u8 nFlags = sArg[ 5 ];
	if ( nFlags & ~3 )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_Level lev;
	if ( gpu64LevelSlotBytes[ 0 ] == 0
	     || !gpu64_levelParse( &lev, gpu64LevelSlot[ 0 ], gpu64LevelSlotBytes[ 0 ] ) )
		return GPU64_ERR_BAD_ARGS;

	unsigned w, h;
	const u8 *pPic = gpu64_levelPic( &lev, sArg[ 0 ], &w, &h );
	if ( pPic == 0 )
		return GPU64_ERR_BAD_ARGS;

	if ( nFlags & 2 )
	{
		const u8 *pPal = gpu64_levelBlob( &lev, lev.nPalOff, 768 );
		const u8 *pNow = pFB->GetPaletteRGB();
		boolean bChanged = FALSE;
		for ( unsigned i = 0; i < 256; i++ )
		{
			const u8 *p = pPal + i * 3;
			const u8 *q = pNow + i * 3;
			if ( p[ 0 ] == q[ 0 ] && p[ 1 ] == q[ 1 ] && p[ 2 ] == q[ 2 ] )
				continue;
			pFB->SetPaletteEntry( (u8)i, p[ 0 ], p[ 1 ], p[ 2 ] );
			bChanged = TRUE;
		}
		// No colormap: nothing renders through it until LOAD_LEVEL,
		// which installs the palette and builds the colormap itself.
		if ( bChanged )
			pFB->CommitPalette();
	}

	pFB->Blit( pPic, (s16)( sArg[ 1 ] | ( sArg[ 2 ] << 8 ) ),
		   (s16)( sArg[ 3 ] | ( sArg[ 4 ] << 8 ) ), w, h,
		   ( nFlags & 1 ) ? 255 : -1 );
	gpu64Regs.result = 0;
	return GPU64_ERR_OK;
}

// LEVEL_NODE: see gpu64_3d.h. The comparison is on the fields a phantom
// can reach; anything wrong in them earns the node a full nodeDefaults()
// via CreateObject, which is also what makes a camera or sprite an object
// again.
static u8 opLevelNode( void )
{
	if ( s_Load.phase != GPU64_LEVELPH_DONE )
		return GPU64_ERR_BAD_ARGS;
	if ( sArg[ 0 ] & ~1 )
		return GPU64_ERR_BAD_ARGS;

	const u16 id = stagedId();
	const unsigned i = (u16)( id - s_Load.nNodeBase );
	if ( id < s_Load.nNodeBase || i >= s_Load.lev.nNode )
		return GPU64_ERR_BAD_ID;

	const u8 *pR = s_Load.lev.pNodeTab + i * GPU64_LEVEL_NODE_STRIDE;
	const u16 nMesh = gpu64_levelRd16( pR );
	if ( nMesh >= s_Load.lev.nMesh )
		return GPU64_ERR_BAD_ARGS;
	const u16 meshId = (u16)( s_Load.nMeshBase + nMesh );

	Gpu64_3dVec filePos;
	filePos.x = gpu64_levelRdS32( pR + 2 );
	filePos.y = gpu64_levelRdS32( pR + 6 );
	filePos.z = gpu64_levelRdS32( pR + 10 );

	Gpu64_3dScene *pS = sceneTarget();
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pS, id, GPU64_3D_NODE_NONE );
	u8 nFixed = 0;

	if ( pN == 0 || pN->type != GPU64_3D_NODE_OBJECT || pN->meshId != meshId ||
	     !pN->visible || pN->viewSpace || pN->scale != GPU64_FX8_ONE ||
	     pN->yaw != 0 || pN->pitch != 0 || pN->roll != 0 )
	{
		// A destroyed node has no position worth keeping.
		const Gpu64_3dVec keep = pN ? pN->pos : filePos;

		if ( pS->bHaveActiveCamera && pS->activeCameraId == id )
			pS->bHaveActiveCamera = FALSE;

		const u8 res = gpu64_3dSceneCreateObject( pS, id, meshId );
		if ( res != GPU64_3D_OK )
			return res;
		gpu64_3dSceneSetPosition( pS, id, &keep );
		pN = gpu64_3dSceneFind( pS, id, GPU64_3D_NODE_OBJECT );
		nFixed |= 1;
	}

	if ( ( sArg[ 0 ] & 1 ) &&
	     ( pN->pos.x != filePos.x || pN->pos.y != filePos.y || pN->pos.z != filePos.z ) )
	{
		gpu64_3dSceneSetPosition( pS, id, &filePos );
		nFixed |= 2;
	}

	gpu64Regs.result = nFixed;
	return GPU64_ERR_OK;
}

// LEVEL_ENT: one entity record, pre-digested for a 6502. Same block
// discipline as CLIP_MOVE, same two reasons for being unkeyed and
// drain-exempt; the layout is in gpu64_3d.h.
static u8 opLevelEnt( void )
{
	u8 space;
	u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	if ( len < GPU64_3D_LEVELENT_BYTES )
		return GPU64_ERR_BAD_ARGS;
	if ( s_Load.phase != GPU64_LEVELPH_DONE )
		return GPU64_ERR_BAD_ARGS;

	u8 blk[ GPU64_3D_LEVELENT_IN_BYTES ];
	u8 res = gpu64_blobRead( space, addr, sizeof( blk ), blk );
	if ( res != GPU64_ERR_OK ) return res;

	if ( blk[ 2 ] != GPU64_3D_LEVELENT_MAGIC_IN )
		return GPU64_ERR_BAD_ARGS;
	if ( blkXor( blk, 3 ) != blk[ 3 ] )
		return GPU64_ERR_BAD_ARGS;

	const unsigned nIndex = (unsigned)blk[ 0 ] | ( (unsigned)blk[ 1 ] << 8 );

	Gpu64_LevelEnt e;
	if ( !gpu64_levelEnt( &s_Load.lev, nIndex, &e ) )
		return GPU64_ERR_OUT_OF_RANGE;

	// The brush model's box, and the run of scene nodes its geometry was
	// built into. Half of E1M1's brush models are trigger volumes, which
	// the converter draws nothing for: those answer nodeCount 0 and are
	// still perfectly usable, because a trigger is its box.
	Gpu64_LevelHull hull;
	boolean bHaveBox = e.nModelIdx != 0 &&
			   gpu64_levelHull( &s_Load.lev, e.nModelIdx, &hull );

	u16 nFirst = 0, nCount = 0;
	if ( e.nModelIdx != 0 )
	{
		for ( unsigned i = 0; i < s_Load.lev.nNode; i++ )
		{
			u16 nModel;
			if ( !gpu64_levelNode( &s_Load.lev, i, 0, 0, &nModel ) )
				break;
			if ( nModel != e.nModelIdx )
				continue;
			if ( nCount == 0 )
				nFirst = (u16)( s_Load.nNodeBase + i );
			if ( nCount < 255 )
				nCount++;
		}
	}

	u8 out[ GPU64_3D_LEVELENT_OUT_BYTES ];
	memset( out, 0, sizeof( out ) );

	blkPutS32( out +  0, e.x );
	blkPutS32( out +  4, e.y );
	blkPutS32( out +  8, e.z );
	if ( bHaveBox )
		for ( unsigned k = 0; k < 3; k++ )
		{
			blkPutS32( out + 12 + k * 4, hull.mins.v[ k ] );
			blkPutS32( out + 24 + k * 4, hull.maxs.v[ k ] );
		}
	blkPutS32( out + 36, e.ofsX );
	blkPutS32( out + 40, e.ofsY );
	blkPutS32( out + 44, e.ofsZ );

	blkPut16( out + 48, e.nYaw );
	blkPut16( out + 50, e.nSpawnFlags );
	blkPut16( out + 52, (u16)e.nParam0 );
	blkPut16( out + 54, (u16)e.nParam1 );
	blkPut16( out + 56, e.nTargetId );
	blkPut16( out + 58, e.nTargetNameId );
	blkPut16( out + 60, nFirst );
	blkPut16( out + 62, s_Load.lev.nEnt );
	out[ 64 ] = (u8)nCount;
	out[ 65 ] = e.nKind;
	out[ 66 ] = (u8)( e.nModelIdx > 255 ? 0 : e.nModelIdx );
	out[ 67 ] = 0;
	out[ 68 ] = GPU64_3D_LEVELENT_MAGIC_OUT;
	out[ 69 ] = blkXor( out, 69 );

	res = gpu64_blobWrite( space, addr + GPU64_3D_LEVELENT_OUT_OFF,
			       GPU64_3D_LEVELENT_OUT_BYTES, out );
	if ( res != GPU64_ERR_OK ) return res;

	gpu64Regs.result = e.nKind;
	return GPU64_ERR_OK;
}

// --- Stage 15a: core-0-side prechecks for the three render opcodes ------
//
// gpu64_3dDispatch() (below) calls exactly one of these before it pushes a
// render opcode onto the ring. Everything that used to make opClearViewport/
// opDrawMesh/opDrawNode fail outright -- a bad resource id, a bad arg, no
// framebuffer -- must still be caught here, on core 0, before the command is
// ever queued: once it is on the ring core 1 runs it unconditionally (its own
// comment in gpu64_3d_core1.cpp says so), so a validation failure that
// reached the ring would either have to be invented a second failure path for
// or would silently execute on bad input. See gpu64-multicore-rule-scoped-to-
// polling-loop (memory) for why this split exists.

// Stage 16: "immediate mode requires the loop stopped" (design doc) --
// CLEAR_VIEWPORT/DRAW_MESH/DRAW_NODE would read/write s_State/s_Scene/the
// framebuffer's draw page directly, exactly what GPU64_3D_OP_RENDER_SCENE is
// doing on core 1 while the loop runs. BUSY, not UNSUPPORTED: this is a
// legal opcode temporarily refused, the same distinction GPU64_ERR_BUSY
// already carries for a PAGE_FLIP against a still-pending one.

static u8 precheckClearViewport( void )
{
	if ( s_LoopRunning )
		return GPU64_ERR_BUSY;
	if ( g_pGpu64FB == 0 || !g_pGpu64FB->IsInitialized() )
		return GPU64_ERR_UNSUPPORTED;
	return GPU64_ERR_OK;
}

static u8 precheckDrawMesh( void )
{
	if ( s_LoopRunning )
		return GPU64_ERR_BUSY;
	if ( resFind( stagedId(), GPU64_3D_RES_MESH ) == 0 )
		return GPU64_ERR_BAD_ID;
	if ( g_pGpu64FB == 0 || !g_pGpu64FB->IsInitialized() )
		return GPU64_ERR_UNSUPPORTED;
	if ( argU16( 12 ) == 0 )				// scale
		return GPU64_ERR_BAD_ARGS;
	return GPU64_ERR_OK;
}

// --- the scene graph ($20-$24, $30-$36, $42) -----------------------------
//
// Thin IO2 wrappers, same shape throughout: unpack ARG into the units
// gpu64_3d_scene.h wants, call the portable function, translate its
// GPU64_3D_* code to the GPU64_ERR_* one the C64 side already knows (they
// share numeric values for BAD_ID/BAD_ARGS/OUT_OF_MEMORY/OK by construction,
// so the "translation" is the identity -- spelled out anyway so a future
// divergence between the two enums fails to compile instead of miscoding).

static u8 opCreateObject( void )
{
	return gpu64_3dSceneCreateObject( sceneTarget(), stagedId(), argU16( 0 ) );
}

static u8 opCreateCamera( void )
{
	return gpu64_3dSceneCreateCamera( sceneTarget(), stagedId() );
}

static u8 opCreateSprite( void )
{
	return gpu64_3dSceneCreateSprite( sceneTarget(), stagedId(), argU16( 0 ) );
}

static u8 opCreateLight( void )
{
	return gpu64_3dSceneCreateLight( sceneTarget(), stagedId() );
}

static u8 opSetSprite( void )
{
	return gpu64_3dSceneSetSprite( sceneTarget(), stagedId(),
					argU16( 0 ), argU16( 2 ), sArg[ 4 ] );
}

static u8 opSetPointLight( void )
{
	// Strength is in colormap levels at the centre and radius is 8.8 world
	// units; either being zero turns the light off without destroying it.
	return gpu64_3dSceneSetPointLight( sceneTarget(), stagedId(),
					    sArg[ 0 ], argU16( 1 ) );
}

static u8 opDestroyNode( void )
{
	// gpu64 (2026-09-10, bench run 14): count the one outcome of this opcode
	// that a session cannot recover from on its own. The live scene losing
	// its active camera is what makes LOOP_START and the SCENE_COMMIT re-arm
	// answer NO_CAMERA forever, and run 14 spent a whole 4096-frame run in
	// that state without being able to say whether DESTROY_NODE or
	// SCENE_RESET had put it there. Only the live scene is watched: the
	// shadow's copy is reseeded from it at the next re-arm anyway.
	Gpu64_3dScene *pScene = sceneTarget();
	const boolean bHadCamera = s_Scene.bHaveActiveCamera;
	const u8 res = gpu64_3dSceneDestroyNode( pScene, stagedId() );
	if ( bHadCamera && !s_Scene.bHaveActiveCamera )
		gpu64_apiDiagBump( &gpu64ApiDiag.camLost );
	return res;
}

static u8 opSetActiveCamera( void )
{
	return gpu64_3dSceneSetActiveCamera( sceneTarget(), stagedId() );
}

static u8 opSetVisible( void )
{
	if ( sArg[ 0 ] > 1 )
		return GPU64_ERR_BAD_ARGS;
	return gpu64_3dSceneSetVisible( sceneTarget(), stagedId(), sArg[ 0 ] != 0 );
}

static u8 opSetPosition( void )
{
	Gpu64_3dVec pos;
	pos.x = argS32( 0 );		// wire is already 16.16 -- SET_POSITION places
	pos.y = argS32( 4 );		// a node anywhere in a 65536-unit world, unlike
	pos.z = argS32( 8 );		// DRAW_MESH's 8.8 immediate-mode offset.
	return gpu64_3dSceneSetPosition( sceneTarget(), stagedId(), &pos );
}

static u8 opSetOrientation( void )
{
	return gpu64_3dSceneSetOrientation( sceneTarget(), stagedId(),
					     argU16( 0 ), argU16( 2 ), argU16( 4 ) );
}

static u8 opMoveLocal( void )
{
	return gpu64_3dSceneMoveLocal( sceneTarget(), stagedId(),
					(s32)argS16( 0 ) << 8,		// 8.8 -> 16.16
					(s32)argS16( 2 ) << 8,
					(s32)argS16( 4 ) << 8 );
}

static u8 opMoveWorld( void )
{
	return gpu64_3dSceneMoveWorld( sceneTarget(), stagedId(),
					(s32)argS16( 0 ) << 8,
					(s32)argS16( 2 ) << 8,
					(s32)argS16( 4 ) << 8 );
}

static u8 opRotateLocal( void )
{
	return gpu64_3dSceneRotateLocal( sceneTarget(), stagedId(),
					  argU16( 0 ), argU16( 2 ), argU16( 4 ) );
}

static u8 opSetScale( void )
{
	return gpu64_3dSceneSetScale( sceneTarget(), stagedId(), argU16( 0 ) );
}

static u8 opGetTransform( void )
{
	u8  space;
	u32 addr, len;
	argBlob( 0, &space, &addr, &len );
	if ( len < 18 )
		return GPU64_ERR_BAD_ARGS;

	Gpu64_3dVec pos;
	u16 yaw, pitch, roll;
	const u8 res = gpu64_3dSceneGetTransform( sceneTarget(), stagedId(), &pos, &yaw, &pitch, &roll );
	if ( res != GPU64_ERR_OK )
		return res;

	// Position (s32 16.16 x/y/z) then yaw/pitch/roll (u16 each), little-endian
	// throughout -- the same byte order every other multi-byte field on this
	// bus uses. 18 bytes, matching the wire format's own accounting.
	u8 buf[ 18 ];
	buf[ 0 ] = (u8)( pos.x );        buf[ 1 ] = (u8)( pos.x >> 8 );
	buf[ 2 ] = (u8)( pos.x >> 16 );  buf[ 3 ] = (u8)( pos.x >> 24 );
	buf[ 4 ] = (u8)( pos.y );        buf[ 5 ] = (u8)( pos.y >> 8 );
	buf[ 6 ] = (u8)( pos.y >> 16 );  buf[ 7 ] = (u8)( pos.y >> 24 );
	buf[ 8 ] = (u8)( pos.z );        buf[ 9 ] = (u8)( pos.z >> 8 );
	buf[ 10 ] = (u8)( pos.z >> 16 ); buf[ 11 ] = (u8)( pos.z >> 24 );
	buf[ 12 ] = (u8)( yaw );         buf[ 13 ] = (u8)( yaw >> 8 );
	buf[ 14 ] = (u8)( pitch );       buf[ 15 ] = (u8)( pitch >> 8 );
	buf[ 16 ] = (u8)( roll );        buf[ 17 ] = (u8)( roll >> 8 );

	return gpu64_blobWrite( space, addr, 18, buf );
}

// pNeedsDraw is set FALSE for a parked (SET_VISIBLE(0)) node: that is not an
// error -- it is DRAW_NODE's equivalent of DRAW_MESH's "every face culled"
// result -- but it means gpu64_3dDispatch() must not push anything onto the
// ring for it. RESULT is written here, directly, on core 0, for exactly that
// case; every other path through DRAW_NODE gets its RESULT from core 1's
// gpu64_3dExecuteRender() after the ring drains.
static u8 precheckDrawNode( boolean *pNeedsDraw )
{
	*pNeedsDraw = FALSE;

	if ( s_LoopRunning )
		return GPU64_ERR_BUSY;

	// NODE_NONE here means "any type"; the type test below is what rejects
	// a camera or a light, so that DRAW_NODE on one of those is BAD_ID and
	// not the "no such node" the old OBJECT-only filter reported.
	Gpu64_3dNode *pN = gpu64_3dSceneFind( &s_Scene, stagedId(), GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_ERR_BAD_ID;
	if ( pN->type != GPU64_3D_NODE_OBJECT && pN->type != GPU64_3D_NODE_SPRITE )
		return GPU64_ERR_BAD_ID;

	if ( !pN->visible )
	{
		gpu64Regs.result = 0;
		return GPU64_ERR_OK;
	}

	// A sprite's texture is resolved at draw time, not here: which of the
	// eight directional views it needs depends on the camera, which is
	// applied on core 1 after the drain. A sprite whose texture is missing
	// draws nothing and reports 0, the same as a fully-culled mesh.
	if ( pN->type == GPU64_3D_NODE_OBJECT && resFind( pN->meshId, GPU64_3D_RES_MESH ) == 0 )
		return GPU64_ERR_BAD_ID;
	if ( g_pGpu64FB == 0 || !g_pGpu64FB->IsInitialized() )
		return GPU64_ERR_UNSUPPORTED;

	*pNeedsDraw = TRUE;
	return GPU64_ERR_OK;
}

// --- Stage 15a: core-1-side execution for the three render opcodes ------
//
// Called from gpu64_3d_core1.cpp's execute() -- core 1, not core 0 -- for
// CLEAR_VIEWPORT, DRAW_MESH and DRAW_NODE only, and only after the matching
// precheck*() above has already proved the command legal. pCmd is the ring
// slot itself; this writes pCmd->err and, for the two draws, pCmd->result,
// and never touches gpu64Regs -- core 0 remains the sole writer of the
// C64-visible register file, copying pCmd's fields across itself once its
// drain wait (waitForDrain(), below) succeeds.
//
// The self-warm blocks below are DRAW_NODE's own, moved here verbatim from
// the old opDrawNode() (CLAUDE.md rule 4): core 1 has its own, separate L1
// instruction cache, so a warm that ran on core 0 has no effect here, and
// this function is new enough on core 1 -- it has never executed before its
// first post-15a DRAW_NODE -- to need the same self-warm opDrawNode() did.
// DRAW_MESH's warm is new for the same reason: before 15a, opDrawMesh() had
// been kept warm by continuous use on core 0 since milestone 6, but that
// history is core-0 silicon and does not carry over to core 1's cache.
// gpu64 (2026-09-10, bench run 13): the i-cache warms that used to open this
// function and gpu64_3dExecuteRenderScene() below are GONE, and must not come
// back. Each CACHE_PRELOAD_INSTRUCTION_CACHE + FORCE_READ_LINEARa pair is 32
// PLI plus 2048 *volatile* byte loads that the compiler may not coalesce or
// elide -- and RENDER_SCENE ran three pairs, ~6100 unbroken memory accesses,
// at the top of every frame with no GPU64_3D_YIELD() anywhere in them. That
// is the longest unbroken run of bus traffic left on core 1, and it is the
// standing suspect for run 13's finding: 442 mis-sampled C64 writes with this
// loop rendering against exactly 0 with it stopped, matched denominators.
//
// CLAUDE.md's rule 4 ("cycle-critical code reachable from a command must warm
// its own i-cache") is about core 0, which has a per-C64-cycle deadline. Core
// 1 has none: a cold first frame here is slower and nothing observes it, and
// in the autonomous loop this code runs 50x a second and stays warm by use.
// The warms were paying core 0's insurance premium out of core 0's own
// timing budget.
void gpu64_3dExecuteRender( Gpu64_3dCmd *pCmd )
{
	Gpu64_3dTarget target;

	switch ( pCmd->op )
	{
	case GPU64_3D_OP_CLEAR_VIEWPORT:
		if ( !makeTarget( &target, pCmd->page ) )
		{
			// precheckClearViewport() already proved this can't happen.
			pCmd->err = GPU64_ERR_UNSUPPORTED;
			return;
		}
		gpu64_3dClearViewport( &s_State, &target );
		cleanViewport( pCmd->page );
		pCmd->err = GPU64_ERR_OK;
		return;

	case GPU64_3D_OP_DRAW_MESH:
	{
		const Gpu64_3dResource *pR = resFind( pCmd->id, GPU64_3D_RES_MESH );
		if ( pR == 0 || !makeTarget( &target, pCmd->page ) )
		{
			// precheckDrawMesh() already proved this can't happen.
			pCmd->err = GPU64_ERR_BAD_ID;
			return;
		}

		Gpu64_3dVec pos;
		pos.x = (s32)(s16)( pCmd->arg[ 0 ] | ( pCmd->arg[ 1 ] << 8 ) ) << 8;	// 8.8 -> 16.16
		pos.y = (s32)(s16)( pCmd->arg[ 2 ] | ( pCmd->arg[ 3 ] << 8 ) ) << 8;
		pos.z = (s32)(s16)( pCmd->arg[ 4 ] | ( pCmd->arg[ 5 ] << 8 ) ) << 8;

		Gpu64_3dMat rot;
		const u16 yaw   = (u16)( pCmd->arg[ 6 ]  | ( pCmd->arg[ 7 ]  << 8 ) );
		const u16 pitch = (u16)( pCmd->arg[ 8 ]  | ( pCmd->arg[ 9 ]  << 8 ) );
		const u16 roll  = (u16)( pCmd->arg[ 10 ] | ( pCmd->arg[ 11 ] << 8 ) );
		gpu64_3dMatFromEuler( &rot, yaw, pitch, roll );

		const u16 scale = (u16)( pCmd->arg[ 12 ] | ( pCmd->arg[ 13 ] << 8 ) );

		const unsigned n = gpu64_3dDrawMesh( &s_State, &target, &s_Scratch, &pR->mesh,
						     &pos, &rot, scale, lookupTexture, 0 );
		cleanViewport( pCmd->page );

		// RESULT is the triangle count, saturated to a byte -- see the
		// original opDrawMesh() comment this replaces: it is the difference
		// between "drawn, wrong part of the screen" and "every face culled".
		pCmd->result = (u8)( n > 255 ? 255 : n );
		pCmd->err = GPU64_ERR_OK;
		return;
	}

	case GPU64_3D_OP_DRAW_NODE:
	{
		// Re-resolved by id rather than carried as a pointer from precheck:
		// precheck ran on core 0 before the push, this runs on core 1 after
		// the drain, and nothing about the node or the resource table is
		// locked between the two -- re-finding both here is what makes that
		// safe rather than merely convenient.
		Gpu64_3dNode *pN = gpu64_3dSceneFind( &s_Scene, pCmd->id, GPU64_3D_NODE_NONE );
		if ( pN == 0 || !makeTarget( &target, pCmd->page ) )
		{
			// precheckDrawNode() already proved this can't happen.
			pCmd->err = GPU64_ERR_BAD_ID;
			return;
		}

		// The active camera, if any, is applied fresh on every DRAW_NODE: a
		// program that moves the camera between two DRAW_NODE calls in the
		// same frame must see both nodes drawn from where the camera is
		// *now*, not from wherever it was at some earlier SET_ACTIVE_CAMERA.
		gpu64_3dSceneApplyCamera( &s_Scene, &s_State );

		// Lights are view-space, so they have to be re-gathered after the
		// camera and never before it (gpu64_3dSceneApplyLights()'s own
		// contract). Cheap enough to redo per DRAW_NODE: it is a walk of the
		// node table, not a draw.
		gpu64_3dSceneApplyLights( &s_Scene, &s_State );

		unsigned n;
		if ( pN->type == GPU64_3D_NODE_SPRITE )
		{
			n = gpu64_3dDrawSprite( &s_State, &target, &pN->pos,
						 pN->spriteW, pN->spriteH, pN->texId,
						 pN->yaw, pN->spriteFlags, lookupTexture, 0 );
		} else
		{
			const Gpu64_3dResource *pR = resFind( pN->meshId, GPU64_3D_RES_MESH );
			if ( pR == 0 )
			{
				pCmd->err = GPU64_ERR_BAD_ID;
				return;
			}
			n = gpu64_3dDrawMesh( &s_State, &target, &s_Scratch, &pR->mesh,
					       &pN->pos, &pN->rot, pN->scale, lookupTexture, 0 );
		}
		cleanViewport( pCmd->page );

		pCmd->result = (u8)( n > 255 ? 255 : n );
		pCmd->err = GPU64_ERR_OK;
		return;
	}

	default:
		// Unreachable: gpu64_3dDispatch() only ever pushes the three render
		// ops above onto the ring for gpu64_3dExecuteRender() to see.
		pCmd->err = GPU64_ERR_BAD_OPCODE;
		return;
	}
}

// --- Stage 16: core-1-side execution for the autonomous loop's own frame -
//
// One whole frame: clear the viewport, apply the active camera once, then
// draw every visible OBJECT node in scene order. Same contract as
// gpu64_3dExecuteRender() above -- writes pCmd->err/pCmd->result, never
// touches gpu64Regs. Reads s_Scene/s_State directly, exactly as DRAW_NODE
// does, which stays safe for the same reason DRAW_NODE was: at most one
// GPU64_3D_OP_RENDER_SCENE is ever in flight (LOOP_START/SCENE_COMMIT never
// push a second one before the first has drained -- see pollLoopFrame()),
// and the shadow copy is what stops a C64 mutator from racing this read
// while this frame is in flight (see s_ShadowScene/s_ShadowState above).
//
// A mesh a live node points at that no longer resolves (freed out from
// under it) is skipped, not a failure -- one bad node must not blank an
// otherwise-good autonomous frame the way it would a single explicit
// DRAW_NODE, which the C64 can see failed and retry.
void gpu64_3dExecuteRenderScene( Gpu64_3dCmd *pCmd )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	Gpu64_3dTarget target;
	if ( pFB == 0 || !makeTarget( &target, pCmd->page ) )
	{
		// LOOP_START already proved a framebuffer exists before it ever
		// pushed this op.
		pCmd->err = GPU64_ERR_UNSUPPORTED;
		return;
	}

	gpu64_3dSceneRender( &s_Scene, &s_State, &target, &s_Scratch,
			      lookupMesh, 0, lookupTexture, 0,
			      levelVisLookup, 0 );

	cleanViewport( pCmd->page );

	// The page this frame was actually rendered into, which is what the C64
	// reads back as RESULT after SCENE_COMMIT. Not GetDrawPage(): by the time
	// this line runs, core 0 may have advanced the draw page for the *next*
	// frame, and answering with that one told the C64 the wrong page had been
	// drawn (Finding 2).
	pCmd->result = pCmd->page;
	pCmd->err = GPU64_ERR_OK;
}

// --- dispatch -----------------------------------------------------------

// gpu64 (2026-09-10): everything LOOP_START does once its mode argument has
// been accepted. Factored out because SCENE_COMMIT now calls it too -- see
// its own case below for why a commit against a stopped loop re-arms the
// loop instead of refusing for the rest of the session.
static u8 loopStartArmed( void )
{
	if ( g_pGpu64FB == 0 || !g_pGpu64FB->IsInitialized() )
		return GPU64_ERR_UNSUPPORTED;
	// Enforced here, once, at the moment a render is actually asked for --
	// not per frame inside gpu64_3dExecuteRenderScene(), which (like
	// gpu64_3dSceneApplyCamera() it calls) has no precedent anywhere else in
	// this file for failing a render outright over a missing camera; it falls
	// back to the identity view instead. A camera destroyed mid-loop by a
	// later, shadow-redirected DESTROY_NODE is deliberately not re-checked
	// per commit for the same reason.
	if ( !s_Scene.bHaveActiveCamera )
		return GPU64_ERR_NO_CAMERA;

	// The shadow starts as a copy of the live session: the loop's first
	// frame, and every isShadowRedirectable() edit up to the first
	// SCENE_COMMIT, must still see whatever the C64 already set up before
	// LOOP_START. Correct for a re-arm too: while the loop was stopped,
	// sceneForEdit() was handing out the live scene, so the shadow is the
	// stale copy of the pair and reseeding it is what makes them agree.
	s_ShadowScene = s_Scene;
	s_ShadowState = s_State;

	gpu64Regs.status &= ~GPU64_STATUS_FRAME_READY;

	if ( !pushLoopFrame() )
		return GPU64_ERR_QUEUE_FULL;

	s_LoopRunning = TRUE;
	return GPU64_ERR_OK;
}

static u8 execute( u8 op )
{
	// gpu64 (2026-09-10, bench run 14): one gate, in front of every class 1
	// opcode that destroys state the session cannot rebuild from the next
	// frame's commands. gpu64_api.h has the contract and why it exists; the
	// short version is that two bench runs have caught an opcode executing
	// that the C64 never sent, and on a destructive one that ends the
	// session with nothing to show for it.
	//
	// All four, not just the two the 2026-09-10 morning's version gated, and
	// unconditionally rather than only while the loop runs. A DESTROY_NODE
	// on the active camera is as fatal as a SCENE_RESET -- run 14 could not
	// say which of the two had fired, and that ambiguity is itself the
	// argument for treating them alike. The cost is one ARG write per call
	// at eleven call sites in the whole tree.
	switch ( op )
	{
	case GPU64_3D_OP_SCENE_RESET:
	case GPU64_3D_OP_LOOP_STOP:
	case GPU64_3D_OP_DESTROY_NODE:
	case GPU64_3D_OP_FREE_RESOURCE:
	case GPU64_3D_OP_LOAD_LEVEL:
		if ( !gpu64_apiKeyed() )
		{
			// BAD_ARGS and not a silent OK: a caller whose ARG15 write was
			// lost to the sampling defect has to be able to find out and
			// retry, and ERRCODE is the only channel it has. The opcode is
			// latched because that is the reading run 14 needed and did not
			// have -- a phantom names itself here.
			gpu64_apiDiagBump( &gpu64ApiDiag.keyRefused );
			gpu64ApiDiag.keyRefusedOp = op;
			return GPU64_ERR_BAD_ARGS;
		}
		break;
	default:
		break;
	}

	switch ( op )
	{
	case GPU64_3D_OP_SCENE_RESET:
		// gpu64 (2026-09-10): count the wipes that get through, so a run
		// can tell "the key prevented it" from "it never happened". Only
		// the ones that had something to destroy: the setup-time reset
		// every program opens with is not an event.
		if ( s_Scene.bHaveActiveCamera )
			gpu64_apiDiagBump( &gpu64ApiDiag.sceneWipes );
		// "Destroys every node, stops the loop, leaves uploaded resources
		// alone" -- docs/class1-3d-mesh-reference.md. Stage 16: the loop
		// now exists, so this must actually stop it and drop its shadow
		// copy, not just the live session -- otherwise a RESET followed
		// by a fresh LOOP_START would seed the new loop's shadow from a
		// stale one instead of from the session gpu64_3dStateDefaults()/
		// gpu64_3dSceneReset() just produced.
		gpu64_3dStateDefaults( &s_State );
		gpu64_3dSceneReset( &s_Scene );
		loopStop( GPU64_LOOPSTOP_SCENE_RESET );
		s_LoopFramePending = FALSE;
		gpu64Regs.status &= ~GPU64_STATUS_FRAME_READY;
		gpu64_3dStateDefaults( &s_ShadowState );
		gpu64_3dSceneReset( &s_ShadowScene );
		// The scene this load was populating is gone; finishing it would
		// add nodes to a scene the caller just asked to be empty.
		levelLoadAbort();
		return GPU64_ERR_OK;

	case GPU64_3D_OP_SET_VIEWPORT:		return opSetViewport();
	case GPU64_3D_OP_SET_PERSPECTIVE:	return opSetPerspective();
	case GPU64_3D_OP_SET_LIGHT:		return opSetLight();
	case GPU64_3D_OP_BUILD_COLORMAP:	return opBuildColormap();

	case GPU64_3D_OP_SET_BACKGROUND:
		stateTargetForEdit()->background = sArg[ 0 ];
		return GPU64_ERR_OK;

	case GPU64_3D_OP_ARENA_STATUS:
	{
		// Free arena in 128 KB units. A 32 MB arena is exactly 256 of them,
		// so a full arena reads 255 (saturated) and an exhausted one reads 0
		// -- the whole range is usable and no scaling constant has to be
		// agreed with the C64 side beyond "128 KB".
		const u32 free = GPU64_3D_ARENA_BYTES - gpu64_3dArenaUsed();
		const u32 units = free >> 17;
		gpu64Regs.result = (u8)( units > 255 ? 255 : units );
		return GPU64_ERR_OK;
	}

	case GPU64_3D_OP_LOOP_STOP:
		// Keyed by the gate at the top of this function.
		//
		// Always succeeds, even against a loop that was not running --
		// answering an error instead would make a correctly written
		// program's teardown call fail. Deliberately leaves
		// s_LoopFramePending alone: a frame already queued when LOOP_STOP
		// arrives still finishes on core 1 regardless (there is no
		// ring-cancel primitive, and letting it finish is cheaper than
		// building one just to discard the result), and a later
		// pollLoopFrame() still harvests it -- harmlessly, since nothing
		// reads GPU64_STATUS_FRAME_READY once the loop is not running.
		loopStop( GPU64_LOOPSTOP_OPCODE );
		return GPU64_ERR_OK;

	case GPU64_3D_OP_UPLOAD_MESH:		return opUploadMesh();
	case GPU64_3D_OP_UPLOAD_TEXTURE:	return opUploadTexture();
	case GPU64_3D_OP_FREE_RESOURCE:		return opFreeResource();

	// milestone 18. LOAD_LEVEL is keyed by the gate above; LEVEL_STEP is
	// not, because it destroys nothing a phantom could not cause by simply
	// arriving early -- an unarmed LEVEL_STEP answers BAD_ARGS and a
	// duplicate one only re-does a slice of the load already in progress.
	case GPU64_3D_OP_LOAD_LEVEL:		return opLoadLevel();
	case GPU64_3D_OP_LEVEL_STEP:		return opLevelStep();
	// Unkeyed and, alone among the non-render opcodes, exempt from the
	// pre-execute drain -- see isDrainExempt() and gpu64_3d.h.
	case GPU64_3D_OP_CLIP_MOVE:		return opClipMove();
	case GPU64_3D_OP_LEVEL_ENT:		return opLevelEnt();
	case GPU64_3D_OP_LEVEL_PALETTE:		return opLevelPalette();
	case GPU64_3D_OP_LEVEL_NODE:		return opLevelNode();
	case GPU64_3D_OP_LEVEL_PICTURE:		return opLevelPicture();
	case GPU64_3D_OP_WORLD_TICK:		return opWorldTick();

	case GPU64_3D_OP_CREATE_OBJECT:		return opCreateObject();
	case GPU64_3D_OP_CREATE_CAMERA:		return opCreateCamera();
	case GPU64_3D_OP_DESTROY_NODE:		return opDestroyNode();
	case GPU64_3D_OP_SET_ACTIVE_CAMERA:	return opSetActiveCamera();
	case GPU64_3D_OP_SET_VISIBLE:		return opSetVisible();
	case GPU64_3D_OP_CREATE_SPRITE:		return opCreateSprite();
	case GPU64_3D_OP_CREATE_LIGHT:		return opCreateLight();
	case GPU64_3D_OP_SET_SPRITE:		return opSetSprite();
	case GPU64_3D_OP_SET_POINT_LIGHT:	return opSetPointLight();

	case GPU64_3D_OP_SET_POSITION:		return opSetPosition();
	case GPU64_3D_OP_SET_ORIENTATION:	return opSetOrientation();
	case GPU64_3D_OP_MOVE_LOCAL:		return opMoveLocal();
	case GPU64_3D_OP_MOVE_WORLD:		return opMoveWorld();
	case GPU64_3D_OP_ROTATE_LOCAL:		return opRotateLocal();
	case GPU64_3D_OP_SET_SCALE:		return opSetScale();
	case GPU64_3D_OP_GET_TRANSFORM:		return opGetTransform();

	// CLEAR_VIEWPORT, DRAW_MESH and DRAW_NODE are not handled here as of
	// Stage 15a: gpu64_3dDispatch() intercepts all three before execute() is
	// ever called, running a precheck*() on core 0 and, once that passes,
	// pushing them to run on core 1 (gpu64_3dExecuteRender()). They fall
	// through to BAD_OPCODE below if execute() is ever reached with one,
	// which should not happen.

	case GPU64_3D_OP_LOOP_START:
		if ( s_LoopRunning )
			return GPU64_ERR_BUSY;
		// ARG[0] selects the mode: 0 is handshake (SCENE_COMMIT-driven,
		// what this build implements), 1 is free-running vsync-driven,
		// which is Stage 18 -- see dev/gap_filling_plan.md. Answering
		// UNSUPPORTED for mode 1 rather than silently treating it as mode
		// 0 keeps a program written against a future firmware from
		// running the wrong protocol against an old one without noticing.
		if ( sArg[ 0 ] != 0 )
			return GPU64_ERR_UNSUPPORTED;
		return loopStartArmed();

	case GPU64_3D_OP_SCENE_COMMIT:
	{
		if ( !s_LoopRunning )
		{
			// gpu64 (2026-09-10): this used to answer UNSUPPORTED, and it
			// is the refusal both bench runs of that day spent most of
			// their frames in -- a stopped loop was permanent, the HDMI
			// picture froze, and the C64 had no way to tell that from a
			// busy frame. The loop is now what the C64 side always
			// assumed it was: running for as long as commits keep
			// arriving. A commit against a stopped loop re-arms it and
			// reports the re-arm as OK, which makes every spurious stop
			// self-healing within one frame instead of fatal.
			//
			// A deliberate teardown is unaffected: LOOP_STOP is followed
			// by the program exiting, so no commit arrives to undo it.
			// The counter still climbs, so GET_HEALTH 80-83 (commitNoLoop)
			// remains the measure of how often this happened -- it is now
			// a repair count rather than a failure count, and paired with
			// loopStops (88-91) and loopStopCause (87) it still says what
			// stopped the loop each time.
			gpu64_apiDiagBump( &gpu64ApiDiag.commitNoLoop );
			gpu64ApiDiag.lastRefuseState = gpu64_apiDiagStateByte();
			gpu64ApiDiag.lastRefuseOp    = GPU64_3D_OP_SCENE_COMMIT;
			gpu64ApiDiag.lastRefuseClass = 1;
			return loopStartArmed();
		}

		// gpu64_3dDispatch()'s generic branch already drained the ring
		// unconditionally before calling execute() -- SCENE_COMMIT is
		// deliberately absent from isShadowRedirectable() -- so the frame
		// pushLoopFrame() queued last time is guaranteed finished by now.
		// That drain does not itself harvest RESULT/FRAME_READY, only
		// pollLoopFrame() does, so call it explicitly rather than assume
		// an earlier, incidental call already did.
		pollLoopFrame();
		if ( !( gpu64Regs.status & GPU64_STATUS_FRAME_READY ) )
			// Should not happen given the drain above; kept as a
			// stated precondition rather than an assumption.
			return GPU64_ERR_BUSY;

		// Same armed-branch pattern as PAGE_FLIP (gpu64_api.cpp's
		// doSystem()) -- SCENE_COMMIT's flip is the same flip, just
		// driven by the loop instead of by the C64 calling PAGE_FLIP
		// itself.
		if ( !gpu64Vsync.calibrated )
		{
			gpu64_apiDiagBump( &gpu64ApiDiag.commitNoClock );
			gpu64ApiDiag.lastRefuseState = gpu64_apiDiagStateByte();
			gpu64ApiDiag.lastRefuseOp    = GPU64_3D_OP_SCENE_COMMIT;
			gpu64ApiDiag.lastRefuseClass = 1;
			return GPU64_ERR_UNSUPPORTED;
		}
		if ( gpu64Vsync.flipPending )
			return GPU64_ERR_BUSY;
		CGpu64FrameBuffer *pFB = g_pGpu64FB;
		if ( pFB == 0 )
		{
			gpu64ApiDiag.lastRefuseState = gpu64_apiDiagStateByte();
			gpu64ApiDiag.lastRefuseOp    = GPU64_3D_OP_SCENE_COMMIT;
			gpu64ApiDiag.lastRefuseClass = 1;
			return GPU64_ERR_UNSUPPORTED;
		}
		// The page this commit would hand core 1 must be off the screen.
		// The previous flip's post only lands at the VideoCore's next vsync,
		// so after a light frame the commit can arrive while the page it
		// retired is still being scanned -- core 1 then clears it to the
		// sky colour and paints it in full view. That was the half-drawn
		// frame Stunt showed at the bench on 2026-09-27, one refresh at a
		// time. Nothing is published, so the program's retry is the same
		// commit a moment later -- the same contract as flipPending above.
		if ( !pFB->NextDrawPageIdle() )
		{
			gpu64ApiDiag.commitScanBusy++;
			return GPU64_ERR_BUSY;
		}

		// Publish: everything isShadowRedirectable() redirected since
		// LOOP_START (or the last commit) becomes what the *next* frame
		// renders from.
		s_Scene = s_ShadowScene;
		s_State = s_ShadowState;

		pFB->PrepareFlip();
		gpu64_vsyncWarmCommit();
		gpu64Vsync.flipPending = 1;
		gpu64Regs.status |= GPU64_STATUS_BUSY;
		gpu64Regs.status &= ~GPU64_STATUS_FRAME_READY;

		if ( !pushLoopFrame() )
			return GPU64_ERR_QUEUE_FULL;

		s_CommitSeen   = TRUE;
		s_LastCommitUs = gpu64_vsyncNow();
		return GPU64_ERR_OK;
	}
	}

	return GPU64_ERR_BAD_OPCODE;
}

static boolean isRenderOp( u8 op )
{
	return op == GPU64_3D_OP_CLEAR_VIEWPORT
	    || op == GPU64_3D_OP_DRAW_MESH
	    || op == GPU64_3D_OP_DRAW_NODE;
}

// Generous on purpose: this is a backstop against a wedged core 1, not a
// tuned budget -- see dev/gap_filling_plan.md's Stage 15 section. 50ms is
// a large multiple of any real draw this arena's size limit allows, and the
// cost of guessing too high is a slightly later ERRCODE on a build that was
// already broken; the cost of guessing too low is a spurious WORKER_TIMEOUT
// on a slow-but-fine frame.
#define GPU64_3D_DRAIN_TIMEOUT_US	50000

// Spins core 0 until the ring drains to head, or the timeout backstop fires.
// This is not the MMIO poll CLAUDE.md's multicore section forbids from a
// second core -- gpu64_3dRing.tail is a DRAM location, not a VideoCore
// peripheral register, and this runs on core 0 itself, watching core 1, which
// is the direction that rule never applied to. CNTVCT_EL0/CNTFRQ_EL0 rather
// than the BCM system timer for the same reason gpu64_ladder.cpp uses them:
// the only clock safe to read off core 0's own silicon.
static boolean waitForDrain( u32 head )
{
	u64 freq;
	asm volatile( "MRS %0, CNTFRQ_EL0" : "=r" (freq) );
	const u64 timeoutTicks = ( freq / 1000000ULL ) * GPU64_3D_DRAIN_TIMEOUT_US;

	u64 start;
	asm volatile( "MRS %0, CNTVCT_EL0" : "=r" (start) );

	while ( gpu64_3dRing.tail != head )
	{
		asm volatile( "YIELD" );

		u64 now;
		asm volatile( "MRS %0, CNTVCT_EL0" : "=r" (now) );
		if ( now - start > timeoutTicks )
			return gpu64_3dRing.tail == head;	// one last look before giving up
	}
	return TRUE;
}

// --- Stage 15b: observation points ---------------------------------------
//
// Under 15a every dispatch drained the ring before returning, so the ring
// never held more than one entry and "wait for the ring to empty" and "wait
// for this op's own completion" were the same thing. 15b lets CLEAR_VIEWPORT/
// DRAW_MESH/DRAW_NODE queue and return without waiting (below), which is the
// whole point -- a frame of DRAW_NODE calls now overlaps the C64 free-running
// instead of stalling on every one -- but it breaks that equivalence, and
// with it docs/class1-3d-mesh-reference.md's old "RESULT is valid the instant
// dispatch returns" contract for those three opcodes.
//
// What replaces it: RESULT is valid as of the last *observation point* --
// anywhere this file or gpu64_api.cpp forces the ring to fully drain before
// going on to do something that would otherwise race a still-in-flight
// render. That is every non-render class 1 opcode below (they mutate
// s_State/s_Scene/s_Res, which a queued render also reads), plus, from
// gpu64_api.cpp, a page flip/vsync commit and any class 0 opcode that touches
// the framebuffer -- see gpu64_3dSync() at the bottom of this section. A
// program that wants a DRAW_NODE's own RESULT right now (the debugging
// workflow docs/class1-3d-mesh-reference.md calls out: "check RESULT after
// your first upload of any new mesh") gets it by issuing any one of those as
// the next call and then reading RESULT -- it does not need a dedicated
// "sync" opcode, because ordinary programs already do this constantly.
//
// s_LastRenderSlot/s_LastRenderPending are core-0-private bookkeeping: which
// ring slot the most recently pushed render op landed in, and whether it has
// been flushed to gpu64Regs.result yet. Nothing on core 1 reads either.
static u32     s_LastRenderSlot;
static boolean s_LastRenderPending;

// Waits for the ring to fully drain, then -- if a render op was pushed since
// the last flush -- copies its RESULT across, exactly as gpu64_3dDispatch()
// used to do inline for every DRAW_MESH/DRAW_NODE before 15b. Returns FALSE
// only on the GPU64_3D_DRAIN_TIMEOUT_US backstop (a wedged core 1).
static boolean drainAndFlush( void )
{
	if ( !waitForDrain( gpu64_3dRing.head ) )
		return FALSE;

	// Pairs with the DMB gpu64_3dWorker() issues (gpu64_3d_core1.cpp) after
	// execute() writes err/result and before it publishes tail -- without
	// this, having observed tail == head does not by itself guarantee this
	// core sees the slot's new contents.
	asm volatile( "DMB ISH" ::: "memory" );

	if ( s_LastRenderPending )
	{
		const Gpu64_3dCmd *pSlot = &gpu64_3dRing.slot[ s_LastRenderSlot ];

		// Not for CLEAR_VIEWPORT: that opcode never touched RESULT before
		// Stage 15a either, and copying pSlot->result here would leak
		// whatever stale value a previous DRAW_MESH/DRAW_NODE left in this
		// slot (slots are a ring, reused, not per-opcode storage).
		if ( pSlot->op != GPU64_3D_OP_CLEAR_VIEWPORT )
			gpu64Regs.result = pSlot->result;
		s_LastRenderPending = FALSE;
	}
	return TRUE;
}

// gpu64_api.cpp's observation-point hook: a page flip/vsync commit or a
// class 0 opcode that touches the framebuffer must not run ahead of a
// render this file has queued but not yet executed on core 1 -- see the
// comment above drainAndFlush(). A no-op (returns TRUE immediately) whenever
// GPU64_3D_ENABLED is off, so callers do not need their own #ifdef for the
// common case; they still need one around the call itself, the same as
// every other gpu64_3d.h entry point, because this whole file is compiled
// out with the toggle.
boolean gpu64_3dSync( void )
{
	pollLoopFrame();
	return drainAndFlush();
}

// --- Stage 16: the autonomous loop's own frame handoff --------------------
//
// pushLoopFrame() and pollLoopFrame() are drainAndFlush()'s counterparts for
// GPU64_3D_OP_RENDER_SCENE: the same slot/target bookkeeping
// s_LastRenderSlot/s_LastRenderPending use for DRAW_MESH/DRAW_NODE, kept
// separate (as s_LoopFrameSlot/s_LoopFrameTarget/s_LoopFramePending) because
// they gate a different C64-visible contract -- GPU64_STATUS_FRAME_READY and
// RESULT-as-"the page SCENE_COMMIT just rendered", not a triangle count.

// Pushes one GPU64_3D_OP_RENDER_SCENE and arms pollLoopFrame() to notice when
// it finishes. Called from LOOP_START and from SCENE_COMMIT's armed branch,
// each exactly once, always with s_LoopFramePending already FALSE by
// construction -- LOOP_START only runs when the loop was not running, and
// SCENE_COMMIT only reaches this call after pollLoopFrame() has just cleared
// it -- so the ring never carries more than one RENDER_SCENE at a time,
// matching gpu64_3dExecuteRenderScene()'s own "at most one in flight"
// comment. FALSE only on QUEUE_FULL, which the caller reports as such -- the
// ring being full of C64-issued traffic at the exact instant the loop wants
// to queue its own frame is a real, reportable condition, not swallowed.
static boolean pushLoopFrame( void )
{
	const u32 slot = gpu64_3dRing.head;
	// The page is decided here, on core 0, with the C64 halted for this
	// dispatch -- and, at SCENE_COMMIT's call site, *after* PrepareFlip() has
	// already handed the draw page over. That ordering is the fix for
	// Finding 2: the page this frame renders into is by construction neither
	// the one on screen nor the one queued to replace it.
	if ( !gpu64_3dRingPush( GPU64_3D_OP_RENDER_SCENE, currentDrawPage() ) )
		return FALSE;
	s_LoopFrameSlot = slot;
	s_LoopFrameTarget = gpu64_3dRing.head;
	s_LoopFramePending = TRUE;
	return TRUE;
}

// The non-blocking counterpart to drainAndFlush(): one modular-distance test
// against the ring's tail, cheap enough to call from every dispatch (gpu64_3dDispatch()'s
// own top, below, and gpu64_3dSync() above) without turning every command the
// C64 sends while the loop runs into a drain -- the whole point of Stage 16's
// shadow redirection. Never blocks, never spins. Harvests the just-finished
// frame's page number into gpu64Regs.result and raises
// GPU64_STATUS_FRAME_READY the moment core 1 publishes tail == target; does
// not consult pCmd->err the same way drainAndFlush() does not for its render
// ops -- LOOP_START/SCENE_COMMIT already validated the framebuffer exists
// before ever pushing, so gpu64_3dExecuteRenderScene() has nothing left to
// fail on.
static inline void pollLoopFrame( void )
{
	if ( !s_LoopFramePending )
		return;

	// gpu64_3dRing.tail is a masked ring index (wraps mod
	// GPU64_3D_RING_ENTRIES), not a monotonic counter -- so an exact
	// equality test against s_LoopFrameTarget is not safe. A drain
	// triggered by an *unrelated* ring push (a second LOOP_START while one
	// is already running is exactly this test suite's own step 6, "LOOP_START
	// again while running" -- gpu64_3dDispatch()'s non-render branch pushes
	// that op onto the ring and drains unconditionally, before execute()
	// ever gets to reject it with BUSY) can walk tail straight past
	// s_LoopFrameTarget without ever landing on it exactly, orphaning
	// s_LoopFramePending forever: SCENE_COMMIT then answers BUSY on every
	// subsequent call (line ~1040 below), and the render the C64 last
	// queued never gets harvested, freezing the HDMI side while the C64
	// program keeps running. Test "has tail reached or passed target" in
	// modular space instead -- safe because real per-dispatch progress is a
	// handful of slots, nowhere near the half-ring wraparound ambiguity
	// point.
	u32 distance = ( gpu64_3dRing.tail - s_LoopFrameTarget ) & GPU64_3D_RING_MASK;
	if ( distance >= GPU64_3D_RING_ENTRIES / 2 )
		return;

	// Pairs with the DMB gpu64_3dWorker() issues (gpu64_3d_core1.cpp) after
	// execute() writes err/result and before it publishes tail -- same
	// pairing drainAndFlush() relies on above.
	asm volatile( "DMB ISH" ::: "memory" );

	const Gpu64_3dCmd *pSlot = &gpu64_3dRing.slot[ s_LoopFrameSlot ];
	gpu64Regs.result = pSlot->result;
	gpu64Regs.status |= GPU64_STATUS_FRAME_READY;
	s_LoopFramePending = FALSE;
}

// The frame-clock entry point. See gpu64_3d.h for why this exists and why
// gpu64_vsyncCommitFlip()'s DMA hold is the only place allowed to call it.
void gpu64_3dPollLoopFrame( void )
{
	pollLoopFrame();
}

u8 gpu64_3dDispatch( u8 op )
{
	u8 res;

	// Stage 16: pump the autonomous loop's handoff forward on every single
	// dispatch, not just SCENE_COMMIT -- a program polling STATUS between
	// SCENE_COMMIT calls must see GPU64_STATUS_FRAME_READY as soon as core 1
	// actually finishes, not only once it happens to issue a SCENE_COMMIT.
	// Cheap: see pollLoopFrame()'s own comment for why this never blocks.
	pollLoopFrame();

	// gpu64_3dHost.pushed is documented as "commands accepted onto the ring"
	// (gpu64_3d_internals.h). Before Stage 15a that was every non-QUEUE_FULL
	// dispatch, because every accepted op was pushed unconditionally, before
	// validation. Hazard #2's fix (dev/gap_filling_plan.md, Stage 15)
	// means a render op that fails its precheck, or a parked DRAW_NODE that
	// needs no draw, now never touches the ring at all -- didPush tracks
	// that exactly, so the counter keeps meaning what its comment says.
	boolean didPush = FALSE;

	if ( isRenderOp( op ) )
	{
		// Stage 15a: precheck on core 0 before the command ever reaches the
		// ring -- see the precheck*() comment above for why this order
		// matters now that core 1 actually executes what it drains, instead
		// of only counting it.
		boolean needsDraw = TRUE;

		if ( op == GPU64_3D_OP_CLEAR_VIEWPORT )
			res = precheckClearViewport();
		else if ( op == GPU64_3D_OP_DRAW_MESH )
			res = precheckDrawMesh();
		else
			res = precheckDrawNode( &needsDraw );

		if ( res == GPU64_ERR_OK && needsDraw )
		{
			u32 slot = gpu64_3dRing.head;

			if ( !gpu64_3dRingPush( op, currentDrawPage() ) )
			{
				// QUEUE_FULL backpressure (Stage 15b): wait for the ring to
				// fully drain, bus held, rather than reject outright -- a
				// C64 program drawing faster than core 1 renders should see
				// its draws queue, not fail. gap_filling_plan.md's Stage 15b
				// QUEUE_FULL note prefers this to an outright reject, with
				// GPU64_3D_DRAIN_TIMEOUT_US as the backstop against a
				// wedged core 1. A ring that has just fully drained cannot
				// immediately be full again -- core 0 is the only producer,
				// and it is not doing anything else meanwhile -- so the
				// retried push below is not itself allowed to fail.
				if ( !drainAndFlush() )
				{
					gpu64_3dHost.rejected++;
					res = GPU64_ERR_WORKER_TIMEOUT;
				}
				else
				{
					slot = gpu64_3dRing.head;
					if ( gpu64_3dRingPush( op, currentDrawPage() ) )
					{
						didPush = TRUE;
					}
					else
					{
						// Unreachable in practice -- a ring that just fully
						// drained cannot be full again one push later, core
						// 0 being the only producer -- but fail safely
						// rather than assume it.
						gpu64_3dHost.rejected++;
						res = GPU64_ERR_QUEUE_FULL;
					}
				}
			}
			else
			{
				didPush = TRUE;
			}

			if ( didPush )
			{
				// Stage 15b: push and return -- this draw's own completion
				// is not waited for here. RESULT/ERRCODE for *this specific
				// call* become valid at the next observation point; see the
				// comment above drainAndFlush().
				s_LastRenderSlot = slot;
				s_LastRenderPending = TRUE;
			}
		}
	}
	else
	{
		// The ring is fed first, and a full ring refuses the command
		// outright: the design's rule is that a failed dispatch does
		// nothing, so a command must not execute and then be reported as
		// rejected.
		if ( !gpu64_3dRingPush( op, currentDrawPage() ) )
		{
			gpu64_3dHost.rejected++;
			return GPU64_ERR_QUEUE_FULL;
		}
		didPush = TRUE;

		// Stage 15b: execute(), below, is about to mutate s_State/s_Scene/
		// s_Res synchronously on core 0 -- and a render queued earlier, not
		// yet executed, reads exactly that state on core 1. Draining first
		// turns this from a race into an observation point: a draw issued
		// before this dispatch always sees the state that was live when
		// *it* was issued, and this dispatch's own mutation is only visible
		// to draws issued after it returns. This has the same ordering
		// effect as literally sequencing every one of these ~15 opcodes
		// through the ring the way the three render ops are, without adding
		// a second, core-1-side execution path for opcodes that still only
		// ever run on core 0 -- gap_filling_plan.md's Stage 15b "sequence
		// the state opcodes through the ring too" note.
		//
		// Stage 16: while the autonomous loop runs, an isShadowRedirectable()
		// opcode's handler writes only s_ShadowScene/s_ShadowState (see
		// sceneTarget()/stateTargetForEdit()), never s_Scene/s_State/s_Res --
		// exactly the state GPU64_3D_OP_RENDER_SCENE reads on core 1 -- so it
		// no longer races a frame the loop has queued, and the drain is not
		// needed for it. This is what actually delivers the loop's promised
		// concurrency: without this, every SET_POSITION issued while the loop
		// runs would still stall behind whatever frame core 1 is mid-render,
		// exactly the cost the shadow copy exists to remove.
		if ( !( s_LoopRunning && isDrainExempt( op ) ) )
		{
			if ( !drainAndFlush() )
				return GPU64_ERR_WORKER_TIMEOUT;
		}

		res = execute( op );
	}

	// gpu64: put back what this dispatch just evicted, while the bus is still
	// held. A DRAW_MESH walks a framebuffer, a z-buffer and the arena, which
	// is far more than the class 0 CLEAR that was already enough to evict
	// warmCache()'s work -- rule 4 in dev/progress_tracker.md's polling-loop
	// timing rules, "preloaded at start-up is not durable". Doing it here
	// rather than in the loop is rule 5, and it is what makes the cost free:
	// it lands inside the window the C64 is already stopped for.
	//
	// Unconditional rather than only after the heavy opcodes: a dispatch is
	// the biggest instruction-cache consumer in the system whatever it does,
	// and the warm is two data preloads and one window preload against a
	// dispatch that has already paid for a DMA round trip.
	gpu64_apiWarmPollingLoop();

	if ( res == GPU64_ERR_BAD_OPCODE )
		gpu64_3dHost.badOpcode++;
	else if ( didPush )
		gpu64_3dHost.pushed++;

	return res;
}

char *gpu64_3dReport( char *p )
{
	static const char hex[] = "0123456789ABCDEF";

	// Deliberately terse: this goes into the HDMI log overlay, which is 40
	// columns.
	const char *pLabel = "3D ok/rej/bad ";
	while ( *pLabel )
		*p++ = *pLabel++;

	u32 v[ 3 ] = { gpu64_3dHost.pushed, gpu64_3dHost.rejected, gpu64_3dHost.badOpcode };
	for ( unsigned i = 0; i < 3; i++ )
	{
		if ( i ) *p++ = '/';
		*p++ = hex[ ( v[ i ] >> 12 ) & 0xf ];
		*p++ = hex[ ( v[ i ] >>  8 ) & 0xf ];
		*p++ = hex[ ( v[ i ] >>  4 ) & 0xf ];
		*p++ = hex[   v[ i ]         & 0xf ];
	}

	*p++ = ' ';
	*p++ = 'c';
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >> 12 ) & 0xf ];
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >>  8 ) & 0xf ];
	*p++ = hex[ ( gpu64_3dWorkerStats.consumed >>  4 ) & 0xf ];
	*p++ = hex[   gpu64_3dWorkerStats.consumed         & 0xf ];

	// Arena use in KB, so an upload that silently failed to land is visible
	// without a second command.
	*p++ = ' ';
	*p++ = 'a';
	const u32 kb = gpu64_3dArenaUsed() >> 10;
	*p++ = hex[ ( kb >> 12 ) & 0xf ];
	*p++ = hex[ ( kb >>  8 ) & 0xf ];
	*p++ = hex[ ( kb >>  4 ) & 0xf ];
	*p++ = hex[   kb         & 0xf ];

	return p;
}

#endif	// GPU64_3D_ENABLED

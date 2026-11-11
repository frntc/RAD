// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
//
// gpu64_level.h
//
// The .g64lev level container: a whole 3D level (geometry, textures,
// entities and collision hulls) in one file the Pi holds in the REU image,
// so that putting a level on the HDMI screen costs the C64 two commands
// instead of 620 KB of REU uploads it does not have room for.
//
// Written by tools/gen_quakelevel.py and validated by tools/check_g64lev.py;
// the same bytes are rendered on a PC by tools/hostsim/levelsim.cpp before
// any of it goes near the bench. Format version 8 -- version 1 had no
// entities, 2 no collision, 3 no entity kind, 4 no hull 0, 5 no visibility,
// 6 no pictures, 7 no start position or body lengths (it took them from
// constants compiled into the firmware).
//
// This header is deliberately free of every gpu64 subsystem: it parses and
// range-checks, and says nothing about resources, scenes or the arena. The
// part that turns a parsed level into gpu64 resources lives in
// gpu64_3d_class1.cpp, which is where the resource table and the arena are.
//
#ifndef _gpu64_level_h
#define _gpu64_level_h

#include <circle/types.h>

// 'G','6','4','L' read as a little-endian u32: 0x47, 0x36, 0x34, 0x4c.
#define GPU64_LEVEL_MAGIC	0x4c343647
#define GPU64_LEVEL_VERSION	8

// 52 bytes of counts and offsets, then (v8) the level's start and body:
//   52 s32 x 3  start position, 16.16 world units (0 when none)
//   64 u16      start yaw, gpu64 units
//   66 u8 x 4   step-up, eye, feet, waist -- in the level's own units (the
//               ones nScale converts), see Gpu64_Level below
//   70 u16      flags: bit 0 = the start position is present
#define GPU64_LEVEL_HEADER_BYTES	72
#define GPU64_LEVEL_HAS_START		0x0001

// The seven table strides, all of which tools/gen_quakelevel.py packs with
// struct's '<' so none of them carries padding.
#define GPU64_LEVEL_TEX_STRIDE		12	// <HBBII  id, wshift, hshift, off, len
#define GPU64_LEVEL_MESH_STRIDE		16	// <IIII   vOff, vLen, fOff, fLen
#define GPU64_LEVEL_NODE_STRIDE		16	// <HiiiH  meshIdx, x, y, z (16.16), modelIdx
#define GPU64_LEVEL_ENT_STRIDE		44	// <6H2h3i3iBBH see gpu64_levelEnt below
#define GPU64_LEVEL_PLANE_STRIDE	12	// <3hiBB  n (1.15), dist (16.16), type, pad
#define GPU64_LEVEL_HULL_STRIDE		36	// <2i6ii  head1, head2, mins[3], maxs[3], head0
#define GPU64_LEVEL_CLIP_STRIDE		 8	// <HhhH   plane, child0, child1, pad

#ifndef GPU64_HOSTSIM
// What LOAD_LEVEL and LEVEL_PICTURE read: slot N of the level pack, which is
// the REU image the user selected in the RAD menu (a .reu that starts with
// GPU64_PACK_MAGIC, built by tools/pack_levels.py). The slots point straight
// into reuMemory, with no copy. gpu64 reads nothing else -- no file names,
// no SD fallback -- so a program and its data ship together as a .prg plus
// a .reu. Bytes 0 = an empty slot.
#define GPU64_LEVEL_SLOTS	8
extern const u8 *gpu64LevelSlot[ GPU64_LEVEL_SLOTS ];
extern u32       gpu64LevelSlotBytes[ GPU64_LEVEL_SLOTS ];
extern u32       gpu64LevelSlotCount;	// slots filled, from 0 up
#endif

// The level pack (.reu image). Little-endian, at REU address 0:
//   0  'G64P'   4  u16 version (1)   6  u16 count   8  u32 image bytes
//   12 u32 FNV-1a of bytes 16..16+16*count
//   16 count x { u32 offset, u32 bytes, u32 FNV-1a of the level, char name[4] }
// Levels are 4 KB aligned. Every checksum is verified once, at start-up.
#define GPU64_PACK_MAGIC	0x50343647	// "G64P"
#define GPU64_PACK_VERSION	1
#define GPU64_PACK_HEADER	16
#define GPU64_PACK_ENTRY	16

// Every offset in a .g64lev is relative to the blob area, which starts at
// `base`; a parsed level therefore hands out pointers, never offsets, so that
// nothing downstream can forget to add it.
typedef struct
{
	const u8	*pFile;
	u32		nFileBytes;

	u16		nScale;		// level units per gpu64 world unit
	u16		nTex, nMesh, nNode, nEnt, nPlane, nHull;
	u32		nClip;

	const u8	*pTexTab;
	const u8	*pMeshTab;
	const u8	*pNodeTab;
	const u8	*pEntTab;
	const u8	*pPlaneTab;
	const u8	*pHullTab;
	const u8	*pClipTab;

	const u8	*pBlob;		// pFile + base
	u32		nBlobBytes;	// nFileBytes - base

	u32		nPalOff;	// 768 RGB triples, the level's own palette
	u32		nStrOff;	// entity string pool, NUL-terminated

	// Visibility (v6); see gpu64_levelVisRow(). nVisLeafs == 0 means the
	// file carries none and nothing is culled.
	u16		nVisLeafs, nVisRowBytes, nVisWorldNodes, nVisNodes, nVisHead;
	const u8	*pVisNodes;	// nVisNodes x <HhhH> plane, child0, child1
	const u8	*pVisRows;	// nVisLeafs x nVisRowBytes

	// Pictures (v7): full-screen and menu art that is not a texture --
	// the title, the menu, the intermission. See gpu64_levelPic().
	u16		nPic;
	const u8	*pPicTab;	// nPic x <HHI> w, h, blob offset

	// The start and the body (v8). All of it is the converter's to choose:
	// gpu64 has no idea what game a level belongs to, so where its camera
	// starts and how tall a walker in it is travel with the level.
	//   nStepUp  the tallest step CLIP_MOVE's STEP mode climbs
	//   nEye     the eye above the origin the hulls are built around
	//   nFeet    below the origin: where a walker's feet test liquid
	//   nWaist   above the origin: where the second liquid test is
	// In level units, converted through nScale.
	boolean		bHasStart;
	s32		nStartX, nStartY, nStartZ;	// the origin, 16.16
	u16		nStartYaw;
	u8		nStepUp, nEye, nFeet, nWaist;
}
Gpu64_Level;

// Reads the header, derives the seven table pointers and insists that they
// end exactly where the header says the blob area begins. FALSE on any of:
// wrong magic, wrong version, a truncated file, tables that do not add up.
// Nothing here trusts a length: a level file arrives from an SD card the
// firmware does not own.
boolean gpu64_levelParse( Gpu64_Level *pL, const u8 *pFile, u32 nBytes );

// A range-checked pointer into the blob area, or 0 if [nOff, nOff+nLen) is
// not wholly inside it.
const u8 *gpu64_levelBlob( const Gpu64_Level *pL, u32 nOff, u32 nLen );

// Picture nIndex: its pixels (w*h palette indices, row-major) and size, or
// 0 if there is no such picture. The size and the range were checked by
// gpu64_levelParse(), so a non-zero answer is safe to read in full.
const u8 *gpu64_levelPic( const Gpu64_Level *pL, unsigned nIndex,
			  unsigned *pW, unsigned *pH );

// A NUL-terminated string from the entity string pool, or 0 if the pool's
// bytes run out before the terminator does.
const char *gpu64_levelStr( const Gpu64_Level *pL, u32 nOff );

// One entity, decoded. The two `nParam` values are whichever pair of keys
// ENT_PARAMS in the converter chose for this classname -- health and count
// by default -- so a game reads them without parsing text at run time.
typedef struct
{
	const char	*pClassName;
	const char	*pTarget;
	const char	*pTargetName;

	// The same two strings as opaque ids: their offsets in the level's
	// string pool, which the converter dedupes, so two entities wired to
	// each other have equal ids and 0 means "no string". A 6502 game
	// matches a button to its door by comparing these, with no string
	// compare and no pool to walk.
	u16		nTargetId, nTargetNameId;
	u16		nYaw;		// gpu64 yaw, 0..65535 -- already converted
	u16		nSpawnFlags;
	u16		nModelIdx;	// 0 = not a brush model (door, platform...)
	s16		nParam0, nParam1;
	s32		x, y, z;	// 16.16 world units, gpu64 axes

	// The displacement from where a brush mover sits in the file to its
	// other position -- a door's open, a plat's bottom, a button's
	// pressed. Zero for everything else. Computed by the converter, from
	// Quake's own formula over keys (angle -1/-2, lip) and a bounding box
	// the C64 never sees; see open_ofs() in tools/gen_quakelevel.py.
	s32		ofsX, ofsY, ofsZ;

	// What this classname *is*, so a 6502 game switches on a byte instead
	// of strcmp-ing its way through 369 records. Grouped in ranges: 1-9
	// world and wiring, 10-19 brush movers, 20-29 triggers, 30-49
	// pickups, 50-79 monsters, 80-89 scenery, 90-99 lights and sound; 0
	// is a classname the converter has no opinion about. ENT_KIND in
	// tools/gen_quakelevel.py is the table, and the "What is in a level"
	// section of docs/class1-3d-mesh-reference.md publishes it.
	u8		nKind;
}
Gpu64_LevelEnt;


// FALSE if nIndex is past the table or a string offset leaves the pool.
boolean gpu64_levelEnt( const Gpu64_Level *pL, unsigned nIndex, Gpu64_LevelEnt *pE );

// --- collision ----------------------------------------------------------
//
// Quake's hulls are BSP trees of clipnodes over the shared plane array, one
// tree per size of thing that walks them, and they are already expanded by
// that size at compile time. So collision is a *point* trace against the
// hull for the right size -- there is no box sweep here and there does not
// need to be.
//
// Everything below is a pure function of the level file, which is immutable
// once loaded. Nothing here touches the scene, the arena or core 1, which is
// why CLIP_MOVE is the one class-1 opcode that answers while the render loop
// is running.

// Quake CONTENTS_*, passed through by the converter unchanged: a clipnode
// child is a node index when >= 0 and one of these when < 0.
#define GPU64_LEVEL_CONTENTS_EMPTY	( -1 )
#define GPU64_LEVEL_CONTENTS_SOLID	( -2 )
#define GPU64_LEVEL_CONTENTS_WATER	( -3 )
#define GPU64_LEVEL_CONTENTS_SLIME	( -4 )
#define GPU64_LEVEL_CONTENTS_LAVA	( -5 )
#define GPU64_LEVEL_CONTENTS_SKY	( -6 )

// The same thing as a small positive number, which is what crosses the bus:
// a 6502 comparing against 1 is cheaper than one comparing against $FE, and
// an unknown value has somewhere to go.
#define GPU64_CLIP_CONT_EMPTY		0
#define GPU64_CLIP_CONT_SOLID		1
#define GPU64_CLIP_CONT_WATER		2
#define GPU64_CLIP_CONT_SLIME		3
#define GPU64_CLIP_CONT_LAVA		4
#define GPU64_CLIP_CONT_SKY		5
#define GPU64_CLIP_CONT_UNKNOWN		255

// Level units to 16.16 world units: 18 -> 36864 at a scale of 32.
static inline s32 gpu64_levelQU( const Gpu64_Level *pL, s32 nLevelUnits )
{
	return (s32)( ( (s64)nLevelUnits << 16 ) / (s32)pL->nScale );
}

// DIST_EPSILON: a trace stops this far short of the plane it hits so that the
// endpoint is outside the solid and the next frame's trace does not start
// inside it. 1/32 of a level unit, so 64 at a scale of 32.
static inline s32 gpu64_levelEpsilon( const Gpu64_Level *pL )
{
	s32 e = gpu64_levelQU( pL, 1 ) / 32;
	return ( e < 1 ) ? 1 : e;
}

// The eye sits the level's nEye above the origin the hulls are expanded
// around, and the level loader already puts the camera there. So a caller
// that hands its camera position straight to a trace is that much too high
// and walks through the tops of walls. GPU64_CLIP_MODE_EYE exists so it
// does not have to think about it.
static inline s32 gpu64_levelEyeOfs( const Gpu64_Level *pL )
{
	return gpu64_levelQU( pL, pL->nEye );
}

// The recursion and iteration bound. A level file comes off an SD card the
// firmware does not own and gpu64_levelParse() checks table *sizes*, not that
// the clipnode tree is acyclic -- so every walk of it is bounded, and the
// answer when the bound is hit is the one that stops the player rather than
// the one that reads memory the level does not have.
#define GPU64_LEVEL_TRACE_MAXDEPTH	96

// A position or a displacement: x, y, z in 16.16 world units on gpu64 axes,
// y up. An array rather than three named fields because a plane's `type`
// indexes it.
typedef struct
{
	s32	v[ 3 ];
}
Gpu64_LevelVec;

// --- visibility ----------------------------------------------------------
//
// Quake's PVS, baked by tools/gen_quakelevel.py's build_vis() down to the
// granularity gpu64 draws at: for each leaf of the drawing BSP, one bit per
// world node (the level's first nVisWorldNodes nodes, the chunks of brush
// model 0) saying whether any face of that chunk lies in a leaf that can be
// seen from somewhere inside this one. Conservative by construction -- a
// chunk it clears is one Quake itself would not have drawn from there.
//
// Returns that row for the leaf containing pEye (16.16, gpu64 axes, the
// camera's own position) and its bit count in *pnNodes, or 0 when nothing
// may be culled: no vis section, a walk that hit its bound, or an eye in
// the shared solid leaf 0 -- a camera outside the map, which is exactly
// when a player is trying to see what went wrong.
//
// A pure function of the immutable level file, like the traces below, so
// core 1 may call it while rendering.
const u8 *gpu64_levelVisRow( const Gpu64_Level *pL, const Gpu64_LevelVec *pEye,
			     u16 *pnNodes );

// One placed chunk of level geometry: which mesh, where, and which Quake
// brush model it came from -- 0 for the world itself, and otherwise the same
// model index a Gpu64_LevelEnt carries, which is what lets a game find the
// nodes belonging to its doors. A model's chunks are contiguous, because the
// converter emits them model by model.
boolean gpu64_levelNode( const Gpu64_Level *pL, unsigned nIndex,
			 u16 *pMeshIdx, Gpu64_LevelVec *pPos, u16 *pModelIdx );

typedef struct
{
	s32		nHeadNode;	// the hull this trace ran against
	boolean		bAllSolid;	// the whole sweep was inside solid
	boolean		bStartSolid;	// it began inside solid
	boolean		bHitPlane;	// nPlane* below are meaningful
	s32		nFraction;	// 16.16, 0 .. 65536
	Gpu64_LevelVec	end;
	s32		nPlaneN[ 3 ];	// 16.16 unit normal, facing back at the sweep
}
Gpu64_LevelTrace;

// One brush model's collision: the two expanded hulls and the box the model
// occupies. Model 0 is the world; the rest are doors, platforms and the other
// moving brushes, which a move traces against when it names them in its
// Gpu64_LevelMover list.
typedef struct
{
	s32		nHead[ 2 ];	// [0] is hull 1, [1] is hull 2 -- the file's two box sizes
	Gpu64_LevelVec	mins, maxs;
	// Hull 0, the point hull (v5): the level itself, not shrunk by any
	// box. What a shot or a line of sight traces against. The converter
	// builds it from the drawing BSP the way Quake's Mod_MakeHull0 does,
	// and appends it to the clipnode table.
	s32		nHead0;
}
Gpu64_LevelHull;

boolean gpu64_levelHull( const Gpu64_Level *pL, unsigned nModel, Gpu64_LevelHull *pH );

// The contents of the leaf containing pP. SOLID for a malformed tree, which
// is the safe direction: a trace that cannot be resolved blocks.
s16 gpu64_levelPointContents( const Gpu64_Level *pL, s32 nHeadNode,
			      const Gpu64_LevelVec *pP );

// Sweeps a point from pStart to pEnd. This is Quake's SV_RecursiveHullCheck
// in 16.16 -- same epsilon, same back-off, same meaning for every field of
// the result.
void gpu64_levelTrace( const Gpu64_Level *pL, s32 nHeadNode,
		       const Gpu64_LevelVec *pStart, const Gpu64_LevelVec *pEnd,
		       Gpu64_LevelTrace *pTr );

// --- the move ------------------------------------------------------------
//
// What CLIP_MOVE actually runs: a displacement resolved against the level,
// optionally sliding along what it hits, climbing a step and settling onto
// the floor. The caller keeps the position; this only answers where the
// position ends up.

#define GPU64_CLIP_MODE_SLIDE	0x01	// slide along a plane instead of stopping
#define GPU64_CLIP_MODE_STEP	0x02	// climb up to the level's nStepUp
#define GPU64_CLIP_MODE_FLOOR	0x04	// settle onto the floor after moving
#define GPU64_CLIP_MODE_EYE	0x08	// positions are eye height, not origin
#define GPU64_CLIP_MODE_WALK	( GPU64_CLIP_MODE_SLIDE | GPU64_CLIP_MODE_STEP | \
				  GPU64_CLIP_MODE_FLOOR )

#define GPU64_CLIP_ONGROUND	0x01	// ended standing on a floor
#define GPU64_CLIP_WALL		0x02	// the move was stopped by a wall
#define GPU64_CLIP_CEILING	0x04	// something was hit from below
#define GPU64_CLIP_STEPPED	0x08	// a step-up is what got it through
#define GPU64_CLIP_STARTSOLID	0x10	// it began inside solid
#define GPU64_CLIP_ALLSOLID	0x20	// it never left solid -- nothing moved
#define GPU64_CLIP_TRUNCATED	0x40	// out of slide iterations, move cut short

typedef struct
{
	Gpu64_LevelVec	end;
	u8		nFlags;		// GPU64_CLIP_* above
	u8		nContents;	// GPU64_CLIP_CONT_* at the end position's feet
	u8		nFraction;	// 0..255 of the requested displacement
	u8		nBumps;		// slide iterations used, diagnostic
	u8		nWaterLevel;	// liquid depth: 0 dry, 1 feet, 2 waist, 3 eyes
}
Gpu64_LevelMove;

// A brush model the move is also traced against, at wherever the game has
// currently put it. A closed door is a hull standing in a doorway that the
// world's own hull knows nothing about -- the BSP compiler put the doorway's
// hole in the world and the door itself in a model of its own -- so without
// these a player walks through every door on the map.
//
// `ofs` is the displacement the game has applied to that model since the
// level loaded, i.e. the same vector it passed to SET_POSITION on the model's
// nodes. The hull does not move, so the sweep is what moves: it is traced in
// the model's own frame and the answer is brought back.
typedef struct
{
	u16		nModel;		// 1.. -- 0 would be the world, which is
					// already traced
	Gpu64_LevelVec	ofs;		// 16.16 world units
}
Gpu64_LevelMover;

// How many of those one move may carry. The cost is one hull descent each,
// which is nothing next to the world's, and E1M1's worst room has three
// movers in it; the bound exists so the block a caller sends has a size.
#define GPU64_LEVEL_MAX_MOVERS	16

// nHull is 1 or 2, the level's two box hulls; nModel is 0 for the
// world. FALSE only for arguments the level cannot satisfy -- a hull that
// does not exist -- never for a move that simply could not happen, which is
// an ordinary result with flags set.
boolean gpu64_levelMove( const Gpu64_Level *pL, unsigned nModel, unsigned nHull,
			 u8 nMode, const Gpu64_LevelVec *pStart,
			 const Gpu64_LevelVec *pDelta, Gpu64_LevelMove *pOut );

// The same move, also blocked by the brush models named in pMovers. Quake's
// SV_Move over SV_ClipMoveToEntity: every hull is swept, the nearest impact
// wins, and being inside any of them is being stuck. gpu64_levelMove() is
// this with nMovers 0.
boolean gpu64_levelMoveEnts( const Gpu64_Level *pL, unsigned nModel, unsigned nHull,
			     u8 nMode, const Gpu64_LevelVec *pStart,
			     const Gpu64_LevelVec *pDelta,
			     const Gpu64_LevelMover *pMovers, unsigned nMovers,
			     Gpu64_LevelMove *pOut );

// A straight sweep with no sliding, stepping or settling: Quake's SV_Move
// for a bullet or a line of sight. nHull is 0 (the point hull), 1 or 2; the
// movers block it exactly as they block gpu64_levelMoveEnts(). FALSE only for
// arguments the level cannot satisfy, as there.
//
// *pHit (optional) is what the sweep ran into, as GPU64_CLIP_CONT_*: the
// world's contents one Quake unit beyond the impact, so a shot into the sky
// says SKY and makes no puff. EMPTY when nothing was hit; SOLID for a hit on
// a mover, whose contents are always solid.
boolean gpu64_levelTraceEnts( const Gpu64_Level *pL, unsigned nHull,
			      const Gpu64_LevelVec *pStart, const Gpu64_LevelVec *pEnd,
			      const Gpu64_LevelMover *pMovers, unsigned nMovers,
			      Gpu64_LevelTrace *pTr, u8 *pHit );

// Fills the slots above from the REU image's level pack. Call once per
// program launch, from the REU start-up path in rad_main.cpp, before
// reuUsingPolling().
#ifndef GPU64_HOSTSIM
void gpu64_levelPreload( void );
#endif

// Reads the little-endian scalars a level file is built out of. Public
// because the class-1 loader walks the tables itself.
static inline u16 gpu64_levelRd16( const u8 *p )
{
	return (u16)( p[ 0 ] | ( p[ 1 ] << 8 ) );
}

static inline u32 gpu64_levelRd32( const u8 *p )
{
	return (u32)p[ 0 ] | ( (u32)p[ 1 ] << 8 ) | ( (u32)p[ 2 ] << 16 ) | ( (u32)p[ 3 ] << 24 );
}

static inline s32 gpu64_levelRdS32( const u8 *p )
{
	return (s32)gpu64_levelRd32( p );
}

static inline s16 gpu64_levelRdS16( const u8 *p )
{
	return (s16)gpu64_levelRd16( p );
}

#endif

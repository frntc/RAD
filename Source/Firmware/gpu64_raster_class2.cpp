// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 8 -- class 2's wire layer: the register decode, the blob
 pulls, the texture table and its arena.

 Everything that touches IO2, the REU or the framebuffer object lives here;
 everything that touches pixels lives in gpu64_raster_core.cpp, which is
 portable and compiled by tools/rastercheck on a PC. Keeping the two apart is
 what makes a pixel bug findable without a bench round.

 Reference: docs/api_design.md. Rationale: dev/milestone8_raster_design.md.
*/
#include "gpu64_raster.h"

#ifdef GPU64_RASTER_ENABLED

#include "gpu64_raster_core.h"
#include "gpu64_api.h"
#include "gpu64_fb.h"
#include <circle/util.h>

// --- the arena ----------------------------------------------------------
//
// A bump allocator over one static block, exactly like class 1's and for the
// same reasons: no heap on a path the dispatch runs, and explicit free only.
// FREE_TEXTURE reclaims the table slot, not the bytes -- a program that
// re-uploads in a loop will exhaust this and get a truthful OUT_OF_MEMORY.
//
// 8 MB rather than class 1's 32: a class 2 texture set is Doom-shaped (walls
// and flats, a few hundred KB) and this arena is additional to class 1's, so
// it buys margin without doubling the firmware's static footprint.
#define GPU64_RASTER_ARENA_BYTES	( 8 * 1024 * 1024 )

static u8  s_Arena[ GPU64_RASTER_ARENA_BYTES ] __attribute__(( aligned( 64 ) ));
static u32 s_ArenaUsed;

static u8 *arenaAlloc( u32 nBytes )
{
	// 64-byte aligned so no two textures share a cache line: the column
	// loop walks one texture linearly and there is no reason to let a
	// neighbour ride along in the same fill.
	nBytes = ( nBytes + 63 ) & ~63u;
	if ( nBytes > GPU64_RASTER_ARENA_BYTES - s_ArenaUsed )
		return 0;

	u8 *p = s_Arena + s_ArenaUsed;
	s_ArenaUsed += nBytes;
	return p;
}

// --- session state ------------------------------------------------------

// IDs are 1..255 -- one byte in a record, see the design doc. Slot n holds
// id n + 1, so the table needs no search and a lookup is an index.
#define GPU64_RASTER_MAX_TEXTURES	255

static Gpu64RasterTexture s_Tex[ GPU64_RASTER_MAX_TEXTURES ];
static u8 s_TexLive[ GPU64_RASTER_MAX_TEXTURES ];

static Gpu64RasterState s_State;

// The colormap, copied out of the blob into storage of our own: the C64 is
// free to reuse the RAM it uploaded from the moment the command returns.
static u8 s_Colormap[ GPU64_RASTER_MAX_LEVELS * 256 ];

// The sector table, copied out of the blob for the same reason the colormap
// is: the C64 owns the RAM it uploaded from again the moment SET_SECTORS
// returns, and DRAW_SECTORS reads this on every column of every wall.
static Gpu64RasterSector s_Sectors[ GPU64_RASTER_MAX_SECTORS ];

// Scratch for a SET_SECTORS that has not validated yet. Static, not a local:
// this runs on reuUsingPolling()'s stack and a kilobyte of frame there is a
// cost the polling loop should not be asked to carry.
static Gpu64RasterSector s_SectorsNew[ GPU64_RASTER_MAX_SECTORS ];

// The vertex pool and the texinfo table, UPLOAD_VERTS / UPLOAD_TEXINFO.
// Copied out of the blob for the same reason the sector table is: the C64
// owns the RAM it uploaded from again the moment the command returns, and
// DRAW_POLYS reads these on every vertex of every face.
static Gpu64RasterVertex  s_Verts[ GPU64_RASTER_MAX_VERTS ];
static Gpu64RasterTexinfo s_Texinfo[ GPU64_RASTER_MAX_TEXINFO ];

// The resident world. Poly records, in their wire form, kept across frames so
// that DRAW_WORLD can name a range of them instead of shipping them again.
// This is the bandwidth answer: a Quake level is thousands of faces and the
// C64 cannot afford to DMA them sixty times a second -- nor should it risk
// the bulk path's known defects once per frame when the geometry never
// changes. Stored raw rather than parsed because gpu64_rasterPolys() reads
// wire records directly, so DRAW_WORLD is a pointer and a count.
static u8  s_World[ GPU64_RASTER_MAX_WORLD_POLYS * GPU64_RASTER_POLY_BYTES ];
static u16 s_WorldPolys;

// Blob staging. Static for the same reason gpu64_api.cpp's are: this runs on
// reuUsingPolling()'s stack. One buffer is enough -- no class 2 opcode has
// both a source and a destination blob.
static u8 s_Stage[ 65536 ];

// What RASTER_STATS reports. Reset per batch, not per session, so a program
// reads the outcome of the batch it just issued.
static Gpu64RasterBatchResult s_LastBatch;
static u16 s_LastRequested;

#define sArg	gpu64Regs.arg

static inline u16 argU16( unsigned i )
{
	return (u16)( sArg[ i ] | ( sArg[ i + 1 ] << 8 ) );
}

static inline int argS16( unsigned i )
{
	return (int)(s16)argU16( i );
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

// --- the texture table --------------------------------------------------

static const Gpu64RasterTexture *lookupTexture( void *, u8 nId )
{
	if ( nId == 0 || nId > GPU64_RASTER_MAX_TEXTURES )
		return 0;
	return s_TexLive[ nId - 1 ] ? &s_Tex[ nId - 1 ] : 0;
}

static u16 liveTextures( void )
{
	u16 n = 0;
	for ( unsigned i = 0; i < GPU64_RASTER_MAX_TEXTURES; i++ )
		if ( s_TexLive[ i ] ) n++;
	return n;
}

// --- lifecycle ----------------------------------------------------------

void gpu64_rasterReset( void )
{
	gpu64_rasterZReset();
	memset( s_Tex, 0, sizeof( s_Tex ) );
	memset( s_TexLive, 0, sizeof( s_TexLive ) );
	s_ArenaUsed = 0;
	gpu64_rasterStateDefaults( &s_State );
	memset( s_Sectors, 0, sizeof( s_Sectors ) );
	memset( s_Verts, 0, sizeof( s_Verts ) );
	memset( s_Texinfo, 0, sizeof( s_Texinfo ) );
	s_WorldPolys = 0;
	memset( &s_LastBatch, 0, sizeof( s_LastBatch ) );
	s_LastRequested = 0;
}

// --- the draw target ----------------------------------------------------

static boolean makeTarget( Gpu64RasterTarget *pTarget )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 || !pFB->IsInitialized() )
		return FALSE;

	pTarget->pPixels = pFB->PageBuffer( pFB->GetDrawPage() );
	pTarget->pitch   = pFB->GetPitch();
	return pTarget->pPixels != 0;
}

// The VideoCore scans DRAM directly and is not coherent with the ARM, so
// nothing drawn is visible until the rows are cleaned. Only the view's rows:
// the rest of the page is the C64's status bar and was cleaned when the C64
// drew it. This is also the reason SET_VIEW exists as firmware state rather
// than as a per-record clamp.
static void cleanView( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB )
		pFB->CleanRows( pFB->GetDrawPage(), s_State.viewY,
				(unsigned)s_State.viewY + s_State.viewH );
}

// --- opcodes ------------------------------------------------------------

static u8 opSetView( void )
{
	const u16 x = argU16( 0 ), y = argU16( 2 ), w = argU16( 4 ), h = argU16( 6 );

	if ( w == 0 || h == 0 )
		return GPU64_ERR_BAD_ARGS;
	if ( (u32)x + w > GPU64_RASTER_SURFACE_W || (u32)y + h > GPU64_RASTER_SURFACE_H )
		return GPU64_ERR_BAD_ARGS;

	s_State.viewX = x;
	s_State.viewY = y;
	s_State.viewW = w;
	s_State.viewH = h;
	return GPU64_ERR_OK;
}

static u8 opSetColormap( void )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );
	const u8 levels = sArg[ 6 ];

	// levels = 0 is the way back to identity lighting, and takes no blob.
	if ( levels == 0 )
	{
		s_State.pColormap = 0;
		s_State.levels = 0;
		return GPU64_ERR_OK;
	}
	if ( levels > GPU64_RASTER_MAX_LEVELS )
		return GPU64_ERR_BAD_ARGS;
	if ( len != (u32)levels * 256 )
		return GPU64_ERR_BAD_ARGS;

	const u8 res = gpu64_blobRead( space, addr, len, s_Colormap );
	if ( res != GPU64_ERR_OK )
		return res;

	s_State.pColormap = s_Colormap;
	s_State.levels = levels;
	return GPU64_ERR_OK;
}

// The camera. Ten of these sixteen bytes are the entire per-frame cost of a
// static level: the walls do not move, so this is the only thing a frame has
// to say. Everything is validated here rather than per record, because a
// camera that cannot be projected would otherwise reject a whole batch once
// per record and say nothing about why.
static u8 opSetCamera( void )
{
	const u16 proj = argU16( 10 );
	const int eyeH = argS16( 6 ), ceilH = argS16( 8 );

	if ( proj == 0 )
		return GPU64_ERR_BAD_ARGS;
	if ( eyeH <= 0 || ceilH <= eyeH )
		return GPU64_ERR_BAD_ARGS;

	s_State.camX	  = (s16)argU16( 0 );
	s_State.camY	  = (s16)argU16( 2 );
	s_State.camAng	  = sArg[ 4 ];
	s_State.camFlags  = sArg[ 5 ];
	s_State.camEyeH	  = (s16)eyeH;
	s_State.camCeilH  = (s16)ceilH;
	s_State.camProj	  = proj;
	s_State.camFloorCol = sArg[ 12 ];
	s_State.camCeilCol  = sArg[ 13 ];
	s_State.camHorizon  = (s16)(s8)sArg[ 14 ];
	return GPU64_ERR_OK;
}

// SET_CAMERA3D. Twelve inline argument bytes rather than a blob: it is the
// only thing a frame of a static level has to say, and twelve STAs beat a
// descriptor and a DMA fetch. A separate opcode from SET_CAMERA and not an
// extension of it because SET_CAMERA's fifteen bytes are full, and because
// its horizon is a shear where this pitch is a rotation.
static u8 opSetCamera3D( void )
{
	const u16 proj = argU16( 8 );

	if ( proj == 0 )
		return GPU64_ERR_BAD_ARGS;

	s_State.cam3X	  = (s16)argU16( 0 );
	s_State.cam3Y	  = (s16)argU16( 2 );
	s_State.cam3Z	  = (s16)argU16( 4 );
	s_State.cam3Yaw	  = sArg[ 6 ];
	s_State.cam3Pitch = (s8)sArg[ 7 ];
	s_State.cam3Proj  = proj;
	s_State.cam3Flags = sArg[ 10 ];
	return GPU64_ERR_OK;
}

// SET_LIGHT. One dynamic point light per call, inline arguments, no blob --
// because the light a game changes every frame is a muzzle flash or a rocket,
// and making that cost a DMA fetch would have made it unusable at the one
// moment it exists for. Ten stores set a light; two turn one off.
//
//   ARG0     slot, 0..GPU64_RASTER_MAX_LIGHTS-1
//   ARG1-2   x, ARG3-4 y, ARG5-6 z	world 8.8, little endian
//   ARG7-8   radius, world 8.8		0 = slot off
//   ARG9     strength			colormap levels at the centre, 0 = off
static u8 opSetLight( void )
{
	const unsigned slot = sArg[ 0 ];

	if ( slot >= GPU64_RASTER_MAX_LIGHTS )
		return GPU64_ERR_BAD_ARGS;

	if ( !gpu64_rasterSetLight( &s_State, slot,
				    (s16)argU16( 1 ), (s16)argU16( 3 ),
				    (s16)argU16( 5 ), argU16( 7 ),
				    sArg[ 9 ] ) )
		return GPU64_ERR_BAD_ARGS;

	return GPU64_ERR_OK;
}

static u8 opStats( void )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	if ( len < 16 )
		return GPU64_ERR_BAD_ARGS;

	u8 info[ 16 ];
	info[  0 ] = 'R';
	info[  1 ] = '2';
	info[  2 ] = (u8)( s_LastBatch.accepted & 0xff );
	info[  3 ] = (u8)( s_LastBatch.accepted >> 8 );
	info[  4 ] = (u8)( s_LastBatch.rejected & 0xff );
	info[  5 ] = (u8)( s_LastBatch.rejected >> 8 );
	info[  6 ] = (u8)( s_LastRequested & 0xff );
	info[  7 ] = (u8)( s_LastRequested >> 8 );
	info[  8 ] = (u8)( s_LastBatch.pixels & 0xff );
	info[  9 ] = (u8)( ( s_LastBatch.pixels >>  8 ) & 0xff );
	info[ 10 ] = (u8)( ( s_LastBatch.pixels >> 16 ) & 0xff );
	info[ 11 ] = (u8)( ( s_LastBatch.pixels >> 24 ) & 0xff );

	const u16 live = liveTextures();
	info[ 12 ] = (u8)( live & 0xff );
	info[ 13 ] = (u8)( live >> 8 );

	const u32 freeKB = ( GPU64_RASTER_ARENA_BYTES - s_ArenaUsed ) >> 10;
	info[ 14 ] = (u8)( freeKB & 0xff );
	info[ 15 ] = (u8)( ( freeKB >> 8 ) & 0xff );

	return gpu64_blobWrite( space, addr, 16, info );
}

static u8 opFillView( void )
{
	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterFillView( &s_State, &target, sArg[ 0 ] );
	cleanView();
	return GPU64_ERR_OK;
}

static u8 opUploadTexture( void )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	const u16 id = stagedId();
	const u16 w  = argU16( 6 );
	const u16 h  = argU16( 8 );
	const u8  flags = sArg[ 10 ];

	if ( id == 0 || id > GPU64_RASTER_MAX_TEXTURES )
		return GPU64_ERR_BAD_ID;
	if ( w == 0 || h == 0 || w > GPU64_RASTER_MAX_DIM || h > GPU64_RASTER_MAX_DIM )
		return GPU64_ERR_BAD_ARGS;
	if ( ( h & ( h - 1 ) ) != 0 )				// h must be a power of two
		return GPU64_ERR_BAD_ARGS;
	if ( len != (u32)w * h )
		return GPU64_ERR_BAD_ARGS;
	if ( len > sizeof( s_Stage ) )
		return GPU64_ERR_BAD_ARGS;

	u8 res = gpu64_blobRead( space, addr, len, s_Stage );
	if ( res != GPU64_ERR_OK )
		return res;

	u8 *pDst = arenaAlloc( len );
	if ( pDst == 0 )
		return GPU64_ERR_OUT_OF_MEMORY;

	Gpu64RasterTexture tex;
	if ( !gpu64_rasterBuildTexture( &tex, pDst, s_Stage, len, w, h, ( flags & 0x01 ) != 0 ) )
		return GPU64_ERR_BAD_ARGS;

	// Re-upload to a live ID replaces it. The old bytes are not reclaimed --
	// bump allocator, same limit class 1 documents.
	s_Tex[ id - 1 ] = tex;
	s_TexLive[ id - 1 ] = 1;
	return GPU64_ERR_OK;
}

static u8 opFreeTexture( void )
{
	const u16 id = stagedId();
	if ( id == 0 || id > GPU64_RASTER_MAX_TEXTURES )
		return GPU64_ERR_BAD_ID;
	if ( !s_TexLive[ id - 1 ] )
		return GPU64_ERR_BAD_ID;

	s_TexLive[ id - 1 ] = 0;
	return GPU64_ERR_OK;
}

// Shared front half of DRAW_COLUMNS and DRAW_SPANS: validate, pull, check.
// Returns GPU64_ERR_OK with *pCount set, having left the records in s_Stage.
static u8 pullBatchStride( u32 *pCount, u32 nStride )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );

	const u32 count = argU16( 6 );
	const u8  flags = sArg[ 8 ];

	*pCount = 0;
	s_LastRequested = (u16)count;
	memset( &s_LastBatch, 0, sizeof( s_LastBatch ) );

	if ( count == 0 )
		return GPU64_ERR_OK;			// no-op, not an error

	u32 want = count * nStride;
	if ( flags & GPU64_RASTER_BATCH_CHECKSUM )
		want += 2;

	if ( len != want )
		return GPU64_ERR_BAD_ARGS;
	if ( want > sizeof( s_Stage ) )
		return GPU64_ERR_BAD_ARGS;

	const u8 res = gpu64_blobRead( space, addr, len, s_Stage );
	if ( res != GPU64_ERR_OK )
		return res;

	if ( flags & GPU64_RASTER_BATCH_CHECKSUM )
	{
		const u32 body = count * nStride;
		const u16 want16 = (u16)( s_Stage[ body ] | ( s_Stage[ body + 1 ] << 8 ) );
		if ( gpu64_rasterChecksum( s_Stage, body ) != want16 )
			// Rule 3 in CLAUDE.md: a bulk upload through this path needs a
			// way to notice it did not arrive. Nothing is drawn -- the
			// spec's "a failed dispatch does nothing" rule.
			return GPU64_ERR_BAD_ARGS;
	}

	*pCount = count;
	return GPU64_ERR_OK;
}

static inline u8 pullBatch( u32 *pCount )
{
	return pullBatchStride( pCount, GPU64_RASTER_REC_BYTES );
}

static u8 opDrawColumns( void )
{
	u32 count;
	u8 res = pullBatch( &count );
	if ( res != GPU64_ERR_OK || count == 0 )
		return res;

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterColumns( &s_State, &target, s_Stage, count, sArg[ 9 ],
			     lookupTexture, 0, &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

static u8 opDrawSpans( void )
{
	u32 count;
	u8 res = pullBatch( &count );
	if ( res != GPU64_ERR_OK || count == 0 )
		return res;

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterSpans( &s_State, &target, s_Stage, count,
			   lookupTexture, 0, &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

static u8 opDrawWalls( void )
{
	u32 count;
	u8 res = pullBatch( &count );
	if ( res != GPU64_ERR_OK || count == 0 )
		return res;

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterWalls( &s_State, &target, s_Stage, count, sArg[ 9 ],
			   lookupTexture, 0, &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

// SET_SECTORS. The table is the level's vertical shape and, like the walls,
// it is uploaded once and not touched again -- so it is checked once, here,
// rather than on every column that reads it.
static u8 opSetSectors( void )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );
	const u16 count = argU16( 6 );

	// count = 0 drops the table, which is the way back to a level that
	// only DRAW_WALLS can draw.
	if ( count == 0 )
	{
		s_State.pSectors = 0;
		s_State.sectors = 0;
		return GPU64_ERR_OK;
	}
	if ( count > GPU64_RASTER_MAX_SECTORS )
		return GPU64_ERR_BAD_ARGS;
	if ( len != (u32)count * GPU64_RASTER_SEC_BYTES )
		return GPU64_ERR_BAD_ARGS;

	const u8 res = gpu64_blobRead( space, addr, len, s_Stage );
	if ( res != GPU64_ERR_OK )
		return res;

	// Build into the live table only after it validates: a rejected upload
	// must leave the table that was already working alone.
	if ( !gpu64_rasterBuildSectors( s_SectorsNew, s_Stage, count ) )
		return GPU64_ERR_BAD_ARGS;

	memcpy( s_Sectors, s_SectorsNew, count * sizeof( Gpu64RasterSector ) );
	s_State.pSectors = s_Sectors;
	s_State.sectors = count;
	return GPU64_ERR_OK;
}

// UPLOAD_VERTS and UPLOAD_TEXINFO. Both are level-lifetime tables and both
// follow SET_SECTORS' shape exactly: count 0 drops the table, a bad count or
// a length that does not match rejects the upload and leaves what was there
// alone.
static u8 opUploadVerts( void )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );
	const u16 count = argU16( 6 );

	if ( count == 0 )
	{
		s_State.pVerts = 0;
		s_State.verts = 0;
		return GPU64_ERR_OK;
	}
	if ( count > GPU64_RASTER_MAX_VERTS )
		return GPU64_ERR_BAD_ARGS;
	if ( len != (u32)count * GPU64_RASTER_VERT_BYTES )
		return GPU64_ERR_BAD_ARGS;

	const u8 res = gpu64_blobRead( space, addr, len, s_Stage );
	if ( res != GPU64_ERR_OK )
		return res;

	gpu64_rasterBuildVerts( s_Verts, s_Stage, count );
	s_State.pVerts = s_Verts;
	s_State.verts = count;
	return GPU64_ERR_OK;
}

static u8 opUploadTexinfo( void )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );
	const u16 count = argU16( 6 );

	if ( count == 0 )
	{
		s_State.pTexinfo = 0;
		s_State.texinfos = 0;
		return GPU64_ERR_OK;
	}
	if ( count > GPU64_RASTER_MAX_TEXINFO )
		return GPU64_ERR_BAD_ARGS;
	if ( len != (u32)count * GPU64_RASTER_TEXINFO_BYTES )
		return GPU64_ERR_BAD_ARGS;

	const u8 res = gpu64_blobRead( space, addr, len, s_Stage );
	if ( res != GPU64_ERR_OK )
		return res;

	gpu64_rasterBuildTexinfo( s_Texinfo, s_Stage, count );
	s_State.pTexinfo = s_Texinfo;
	s_State.texinfos = count;
	return GPU64_ERR_OK;
}

// DRAW_POLYS. Unlike DRAW_SECTORS this does NOT clear the depth buffer: a
// Quake frame may be several batches, and FILL_VIEW is the op that owns the
// clear -- see dev/milestone9_poly_design.md.
static u8 opDrawPolys( void )
{
	u32 count;
	u8 res = pullBatchStride( &count, GPU64_RASTER_POLY_BYTES );
	if ( res != GPU64_ERR_OK || count == 0 )
		return res;

	if ( s_State.cam3Proj == 0 )
		return GPU64_ERR_BAD_ARGS;	// SET_CAMERA3D was never sent
	if ( s_State.pVerts == 0 )
		return GPU64_ERR_BAD_ARGS;	// UPLOAD_VERTS was never sent

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterPolys( &s_State, &target, s_Stage, count, sArg[ 9 ],
			   lookupTexture, 0, &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

// UPLOAD_POLYS. The resident world, filled in chunks: ARG8..9 is the
// destination index, so a level larger than the 64 KB staging buffer arrives
// as several commands and the pool grows to cover the highest one written.
// count = 0 with first = 0 drops the pool, the same "way back" SET_SECTORS
// and UPLOAD_VERTS both offer.
static u8 opUploadPolys( void )
{
	u8 space; u32 addr, len;
	argBlob( 0, &space, &addr, &len );
	const u16 count = argU16( 6 );
	const u16 first = argU16( 8 );

	if ( count == 0 )
	{
		if ( first == 0 )
			s_WorldPolys = 0;
		return GPU64_ERR_OK;
	}
	if ( (u32)first + count > GPU64_RASTER_MAX_WORLD_POLYS )
		return GPU64_ERR_BAD_ARGS;
	if ( len != (u32)count * GPU64_RASTER_POLY_BYTES )
		return GPU64_ERR_BAD_ARGS;

	const u8 res = gpu64_blobRead( space, addr, len, s_Stage );
	if ( res != GPU64_ERR_OK )
		return res;

	memcpy( s_World + (u32)first * GPU64_RASTER_POLY_BYTES, s_Stage, len );
	if ( first + count > s_WorldPolys )
		s_WorldPolys = (u16)( first + count );
	return GPU64_ERR_OK;
}

// DRAW_WORLD. DRAW_POLYS over a range of the resident pool: ARG0..1 first,
// ARG2..3 count, ARG9 the mask key -- ten register writes and no blob at all,
// which is the whole point. The 6502 does its own visibility work by choosing
// ranges, so a level laid out room by room can be drawn a room at a time.
static u8 opDrawWorld( void )
{
	const u16 first = argU16( 0 );
	const u16 count = argU16( 2 );

	s_LastRequested = count;
	memset( &s_LastBatch, 0, sizeof( s_LastBatch ) );

	if ( count == 0 )
		return GPU64_ERR_OK;			// no-op, not an error
	if ( (u32)first + count > s_WorldPolys )
		return GPU64_ERR_BAD_ARGS;
	if ( s_State.cam3Proj == 0 )
		return GPU64_ERR_BAD_ARGS;	// SET_CAMERA3D was never sent
	if ( s_State.pVerts == 0 )
		return GPU64_ERR_BAD_ARGS;	// UPLOAD_VERTS was never sent

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterPolys( &s_State, &target,
			   s_World + (u32)first * GPU64_RASTER_POLY_BYTES,
			   count, sArg[ 9 ], lookupTexture, 0, &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

static u8 opDrawSectors( void )
{
	u32 count;
	u8 res = pullBatchStride( &count, GPU64_RASTER_WALL2_BYTES );
	if ( res != GPU64_ERR_OK || count == 0 )
		return res;

	if ( s_State.pSectors == 0 )
		return GPU64_ERR_BAD_ARGS;	// SET_SECTORS was never sent

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterSectors( &s_State, &target, s_Stage, count, sArg[ 9 ],
			     lookupTexture, 0, &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

// DRAW_THINGS. No table to validate and no state to touch: a thing batch is
// the one part of a level that changes every frame, so it goes straight
// through, exactly like DRAW_COLUMNS. The one thing worth checking is that
// a batch asking for the 3D camera has actually been given one, because
// otherwise every record would be silently rejected and the caller would
// see an empty view with an OK status.
static u8 opDrawThings( void )
{
	u32 count;
	u8 res = pullBatch( &count );
	if ( res != GPU64_ERR_OK || count == 0 )
		return res;

	if ( ( sArg[ 8 ] & GPU64_RASTER_BATCH_CAM3D ) && s_State.cam3Proj == 0 )
		return GPU64_ERR_BAD_ARGS;	// SET_CAMERA3D was never sent

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	gpu64_rasterThings( &s_State, &target, s_Stage, count, sArg[ 9 ], sArg[ 8 ],
			    lookupTexture, 0, &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

static u8 opDrawSprite( void )
{
	const u16 id = stagedId();
	if ( id == 0 || id > GPU64_RASTER_MAX_TEXTURES )
		return GPU64_ERR_BAD_ID;
	const Gpu64RasterTexture *pTex = lookupTexture( 0, (u8)id );
	if ( pTex == 0 )
		return GPU64_ERR_BAD_ID;

	const int x = argS16( 0 ), y = argS16( 2 );
	const u16 w = argU16( 4 ), h = argU16( 6 );

	memset( &s_LastBatch, 0, sizeof( s_LastBatch ) );
	s_LastRequested = 1;

	if ( w == 0 || h == 0 )
	{
		// Not an error -- a zero-sized BLIT is not one either. But it is a
		// primitive that did not draw, and STATS says so, which is the same
		// verdict the core gives a degenerate column record.
		s_LastBatch.rejected = 1;
		return GPU64_ERR_OK;
	}

	Gpu64RasterTarget target;
	if ( !makeTarget( &target ) )
		return GPU64_ERR_UNSUPPORTED;

	// The core counts the sprite itself, the same way it counts a column
	// record -- counting it here as well is what made STATS report two
	// primitives for one sprite.
	gpu64_rasterSprite( &s_State, &target, pTex, x, y, w, h,
			    sArg[ 8 ], sArg[ 9 ], argS16( 10 ), argS16( 12 ),
			    sArg[ 14 ], &s_LastBatch );
	cleanView();
	return GPU64_ERR_OK;
}

// --- dispatch -----------------------------------------------------------

u8 gpu64_rasterDispatch( u8 op )
{
	u8 res;

	switch ( op )
	{
	case GPU64_RASTER_OP_RESET:		gpu64_rasterReset(); res = GPU64_ERR_OK; break;
	case GPU64_RASTER_OP_SET_VIEW:		res = opSetView(); break;
	case GPU64_RASTER_OP_SET_COLORMAP:	res = opSetColormap(); break;
	case GPU64_RASTER_OP_STATS:		res = opStats(); break;
	case GPU64_RASTER_OP_FILL_VIEW:		res = opFillView(); break;
	case GPU64_RASTER_OP_SET_CAMERA:	res = opSetCamera(); break;
	case GPU64_RASTER_OP_SET_SECTORS:	res = opSetSectors(); break;
	case GPU64_RASTER_OP_SET_CAMERA3D:	res = opSetCamera3D(); break;
	case GPU64_RASTER_OP_SET_LIGHT:		res = opSetLight(); break;

	case GPU64_RASTER_OP_UPLOAD_TEXTURE:	res = opUploadTexture(); break;
	case GPU64_RASTER_OP_FREE_TEXTURE:	res = opFreeTexture(); break;
	case GPU64_RASTER_OP_UPLOAD_VERTS:	res = opUploadVerts(); break;
	case GPU64_RASTER_OP_UPLOAD_TEXINFO:	res = opUploadTexinfo(); break;
	case GPU64_RASTER_OP_UPLOAD_POLYS:	res = opUploadPolys(); break;

	case GPU64_RASTER_OP_DRAW_COLUMNS:	res = opDrawColumns(); break;
	case GPU64_RASTER_OP_DRAW_SPANS:	res = opDrawSpans(); break;
	case GPU64_RASTER_OP_DRAW_SPRITE:	res = opDrawSprite(); break;
	case GPU64_RASTER_OP_DRAW_WALLS:	res = opDrawWalls(); break;
	case GPU64_RASTER_OP_DRAW_SECTORS:	res = opDrawSectors(); break;
	case GPU64_RASTER_OP_DRAW_THINGS:	res = opDrawThings(); break;
	case GPU64_RASTER_OP_DRAW_POLYS:	res = opDrawPolys(); break;
	case GPU64_RASTER_OP_DRAW_WORLD:	res = opDrawWorld(); break;

	default:
		res = GPU64_ERR_BAD_OPCODE;
		break;
	}

	// gpu64: put back what this dispatch just evicted, while the bus is
	// still held -- rule 5 of the polling-loop rules, the same thing class 1
	// and PAGE_FLIP do. A batch walks a staging buffer, a texture arena and
	// the framebuffer, which is more than the class 0 CLEAR that was already
	// enough to evict warmCache()'s work.
	gpu64_apiWarmPollingLoop();

	return res;
}

char *gpu64_rasterReport( char *p )
{
	static const char hex[] = "0123456789ABCDEF";

	for ( const char *q = "R2 tex="; *q; q++ ) *p++ = *q;
	const u16 live = liveTextures();
	*p++ = hex[ ( live >> 4 ) & 0xf ];
	*p++ = hex[ live & 0xf ];

	for ( const char *q = " acc="; *q; q++ ) *p++ = *q;
	*p++ = hex[ ( s_LastBatch.accepted >> 4 ) & 0xf ];
	*p++ = hex[ s_LastBatch.accepted & 0xf ];

	for ( const char *q = " rej="; *q; q++ ) *p++ = *q;
	*p++ = hex[ ( s_LastBatch.rejected >> 4 ) & 0xf ];
	*p++ = hex[ s_LastBatch.rejected & 0xf ];

	for ( const char *q = " arenaKB="; *q; q++ ) *p++ = *q;
	u32 kb = s_ArenaUsed >> 10;
	*p++ = hex[ ( kb >> 12 ) & 0xf ];
	*p++ = hex[ ( kb >> 8 ) & 0xf ];
	*p++ = hex[ ( kb >> 4 ) & 0xf ];
	*p++ = hex[ kb & 0xf ];

	return p;
}

#endif	// GPU64_RASTER_ENABLED

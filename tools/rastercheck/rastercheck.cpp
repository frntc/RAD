// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 rastercheck -- runs the firmware's class 2 raster core natively so it
 can be diffed, pixel for pixel, against the reference model in
 tools/prgsim/gpu64model.py.

 The two were written from opposite ends: the core is the firmware, the model
 is a reading of docs/class2-raster-reference.md. Neither is authoritative on its own, and
 every place they disagree is either a firmware bug or a document that does
 not say what the firmware does. Both are worth finding, and both are worth
 finding here rather than at the bench -- see CLAUDE.md on bench time.

 Source/Firmware/gpu64_raster_core.cpp is compiled unchanged; only
 <circle/types.h> is stubbed, exactly as tools/hostsim does it.

 Usage: ./rastercheck <scenario-file>   -- result binary on stdout.
 The scenario and result formats are produced and consumed by check.py, which
 is the only intended caller.
*/
#include "gpu64_raster_core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SURF_W GPU64_RASTER_SURFACE_W
#define SURF_H GPU64_RASTER_SURFACE_H

static u8	g_Pixels[ SURF_W * SURF_H ];
static u8	g_Colormap[ GPU64_RASTER_MAX_LEVELS * 256 ];
static u8	g_TexBytes[ 256 ][ 64 * 64 ];
static Gpu64RasterTexture g_Tex[ 256 ];
static boolean	g_TexLive[ 256 ];

// --- scenario reader ----------------------------------------------------
static const u8 *g_p, *g_pEnd;

static void need( size_t n )
{
	if( (size_t) ( g_pEnd - g_p ) < n )
	{
		fprintf( stderr, "rastercheck: scenario truncated\n" );
		exit( 2 );
	}
}

static u8 rd8( void )    { need( 1 ); return *g_p++; }
static u16 rd16( void )  { u16 v = rd8(); return v | ( (u16) rd8() << 8 ); }
static u32 rd32( void )  { u32 v = rd16(); return v | ( (u32) rd16() << 16 ); }
static int rds32( void ) { return (int) (s32) rd32(); }

static const Gpu64RasterTexture *lookup( void *pCtx, u8 nId )
{
	if( nId == 0 || !g_TexLive[ nId ] )
		return 0;
	return &g_Tex[ nId ];
}

int main( int argc, char **argv )
{
	if( argc < 2 )
	{
		fprintf( stderr, "usage: rastercheck <scenario-file>\n" );
		return 2;
	}

	FILE *f = fopen( argv[ 1 ], "rb" );
	if( !f ) { perror( argv[ 1 ] ); return 2; }
	static u8 buf[ 8 * 1024 * 1024 ];
	size_t n = fread( buf, 1, sizeof buf, f );
	fclose( f );
	g_p = buf;
	g_pEnd = buf + n;

	if( rd32() != 0x4B433252 )
	{
		fprintf( stderr, "rastercheck: bad magic\n" );
		return 2;
	}

	Gpu64RasterState state;
	gpu64_rasterStateDefaults( &state );
	state.viewX = rd16();
	state.viewY = rd16();
	state.viewW = rd16();
	state.viewH = rd16();

	unsigned nLevels = rd16();
	if( nLevels )
	{
		need( nLevels * 256 );
		memcpy( g_Colormap, g_p, nLevels * 256 );
		g_p += nLevels * 256;
		state.pColormap = g_Colormap;
		state.levels = (u16) nLevels;
	}

	unsigned nTex = rd16();
	for( unsigned i = 0; i < nTex; i++ )
	{
		u8  id = rd8();
		u16 w  = rd16();
		u16 h  = rd16();
		need( (size_t) w * h );
		// The scenario carries texels already column-major, which is the
		// storage order, so bSrcRowMajor is FALSE and this is a copy.
		if( !gpu64_rasterBuildTexture( &g_Tex[ id ], g_TexBytes[ id ],
					       g_p, (u32) w * h, w, h, FALSE ) )
		{
			fprintf( stderr, "rastercheck: texture %u rejected\n", id );
			return 2;
		}
		g_p += (size_t) w * h;
		g_TexLive[ id ] = TRUE;
	}

	memset( g_Pixels, rd8(), sizeof g_Pixels );

	Gpu64RasterTarget target;
	target.pPixels = g_Pixels;
	target.pitch   = SURF_W;

	Gpu64RasterBatchResult res;
	memset( &res, 0, sizeof res );

	u8 kind = rd8();
	u8 key  = rd8();

	// The camera is in every scenario, not only the wall ones: it costs
	// ten bytes and it means the two sides can never disagree about which
	// scenarios carry it.
	state.camX	  = (s16) rd16();
	state.camY	  = (s16) rd16();
	state.camAng	  = rd8();
	state.camFlags	  = rd8();
	state.camEyeH	  = (s16) rd16();
	state.camCeilH	  = (s16) rd16();
	state.camProj	  = rd16();
	state.camFloorCol = rd8();
	state.camCeilCol  = rd8();
	state.camHorizon  = (s16) rd16();

	// The sector table, likewise in every scenario. Zero sectors is a
	// legal state and DRAW_SECTORS is expected to reject a whole batch
	// against it, which is a case worth generating.
	static Gpu64RasterSector sectors[ GPU64_RASTER_MAX_SECTORS ];
	unsigned nSec = rd16();
	if( nSec )
	{
		need( (size_t) nSec * GPU64_RASTER_SEC_BYTES );
		if( gpu64_rasterBuildSectors( sectors, g_p, nSec ) )
		{
			state.pSectors = sectors;
			state.sectors  = (u16) nSec;
		}
		g_p += (size_t) nSec * GPU64_RASTER_SEC_BYTES;
	}

	// The 3D camera and the two level tables, milestone 9. In every
	// scenario for the same reason the 2D camera is: it costs a few bytes
	// and it removes any question of which scenarios carry them.
	state.cam3X	= (s16) rd16();
	state.cam3Y	= (s16) rd16();
	state.cam3Z	= (s16) rd16();
	state.cam3Yaw	= rd8();
	state.cam3Pitch	= (s8) rd8();
	state.cam3Proj	= rd16();
	state.cam3Flags	= rd8();

	static Gpu64RasterVertex verts[ GPU64_RASTER_MAX_VERTS ];
	unsigned nVerts = rd16();
	if( nVerts )
	{
		need( (size_t) nVerts * GPU64_RASTER_VERT_BYTES );
		gpu64_rasterBuildVerts( verts, g_p, nVerts );
		state.pVerts = verts;
		state.verts  = (u16) nVerts;
		g_p += (size_t) nVerts * GPU64_RASTER_VERT_BYTES;
	}

	static Gpu64RasterTexinfo texinfo[ GPU64_RASTER_MAX_TEXINFO ];
	unsigned nTI = rd16();
	if( nTI )
	{
		need( (size_t) nTI * GPU64_RASTER_TEXINFO_BYTES );
		gpu64_rasterBuildTexinfo( texinfo, g_p, nTI );
		state.pTexinfo = texinfo;
		state.texinfos = (u16) nTI;
		g_p += (size_t) nTI * GPU64_RASTER_TEXINFO_BYTES;
	}

	// Milestone 12's dynamic lights: a count, then one record each. Set
	// through the same entry point the firmware's SET_LIGHT uses, so the
	// derived r2/fall are the ones a real program would get.
	unsigned nLights = rd16();
	for( unsigned i = 0; i < nLights; i++ )
	{
		u8 slot = rd8();
		int lx = rds32(), ly = rds32(), lz = rds32();
		unsigned radius = rd16();
		u8 strength = rd8();
		gpu64_rasterSetLight( &state, slot, lx, ly, lz,
				      (u16) radius, strength );
	}

	if( kind == 2 )
	{
		u8 id = rd8();
		int x = rds32(), y = rds32();
		unsigned w = rd32(), h = rd32();
		u8 light = rd8();
		u8 sprKey = rd8();
		int cy0 = rds32(), cy1 = rds32();
		u8 flags = rd8();
		const Gpu64RasterTexture *pTex = lookup( 0, id );
		if( pTex )
			gpu64_rasterSprite( &state, &target, pTex, x, y, w, h,
					    light, sprKey, cy0, cy1, flags, &res );
	}
	else
	{
		u32 count = rd32();
		const size_t stride = ( kind == 4 || kind == 5 || kind == 7 )
					? GPU64_RASTER_WALL2_BYTES
					: GPU64_RASTER_REC_BYTES;
		need( (size_t) count * stride );
		if( kind == 0 )
			gpu64_rasterColumns( &state, &target, g_p, count, key,
					     lookup, 0, &res );
		else if( kind == 1 )
			gpu64_rasterSpans( &state, &target, g_p, count,
					   lookup, 0, &res );
		else if( kind == 3 )
			gpu64_rasterWalls( &state, &target, g_p, count, key,
					   lookup, 0, &res );
		else if( kind == 6 || kind == 9 )
			gpu64_rasterPolys( &state, &target, g_p, count, key,
					   lookup, 0, &res );
		else if( kind == 8 )
			gpu64_rasterThings( &state, &target, g_p, count, key,
					    GPU64_RASTER_BATCH_CAM3D,
					    lookup, 0, &res );
		else
			gpu64_rasterSectors( &state, &target, g_p, count, key,
					     lookup, 0, &res );

		// kind 5 is two batches, not one: DRAW_SECTORS to fill the
		// depth buffer and then DRAW_THINGS against it. A thing that
		// nothing occludes tests almost nothing, so the scenario has
		// to carry the geometry that occludes it.
		if( kind == 5 )
		{
			g_p += (size_t) count * stride;
			u32 things = rd32();
			need( (size_t) things * GPU64_RASTER_REC_BYTES );
			gpu64_rasterThings( &state, &target, g_p, things, key, 0,
					    lookup, 0, &res );
		}

		// kind 7 is the cross-layer case: a Doom level and a Quake one
		// into the same depth buffer. Nothing in the API forbids it and
		// a level being ported would do exactly this.
		if( kind == 7 )
		{
			g_p += (size_t) count * stride;
			u32 polys = rd32();
			need( (size_t) polys * GPU64_RASTER_POLY_BYTES );
			gpu64_rasterPolys( &state, &target, g_p, polys, key,
					   lookup, 0, &res );
		}

		// kind 9 is milestone 10's whole point: a Quake level and then
		// its monsters, both through SET_CAMERA3D, sharing one depth
		// buffer. Occlusion across the two layers is the part a single
		// batch cannot exercise.
		if( kind == 9 )
		{
			g_p += (size_t) count * stride;
			u32 things = rd32();
			need( (size_t) things * GPU64_RASTER_REC_BYTES );
			gpu64_rasterThings( &state, &target, g_p, things, key,
					    GPU64_RASTER_BATCH_CAM3D,
					    lookup, 0, &res );
		}
	}

	u8 out[ 8 ];
	out[ 0 ] = res.accepted & 0xFF;
	out[ 1 ] = ( res.accepted >> 8 ) & 0xFF;
	out[ 2 ] = res.rejected & 0xFF;
	out[ 3 ] = ( res.rejected >> 8 ) & 0xFF;
	for( int i = 0; i < 4; i++ )
		out[ 4 + i ] = ( res.pixels >> ( 8 * i ) ) & 0xFF;
	fwrite( out, 1, sizeof out, stdout );
	fwrite( g_Pixels, 1, sizeof g_Pixels, stdout );
	return 0;
}

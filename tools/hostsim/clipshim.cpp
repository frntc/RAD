// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
//
// clipshim.cpp - the firmware's collision trace, as a shared library, so that
// tools/prgsim can model CLIP_MOVE ($15) without a second implementation of
// it.
//
// Same rule gpu64class1.py states for the renderer: a class-1 answer that a
// Python port could disagree with is not an oracle, it is a second thing to
// be wrong. The renderer is delegated to tools/hostsim/scenesim over a frame
// stream; a hull trace is a 40-byte question and a 24-byte answer, so it is
// delegated through ctypes instead of a subprocess.
//
// Built by tools/hostsim/Makefile as libclipmove.so. Nothing in the firmware
// build looks at this file.
//
#include <stdlib.h>
#include <string.h>

#include "gpu64_level.h"

static Gpu64_Level	s_Lev;
static unsigned char *	s_pBlob;
static int		s_bOpen;

// Takes a copy: Gpu64_Level keeps pointers into the file image, and the
// bytes() object Python hands in is not ours to hold.
extern "C" int gpu64shim_open( const unsigned char *pData, unsigned nBytes )
{
	free( s_pBlob );
	s_pBlob = (unsigned char *)malloc( nBytes );
	s_bOpen = 0;
	if ( s_pBlob == 0 )
		return 0;
	memcpy( s_pBlob, pData, nBytes );
	if ( !gpu64_levelParse( &s_Lev, s_pBlob, nBytes ) )
		return 0;
	s_bOpen = 1;
	return 1;
}

extern "C" int gpu64shim_scale( void )
{
	return s_bOpen ? (int)s_Lev.nScale : 0;
}

// pIn:  start x,y,z then delta x,y,z, six s32 16.16.
// pOut: end x,y,z then flags, contents, fraction, bumps -- seven s32, the
//       four bytes widened so the ctypes side has one array type and no
//       packing to agree about.
// Returns 0 exactly where gpu64_levelMove() answers FALSE, i.e. where
// CLIP_MOVE answers BAD_ARGS.
// pMovers: four ints per mover -- model, then ofs x, y, z in 16.16. That is
// the CLIP_MOVE mover record with its two reserved fields left out, because
// the block's own packing is prgsim's job to model, not this shim's.
extern "C" int gpu64shim_move_ents( const int *pIn, int nModel, int nHull,
				    int nMode, const int *pMovers, int nMovers,
				    int *pOut )
{
	if ( !s_bOpen )
		return 0;
	if ( nMovers < 0 || nMovers > GPU64_LEVEL_MAX_MOVERS )
		return 0;

	Gpu64_LevelVec start, delta;
	for ( unsigned k = 0; k < 3; k++ )
	{
		start.v[ k ] = pIn[ k ];
		delta.v[ k ] = pIn[ 3 + k ];
	}

	Gpu64_LevelMover mvrs[ GPU64_LEVEL_MAX_MOVERS ];
	for ( int i = 0; i < nMovers; i++ )
	{
		mvrs[ i ].nModel = (u16)pMovers[ i * 4 + 0 ];
		for ( unsigned k = 0; k < 3; k++ )
			mvrs[ i ].ofs.v[ k ] = pMovers[ i * 4 + 1 + k ];
	}

	Gpu64_LevelMove mv;
	if ( !gpu64_levelMoveEnts( &s_Lev, (unsigned)nModel, (unsigned)nHull,
				   (u8)nMode, &start, &delta, mvrs,
				   (unsigned)nMovers, &mv ) )
		return 0;

	for ( unsigned k = 0; k < 3; k++ )
		pOut[ k ] = mv.end.v[ k ];
	pOut[ 3 ] = mv.nFlags;
	pOut[ 4 ] = mv.nContents;
	pOut[ 5 ] = mv.nFraction;
	pOut[ 6 ] = mv.nBumps;
	pOut[ 7 ] = mv.nWaterLevel;
	return 1;
}

extern "C" int gpu64shim_move( const int *pIn, int nModel, int nHull, int nMode,
			       int *pOut )
{
	return gpu64shim_move_ents( pIn, nModel, nHull, nMode, 0, 0, pOut );
}

// WORLD_TICK's trace query (milestone 20). pIn: start x,y,z then end x,y,z,
// six s32 16.16. pOut: end x,y,z, fraction 16.16, startsolid, allsolid,
// hit contents byte -- seven s32. Returns 0 exactly where
// gpu64_levelTraceEnts() answers FALSE. pMovers as gpu64shim_move_ents.
extern "C" int gpu64shim_trace_ents( const int *pIn, int nHull,
				     const int *pMovers, int nMovers,
				     int *pOut )
{
	if ( !s_bOpen )
		return 0;
	if ( nMovers < 0 || nMovers > GPU64_LEVEL_MAX_MOVERS )
		return 0;

	Gpu64_LevelVec start, end;
	for ( unsigned k = 0; k < 3; k++ )
	{
		start.v[ k ] = pIn[ k ];
		end.v[ k ]   = pIn[ 3 + k ];
	}

	Gpu64_LevelMover mvrs[ GPU64_LEVEL_MAX_MOVERS ];
	for ( int i = 0; i < nMovers; i++ )
	{
		mvrs[ i ].nModel = (u16)pMovers[ i * 4 + 0 ];
		for ( unsigned k = 0; k < 3; k++ )
			mvrs[ i ].ofs.v[ k ] = pMovers[ i * 4 + 1 + k ];
	}

	Gpu64_LevelTrace tr;
	u8 hit = 0;
	if ( !gpu64_levelTraceEnts( &s_Lev, (unsigned)nHull, &start, &end, mvrs,
				    (unsigned)nMovers, &tr, &hit ) )
		return 0;

	for ( unsigned k = 0; k < 3; k++ )
		pOut[ k ] = tr.end.v[ k ];
	pOut[ 3 ] = tr.nFraction;
	pOut[ 4 ] = tr.bStartSolid ? 1 : 0;
	pOut[ 5 ] = tr.bAllSolid ? 1 : 0;
	pOut[ 6 ] = hit;
	return 1;
}

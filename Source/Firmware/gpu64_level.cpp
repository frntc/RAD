// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
//
// gpu64_level.cpp -- the .g64lev parser and the level pack lookup.
//
// See gpu64_level.h for the format and for why the pack is verified exactly
// once, before reuUsingPolling() ever runs.
//
#include "gpu64_level.h"
#include <circle/util.h>

// The pack lookup is firmware-only: tools/hostsim/levelsim.cpp links this
// file for gpu64_levelParse() -- so that there is exactly one .g64lev parser
// and the pre-bench gate tests the same code the Pi runs -- and has no
// reuMemory and no CLogger.
#ifndef GPU64_HOSTSIM
#include <circle/logger.h>
#endif

#ifndef GPU64_HOSTSIM
const u8 *gpu64LevelSlot[ GPU64_LEVEL_SLOTS ];
u32       gpu64LevelSlotBytes[ GPU64_LEVEL_SLOTS ];
u32       gpu64LevelSlotCount = 0;

static u32 levelFnv( const u8 *p, u32 n )
{
	u32 h = 2166136261u;
	while ( n-- )
		h = ( h ^ *p++ ) * 16777619u;
	return h;
}

// The level pack: the REU image the user selected in the RAD menu, which
// rad_main.cpp has read into reuMemory by the time this runs. gpu64 knows no
// file names -- a program ships as a .prg plus its .reu, and the pack is
// whatever that .reu holds. The slots point straight into it: the levels
// are used where they lie, so a program that DMAs into REU space over them
// destroys them. Returns the slots filled, 0 when pImg holds no pack or a
// broken one; a broken pack fills nothing, rather than some levels, so a
// bad image is never half-believed.
static u32 levelFromPack( const u8 *pImg, u32 nSize )
{
	extern CLogger *logger;

	if ( pImg == 0 || nSize < GPU64_PACK_HEADER
	     || gpu64_levelRd32( pImg ) != GPU64_PACK_MAGIC )
	{
		logger->Write( "gpu64", LogNotice,
			"Level pack: the REU image is not one -- LOAD_LEVEL and"
			" LEVEL_PICTURE will answer BAD_ARGS" );
		return 0;
	}

	const u32 nVer   = gpu64_levelRd16( pImg + 4 );
	const u32 nCount = gpu64_levelRd16( pImg + 6 );
	const u32 nImg   = gpu64_levelRd32( pImg + 8 );
	if ( nVer != GPU64_PACK_VERSION || nCount == 0 || nCount > GPU64_LEVEL_SLOTS
	     || nImg > nSize
	     || GPU64_PACK_HEADER + nCount * GPU64_PACK_ENTRY > nImg
	     || levelFnv( pImg + GPU64_PACK_HEADER, nCount * GPU64_PACK_ENTRY )
	        != gpu64_levelRd32( pImg + 12 ) )
	{
		logger->Write( "gpu64", LogNotice,
			"Level pack: bad header (v%u, %u levels, %u bytes in %u KB)"
			" -- not used", nVer, nCount, nImg, (unsigned)( nSize / 1024 ) );
		return 0;
	}

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *e    = pImg + GPU64_PACK_HEADER + i * GPU64_PACK_ENTRY;
		const u32 nOfs = gpu64_levelRd32( e );
		const u32 nLen = gpu64_levelRd32( e + 4 );
		Gpu64_Level lev;
		if ( nOfs > nImg || nLen > nImg - nOfs || ( nOfs & 63 )
		     || levelFnv( pImg + nOfs, nLen ) != gpu64_levelRd32( e + 8 )
		     || !gpu64_levelParse( &lev, pImg + nOfs, nLen ) )
		{
			logger->Write( "gpu64", LogNotice,
				"Level pack: level %u (%.4s) is damaged -- not used",
				i, (const char *)( e + 12 ) );
			return 0;
		}
	}

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *e = pImg + GPU64_PACK_HEADER + i * GPU64_PACK_ENTRY;
		gpu64LevelSlot[ i ]      = pImg + gpu64_levelRd32( e );
		gpu64LevelSlotBytes[ i ] = gpu64_levelRd32( e + 4 );
		logger->Write( "gpu64", LogNotice, "Level pack: slot %u = %.4s, %u KB",
			i, (const char *)( e + 12 ), (unsigned)( gpu64LevelSlotBytes[ i ] / 1024 ) );
	}
	return nCount;
}

// Run at every program launch, before reuUsingPolling(): the checksums are
// verified once here, not on each use.
void gpu64_levelPreload( void )
{
	for ( u32 i = 0; i < GPU64_LEVEL_SLOTS; i++ )
	{
		gpu64LevelSlot[ i ]      = 0;
		gpu64LevelSlotBytes[ i ] = 0;
	}

	extern u8 *reuMemory;
	extern u32 REU_SIZE_KB;
	gpu64LevelSlotCount = levelFromPack( reuMemory, REU_SIZE_KB * 1024 );
}

#endif	// GPU64_HOSTSIM

boolean gpu64_levelParse( Gpu64_Level *pL, const u8 *pFile, u32 nBytes )
{
	if ( pFile == 0 || nBytes < GPU64_LEVEL_HEADER_BYTES )
		return FALSE;
	if ( gpu64_levelRd32( pFile ) != GPU64_LEVEL_MAGIC )
		return FALSE;
	if ( gpu64_levelRd16( pFile + 4 ) != GPU64_LEVEL_VERSION )
		return FALSE;

	memset( pL, 0, sizeof( *pL ) );
	pL->pFile      = pFile;
	pL->nFileBytes = nBytes;

	pL->nScale = gpu64_levelRd16( pFile + 6 );
	pL->nTex   = gpu64_levelRd16( pFile + 8 );
	pL->nMesh  = gpu64_levelRd16( pFile + 10 );
	pL->nNode  = gpu64_levelRd16( pFile + 12 );
	pL->nEnt   = gpu64_levelRd16( pFile + 14 );
	pL->nPlane = gpu64_levelRd16( pFile + 16 );
	pL->nHull  = gpu64_levelRd16( pFile + 18 );
	pL->nClip  = gpu64_levelRd32( pFile + 20 );

	pL->nStartX   = (s32)gpu64_levelRd32( pFile + 52 );
	pL->nStartY   = (s32)gpu64_levelRd32( pFile + 56 );
	pL->nStartZ   = (s32)gpu64_levelRd32( pFile + 60 );
	pL->nStartYaw = gpu64_levelRd16( pFile + 64 );
	pL->nStepUp   = pFile[ 66 ];
	pL->nEye      = pFile[ 67 ];
	pL->nFeet     = pFile[ 68 ];
	pL->nWaist    = pFile[ 69 ];
	pL->bHasStart = ( gpu64_levelRd16( pFile + 70 ) & GPU64_LEVEL_HAS_START ) ? TRUE : FALSE;

	const u32 nBase = gpu64_levelRd32( pFile + 24 );
	pL->nPalOff = gpu64_levelRd32( pFile + 28 );
	pL->nStrOff = gpu64_levelRd32( pFile + 32 );

	if ( pL->nScale == 0 )
		return FALSE;			// a divisor, used as one below

	// u64 throughout: every count here comes off an SD card, and nClip
	// alone can multiply a u32 past the end of one.
	u64 o = GPU64_LEVEL_HEADER_BYTES;
	pL->pTexTab   = pFile + o;	o += (u64)pL->nTex   * GPU64_LEVEL_TEX_STRIDE;
	pL->pMeshTab  = pFile + o;	o += (u64)pL->nMesh  * GPU64_LEVEL_MESH_STRIDE;
	pL->pNodeTab  = pFile + o;	o += (u64)pL->nNode  * GPU64_LEVEL_NODE_STRIDE;
	pL->pEntTab   = pFile + o;	o += (u64)pL->nEnt   * GPU64_LEVEL_ENT_STRIDE;
	pL->pPlaneTab = pFile + o;	o += (u64)pL->nPlane * GPU64_LEVEL_PLANE_STRIDE;
	pL->pHullTab  = pFile + o;	o += (u64)pL->nHull  * GPU64_LEVEL_HULL_STRIDE;
	pL->pClipTab  = pFile + o;	o += (u64)pL->nClip  * GPU64_LEVEL_CLIP_STRIDE;

	// The tables must end exactly where the blob area begins. Not "at most":
	// an equality is the one test that catches a converter whose table
	// strides and the loader's have silently diverged, which is a class of
	// bug that otherwise renders a level made of garbage.
	if ( o != (u64)nBase )
		return FALSE;
	if ( (u64)nBase > (u64)nBytes )
		return FALSE;

	pL->pBlob      = pFile + nBase;
	pL->nBlobBytes = nBytes - nBase;

	// The palette is fixed-size and always read, so check it once here
	// rather than at every use.
	if ( gpu64_levelBlob( pL, pL->nPalOff, 768 ) == 0 )
		return FALSE;
	if ( (u64)pL->nStrOff > (u64)pL->nBlobBytes )
		return FALSE;

	// Visibility. Absent is allowed and means "cull nothing"; present but
	// inconsistent is refused, for the same reason the tables are: a
	// section whose header disagrees with its own length is a converter
	// and a loader that no longer agree on the layout.
	const u32 nVisOff = gpu64_levelRd32( pFile + 36 );
	const u32 nVisLen = gpu64_levelRd32( pFile + 40 );
	if ( nVisLen != 0 )
	{
		const u8 *pV = gpu64_levelBlob( pL, nVisOff, nVisLen );
		if ( pV == 0 || nVisLen < 12 )
			return FALSE;
		pL->nVisLeafs      = gpu64_levelRd16( pV + 0 );
		pL->nVisRowBytes   = gpu64_levelRd16( pV + 2 );
		pL->nVisWorldNodes = gpu64_levelRd16( pV + 4 );
		pL->nVisNodes      = gpu64_levelRd16( pV + 6 );
		pL->nVisHead       = gpu64_levelRd16( pV + 8 );
		if ( 12 + (u64)pL->nVisNodes * 8
		        + (u64)pL->nVisLeafs * pL->nVisRowBytes != nVisLen
		     || (u32)pL->nVisRowBytes * 8 < pL->nVisWorldNodes
		     || pL->nVisWorldNodes > pL->nNode
		     || pL->nVisHead >= pL->nVisNodes )
			return FALSE;
		pL->pVisNodes = pV + 12;
		pL->pVisRows  = pL->pVisNodes + (u32)pL->nVisNodes * 8;

		// The renderer matches a node to its bit by id *and* by mesh, and
		// the mesh half assumes world node j draws node 0's mesh + j. Node
		// 0's mesh is not mesh 0: the actor catalogue comes first in the
		// file. The converter builds them that way; a file that does not
		// gets no culling rather than the wrong culling.
		const u16 nMesh0 = pL->nVisWorldNodes ? gpu64_levelRd16( pL->pNodeTab ) : 0;
		for ( unsigned j = 0; j < pL->nVisWorldNodes; j++ )
			if ( gpu64_levelRd16( pL->pNodeTab + j * GPU64_LEVEL_NODE_STRIDE ) != nMesh0 + j )
				pL->nVisLeafs = 0;
	}

	// Pictures (v7): u16 count, u16 zero, then count x <HHI> w, h, offset.
	// Every picture is range-checked here, once, so that drawing one is a
	// straight copy -- and a section that disagrees with its own length is
	// refused like the visibility section above, for the same reason.
	const u32 nPicOff = gpu64_levelRd32( pFile + 44 );
	const u32 nPicLen = gpu64_levelRd32( pFile + 48 );
	if ( nPicLen != 0 )
	{
		const u8 *pP = gpu64_levelBlob( pL, nPicOff, nPicLen );
		if ( pP == 0 || nPicLen < 4 )
			return FALSE;
		const u16 n = gpu64_levelRd16( pP );
		if ( 4 + (u64)n * 8 != nPicLen )
			return FALSE;
		for ( unsigned i = 0; i < n; i++ )
		{
			const u8 *r = pP + 4 + i * 8;
			const u32 w = gpu64_levelRd16( r ), h = gpu64_levelRd16( r + 2 );
			if ( w == 0 || h == 0
			     || gpu64_levelBlob( pL, gpu64_levelRd32( r + 4 ), w * h ) == 0 )
				return FALSE;
		}
		pL->nPic    = n;
		pL->pPicTab = pP + 4;
	}

	return TRUE;
}

const u8 *gpu64_levelPic( const Gpu64_Level *pL, unsigned nIndex,
			  unsigned *pW, unsigned *pH )
{
	if ( nIndex >= pL->nPic )
		return 0;
	const u8 *r = pL->pPicTab + nIndex * 8;
	*pW = gpu64_levelRd16( r );
	*pH = gpu64_levelRd16( r + 2 );
	return pL->pBlob + gpu64_levelRd32( r + 4 );
}

const u8 *gpu64_levelBlob( const Gpu64_Level *pL, u32 nOff, u32 nLen )
{
	if ( (u64)nOff + nLen > (u64)pL->nBlobBytes )
		return 0;
	return pL->pBlob + nOff;
}

const char *gpu64_levelStr( const Gpu64_Level *pL, u32 nOff )
{
	if ( (u64)pL->nStrOff + nOff >= (u64)pL->nBlobBytes )
		return 0;

	const char *pS  = (const char *)( pL->pBlob + pL->nStrOff + nOff );
	const u32   nMax = pL->nBlobBytes - pL->nStrOff - nOff;

	for ( u32 i = 0; i < nMax; i++ )
		if ( pS[ i ] == 0 )
			return pS;
	return 0;				// unterminated -- refuse it
}

boolean gpu64_levelEnt( const Gpu64_Level *pL, unsigned nIndex, Gpu64_LevelEnt *pE )
{
	if ( nIndex >= pL->nEnt )
		return FALSE;

	const u8 *p = pL->pEntTab + (u32)nIndex * GPU64_LEVEL_ENT_STRIDE;

	pE->pClassName  = gpu64_levelStr( pL, gpu64_levelRd16( p + 0 ) );
	pE->nYaw        = gpu64_levelRd16( p + 2 );
	pE->nSpawnFlags = gpu64_levelRd16( p + 4 );
	pE->nModelIdx   = gpu64_levelRd16( p + 6 );
	pE->nTargetId     = gpu64_levelRd16( p + 8 );
	pE->nTargetNameId = gpu64_levelRd16( p + 10 );
	pE->pTarget     = gpu64_levelStr( pL, pE->nTargetId );
	pE->pTargetName = gpu64_levelStr( pL, pE->nTargetNameId );
	pE->nParam0     = gpu64_levelRdS16( p + 12 );
	pE->nParam1     = gpu64_levelRdS16( p + 14 );
	pE->x           = gpu64_levelRdS32( p + 16 );
	pE->y           = gpu64_levelRdS32( p + 20 );
	pE->z           = gpu64_levelRdS32( p + 24 );
	pE->ofsX        = gpu64_levelRdS32( p + 28 );
	pE->ofsY        = gpu64_levelRdS32( p + 32 );
	pE->ofsZ        = gpu64_levelRdS32( p + 36 );
	pE->nKind       = p[ 40 ];

	return pE->pClassName != 0 && pE->pTarget != 0 && pE->pTargetName != 0;
}

boolean gpu64_levelNode( const Gpu64_Level *pL, unsigned nIndex,
			 u16 *pMeshIdx, Gpu64_LevelVec *pPos, u16 *pModelIdx )
{
	if ( nIndex >= pL->nNode )
		return FALSE;

	const u8 *p = pL->pNodeTab + (u32)nIndex * GPU64_LEVEL_NODE_STRIDE;

	if ( pMeshIdx )
		*pMeshIdx = gpu64_levelRd16( p + 0 );
	if ( pPos )
		for ( unsigned k = 0; k < 3; k++ )
			pPos->v[ k ] = gpu64_levelRdS32( p + 2 + k * 4 );
	if ( pModelIdx )
		*pModelIdx = gpu64_levelRd16( p + 14 );

	return TRUE;
}

// --- collision -----------------------------------------------------------
//
// Quake's SV_RecursiveHullCheck, SV_HullPointContents and SV_FlyMove, in
// 16.16 over the tables gen_quakelevel.py wrote. Nothing here re-derives a
// hull: the BSP compiler already expanded them by the size of the thing that
// walks them, so this is a point trace and the size is chosen by picking the
// tree.
//
// The arithmetic is s64 throughout the plane tests. A world coordinate is at
// most 128 units, which is 2^23 in 16.16, and a plane normal is 2^16, so a
// single term of the dot product is 2^39 -- outside s32 by seven bits and
// inside s64 by twenty-four.

// A plane's normal arrives as three 1.15 fixed-point components. 1.15 to
// 16.16 is a shift of one, which turns the converter's 32767 into 65534: a
// unit normal comes back 0.99997 long. That is three parts in a hundred
// thousand on a test whose epsilon is one part in a thousand, and the axial
// planes -- which are most of a Quake level -- do not go through this path at
// all.
static void planeRead( const Gpu64_Level *pL, unsigned i, s32 *pN, s32 *pDist, u8 *pType )
{
	const u8 *p = pL->pPlaneTab + (u32)i * GPU64_LEVEL_PLANE_STRIDE;
	pN[ 0 ] = (s32)gpu64_levelRdS16( p     ) << 1;
	pN[ 1 ] = (s32)gpu64_levelRdS16( p + 2 ) << 1;
	pN[ 2 ] = (s32)gpu64_levelRdS16( p + 4 ) << 1;
	*pDist  = gpu64_levelRdS32( p + 6 );
	*pType  = p[ 10 ];
}

// Signed distance from the plane to a point, 16.16. `type` names the axis an
// axial plane is perpendicular to; taking that component directly is both
// faster and exact, which is why the converter permutes `type` through the
// same axis swap the geometry goes through.
static inline s64 planeDist( const s32 *pN, u8 nType, s32 nDist, const s32 *pP )
{
	if ( nType < 3 )
		return (s64)pP[ nType ] - nDist;
	return ( ( (s64)pN[ 0 ] * pP[ 0 ] + (s64)pN[ 1 ] * pP[ 1 ] +
		   (s64)pN[ 2 ] * pP[ 2 ] ) >> 16 ) - nDist;
}

boolean gpu64_levelHull( const Gpu64_Level *pL, unsigned nModel, Gpu64_LevelHull *pH )
{
	if ( pL == 0 || pH == 0 || nModel >= pL->nHull )
		return FALSE;

	const u8 *p = pL->pHullTab + (u32)nModel * GPU64_LEVEL_HULL_STRIDE;
	pH->nHead[ 0 ] = gpu64_levelRdS32( p );
	pH->nHead[ 1 ] = gpu64_levelRdS32( p + 4 );
	pH->nHead0     = gpu64_levelRdS32( p + 32 );
	for ( unsigned k = 0; k < 3; k++ )
	{
		pH->mins.v[ k ] = gpu64_levelRdS32( p + 8  + k * 4 );
		pH->maxs.v[ k ] = gpu64_levelRdS32( p + 20 + k * 4 );
	}
	return TRUE;
}

s16 gpu64_levelPointContents( const Gpu64_Level *pL, s32 nNum, const Gpu64_LevelVec *pP )
{
	unsigned nSteps = 0;

	while ( nNum >= 0 )
	{
		if ( (u32)nNum >= pL->nClip || ++nSteps > GPU64_LEVEL_TRACE_MAXDEPTH )
			return GPU64_LEVEL_CONTENTS_SOLID;

		const u8 *pC = pL->pClipTab + (u32)nNum * GPU64_LEVEL_CLIP_STRIDE;
		const u16 nPl = gpu64_levelRd16( pC );
		if ( nPl >= pL->nPlane )
			return GPU64_LEVEL_CONTENTS_SOLID;

		s32 n[ 3 ], nDist;
		u8  nType;
		planeRead( pL, nPl, n, &nDist, &nType );

		nNum = ( planeDist( n, nType, nDist, pP->v ) < 0 )
			? gpu64_levelRdS16( pC + 4 )		// child 1, behind
			: gpu64_levelRdS16( pC + 2 );		// child 0, in front
	}

	return (s16)nNum;
}

const u8 *gpu64_levelVisRow( const Gpu64_Level *pL, const Gpu64_LevelVec *pEye,
			     u16 *pnNodes )
{
	if ( pL == 0 || pL->nVisLeafs == 0 )
		return 0;

	s32 nNum = pL->nVisHead;
	unsigned nSteps = 0;
	while ( nNum >= 0 )
	{
		if ( (u32)nNum >= pL->nVisNodes || ++nSteps > GPU64_LEVEL_TRACE_MAXDEPTH )
			return 0;

		const u8 *pN = pL->pVisNodes + (u32)nNum * 8;
		const u16 nPl = gpu64_levelRd16( pN );
		if ( nPl >= pL->nPlane )
			return 0;

		s32 n[ 3 ], nDist;
		u8  nType;
		planeRead( pL, nPl, n, &nDist, &nType );

		nNum = ( planeDist( n, nType, nDist, pEye->v ) < 0 )
			? gpu64_levelRdS16( pN + 4 )
			: gpu64_levelRdS16( pN + 2 );
	}

	const s32 nLeaf = -1 - nNum;
	if ( nLeaf <= 0 || nLeaf >= pL->nVisLeafs )
		return 0;

	*pnNodes = pL->nVisWorldNodes;
	return pL->pVisRows + (u32)nLeaf * pL->nVisRowBytes;
}

// The sweep, on the segment [p1f, p2f] of the caller's original line. Returns
// TRUE while nothing has been hit -- Quake's own convention, where FALSE means
// "the trace is finished, do not keep looking".
static boolean hullRecurse( const Gpu64_Level *pL, s32 nNum, s32 p1f, s32 p2f,
			    const s32 *p1, const s32 *p2,
			    Gpu64_LevelTrace *pTr, unsigned nDepth )
{
	if ( nNum < 0 )
	{
		if ( nNum != GPU64_LEVEL_CONTENTS_SOLID )
			pTr->bAllSolid = FALSE;
		else
			pTr->bStartSolid = TRUE;
		return TRUE;				// empty space, keep going
	}

	// Both of these mean the level file is wrong, and the safe answer to
	// that is the one that stops the player.
	if ( (u32)nNum >= pL->nClip || nDepth >= GPU64_LEVEL_TRACE_MAXDEPTH )
		return FALSE;

	const u8 *pC = pL->pClipTab + (u32)nNum * GPU64_LEVEL_CLIP_STRIDE;
	const u16 nPl = gpu64_levelRd16( pC );
	if ( nPl >= pL->nPlane )
		return FALSE;

	const s32 nChild[ 2 ] = { gpu64_levelRdS16( pC + 2 ), gpu64_levelRdS16( pC + 4 ) };

	s32 n[ 3 ], nDist;
	u8  nType;
	planeRead( pL, nPl, n, &nDist, &nType );

	const s64 t1 = planeDist( n, nType, nDist, p1 );
	const s64 t2 = planeDist( n, nType, nDist, p2 );

	if ( t1 >= 0 && t2 >= 0 )
		return hullRecurse( pL, nChild[ 0 ], p1f, p2f, p1, p2, pTr, nDepth + 1 );
	if ( t1 < 0 && t2 < 0 )
		return hullRecurse( pL, nChild[ 1 ], p1f, p2f, p1, p2, pTr, nDepth + 1 );

	// The segment crosses the plane. Stop DIST_EPSILON short of it, on
	// whichever side the sweep started, so the endpoint is outside the
	// solid rather than exactly on its surface.
	const s64 nDen = t1 - t2;			// never 0: the signs differ
	const s32 nEps = gpu64_levelEpsilon( pL );
	const s64 nNumer = ( t1 < 0 ) ? ( t1 + nEps ) : ( t1 - nEps );

	s32 nFrac = (s32)( ( nNumer << 16 ) / nDen );
	if ( nFrac < 0 )       nFrac = 0;
	if ( nFrac > 0x10000 ) nFrac = 0x10000;

	s32 mid[ 3 ];
	s32 midf = p1f + (s32)( ( (s64)( p2f - p1f ) * nFrac ) >> 16 );
	for ( unsigned k = 0; k < 3; k++ )
		mid[ k ] = p1[ k ] + (s32)( ( (s64)( p2[ k ] - p1[ k ] ) * nFrac ) >> 16 );

	const unsigned nSide = ( t1 < 0 ) ? 1 : 0;

	// The near half first: if anything in there stops the sweep, the far
	// half never happened.
	if ( !hullRecurse( pL, nChild[ nSide ], p1f, midf, p1, mid, pTr, nDepth + 1 ) )
		return FALSE;

	Gpu64_LevelVec vMid;
	vMid.v[ 0 ] = mid[ 0 ]; vMid.v[ 1 ] = mid[ 1 ]; vMid.v[ 2 ] = mid[ 2 ];

	if ( gpu64_levelPointContents( pL, nChild[ nSide ^ 1 ], &vMid )
	     != GPU64_LEVEL_CONTENTS_SOLID )
		return hullRecurse( pL, nChild[ nSide ^ 1 ], midf, p2f, mid, p2, pTr, nDepth + 1 );

	if ( pTr->bAllSolid )
		return FALSE;			// it never got out of solid

	// This is the impact. The normal is turned to face back along the
	// sweep, which is what makes the slide in gpu64_levelMove() push the
	// move away from the surface rather than into it.
	pTr->bHitPlane = TRUE;
	for ( unsigned k = 0; k < 3; k++ )
		pTr->nPlaneN[ k ] = ( nSide == 0 ) ? n[ k ] : -n[ k ];

	// Back off until the endpoint is genuinely outside the solid. The
	// epsilon above puts it there for the plane just crossed; it says
	// nothing about a second surface meeting this one within the epsilon,
	// which is what an inside corner is.
	unsigned nBack = 0;
	while ( gpu64_levelPointContents( pL, pTr->nHeadNode, &vMid )
		== GPU64_LEVEL_CONTENTS_SOLID )
	{
		nFrac -= 0x1999;		// 0.1, Quake's own step
		if ( nFrac < 0 || ++nBack > 16 )
		{
			pTr->nFraction = midf;
			pTr->end       = vMid;
			return FALSE;
		}

		midf = p1f + (s32)( ( (s64)( p2f - p1f ) * nFrac ) >> 16 );
		for ( unsigned k = 0; k < 3; k++ )
			vMid.v[ k ] = p1[ k ] + (s32)( ( (s64)( p2[ k ] - p1[ k ] ) * nFrac ) >> 16 );
	}

	pTr->nFraction = midf;
	pTr->end       = vMid;
	return FALSE;
}

void gpu64_levelTrace( const Gpu64_Level *pL, s32 nHeadNode,
		       const Gpu64_LevelVec *pStart, const Gpu64_LevelVec *pEnd,
		       Gpu64_LevelTrace *pTr )
{
	memset( pTr, 0, sizeof( *pTr ) );
	pTr->nHeadNode  = nHeadNode;
	pTr->bAllSolid  = TRUE;			// cleared by the first empty leaf
	pTr->nFraction  = 0x10000;
	pTr->end        = *pEnd;

	hullRecurse( pL, nHeadNode, 0, 0x10000, pStart->v, pEnd->v, pTr, 0 );
}

// A trace against the world and against every brush model the caller named,
// which is Quake's SV_Move: sweep each hull in turn, keep the nearest impact,
// and let being inside any one of them be being stuck. A model's hull sits
// where the BSP compiler left it, so the sweep is moved into the model's
// frame rather than the hull into the world's, and the answer is brought back
// by the same offset.
typedef struct
{
	s32		nHead;
	Gpu64_LevelVec	ofs;
}
Gpu64_LevelMoverHull;

static void traceMulti( const Gpu64_Level *pL, s32 nHeadNode,
			const Gpu64_LevelMoverHull *pM, unsigned nM,
			const Gpu64_LevelVec *pStart, const Gpu64_LevelVec *pEnd,
			Gpu64_LevelTrace *pTr )
{
	gpu64_levelTrace( pL, nHeadNode, pStart, pEnd, pTr );

	for ( unsigned i = 0; i < nM; i++ )
	{
		Gpu64_LevelVec s2, e2;
		for ( unsigned k = 0; k < 3; k++ )
		{
			s2.v[ k ] = pStart->v[ k ] - pM[ i ].ofs.v[ k ];
			e2.v[ k ] = pEnd->v[ k ]   - pM[ i ].ofs.v[ k ];
		}

		Gpu64_LevelTrace t2;
		gpu64_levelTrace( pL, pM[ i ].nHead, &s2, &e2, &t2 );

		// Quake's order, and it matters: allsolid and startsolid take
		// the whole trace over regardless of fraction, because a sweep
		// that begins inside a door is not a sweep that got 100% of
		// the way to its destination.
		const boolean bTake = t2.bAllSolid || t2.bStartSolid ||
				      t2.nFraction < pTr->nFraction;
		if ( !bTake )
			continue;

		for ( unsigned k = 0; k < 3; k++ )
			t2.end.v[ k ] += pM[ i ].ofs.v[ k ];

		const boolean bAll   = pTr->bAllSolid   || t2.bAllSolid;
		const boolean bStart = pTr->bStartSolid || t2.bStartSolid;
		*pTr = t2;
		pTr->bAllSolid   = bAll;
		pTr->bStartSolid = bStart;
	}
}

static u8 contentsToByte( s16 nC );

static inline s32 hullHead( const Gpu64_LevelHull *pH, unsigned nHull )
{
	return nHull == 0 ? pH->nHead0 : pH->nHead[ nHull - 1 ];
}

boolean gpu64_levelTraceEnts( const Gpu64_Level *pL, unsigned nHull,
			      const Gpu64_LevelVec *pStart, const Gpu64_LevelVec *pEnd,
			      const Gpu64_LevelMover *pMovers, unsigned nMovers,
			      Gpu64_LevelTrace *pTr, u8 *pHit )
{
	if ( pL == 0 || pStart == 0 || pEnd == 0 || pTr == 0 || nHull > 2 )
		return FALSE;
	if ( nMovers > GPU64_LEVEL_MAX_MOVERS || ( nMovers != 0 && pMovers == 0 ) )
		return FALSE;

	Gpu64_LevelHull hull;
	if ( !gpu64_levelHull( pL, 0, &hull ) )
		return FALSE;

	// Resolved as gpu64_levelMoveEnts() resolves them, and refused for the
	// same reason: a door that stops blocking is a shot through a door.
	Gpu64_LevelMoverHull mv[ GPU64_LEVEL_MAX_MOVERS ];
	unsigned nMv = 0;
	for ( unsigned i = 0; i < nMovers; i++ )
	{
		if ( pMovers[ i ].nModel == 0 )
			continue;
		Gpu64_LevelHull mh;
		if ( !gpu64_levelHull( pL, pMovers[ i ].nModel, &mh ) )
			return FALSE;
		mv[ nMv ].nHead = hullHead( &mh, nHull );
		mv[ nMv ].ofs   = pMovers[ i ].ofs;
		nMv++;
	}

	const s32 nHead = hullHead( &hull, nHull );
	traceMulti( pL, nHead, mv, nMv, pStart, pEnd, pTr );

	if ( pHit != 0 )
	{
		*pHit = GPU64_CLIP_CONT_EMPTY;
		if ( pTr->bHitPlane && pTr->nFraction < 0x10000 )
		{
			// The normal faces back at the sweep, so behind the surface
			// is end - n. One Quake unit clears the trace's back-off.
			const s32 nQu = gpu64_levelQU( pL, 1 );
			Gpu64_LevelVec p;
			for ( unsigned k = 0; k < 3; k++ )
				p.v[ k ] = pTr->end.v[ k ] -
					   (s32)( ( (s64)pTr->nPlaneN[ k ] * nQu ) >> 16 );
			*pHit = contentsToByte( gpu64_levelPointContents( pL, nHead, &p ) );
			if ( *pHit == GPU64_CLIP_CONT_EMPTY )
				*pHit = GPU64_CLIP_CONT_SOLID;	// a mover's face
		}
	}
	return TRUE;
}

// --- the move ------------------------------------------------------------

// Quake's 0.7: a surface this close to level is something you stand on, and
// anything else is something you slide along.
#define GPU64_LEVEL_FLOOR_NY	45875		// 0.7 in 16.16

// Quake's STOP_EPSILON, 0.1 Quake units -- 205 at the converter's scale. A
// slide leaves a component that is the rounding of a division; without this
// the move dribbles sideways for as many iterations as it is given. Passed in
// rather than derived here so clipDisp() stays a pure vector operation.

#define GPU64_LEVEL_MOVE_BUMPS	4
#define GPU64_LEVEL_MOVE_PLANES	5

static inline s64 dot16( const s32 *pA, const s32 *pB )
{
	return ( (s64)pA[ 0 ] * pB[ 0 ] + (s64)pA[ 1 ] * pB[ 1 ] + (s64)pA[ 2 ] * pB[ 2 ] ) >> 16;
}

// out = in - n * ( in . n ): the component along the surface normal removed,
// which is what turns "stopped by a wall" into "slid along it".
static void clipDisp( s32 *pOut, const s32 *pIn, const s32 *pN, s32 nStopEps )
{
	const s64 d = dot16( pIn, pN );
	for ( unsigned k = 0; k < 3; k++ )
	{
		pOut[ k ] = pIn[ k ] - (s32)( ( (s64)pN[ k ] * d ) >> 16 );
		if ( pOut[ k ] > -nStopEps && pOut[ k ] < nStopEps )
			pOut[ k ] = 0;
	}
}

static inline boolean isZero3( const s32 *pV )
{
	return pV[ 0 ] == 0 && pV[ 1 ] == 0 && pV[ 2 ] == 0;
}

// Quake's SV_FlyMove with time folded into the vectors: `vel` is the whole
// step's displacement and `t` is the 16.16 fraction of it still owed. The
// structure -- clip the *last committed* displacement against every plane
// collected so far, fall back to the crease where two planes meet, give up if
// the result reverses -- is Quake's line for line, because the corner cases it
// handles are exactly the ones a room full of right angles produces.
static u8 flyMove( const Gpu64_Level *pL, s32 nHead, boolean bSlide,
		   const Gpu64_LevelMoverHull *pM, unsigned nM,
		   const Gpu64_LevelVec *pStart, const Gpu64_LevelVec *pDelta,
		   Gpu64_LevelVec *pEnd, u8 *pBumps )
{
	u8  nFlags = 0;
	s32 t      = 0x10000;

	// 0.1 Quake units, Quake's STOP_EPSILON, at this level's scale.
	const s32 nStopEps = gpu64_levelQU( pL, 1 ) / 10;

	Gpu64_LevelVec pos = *pStart;
	s32 vel[ 3 ], primal[ 3 ], orig[ 3 ], nNew[ 3 ];
	s32 planes[ GPU64_LEVEL_MOVE_PLANES ][ 3 ];
	unsigned nPlanes = 0, nBump;

	for ( unsigned k = 0; k < 3; k++ )
		vel[ k ] = primal[ k ] = orig[ k ] = pDelta->v[ k ];

	for ( nBump = 0; nBump < GPU64_LEVEL_MOVE_BUMPS; nBump++ )
	{
		if ( isZero3( vel ) || t == 0 )
			break;

		Gpu64_LevelVec end;
		for ( unsigned k = 0; k < 3; k++ )
			end.v[ k ] = pos.v[ k ] + (s32)( ( (s64)vel[ k ] * t ) >> 16 );

		Gpu64_LevelTrace tr;
		traceMulti( pL, nHead, pM, nM, &pos, &end, &tr );

		if ( tr.bAllSolid )
		{
			// Inside the world with nowhere to go. Moving on any
			// answer here is how a player ends up outside the map.
			nFlags |= GPU64_CLIP_ALLSOLID;
			break;
		}
		if ( tr.bStartSolid )
			nFlags |= GPU64_CLIP_STARTSOLID;

		if ( tr.nFraction > 0 )
		{
			pos = tr.end;
			for ( unsigned k = 0; k < 3; k++ )
				orig[ k ] = vel[ k ];
			nPlanes = 0;
		}
		if ( tr.nFraction >= 0x10000 )
			break;				// the whole move happened
		if ( !tr.bHitPlane )
			break;				// stopped with nothing to slide on

		if ( tr.nPlaneN[ 1 ] > GPU64_LEVEL_FLOOR_NY )
			nFlags |= GPU64_CLIP_ONGROUND;
		else if ( tr.nPlaneN[ 1 ] < -GPU64_LEVEL_FLOOR_NY )
			nFlags |= GPU64_CLIP_CEILING;
		else
			nFlags |= GPU64_CLIP_WALL;

		t -= (s32)( ( (s64)t * tr.nFraction ) >> 16 );

		if ( !bSlide )
			break;

		if ( nPlanes >= GPU64_LEVEL_MOVE_PLANES )
		{
			nFlags |= GPU64_CLIP_TRUNCATED;
			break;
		}
		for ( unsigned k = 0; k < 3; k++ )
			planes[ nPlanes ][ k ] = tr.nPlaneN[ k ];
		nPlanes++;

		// A direction that runs along every plane hit since the last
		// time the move actually advanced.
		unsigned i, j;
		for ( i = 0; i < nPlanes; i++ )
		{
			clipDisp( nNew, orig, planes[ i ], nStopEps );
			for ( j = 0; j < nPlanes; j++ )
				if ( j != i && dot16( nNew, planes[ j ] ) < 0 )
					break;
			if ( j == nPlanes )
				break;
		}

		if ( i != nPlanes )
		{
			for ( unsigned k = 0; k < 3; k++ )
				vel[ k ] = nNew[ k ];
		}
		else if ( nPlanes == 2 )
		{
			// Two planes and no single slide direction: the crease
			// between them is the only way out.
			s32 dir[ 3 ];
			dir[ 0 ] = (s32)( ( (s64)planes[0][1] * planes[1][2] -
					    (s64)planes[0][2] * planes[1][1] ) >> 16 );
			dir[ 1 ] = (s32)( ( (s64)planes[0][2] * planes[1][0] -
					    (s64)planes[0][0] * planes[1][2] ) >> 16 );
			dir[ 2 ] = (s32)( ( (s64)planes[0][0] * planes[1][1] -
					    (s64)planes[0][1] * planes[1][0] ) >> 16 );
			const s64 d = dot16( dir, vel );
			for ( unsigned k = 0; k < 3; k++ )
				vel[ k ] = (s32)( ( (s64)dir[ k ] * d ) >> 16 );
		}
		else
		{
			// Three or more: a corner. Stop rather than pick one.
			break;
		}

		// A slide that ends up pointing back the way it came is the
		// inside of a corner squeezing the move out. Quake stops here
		// and so does this, because the alternative is a jitter.
		if ( dot16( vel, primal ) <= 0 )
			break;
	}

	if ( nBump >= GPU64_LEVEL_MOVE_BUMPS )
		nFlags |= GPU64_CLIP_TRUNCATED;

	*pEnd   = pos;
	*pBumps = (u8)nBump;
	return nFlags;
}

// Horizontal distance squared, 16.16 squared in a s64. Only ever compared
// against another one of itself, so the scale never has to come back.
static inline s64 horizDist2( const Gpu64_LevelVec *pA, const Gpu64_LevelVec *pB )
{
	const s64 dx = (s64)pA->v[ 0 ] - pB->v[ 0 ];
	const s64 dz = (s64)pA->v[ 2 ] - pB->v[ 2 ];
	return dx * dx + dz * dz;
}

static u8 contentsToByte( s16 nC )
{
	switch ( nC )
	{
	case GPU64_LEVEL_CONTENTS_EMPTY:	return GPU64_CLIP_CONT_EMPTY;
	case GPU64_LEVEL_CONTENTS_SOLID:	return GPU64_CLIP_CONT_SOLID;
	case GPU64_LEVEL_CONTENTS_WATER:	return GPU64_CLIP_CONT_WATER;
	case GPU64_LEVEL_CONTENTS_SLIME:	return GPU64_CLIP_CONT_SLIME;
	case GPU64_LEVEL_CONTENTS_LAVA:		return GPU64_CLIP_CONT_LAVA;
	case GPU64_LEVEL_CONTENTS_SKY:		return GPU64_CLIP_CONT_SKY;
	}
	return GPU64_CLIP_CONT_UNKNOWN;
}

boolean gpu64_levelMove( const Gpu64_Level *pL, unsigned nModel, unsigned nHull,
			 u8 nMode, const Gpu64_LevelVec *pStart,
			 const Gpu64_LevelVec *pDelta, Gpu64_LevelMove *pOut )
{
	return gpu64_levelMoveEnts( pL, nModel, nHull, nMode, pStart, pDelta,
				    0, 0, pOut );
}

boolean gpu64_levelMoveEnts( const Gpu64_Level *pL, unsigned nModel, unsigned nHull,
			     u8 nMode, const Gpu64_LevelVec *pStart,
			     const Gpu64_LevelVec *pDelta,
			     const Gpu64_LevelMover *pMovers, unsigned nMovers,
			     Gpu64_LevelMove *pOut )
{
	if ( pL == 0 || pStart == 0 || pDelta == 0 || pOut == 0 )
		return FALSE;
	if ( nHull < 1 || nHull > 2 )
		return FALSE;
	if ( nMovers > GPU64_LEVEL_MAX_MOVERS || ( nMovers != 0 && pMovers == 0 ) )
		return FALSE;

	Gpu64_LevelHull hull;
	if ( !gpu64_levelHull( pL, nModel, &hull ) )
		return FALSE;

	// The movers' hulls, resolved once. A model the level does not have is
	// the caller's mistake and is refused here rather than silently not
	// blocking: a door that stops blocking is how a player leaves the map.
	Gpu64_LevelMoverHull mv[ GPU64_LEVEL_MAX_MOVERS ];
	unsigned nMv = 0;
	for ( unsigned i = 0; i < nMovers; i++ )
	{
		if ( pMovers[ i ].nModel == 0 )
			continue;		// an empty slot, not an error
		Gpu64_LevelHull mh;
		if ( !gpu64_levelHull( pL, pMovers[ i ].nModel, &mh ) )
			return FALSE;
		mv[ nMv ].nHead = mh.nHead[ nHull - 1 ];
		mv[ nMv ].ofs   = pMovers[ i ].ofs;
		nMv++;
	}

	const s32 nHead    = hull.nHead[ nHull - 1 ];
	const s32 nStepUp  = gpu64_levelQU( pL, pL->nStepUp );
	const s32 nEyeOfs  = gpu64_levelEyeOfs( pL );

	memset( pOut, 0, sizeof( *pOut ) );

	// The hulls are expanded around the player *origin*, not the eye. A
	// caller working in camera coordinates says so and gets them back.
	Gpu64_LevelVec start = *pStart;
	if ( nMode & GPU64_CLIP_MODE_EYE )
		start.v[ 1 ] -= nEyeOfs;

	const boolean bSlide = ( nMode & GPU64_CLIP_MODE_SLIDE ) != 0;

	Gpu64_LevelVec down;
	u8 nBumps = 0;
	u8 nFlags = flyMove( pL, nHead, bSlide, mv, nMv, &start, pDelta, &down, &nBumps );

	// The step. Quake runs the whole move a second time from a position one
	// step higher, drops back down, and keeps whichever of the two got
	// further along the ground -- rather than deciding up front whether a
	// step is called for, which cannot be known until both have been tried.
	if ( ( nMode & GPU64_CLIP_MODE_STEP ) &&
	     ( nFlags & GPU64_CLIP_WALL ) &&
	     !( nFlags & GPU64_CLIP_ALLSOLID ) )
	{
		Gpu64_LevelVec up = start, upEnd, stepped, target;
		Gpu64_LevelTrace tr;

		up.v[ 1 ] += nStepUp;
		traceMulti( pL, nHead, mv, nMv, &start, &up, &tr );
		upEnd = tr.end;				// a low ceiling cuts it short

		u8 nStepBumps = 0;
		u8 nStepFlags = flyMove( pL, nHead, bSlide, mv, nMv, &upEnd, pDelta,
					 &stepped, &nStepBumps );

		// Back down by however far the step-up actually rose, plus the
		// step again: landing lower than it started is a stair going
		// down, which is the same move.
		target = stepped;
		target.v[ 1 ] -= ( upEnd.v[ 1 ] - start.v[ 1 ] ) + nStepUp;
		traceMulti( pL, nHead, mv, nMv, &stepped, &target, &tr );

		// Only a floor counts. Landing on a wall means the step went
		// out over a ledge and the un-stepped move is the honest one.
		if ( !tr.bAllSolid && tr.bHitPlane &&
		     tr.nPlaneN[ 1 ] > GPU64_LEVEL_FLOOR_NY &&
		     horizDist2( &tr.end, &start ) > horizDist2( &down, &start ) )
		{
			down    = tr.end;
			nFlags  = (u8)( ( nStepFlags & ~GPU64_CLIP_WALL ) |
					GPU64_CLIP_STEPPED | GPU64_CLIP_ONGROUND );
			nBumps  = nStepBumps;
		}
	}

	// Settle onto the floor. Without this the eye keeps whatever height
	// the last horizontal move left it at, which on a downward slope is
	// the air above the slope.
	if ( ( nMode & GPU64_CLIP_MODE_FLOOR ) && !( nFlags & GPU64_CLIP_ALLSOLID ) )
	{
		Gpu64_LevelVec target = down;
		Gpu64_LevelTrace tr;

		target.v[ 1 ] -= nStepUp;
		traceMulti( pL, nHead, mv, nMv, &down, &target, &tr );

		if ( !tr.bAllSolid && tr.bHitPlane && tr.nFraction < 0x10000 &&
		     tr.nPlaneN[ 1 ] > GPU64_LEVEL_FLOOR_NY )
		{
			down    = tr.end;
			nFlags |= GPU64_CLIP_ONGROUND;
		}
		else
			nFlags &= (u8)~GPU64_CLIP_ONGROUND;
	}

	// What the walker is standing in. Not against nHead: the clip hulls
	// are the level grown by a box, and keep only solid and empty, so
	// liquids are simply not there. Hull 0 has them. The type is the
	// feet's (the level's nFeet below the origin), and the level counts
	// how far up the body -- nWaist, then nEye -- the same liquid reaches.
	{
		const s32 nHead0 = hull.nHead0;
		Gpu64_LevelVec p = down;
		p.v[ 1 ] -= gpu64_levelQU( pL, pL->nFeet );
		const s16 nFeet = gpu64_levelPointContents( pL, nHead0, &p );
		pOut->nContents = contentsToByte( nFeet );
		if ( nFeet <= GPU64_LEVEL_CONTENTS_WATER &&
		     nFeet >= GPU64_LEVEL_CONTENTS_LAVA )
		{
			pOut->nWaterLevel = 1;
			p = down;
			p.v[ 1 ] += gpu64_levelQU( pL, pL->nWaist );
			if ( gpu64_levelPointContents( pL, nHead0, &p ) == nFeet )
			{
				pOut->nWaterLevel = 2;
				p = down;
				p.v[ 1 ] += nEyeOfs;
				if ( gpu64_levelPointContents( pL, nHead0, &p ) == nFeet )
					pOut->nWaterLevel = 3;
			}
		}
	}

	if ( nMode & GPU64_CLIP_MODE_EYE )
		down.v[ 1 ] += nEyeOfs;

	pOut->end    = down;
	pOut->nFlags = nFlags;
	pOut->nBumps = nBumps;

	// How much of the requested displacement was actually covered, as a
	// byte, measured on the horizontal plane -- the vertical part is the
	// floor settle and a caller asking "did I get where I aimed" does not
	// mean it.
	{
		Gpu64_LevelVec want = start;
		want.v[ 0 ] += pDelta->v[ 0 ];
		want.v[ 2 ] += pDelta->v[ 2 ];

		const s64 d2 = horizDist2( &want, &start );
		if ( d2 == 0 )
			pOut->nFraction = 255;
		else
		{
			Gpu64_LevelVec got = pOut->end;
			if ( nMode & GPU64_CLIP_MODE_EYE )
				got.v[ 1 ] -= nEyeOfs;
			const s64 g2 = horizDist2( &got, &start );
			if ( g2 >= d2 )
				pOut->nFraction = 255;
			else
			{
				// Both distances are squares, so the ratio of
				// the squares scaled by 255^2 has the answer
				// as its square root. Bit by bit: eight
				// iterations, no division in the loop, and the
				// same answer on every compiler.
				s64 r = g2 * ( 255 * 255 ) / d2;
				u32 x = 0;
				for ( int b = 7; b >= 0; b-- )
				{
					const u32 c = x | (u32)( 1 << b );
					if ( (s64)c * c <= r )
						x = c;
				}
				pOut->nFraction = (u8)x;
			}
		}
	}

	return TRUE;
}

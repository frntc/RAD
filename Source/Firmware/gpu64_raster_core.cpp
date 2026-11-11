// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 8 -- the class 2 raster core. See gpu64_raster_core.h.

 Portable: no Circle dependency past <circle/types.h>, so tools/rastercheck
 compiles this file unchanged and diffs it against the Python reference.
*/
#include "gpu64_raster_core.h"

// --- record decoding ----------------------------------------------------
//
// By hand, byte by byte, rather than by casting the record to a struct. The
// records arrive from a 6502 as little-endian bytes and land in a staging
// buffer with no alignment guarantee; a struct cast would be both an
// alignment bet and an endianness bet, and neither is worth making for what
// costs one shift per field.

static inline u16 recU16( const u8 *p, unsigned i )
{
	return (u16)( p[ i ] | ( p[ i + 1 ] << 8 ) );
}

static inline int recS16( const u8 *p, unsigned i )
{
	return (int)(s16)recU16( p, i );
}

// --- lighting -----------------------------------------------------------
//
// One table lookup per pixel, which is what Doom's own colormap costs. The
// level is clamped rather than rejected: a record asking for a light level
// past the end of the table is a lighting bug in the C64 code, not a reason
// to leave a hole in the wall.

static inline const u8 *lightRow( const Gpu64RasterState *pState, u8 nLight )
{
	if ( pState->pColormap == 0 || pState->levels == 0 )
		return 0;

	unsigned lvl = nLight;
	if ( lvl >= pState->levels )
		lvl = pState->levels - 1;

	return pState->pColormap + lvl * 256;
}

#define GPU64_LIGHT_SHIFT	40

// Milestone 12: the dynamic lights, evaluated at a world point.
//
// The colormap index runs DARK-WARDS -- 0 is the unattenuated palette and
// higher rows are dimmer -- so a light BRIGHTENS by subtracting. That is why
// this returns a level to hand to lightRow() rather than a colour: a dynamic
// light and the distance falloff it fights against are then the same
// quantity, measured in the same units, and one clamp at zero settles both.
//
// Cost is what makes this shippable at all: per light, three subtracts, three
// multiplies, a compare and -- only inside the radius -- one more multiply and
// a shift. No square root and no divide; the divide by r2 was done once, at
// SET_LIGHT time, into `fall`.
static inline u8 lightAdjust( const Gpu64RasterState *pState, u8 nLevel,
			      s32 wx, s32 wy, s32 wz )
{
	int sub = 0;

	for ( unsigned i = 0; i < GPU64_RASTER_MAX_LIGHTS; i++ )
	{
		const Gpu64RasterLight *pL = &pState->lights[ i ];
		if ( pL->fall == 0 )
			continue;

		// 8.8 differences square to 16.16, and three of them summed
		// can reach 3.2e9 at the far plane -- past what an s32 holds,
		// which is why this is all in 64 bits.
		const s64 dx = (s64)wx - pL->x;
		const s64 dy = (s64)wy - pL->y;
		const s64 dz = (s64)wz - pL->z;
		const s64 d2 = dx * dx + dy * dy + dz * dz;
		if ( d2 >= pL->r2 )
			continue;

		sub += (int)( ( pL->fall * ( pL->r2 - d2 ) ) >> GPU64_LIGHT_SHIFT );
	}

	if ( sub <= 0 )
		return nLevel;
	return sub >= (int)nLevel ? 0 : (u8)( (int)nLevel - sub );
}

// The whole dynamic-light path behind one test. Every program written before
// milestone 12 sets no lights, and for those this is FALSE and the inner
// loops keep their hoisted colormap row exactly as they had it.
static inline boolean lightsLive( const Gpu64RasterState *pState )
{
	return pState->lightMask != 0;
}

// --- view clipping ------------------------------------------------------

static inline int viewX0( const Gpu64RasterState *p ) { return (int)p->viewX; }
static inline int viewX1( const Gpu64RasterState *p ) { return (int)p->viewX + (int)p->viewW; }	// exclusive
static inline int viewY0( const Gpu64RasterState *p ) { return (int)p->viewY; }
static inline int viewY1( const Gpu64RasterState *p ) { return (int)p->viewY + (int)p->viewH; }	// exclusive

void gpu64_rasterStateDefaults( Gpu64RasterState *pState )
{
	pState->viewX = 0;
	pState->viewY = 0;
	pState->viewW = GPU64_RASTER_SURFACE_W;
	pState->viewH = GPU64_RASTER_SURFACE_H;
	pState->pColormap = 0;
	pState->levels = 0;

	// A camera looking east from the origin, eye half a unit up in a
	// two-unit room, projecting a 90-degree field of view across a
	// 320-pixel view. Defaults that draw something rather than nothing.
	pState->camX = 0;
	pState->camY = 0;
	pState->camAng = 0;
	pState->camFlags = 0;
	pState->camEyeH = 0x0080;		// 0.5
	pState->camCeilH = 0x0200;		// 2.0
	pState->camProj = 0xA000;		// 160.0 pixels
	pState->camFloorCol = 0;
	pState->camCeilCol = 0;
	pState->camHorizon = 0;
	pState->pSectors = 0;
	pState->sectors = 0;

	// The 3D camera has no drawing default: proj 0 means SET_CAMERA3D was
	// never sent, and DRAW_POLYS rejects a batch against it rather than
	// projecting through a camera the program did not choose.
	pState->cam3X = 0;
	pState->cam3Y = 0;
	pState->cam3Z = 0;
	pState->cam3Yaw = 0;
	pState->cam3Pitch = 0;
	pState->cam3Proj = 0;
	pState->cam3Flags = 0;
	pState->pVerts = 0;
	pState->verts = 0;
	pState->pTexinfo = 0;
	pState->texinfos = 0;

	// No dynamic lights. fall == 0 is the per-slot "off", and lightMask
	// is the fast test the inner loops actually use; the two are kept in
	// step by gpu64_rasterSetLight().
	for ( unsigned i = 0; i < GPU64_RASTER_MAX_LIGHTS; i++ )
	{
		pState->lights[ i ].x = 0;
		pState->lights[ i ].y = 0;
		pState->lights[ i ].z = 0;
		pState->lights[ i ].r2 = 0;
		pState->lights[ i ].fall = 0;
	}
	pState->lightMask = 0;
}

// SET_LIGHT's arithmetic, here rather than in the wire layer so that
// tools/rastercheck sets a light exactly the way the firmware does.
//
// nStrength is in colormap levels at the light's centre and nRadius is world
// 8.8. Either being zero turns the slot off, which is how a game ends a
// muzzle flash: the same twelve stores it started it with.
boolean gpu64_rasterSetLight( Gpu64RasterState *pState, unsigned nSlot,
			      s32 x, s32 y, s32 z, u16 nRadius, u8 nStrength )
{
	if ( nSlot >= GPU64_RASTER_MAX_LIGHTS )
		return FALSE;

	Gpu64RasterLight *pL = &pState->lights[ nSlot ];

	if ( nRadius == 0 || nStrength == 0 )
	{
		pL->r2 = 0;
		pL->fall = 0;
		pState->lightMask &= (u8)~( 1 << nSlot );
		return TRUE;
	}

	pL->x = x;
	pL->y = y;
	pL->z = z;
	pL->r2 = (s64)nRadius * (s64)nRadius;

	// ( strength << SHIFT ) / r2, so that fall * ( r2 - d2 ) >> SHIFT is
	// strength at the centre and zero at the radius. The shift is 40, not
	// the 16 the units suggest, because r2 reaches 2^32 at the largest
	// radius and a smaller shift would round `fall` to zero there -- a
	// wide, gentle light would silently stop existing. At 40 the product
	// is bounded above by strength << 40, well inside an s64, and the
	// quotient has at least eight fractional bits for every legal radius.
	pL->fall = ( (s64)nStrength << GPU64_LIGHT_SHIFT ) / pL->r2;

	pState->lightMask |= (u8)( 1 << nSlot );
	return TRUE;
}

// --- textures -----------------------------------------------------------

static inline boolean isPow2( unsigned v )
{
	return ( v != 0 ) && ( ( v & ( v - 1 ) ) == 0 );
}

boolean gpu64_rasterBuildTexture( Gpu64RasterTexture *pTex, u8 *pDst,
				  const u8 *pSrc, u32 nSrcLen,
				  u16 w, u16 h, boolean bSrcRowMajor )
{
	if ( w == 0 || h == 0 )
		return FALSE;
	if ( w > GPU64_RASTER_MAX_DIM || h > GPU64_RASTER_MAX_DIM )
		return FALSE;
	if ( !isPow2( h ) )
		return FALSE;
	if ( nSrcLen != (u32)w * h )
		return FALSE;

	if ( bSrcRowMajor )
	{
		// Transpose once, here, rather than paying a stride-w scatter in
		// every column the texture is ever drawn in.
		for ( unsigned u = 0; u < w; u++ )
			for ( unsigned v = 0; v < h; v++ )
				pDst[ u * h + v ] = pSrc[ v * w + u ];
	} else
	{
		for ( u32 i = 0; i < nSrcLen; i++ )
			pDst[ i ] = pSrc[ i ];
	}

	pTex->pTexels = pDst;
	pTex->w = w;
	pTex->h = h;
	pTex->hMask = (u16)( h - 1 );
	pTex->wIsPow2 = isPow2( w ) ? 1 : 0;
	pTex->pad = 0;
	return TRUE;
}

// --- FILL_VIEW ----------------------------------------------------------

// --- the per-pixel depth buffer -----------------------------------------
//
// Shared by DRAW_SECTORS, DRAW_THINGS and FILL_VIEW, which is why it sits
// up here rather than beside the code that fills it. The full rationale is
// with gpu64_rasterSectors below.

#define GPU64_Z_EMPTY	0xffff

static u16 s_ZBuf[ GPU64_RASTER_SURFACE_W * GPU64_RASTER_SURFACE_H ];

// s_ZBuf is static, so it powers on as all zeroes -- "nearer than anything",
// which rejects every pixel. DRAW_SECTORS never sees that because it clears
// the view before it draws, but DRAW_THINGS can: a thing batch sent before
// any sector batch, or after a DRAW_SECTORS that bailed out on a missing
// sector table without reaching its clear. One flag makes "the depth buffer
// starts empty" true rather than nearly true, and is what lets the reference
// model simply be born with a full buffer.
static boolean s_ZBufReady = FALSE;

// RASTER_RESET: the next use finds an empty buffer. Without this a program
// that sends things but no sectors would depth-test against whatever the
// PREVIOUS program left there, and two conformance runs in one session would
// not give the same answer.
void gpu64_rasterZReset( void )
{
	s_ZBufReady = FALSE;
}

static void zbufReady( void )
{
	if ( s_ZBufReady )
		return;
	for ( unsigned i = 0;
	      i < GPU64_RASTER_SURFACE_W * GPU64_RASTER_SURFACE_H; i++ )
		s_ZBuf[ i ] = GPU64_Z_EMPTY;
	s_ZBufReady = TRUE;
}

void gpu64_rasterFillView( const Gpu64RasterState *pState,
			   const Gpu64RasterTarget *pTarget, u8 nColour )
{
	const int x0 = viewX0( pState ), x1 = viewX1( pState );
	const int y0 = viewY0( pState ), y1 = viewY1( pState );

	// The depth of the pixels this erases goes with them. FILL_VIEW is
	// the op a frame starts with, and a pixel that has just been painted
	// background has nothing in it -- leaving last frame's depth behind
	// would let geometry that is no longer drawn go on occluding things
	// that are. DRAW_SECTORS clears the view itself and so never noticed;
	// DRAW_THINGS without a sector batch would have been silently blank.
	zbufReady();

	for ( int y = y0; y < y1; y++ )
	{
		u8 *p = pTarget->pPixels + (unsigned)y * pTarget->pitch + x0;
		u16 *pZ = s_ZBuf + (unsigned)y * GPU64_RASTER_SURFACE_W + x0;
		for ( int x = x0; x < x1; x++ )
		{
			*p++ = nColour;
			*pZ++ = GPU64_Z_EMPTY;
		}
	}
}

// --- DRAW_COLUMNS -------------------------------------------------------
//
// Record layout (16 bytes), docs/api_design.md:
//   0  u16 x      2  s16 y0    4  s16 y1    6  u8 texid   7  u8 light
//   8  u16 u     10  u16 v(8.8) 12 s16 dv(8.8) 14 u8 flags 15 reserved
//
// texid 0 means solid colour, the index being the low byte of u.

void gpu64_rasterColumns( const Gpu64RasterState *pState,
			  const Gpu64RasterTarget *pTarget,
			  const u8 *pRecs, u32 nCount, u8 nKey,
			  Gpu64RasterLookupFn pLookup, void *pCtx,
			  Gpu64RasterBatchResult *pResult )
{
	const int vx0 = viewX0( pState ), vx1 = viewX1( pState );
	const int vy0 = viewY0( pState ), vy1 = viewY1( pState );

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_REC_BYTES;

		const int x     = (int)recU16( r, 0 );
		int	  y0    = recS16( r, 2 );
		int	  y1    = recS16( r, 4 );
		const u8  texid = r[ 6 ];
		const u8  light = r[ 7 ];
		const u16 u     = recU16( r, 8 );
		s32	  v     = (s32)(s16)recU16( r, 10 );
		const s32 dv    = (s32)recS16( r, 12 );
		const u8  flags = r[ 14 ];

		// A record outside the view or inside out is a rejection, not a
		// failure -- see the design doc on why the batch survives it.
		if ( x < vx0 || x >= vx1 || y1 < y0 )
		{
			pResult->rejected++;
			continue;
		}

		const Gpu64RasterTexture *pTex = 0;
		if ( texid != 0 )
		{
			pTex = pLookup( pCtx, texid );
			if ( pTex == 0 )
			{
				pResult->rejected++;
				continue;
			}
		}

		pResult->accepted++;

		// Clip vertically, advancing v across the rows that were clipped
		// away so the texture stays pinned to the wall rather than sliding
		// up it as the top of the column leaves the screen. This is the
		// whole reason y0 is allowed to be negative.
		if ( y0 < vy0 )
		{
			v += dv * ( vy0 - y0 );
			y0 = vy0;
		}
		if ( y1 >= vy1 )
			y1 = vy1 - 1;
		if ( y0 > y1 )
			continue;			// accepted, wholly clipped

		const u8 *pMap = lightRow( pState, light );
		u8 *pDst = pTarget->pPixels + (unsigned)y0 * pTarget->pitch + x;
		const unsigned pitch = pTarget->pitch;
		const unsigned nRows = (unsigned)( y1 - y0 + 1 );

		if ( pTex == 0 )
		{
			// Solid: one colour, still lit, so a sky or an untextured
			// floor darkens with distance like everything else.
			u8 c = (u8)( u & 0xff );
			if ( pMap ) c = pMap[ c ];
			for ( unsigned n = 0; n < nRows; n++, pDst += pitch )
				*pDst = c;
			pResult->pixels += nRows;
			continue;
		}

		// u wraps once per record, so a modulo is affordable here in a way
		// it would not be per pixel -- which is exactly why w does not have
		// to be a power of two and h does.
		const unsigned uu = (unsigned)u % pTex->w;
		const u8 *pSrc = pTex->pTexels + uu * pTex->h;
		const u16 hMask = pTex->hMask;

		if ( flags & GPU64_RASTER_COL_MASKED )
		{
			for ( unsigned n = 0; n < nRows; n++, pDst += pitch, v += dv )
			{
				const u8 t = pSrc[ ( v >> 8 ) & hMask ];
				if ( t == nKey )
					continue;
				*pDst = pMap ? pMap[ t ] : t;
				pResult->pixels++;
			}
		} else
		{
			for ( unsigned n = 0; n < nRows; n++, pDst += pitch, v += dv )
			{
				const u8 t = pSrc[ ( v >> 8 ) & hMask ];
				*pDst = pMap ? pMap[ t ] : t;
			}
			pResult->pixels += nRows;
		}
	}
}

// --- DRAW_SPANS ---------------------------------------------------------
//
// Record layout (16 bytes):
//   0  s16 y      2  s16 x0    4  s16 x1   6  u8 texid   7  u8 light
//   8  u16 u(8.8) 10 u16 v(8.8) 12 s16 du  14 s16 dv
//
// texid 0 is solid colour, the index being the low byte of u -- the same
// convention the column record uses.

void gpu64_rasterSpans( const Gpu64RasterState *pState,
			const Gpu64RasterTarget *pTarget,
			const u8 *pRecs, u32 nCount,
			Gpu64RasterLookupFn pLookup, void *pCtx,
			Gpu64RasterBatchResult *pResult )
{
	const int vx0 = viewX0( pState ), vx1 = viewX1( pState );
	const int vy0 = viewY0( pState ), vy1 = viewY1( pState );

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_REC_BYTES;

		const int y     = recS16( r, 0 );
		int	  x0    = recS16( r, 2 );
		int	  x1    = recS16( r, 4 );
		const u8  texid = r[ 6 ];
		const u8  light = r[ 7 ];
		s32	  u     = (s32)(s16)recU16( r, 8 );
		s32	  v     = (s32)(s16)recU16( r, 10 );
		const s32 du    = (s32)recS16( r, 12 );
		const s32 dv    = (s32)recS16( r, 14 );

		if ( y < vy0 || y >= vy1 || x1 < x0 )
		{
			pResult->rejected++;
			continue;
		}

		const Gpu64RasterTexture *pTex = 0;
		if ( texid != 0 )
		{
			pTex = pLookup( pCtx, texid );
			// A span wraps u every pixel, so unlike a column it needs w to
			// be a mask too. A texture that cannot provide that is a
			// rejection with a reason, not a silently wrong floor.
			if ( pTex == 0 || !pTex->wIsPow2 )
			{
				pResult->rejected++;
				continue;
			}
		}

		pResult->accepted++;

		if ( x0 < vx0 )
		{
			const int n = vx0 - x0;
			u += du * n;
			v += dv * n;
			x0 = vx0;
		}
		if ( x1 >= vx1 )
			x1 = vx1 - 1;
		if ( x0 > x1 )
			continue;

		const u8 *pMap = lightRow( pState, light );
		u8 *pDst = pTarget->pPixels + (unsigned)y * pTarget->pitch + x0;
		const unsigned nCols = (unsigned)( x1 - x0 + 1 );

		if ( pTex == 0 )
		{
			u8 c = (u8)( u & 0xff );
			if ( pMap ) c = pMap[ c ];
			for ( unsigned n = 0; n < nCols; n++ )
				*pDst++ = c;
			pResult->pixels += nCols;
			continue;
		}

		const u16 wMask = (u16)( pTex->w - 1 );
		const u16 hMask = pTex->hMask;
		const u16 h = pTex->h;
		const u8 *pTexels = pTex->pTexels;

		for ( unsigned n = 0; n < nCols; n++, u += du, v += dv )
		{
			// Column-major storage: texel( u, v ) at [ u * h + v ].
			const unsigned su = (unsigned)( ( u >> 8 ) & wMask );
			const unsigned sv = (unsigned)( ( v >> 8 ) & hMask );
			const u8 t = pTexels[ su * h + sv ];
			*pDst++ = pMap ? pMap[ t ] : t;
		}
		pResult->pixels += nCols;
	}
}

// --- DRAW_SPRITE --------------------------------------------------------

void gpu64_rasterSprite( const Gpu64RasterState *pState,
			 const Gpu64RasterTarget *pTarget,
			 const Gpu64RasterTexture *pTex,
			 int x, int y, unsigned w, unsigned h,
			 u8 nLight, u8 nKey, int nClipY0, int nClipY1, u8 nFlags,
			 Gpu64RasterBatchResult *pResult )
{
	// A sprite is one primitive, and it is counted the same way a column
	// record is: a degenerate rectangle is rejected, a well-formed one that
	// happens to clip away entirely is accepted and draws nothing.
	if ( w == 0 || h == 0 )
	{
		pResult->rejected++;
		return;
	}
	pResult->accepted++;

	// Steps in 16.16, because a sprite magnified to the full view from a
	// 16-texel source has a step of 1/20 of a texel and 8.8 would quantise
	// it into visible banding. The records use 8.8 because the C64 computes
	// those; this one the firmware computes, so it can afford the precision.
	const u32 uStep = (u32)( ( (u64)pTex->w << 16 ) / w );
	const u32 vStep = (u32)( ( (u64)pTex->h << 16 ) / h );

	int x0 = x, x1 = x + (int)w - 1;
	int y0 = y, y1 = y + (int)h - 1;

	if ( nClipY0 > y0 ) y0 = nClipY0;
	if ( nClipY1 < y1 ) y1 = nClipY1;

	if ( x0 < viewX0( pState ) ) x0 = viewX0( pState );
	if ( x1 >= viewX1( pState ) ) x1 = viewX1( pState ) - 1;
	if ( y0 < viewY0( pState ) ) y0 = viewY0( pState );
	if ( y1 >= viewY1( pState ) ) y1 = viewY1( pState ) - 1;

	if ( x0 > x1 || y0 > y1 )
		return;

	const u8 *pMap = lightRow( pState, nLight );
	const boolean bMasked = ( nFlags & GPU64_RASTER_SPR_MASKED ) != 0;
	const boolean bFlip   = ( nFlags & GPU64_RASTER_SPR_FLIPX ) != 0;
	const u16 texW = pTex->w, texH = pTex->h;

	const u32 uStart = (u32)( x0 - x ) * uStep;
	u32 vRow = (u32)( y0 - y ) * vStep;

	for ( int sy = y0; sy <= y1; sy++, vRow += vStep )
	{
		unsigned sv = vRow >> 16;
		if ( sv >= texH ) sv = texH - 1;		// the last row, not a wrap

		u8 *pDst = pTarget->pPixels + (unsigned)sy * pTarget->pitch + x0;
		u32 uCol = uStart;

		for ( int sx = x0; sx <= x1; sx++, pDst++, uCol += uStep )
		{
			unsigned su = uCol >> 16;
			if ( su >= texW ) su = texW - 1;
			if ( bFlip ) su = texW - 1 - su;

			const u8 t = pTex->pTexels[ su * texH + sv ];
			if ( bMasked && t == nKey )
				continue;
			*pDst = pMap ? pMap[ t ] : t;
			pResult->pixels++;
		}
	}
}

// --- checksum -----------------------------------------------------------


// ======================================================================
// DRAW_WALLS
//
// The one place in class 2 where gpu64 does geometry rather than pixels,
// and the reason it exists: a raycaster on a 1 MHz 6502 spends nearly all
// of its time on the per-column divide, and sends five kilobytes of column
// records across the bus every frame to show for it. A wall segment is
// sixteen bytes and does not change when the player moves. Upload the level
// once, send ten bytes of camera a frame, and the divide happens here.
//
// Everything below is integer and deterministic, because tools/rastercheck
// diffs it against a Python model line for line. In particular: shifts are
// used where a floor is wanted and idiv() where truncation is, never / on a
// value that can be negative, because the two disagree and the disagreement
// would only ever show up as one wrong pixel at the bench.
// ======================================================================

// sin( 2*pi*i/256 ) in 8.8. cos is the same table a quarter turn along, so
// there is one table and not two.
static const s16 s_Sin[ 256 ] = {
	     0,      6,     13,     19,     25,     31,     38,     44,
	    50,     56,     62,     68,     74,     80,     86,     92,
	    98,    104,    109,    115,    121,    126,    132,    137,
	   142,    147,    152,    157,    162,    167,    172,    177,
	   181,    185,    190,    194,    198,    202,    206,    209,
	   213,    216,    220,    223,    226,    229,    231,    234,
	   237,    239,    241,    243,    245,    247,    248,    250,
	   251,    252,    253,    254,    255,    255,    256,    256,
	   256,    256,    256,    255,    255,    254,    253,    252,
	   251,    250,    248,    247,    245,    243,    241,    239,
	   237,    234,    231,    229,    226,    223,    220,    216,
	   213,    209,    206,    202,    198,    194,    190,    185,
	   181,    177,    172,    167,    162,    157,    152,    147,
	   142,    137,    132,    126,    121,    115,    109,    104,
	    98,     92,     86,     80,     74,     68,     62,     56,
	    50,     44,     38,     31,     25,     19,     13,      6,
	     0,     -6,    -13,    -19,    -25,    -31,    -38,    -44,
	   -50,    -56,    -62,    -68,    -74,    -80,    -86,    -92,
	   -98,   -104,   -109,   -115,   -121,   -126,   -132,   -137,
	  -142,   -147,   -152,   -157,   -162,   -167,   -172,   -177,
	  -181,   -185,   -190,   -194,   -198,   -202,   -206,   -209,
	  -213,   -216,   -220,   -223,   -226,   -229,   -231,   -234,
	  -237,   -239,   -241,   -243,   -245,   -247,   -248,   -250,
	  -251,   -252,   -253,   -254,   -255,   -255,   -256,   -256,
	  -256,   -256,   -256,   -255,   -255,   -254,   -253,   -252,
	  -251,   -250,   -248,   -247,   -245,   -243,   -241,   -239,
	  -237,   -234,   -231,   -229,   -226,   -223,   -220,   -216,
	  -213,   -209,   -206,   -202,   -198,   -194,   -190,   -185,
	  -181,   -177,   -172,   -167,   -162,   -157,   -152,   -147,
	  -142,   -137,   -132,   -126,   -121,   -115,   -109,   -104,
	   -98,    -92,    -86,    -80,    -74,    -68,    -62,    -56,
	   -50,    -44,    -38,    -31,    -25,    -19,    -13,     -6,
};

static inline int fsin( u8 a ) { return s_Sin[ a ]; }
static inline int fcos( u8 a ) { return s_Sin[ (u8)( a + 64 ) ]; }

// Truncating division, matching C's / and the model's idiv(). Written out
// because the whole projection depends on the two agreeing about negatives.
static inline s32 idiv( s64 n, s64 d )
{
	return (s32)( n / d );
}

// The per-column depth buffer, as 1/z. Cleared at the top of every batch:
// a batch is a frame's worth of walls, so depth from the previous frame is
// never wanted and a C64 that forgot to clear it could not be told from one
// that did.
static s32 s_Depth[ GPU64_RASTER_SURFACE_W ];

// One endpoint, transformed into view space and projected.
struct WallPoint
{
	s32 vx, vz;		// view space, 8.8: vz is depth, vx is right
	s32 u;			// texture u at this endpoint, 8.8
};

// dx,dy world -> view space. Forward is (cos a, sin a); right is a quarter
// turn clockwise from it, which is the winding the rest of the project uses.
static inline void toView( const Gpu64RasterState *pState, int x, int y,
			   s32 *pVx, s32 *pVz )
{
	const s32 dx = x - pState->camX;
	const s32 dy = y - pState->camY;
	const s32 c  = fcos( pState->camAng );
	const s32 s  = fsin( pState->camAng );

	// Products of two 8.8 values, brought back to 8.8 by an arithmetic
	// shift -- a floor, and the model floors too.
	*pVz = ( dx * c + dy * s ) >> 8;
	*pVx = ( dx * s - dy * c ) >> 8;
}

void gpu64_rasterWalls( const Gpu64RasterState *pState,
			const Gpu64RasterTarget *pTarget,
			const u8 *pRecs, u32 nCount, u8 nKey,
			Gpu64RasterLookupFn pLookup, void *pCtx,
			Gpu64RasterBatchResult *pResult )
{
	const int vx0 = viewX0( pState ), vx1 = viewX1( pState );
	const int vy0 = viewY0( pState ), vy1 = viewY1( pState );

	// A camera with no projection distance, an eye on the floor or a
	// ceiling below the eye cannot be projected. The wire layer rejects
	// those in SET_CAMERA; this is the second line, because the core is
	// also called directly by tools/rastercheck.
	if ( pState->camProj == 0 || pState->camEyeH <= 0
	     || pState->camCeilH <= pState->camEyeH )
	{
		pResult->rejected += nCount;
		return;
	}

	for ( int i = vx0; i < vx1 && i < GPU64_RASTER_SURFACE_W; i++ )
		s_Depth[ i ] = 0;

	const s32 proj    = pState->camProj;
	const int centreX = vx0 + (int)pState->viewW / 2;
	const int horizon = vy0 + (int)pState->viewH / 2 + pState->camHorizon;
	const s32 eyeH    = pState->camEyeH;
	const s32 topH    = pState->camCeilH - pState->camEyeH;
	const unsigned pitch = pTarget->pitch;

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_REC_BYTES;

		WallPoint a, b;
		toView( pState, recS16( r, 0 ), recS16( r, 2 ), &a.vx, &a.vz );
		toView( pState, recS16( r, 4 ), recS16( r, 6 ), &b.vx, &b.vz );
		a.u = recS16( r, 10 );
		b.u = recS16( r, 12 );

		const u8 texid = r[ 8 ];
		const u8 light = r[ 9 ];
		const u8 flags = r[ 14 ];

		// Behind the near plane at both ends: nothing to draw, and not
		// an error -- a level has walls behind the player in every frame.
		if ( a.vz < GPU64_RASTER_NEAR && b.vz < GPU64_RASTER_NEAR )
		{
			pResult->rejected++;
			continue;
		}

		const Gpu64RasterTexture *pTex = 0;
		if ( texid != 0 )
		{
			pTex = pLookup( pCtx, texid );
			if ( pTex == 0 )
			{
				pResult->rejected++;
				continue;
			}
		}

		// Clip the crossing end to the near plane, carrying u with it, so
		// the texture stays pinned to the wall as it slides past the eye.
		if ( a.vz < GPU64_RASTER_NEAR )
		{
			const s32 t = idiv( (s64)( GPU64_RASTER_NEAR - a.vz ) << 16,
					    b.vz - a.vz );		// 0..65536
			a.vx += (s32)( (s64)( b.vx - a.vx ) * t >> 16 );
			a.u  += (s32)( (s64)( b.u  - a.u  ) * t >> 16 );
			a.vz  = GPU64_RASTER_NEAR;
		}
		else if ( b.vz < GPU64_RASTER_NEAR )
		{
			const s32 t = idiv( (s64)( GPU64_RASTER_NEAR - b.vz ) << 16,
					    a.vz - b.vz );
			b.vx += (s32)( (s64)( a.vx - b.vx ) * t >> 16 );
			b.u  += (s32)( (s64)( a.u  - b.u  ) * t >> 16 );
			b.vz  = GPU64_RASTER_NEAR;
		}

		const int sxa = centreX + ( idiv( (s64)a.vx * proj, a.vz ) >> 8 );
		const int sxb = centreX + ( idiv( (s64)b.vx * proj, b.vz ) >> 8 );

		// One-sided, like Doom's. A wall whose endpoints project
		// right-to-left is being seen from behind, and the winding is
		// what says which side that is.
		if ( sxb <= sxa )
		{
			pResult->rejected++;
			continue;
		}

		pResult->accepted++;

		// 1/z and u/z, the two quantities that interpolate linearly in
		// screen space. Everything perspective-correct about this comes
		// from these two lines.
		const s32 iza = idiv( (s64)1 << 22, a.vz );
		const s32 izb = idiv( (s64)1 << 22, b.vz );
		const s32 uza = (s32)( ( (s64)a.u * iza ) >> 8 );
		const s32 uzb = (s32)( ( (s64)b.u * izb ) >> 8 );

		int x0 = sxa, x1 = sxb - 1;		// x1 inclusive
		if ( x0 < vx0 ) x0 = vx0;
		if ( x1 >= vx1 ) x1 = vx1 - 1;
		if ( x0 > x1 )
			continue;			// accepted, wholly clipped

		const s32 span = sxb - sxa;

		for ( int x = x0; x <= x1; x++ )
		{
			const s32 t  = x - sxa;
			const s32 iz = iza + idiv( (s64)( izb - iza ) * t, span );
			if ( iz <= 0 )
				continue;
			if ( iz <= s_Depth[ x ] )
				continue;		// something nearer is here
			s_Depth[ x ] = iz;

			const s32 z = idiv( (s64)1 << 22, iz );	// 8.8
			if ( z <= 0 )
				continue;

			// Where the wall meets the floor and the ceiling in this
			// column. Both are the same projection the endpoints got.
			const int yb = horizon + ( idiv( (s64)eyeH * proj, z ) >> 8 );
			const int yt = horizon - ( idiv( (s64)topH * proj, z ) >> 8 );
			if ( yb <= yt )
				continue;

			u8 lvl = light;
			if ( !( flags & GPU64_RASTER_WALL_FLATLIT ) )
			{
				const int d = light + (int)( z >> 9 );
				lvl = (u8)( d > 255 ? 255 : d );
			}
			const u8 *pMap = lightRow( pState, lvl );

			// The floor and the ceiling of this column, if the camera
			// asked for them. Painted before the wall so a wall that
			// is one pixel tall still wins its own rows.
			if ( pState->camFlags & GPU64_RASTER_CAM_PAINT )
			{
				int ct = vy0, cb = yt - 1;
				if ( cb >= vy1 ) cb = vy1 - 1;
				for ( int y = ct; y <= cb; y++ )
				{
					pTarget->pPixels[ (unsigned)y * pitch + x ] = pState->camCeilCol;
					pResult->pixels++;
				}
				int ft = yb, fb = vy1 - 1;
				if ( ft < vy0 ) ft = vy0;
				for ( int y = ft; y <= fb; y++ )
				{
					pTarget->pPixels[ (unsigned)y * pitch + x ] = pState->camFloorCol;
					pResult->pixels++;
				}
			}

			int ya = yt, yz = yb - 1;
			if ( ya < vy0 ) ya = vy0;
			if ( yz >= vy1 ) yz = vy1 - 1;
			if ( ya > yz )
				continue;

			u8 *pDst = pTarget->pPixels + (unsigned)ya * pitch + x;

			if ( pTex == 0 )
			{
				u8 c = (u8)( recU16( r, 10 ) & 0xff );
				if ( pMap ) c = pMap[ c ];
				for ( int y = ya; y <= yz; y++, pDst += pitch )
					*pDst = c;
				pResult->pixels += (u32)( yz - ya + 1 );
				continue;
			}

			// u for this column, back out of u/z, then wrapped once --
			// the same trade DRAW_COLUMNS makes and for the same reason.
			const s32 uz = uza + idiv( (s64)( uzb - uza ) * t, span );
			s32 uu = idiv( (s64)uz << 8, iz ) >> 8;		// world 8.8 -> texel
			uu %= (s32)pTex->w;
			if ( uu < 0 ) uu += (s32)pTex->w;
			const u8 *pSrc = pTex->pTexels + (unsigned)uu * pTex->h;

			// v walks the texture's full height over the wall's full
			// height on screen, so a wall stays textured the same way
			// however near it is.
			const s32 dv = idiv( (s64)pTex->h << 8, yb - yt );
			s32 v = ( ya - yt ) * dv;
			const u16 hMask = pTex->hMask;

			for ( int y = ya; y <= yz; y++, pDst += pitch, v += dv )
			{
				const u8 tx = pSrc[ ( v >> 8 ) & hMask ];
				if ( ( flags & GPU64_RASTER_WALL_MASKED ) && tx == nKey )
					continue;
				*pDst = pMap ? pMap[ tx ] : tx;
				pResult->pixels++;
			}
		}
	}
}

// --- DRAW_SECTORS -------------------------------------------------------
//
// Everything DRAW_WALLS does, plus the two things a Doom level needs that it
// cannot express: a floor and a ceiling per sector, and a two-sided wall
// with a hole in the middle of it.
//
// The hole is why the depth buffer had to change shape. DRAW_WALLS keeps one
// 1/z per column and lets the nearest wall own the column outright, which is
// exact as long as every wall is opaque from floor to ceiling. A portal is
// not: the near wall owns the band above the far ceiling and the band below
// the far floor, and the gap between them belongs to whatever is behind it.
// So depth here is per pixel. It costs 128 KB of static store and one u16
// compare per pixel written, and it buys the same property DRAW_WALLS bought
// for columns -- records may arrive in any order, and Doom's BSP tree, which
// exists to produce that order, is not needed.
//
// Depth is stored as z itself in 8.8, nearer being smaller, with 0xffff for
// "nothing here yet". 1/z would need 32 bits to keep its precision near the
// eye and would double the buffer for no gain: nothing here interpolates the
// stored value, it is only ever compared.


boolean gpu64_rasterBuildSectors( Gpu64RasterSector *pDst, const u8 *pRecs,
				  u32 nCount )
{
	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_SEC_BYTES;
		const int f = recS16( r, 0 ), c = recS16( r, 2 );

		// A ceiling at or below its own floor is not a sector, and
		// accepting one would put a divide-by-a-negative-height into
		// the flat loop for the whole rest of the session.
		if ( c <= f )
			return FALSE;

		pDst[ i ].floorH  = (s16)f;
		pDst[ i ].ceilH   = (s16)c;
		pDst[ i ].floorCol = r[ 4 ];
		pDst[ i ].ceilCol  = r[ 5 ];
		pDst[ i ].light    = r[ 6 ];
		pDst[ i ].flags    = r[ 7 ];
	}
	return TRUE;
}

// Distance lighting, the one rule both walls and flats use.
static inline u8 litLevel( u8 nBase, s32 z, boolean bFlat )
{
	if ( bFlat )
		return nBase;
	const int d = nBase + (int)( z >> 9 );
	return (u8)( d > 255 ? 255 : d );
}

// Clamp a z into what the depth buffer can hold. Only walls far enough away
// to be a single pixel tall ever reach this, and they compare equal out
// there, which is the right answer for two things that far apart.
static inline u16 zStore( s32 z )
{
	if ( z < 0 ) return 0;
	if ( z >= GPU64_Z_EMPTY ) return GPU64_Z_EMPTY - 1;
	return (u16)z;
}

// The state one column of one wall record needs, gathered so the three
// bands (mid, upper, lower) and the two flats are all one small function.
struct SectorColumn
{
	const Gpu64RasterState	*pState;
	const Gpu64RasterTarget	*pTarget;
	Gpu64RasterBatchResult	*pResult;
	int	x;
	int	vy0, vy1;
	s32	z;			// the wall's z in this column, 8.8
	s32	uu;			// texel u before wrapping; each band
					// wraps it by its own texture's width
	u8	nKey;
};

// One vertical band of wall, rows yt..yb-1 of a face that spans yTop..yBot on
// screen. v walks the texture's full height over yTop..yBot, so an upper band
// carries the whole texture squeezed into its own height -- the same
// convention DRAW_WALLS uses for a full-height wall, applied to each band.
static void bandColumn( const SectorColumn *pC, int yTop, int yBot,
			int yt, int yb, const Gpu64RasterTexture *pTex,
			u8 nColour, const u8 *pMap, u8 nFlags )
{
	if ( yt < pC->vy0 ) yt = pC->vy0;
	if ( yb > pC->vy1 ) yb = pC->vy1;
	if ( yt >= yb || yBot <= yTop )
		return;

	const unsigned pitch = pC->pTarget->pitch;
	const u16 z = zStore( pC->z );
	u8 *pDst = pC->pTarget->pPixels + (unsigned)yt * pitch + pC->x;
	u16 *pZ  = s_ZBuf + (unsigned)yt * GPU64_RASTER_SURFACE_W + pC->x;

	if ( pTex == 0 )
	{
		const u8 c = pMap ? pMap[ nColour ] : nColour;
		for ( int y = yt; y < yb; y++, pDst += pitch, pZ += GPU64_RASTER_SURFACE_W )
		{
			if ( z >= *pZ )
				continue;
			*pZ = z;
			*pDst = c;
			pC->pResult->pixels++;
		}
		return;
	}

	s32 uu = pC->uu % (s32)pTex->w;
	if ( uu < 0 ) uu += (s32)pTex->w;
	const u8 *pSrc = pTex->pTexels + (unsigned)uu * pTex->h;
	const s32 dv = idiv( (s64)pTex->h << 8, yBot - yTop );
	s32 v = ( yt - yTop ) * dv;
	const u16 hMask = pTex->hMask;

	for ( int y = yt; y < yb; y++, pDst += pitch, pZ += GPU64_RASTER_SURFACE_W, v += dv )
	{
		if ( z >= *pZ )
			continue;
		const u8 tx = pSrc[ ( v >> 8 ) & hMask ];
		if ( ( nFlags & GPU64_RASTER_WALL_MASKED ) && tx == pC->nKey )
			continue;
		*pZ = z;
		*pDst = pMap ? pMap[ tx ] : tx;
		pC->pResult->pixels++;
	}
}

// A run of floor or ceiling in one column. Unlike a wall band every row has
// its own z, because a flat is a horizontal plane and its distance is a
// function of the row alone -- which is exactly what makes it depth-test
// correctly against a wall standing on it.
//
// With one correction: the depth written is never nearer than the wall whose
// column painted it. A plane is infinite and a sector's floor is not; the
// part of the plane in front of the wall does not exist. Without the clamp a
// low ceiling two rooms away wins the rows above the wall that hides it,
// because the extended plane really is nearer there -- which was exactly the
// symptom, a corridor's ceiling painted over the room the player is standing
// in. The wall is the near boundary of what this column can see of that
// sector, so clamping to it is both cheap and right. It is the stand-in for
// Doom's visplane clipping, which needs a front-to-back order this does not
// have.
//
// Lighting still uses the row's true distance, so the clamp is invisible.
static void flatColumn( const SectorColumn *pC, int yt, int yb,
			int nHorizon, s32 nDrop, s32 nProj, u8 nColour,
			u8 nBase, boolean bFlatLit )
{
	if ( yt < pC->vy0 ) yt = pC->vy0;
	if ( yb > pC->vy1 ) yb = pC->vy1;
	if ( yt >= yb )
		return;

	const Gpu64RasterState *pState = pC->pState;
	const unsigned pitch = pC->pTarget->pitch;
	u8 *pDst = pC->pTarget->pPixels + (unsigned)yt * pitch + pC->x;
	u16 *pZ  = s_ZBuf + (unsigned)yt * GPU64_RASTER_SURFACE_W + pC->x;

	for ( int y = yt; y < yb; y++, pDst += pitch, pZ += GPU64_RASTER_SURFACE_W )
	{
		// dy has to have the same sign as the drop, or this row is on
		// the wrong side of the horizon for this plane and the plane is
		// simply not visible in it.
		const s32 dy = y - nHorizon;
		if ( dy == 0 || ( dy > 0 ) != ( nDrop > 0 ) )
			continue;

		const s32 z = idiv( (s64)nDrop * nProj, (s64)dy << 8 );
		if ( z <= 0 )
			continue;
		const u16 zs = zStore( z < pC->z ? pC->z : z );
		if ( zs >= *pZ )
			continue;

		const u8 *pMap = lightRow( pState, litLevel( nBase, z, bFlatLit ) );
		*pZ = zs;
		*pDst = pMap ? pMap[ nColour ] : nColour;
		pC->pResult->pixels++;
	}
}

void gpu64_rasterSectors( const Gpu64RasterState *pState,
			  const Gpu64RasterTarget *pTarget,
			  const u8 *pRecs, u32 nCount, u8 nKey,
			  Gpu64RasterLookupFn pLookup, void *pCtx,
			  Gpu64RasterBatchResult *pResult )
{
	const int vx0 = viewX0( pState ), vx1 = viewX1( pState );
	const int vy0 = viewY0( pState ), vy1 = viewY1( pState );

	zbufReady();

	// Same two guards DRAW_WALLS has, minus the ceiling one: with a sector
	// table there is no single ceiling to check, and the eye may legally
	// be below zero.
	if ( pState->camProj == 0 || pState->pSectors == 0 || pState->sectors == 0 )
	{
		pResult->rejected += nCount;
		return;
	}

	for ( int y = vy0; y < vy1; y++ )
		for ( int x = vx0; x < vx1; x++ )
			s_ZBuf[ y * GPU64_RASTER_SURFACE_W + x ] = GPU64_Z_EMPTY;

	const s32 proj    = pState->camProj;
	const int centreX = vx0 + (int)pState->viewW / 2;
	const int horizon = vy0 + (int)pState->viewH / 2 + pState->camHorizon;
	const s32 eyeH    = pState->camEyeH;

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_WALL2_BYTES;

		WallPoint a, b;
		toView( pState, recS16( r, 0 ), recS16( r, 2 ), &a.vx, &a.vz );
		toView( pState, recS16( r, 4 ), recS16( r, 6 ), &b.vx, &b.vz );
		a.u = recS16( r, 8 );
		b.u = recS16( r, 10 );

		const u8 frontId = r[ 12 ];
		const u8 backId  = r[ 13 ];
		const u8 light   = r[ 14 ];
		const u8 flags   = r[ 15 ];

		if ( a.vz < GPU64_RASTER_NEAR && b.vz < GPU64_RASTER_NEAR )
		{
			pResult->rejected++;
			continue;
		}
		if ( frontId >= pState->sectors )
		{
			pResult->rejected++;
			continue;
		}
		const boolean bTwoSided = ( backId != GPU64_RASTER_NO_SECTOR );
		if ( bTwoSided && backId >= pState->sectors )
		{
			pResult->rejected++;
			continue;
		}

		const Gpu64RasterSector *pF = pState->pSectors + frontId;
		const Gpu64RasterSector *pB = bTwoSided ? pState->pSectors + backId : 0;

		// Three textures, any of which may be 0 for "the flat colour in
		// the record instead". A missing id is a rejected record, the
		// same as DRAW_WALLS treats one.
		const Gpu64RasterTexture *pTexM = 0, *pTexU = 0, *pTexL = 0;
		boolean bBadTex = FALSE;
		if ( r[ 16 ] && ( pTexM = pLookup( pCtx, r[ 16 ] ) ) == 0 ) bBadTex = TRUE;
		if ( r[ 17 ] && ( pTexU = pLookup( pCtx, r[ 17 ] ) ) == 0 ) bBadTex = TRUE;
		if ( r[ 18 ] && ( pTexL = pLookup( pCtx, r[ 18 ] ) ) == 0 ) bBadTex = TRUE;
		if ( bBadTex )
		{
			pResult->rejected++;
			continue;
		}

		if ( a.vz < GPU64_RASTER_NEAR )
		{
			const s32 t = idiv( (s64)( GPU64_RASTER_NEAR - a.vz ) << 16,
					    b.vz - a.vz );
			a.vx += (s32)( (s64)( b.vx - a.vx ) * t >> 16 );
			a.u  += (s32)( (s64)( b.u  - a.u  ) * t >> 16 );
			a.vz  = GPU64_RASTER_NEAR;
		}
		else if ( b.vz < GPU64_RASTER_NEAR )
		{
			const s32 t = idiv( (s64)( GPU64_RASTER_NEAR - b.vz ) << 16,
					    a.vz - b.vz );
			b.vx += (s32)( (s64)( a.vx - b.vx ) * t >> 16 );
			b.u  += (s32)( (s64)( a.u  - b.u  ) * t >> 16 );
			b.vz  = GPU64_RASTER_NEAR;
		}

		const int sxa = centreX + ( idiv( (s64)a.vx * proj, a.vz ) >> 8 );
		const int sxb = centreX + ( idiv( (s64)b.vx * proj, b.vz ) >> 8 );

		// Winding is the front face, exactly as in DRAW_WALLS. A
		// two-sided wall is drawn from its front side only; the level
		// carries the other side as a second record with the endpoints
		// swapped and the sectors exchanged.
		if ( sxb <= sxa )
		{
			pResult->rejected++;
			continue;
		}

		pResult->accepted++;

		const s32 iza = idiv( (s64)1 << 22, a.vz );
		const s32 izb = idiv( (s64)1 << 22, b.vz );
		const s32 uza = (s32)( ( (s64)a.u * iza ) >> 8 );
		const s32 uzb = (s32)( ( (s64)b.u * izb ) >> 8 );

		int x0 = sxa, x1 = sxb - 1;
		if ( x0 < vx0 ) x0 = vx0;
		if ( x1 >= vx1 ) x1 = vx1 - 1;
		if ( x0 > x1 )
			continue;

		const s32 span = sxb - sxa;
		const boolean bFlatLit = ( flags & GPU64_RASTER_WALL_FLATLIT ) != 0;
		const boolean bFlats = ( pState->camFlags & GPU64_RASTER_CAM_PAINT )
				       && !( flags & GPU64_RASTER_WALL_NOFLATS );

		SectorColumn col;
		col.pState  = pState;
		col.pTarget = pTarget;
		col.pResult = pResult;
		col.vy0 = vy0;
		col.vy1 = vy1;
		col.nKey = nKey;

		for ( int x = x0; x <= x1; x++ )
		{
			const s32 t  = x - sxa;
			const s32 iz = iza + idiv( (s64)( izb - iza ) * t, span );
			if ( iz <= 0 )
				continue;
			const s32 z = idiv( (s64)1 << 22, iz );
			if ( z <= 0 )
				continue;

			col.x = x;
			col.z = z;
			col.uu = 0;

			// Where the front sector's two planes cut this column.
			// Both are one projection of a height difference from
			// the eye, so a plane above the eye lands above the
			// horizon with no special case.
			const int yF = horizon + ( idiv( (s64)( eyeH - pF->floorH ) * proj, z ) >> 8 );
			const int yC = horizon + ( idiv( (s64)( eyeH - pF->ceilH  ) * proj, z ) >> 8 );

			if ( bFlats )
			{
				// A sky ceiling is not a surface. Painting no
				// pixel and writing no depth is what leaves
				// whatever the frame started with showing
				// through, and what lets a distant wall still
				// draw into those rows afterwards.
				if ( !( pF->flags & GPU64_RASTER_SEC_SKY ) )
					flatColumn( &col, vy0, yC, horizon,
						    eyeH - pF->ceilH, proj,
						    pF->ceilCol, pF->light, bFlatLit );
				flatColumn( &col, yF, vy1, horizon,
					    eyeH - pF->floorH, proj, pF->floorCol,
					    pF->light, bFlatLit );
			}

			const u8 lvl = litLevel( light, z, bFlatLit );
			const u8 *pMap = lightRow( pState, lvl );

			// u is wanted only if some band is textured. Backing it
			// out of u/z is the one divide per column that
			// perspective costs, so it is not paid for a wall drawn
			// in flat colour.
			if ( pTexM || pTexU || pTexL )
			{
				const s32 uz = uza + idiv( (s64)( uzb - uza ) * t, span );
				col.uu = idiv( (s64)uz << 8, iz ) >> 8;
			}

			if ( !bTwoSided )
			{
				bandColumn( &col, yC, yF, yC, yF, pTexM,
					    r[ 19 ], pMap, flags );
				continue;
			}

			const int yBF = horizon + ( idiv( (s64)( eyeH - pB->floorH ) * proj, z ) >> 8 );
			const int yBC = horizon + ( idiv( (s64)( eyeH - pB->ceilH  ) * proj, z ) >> 8 );

			// Lower band: the step up to the far floor, seen from
			// this side. Nothing to draw when the far floor is at or
			// below this one.
			if ( pB->floorH > pF->floorH )
				bandColumn( &col, yBF, yF, yBF, yF, pTexL,
					    r[ 21 ], pMap, flags );

			// Upper band: the drop from this ceiling to the far one.
			// A sky front sector has no upper band -- that is what
			// makes a courtyard wall stop at the sky instead of
			// growing a lintel across it.
			if ( pB->ceilH < pF->ceilH && !( pF->flags & GPU64_RASTER_SEC_SKY ) )
				bandColumn( &col, yC, yBC, yC, yBC, pTexU,
					    r[ 20 ], pMap, flags );

			// The window between them. No wall is drawn in it --
			// but the FAR sector's floor and ceiling are visible
			// through it, and nothing else is going to paint them.
			//
			// Flats are painted by the columns of the walls that
			// stand on them, and a sector's own side walls seen
			// end-on down a corridor cover almost no columns at
			// all, so without this the middle of a window is a
			// hole. Every row still carries its own distance, so a
			// far wall standing on that floor still wins the rows
			// it should. This is one level of nesting only: a
			// sector two portals away is Doom's visplane problem
			// and is not solved here.
			if ( bFlats )
			{
				int wTop = ( yC > yBC ) ? yC : yBC;
				int wBot = ( yF < yBF ) ? yF : yBF;
				if ( !( pB->flags & GPU64_RASTER_SEC_SKY ) )
					flatColumn( &col, wTop, wBot, horizon,
						    eyeH - pB->ceilH, proj,
						    pB->ceilCol, pB->light, bFlatLit );
				flatColumn( &col, wTop, wBot, horizon,
					    eyeH - pB->floorH, proj,
					    pB->floorCol, pB->light, bFlatLit );
			}
		}
	}
}

// ======================================================================
// DRAW_THINGS
//
// Billboard sprites in WORLD space, projected by the same camera as
// DRAW_SECTORS and depth-tested per pixel against the buffer DRAW_SECTORS
// filled. That is what lets a monster stand behind a wall, and it is the
// one thing DRAW_SPRITE cannot do: DRAW_SPRITE takes a screen rectangle, so
// the 6502 would have to do the projection itself, and there is no argument
// room left in it for a depth.
//
// A thing's depth is one value for the whole sprite, as it is in Doom: the
// billboard is a flat card facing the camera and its distance does not vary
// across it. Depth is WRITTEN as well as tested, at drawn pixels only, so
// two things that overlap come out the same way round whichever order they
// arrive in -- the property the rest of class 2 has, and the reason none of
// this needs a sort on the C64.
//
// Milestone 10: GPU64_RASTER_BATCH_CAM3D projects the batch through
// SET_CAMERA3D instead, so a Quake game gets monsters. The card stays upright
// and screen-axis-aligned -- Quake's own vp-parallel-upright sprite -- but its
// two ends are transformed separately, so a thing seen under pitch sits at the
// right rows and grows or shrinks the way the wall behind it does. With a
// pitch of zero the two ends land at the same depth and the arithmetic
// collapses, term for term, onto the 2D path above.
//
// Milestone 11: GPU64_RASTER_THING_DIRECTIONAL gives a thing eight views.
// Byte 13 is the direction the thing FACES; the card is still screen-parallel,
// but which of eight textures goes on it is chosen from where the camera
// stands relative to that facing. A game therefore stops sending a texture id
// that means "the monster seen from behind" and starts sending the monster's
// own heading, which is a number it already has -- and the eight-way choice,
// which in Doom is an angle subtraction the game engine does per thing per
// frame, moves to the side of the bus that has the camera on it.
// ======================================================================

// Which of eight 45-degree sectors a vector points into, 0 for the sector
// centred on +x and counting the way angles do. No arctangent: the vector is
// rotated by the thing's facing (less half a sector, so sector 0 is centred
// on the facing rather than starting at it) and then only the two signs and
// one magnitude comparison remain, which is exact everywhere including on the
// boundaries.
static inline int octant8( s32 x, s32 y )
{
	if ( y >= 0 )
	{
		if ( x >= 0 )
			return ( y <= x ) ? 0 : 1;
		return ( y >= -x ) ? 2 : 3;
	}
	if ( x < 0 )
		return ( -y < -x ) ? 4 : 5;
	return ( -y > x ) ? 6 : 7;
}

void gpu64_rasterThings( const Gpu64RasterState *pState,
			 const Gpu64RasterTarget *pTarget,
			 const u8 *pRecs, u32 nCount, u8 nKey, u8 nBatchFlags,
			 Gpu64RasterLookupFn pLookup, void *pCtx,
			 Gpu64RasterBatchResult *pResult )
{
	const int vx0 = viewX0( pState ), vx1 = viewX1( pState );
	const int vy0 = viewY0( pState ), vy1 = viewY1( pState );

	zbufReady();

	const boolean bCam3 = ( nBatchFlags & GPU64_RASTER_BATCH_CAM3D ) != 0;

	if ( bCam3 ? pState->cam3Proj == 0 : pState->camProj == 0 )
	{
		pResult->rejected += nCount;
		return;
	}

	// The two cameras share every line below this one. What differs is the
	// projection distance, where the horizon row is (SET_CAMERA shears it,
	// SET_CAMERA3D rotates instead) and how a world point reaches view
	// space.
	const s32 proj    = bCam3 ? (s32)pState->cam3Proj : (s32)pState->camProj;
	const int centreX = vx0 + (int)pState->viewW / 2;
	const int horizon = vy0 + (int)pState->viewH / 2
			  + ( bCam3 ? 0 : (int)pState->camHorizon );
	const s32 eyeH    = pState->camEyeH;
	const s32 cyaw = fcos( pState->cam3Yaw ), syaw = fsin( pState->cam3Yaw );
	const s32 cpit = fcos( (u8)pState->cam3Pitch ), spit = fsin( (u8)pState->cam3Pitch );
	const unsigned pitch = pTarget->pitch;

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_REC_BYTES;

		const s32 baseH  = recS16( r, 4 );
		const s32 worldH = (s32)recU16( r, 6 );
		const s32 worldW = (s32)recU16( r, 8 );
		const u8  texid  = r[ 10 ];
		const u8  light  = r[ 11 ];
		const u8  flags  = r[ 12 ];

		// Eight views: the id above is the first of eight consecutive
		// ones and byte 13 is where the thing is looking. The vector
		// used is thing -> CAMERA, so view 0 is the thing facing the
		// player and view 4 is its back walking away.
		u8 useTex = texid;
		if ( flags & GPU64_RASTER_THING_DIRECTIONAL )
		{
			const s32 cx = bCam3 ? (s32)pState->cam3X : (s32)pState->camX;
			const s32 cy = bCam3 ? (s32)pState->cam3Y : (s32)pState->camY;
			const s32 dx = cx - recS16( r, 0 );
			const s32 dy = cy - recS16( r, 2 );
			const u8  g  = (u8)( r[ 13 ] - 16 );
			const s32 c  = fcos( g ), sn = fsin( g );
			const s32 fx = ( dx * c + dy * sn ) >> 8;
			const s32 fy = ( dy * c - dx * sn ) >> 8;
			useTex = (u8)( texid + octant8( fx, fy ) );
		}

		// The card's two ends: hBase and hTop are heights measured
		// DOWNWARD from the eye, so a row is horizon + h*proj/depth in
		// both paths and the 2D expressions survive unchanged.
		s32 vx, vz, vzTop, hBase, hTop;

		if ( bCam3 )
		{
			const s32 dx = recS16( r, 0 ) - (s32)pState->cam3X;
			const s32 dy = recS16( r, 2 ) - (s32)pState->cam3Y;
			const s32 dzB = baseH - (s32)pState->cam3Z;
			const s32 dzT = dzB + worldH;

			const s32 fwd = ( dx * cyaw + dy * syaw ) >> 8;
			vx = ( dx * syaw - dy * cyaw ) >> 8;

			vz    = ( fwd * cpit + dzB * spit ) >> 8;
			vzTop = ( fwd * cpit + dzT * spit ) >> 8;
			hBase = ( fwd * spit - dzB * cpit ) >> 8;
			hTop  = ( fwd * spit - dzT * cpit ) >> 8;
		} else
		{
			toView( pState, recS16( r, 0 ), recS16( r, 2 ), &vx, &vz );
			vzTop = vz;
			hBase = eyeH - baseH;
			hTop  = hBase - worldH;
		}

		// Behind the near plane: not an error. Most of a level's things
		// are behind the player in any given frame, exactly as most of
		// its walls are, and DRAW_SECTORS counts those the same way.
		if ( vz < GPU64_RASTER_NEAR )
		{
			pResult->rejected++;
			continue;
		}

		// The top of a tall thing standing very close can cross the
		// near plane while its base is still in front of it. Clamping
		// rather than rejecting keeps the thing on screen -- a monster
		// must not vanish because the player walked into it -- and the
		// card simply runs off the top of the view.
		if ( vzTop < GPU64_RASTER_NEAR )
			vzTop = GPU64_RASTER_NEAR;
		if ( worldW == 0 || worldH == 0 )
		{
			pResult->rejected++;
			continue;
		}

		// A thing is always textured. There is no flat-colour form: a
		// solid rectangle floating in a room is not a thing anybody
		// wants, and requiring the id makes a dropped byte visible.
		const Gpu64RasterTexture *pTex =
			( texid && useTex ) ? pLookup( pCtx, useTex ) : 0;
		if ( pTex == 0 )
		{
			pResult->rejected++;
			continue;
		}

		pResult->accepted++;

		// The screen rectangle. Width is measured straight across the
		// view, so the card faces the camera with no rotation to do.
		const int wpx  = (int)( idiv( (s64)worldW * proj, vz ) >> 8 );
		const int sxc  = centreX + (int)( idiv( (s64)vx * proj, vz ) >> 8 );
		const int yBot = horizon + (int)( idiv( (s64)hBase * proj, vz ) >> 8 );
		const int yTop = horizon + (int)( idiv( (s64)hTop * proj, vzTop ) >> 8 );
		const int hpx  = yBot - yTop;

		// Well-formed but too far away to have a pixel: accepted and
		// drawn as nothing, the same way a clipped-away column record
		// is. Only a malformed record is rejected.
		if ( wpx <= 0 || hpx <= 0 )
			continue;

		const int xLeft = sxc - wpx / 2;

		int x0 = xLeft, x1 = xLeft + wpx - 1;
		int y0 = yTop,  y1 = yTop + hpx - 1;
		if ( x0 < vx0 ) x0 = vx0;
		if ( x1 >= vx1 ) x1 = vx1 - 1;
		if ( y0 < vy0 ) y0 = vy0;
		if ( y1 >= vy1 ) y1 = vy1 - 1;
		if ( x0 > x1 || y0 > y1 )
			continue;

		// 16.16 steps, for the reason DRAW_SPRITE uses them: a thing
		// magnified to fill the view from a 32-texel source steps by a
		// thirtieth of a texel, and 8.8 would band it.
		const u32 uStep = (u32)( ( (u64)pTex->w << 16 ) / (unsigned)wpx );
		const u32 vStep = (u32)( ( (u64)pTex->h << 16 ) / (unsigned)hpx );

		const boolean bMasked  = ( flags & GPU64_RASTER_THING_MASKED ) != 0;
		const boolean bFlip    = ( flags & GPU64_RASTER_THING_FLIPX ) != 0;
		const boolean bNoDepth = ( flags & GPU64_RASTER_THING_NODEPTH ) != 0;
		const boolean bFlatLit = ( flags & GPU64_RASTER_THING_FLATLIT ) != 0;

		// Dynamic lights are evaluated ONCE per thing, at its centre,
		// not per pixel. A sprite is a billboard a few units across:
		// the light cannot vary meaningfully over it, and paying a
		// per-pixel evaluation for a constant would be the one place
		// in this file where the cost bought nothing.
		u8 lvl = litLevel( light, vz, bFlatLit );
		if ( lightsLive( pState ) )
			lvl = lightAdjust( pState, lvl,
					   recS16( r, 0 ), recS16( r, 2 ),
					   baseH + ( worldH >> 1 ) );

		const u8 *pMap = lightRow( pState, lvl );
		const u16 z = zStore( vz );
		const unsigned texW = pTex->w, texH = pTex->h;

		const u32 uStart = (u32)( x0 - xLeft ) * uStep;
		u32 vRow = (u32)( y0 - yTop ) * vStep;

		for ( int sy = y0; sy <= y1; sy++, vRow += vStep )
		{
			unsigned sv = vRow >> 16;
			if ( sv >= texH ) sv = texH - 1;	// the last row, not a wrap

			u8 *pDst = pTarget->pPixels + (unsigned)sy * pitch + x0;
			u16 *pZ  = s_ZBuf + (unsigned)sy * GPU64_RASTER_SURFACE_W + x0;
			u32 uCol = uStart;

			for ( int sx = x0; sx <= x1; sx++, pDst++, pZ++, uCol += uStep )
			{
				if ( !bNoDepth && z >= *pZ )
					continue;
				unsigned su = uCol >> 16;
				if ( su >= texW ) su = texW - 1;
				if ( bFlip ) su = texW - 1 - su;

				const u8 t = pTex->pTexels[ su * texH + sv ];
				if ( bMasked && t == nKey )
					continue;
				if ( !bNoDepth )
					*pZ = z;
				*pDst = pMap ? pMap[ t ] : t;
				pResult->pixels++;
			}
		}
	}
}

u16 gpu64_rasterChecksum( const u8 *p, u32 nLen )
{
	u16 sum = 0;
	for ( u32 i = 0; i < nLen; i++ )
		sum = (u16)( sum + p[ i ] );
	return sum;
}

// --- DRAW_POLYS ---------------------------------------------------------
//
// A convex polygon in world space, arbitrarily oriented: the primitive both
// a wall record and a sector's flats are special cases of. Rationale is in
// dev/milestone9_poly_design.md; what matters here is the arithmetic.
//
// The pipeline per face is: gather from the vertex pool, transform to view
// space, clip against five planes, project, backface-cull, scan-convert.
// Per pixel it is one depth compare and three divides -- 1/z, s/z and t/z --
// which is Quake's own arithmetic without Quake's sixteen-pixel subdivision
// of it. An exact answer per pixel is what makes tools/rastercheck able to
// diff this against a model written from the document.
//
// The five-plane clip is not only about not drawing off-screen pixels: it is
// what BOUNDS the projected coordinates. A vertex a hair past the near plane
// projects to millions, and the products in the edge and span interpolation
// would then overflow even 64 bits. Clipped to the view, every screen
// coordinate is inside +/-2^17 and every product below has room.

// s64-returning divide, for the interpolants that do not fit in 32 bits.
// Same truncating semantics as idiv() above, and the model's idiv().
static inline s64 idiv64( s64 n, s64 d )
{
	return n / d;
}

// Ceiling of a 8.8 value in whole pixels. The sampling rule is: a pixel
// belongs to the polygon whose interior contains its top-left corner, and
// the spans are half-open -- so two faces sharing an edge neither draw it
// twice nor leave a seam between them.
static inline int ceilPix( s32 v )
{
	return (int)( ( v + 255 ) >> 8 );
}

// A vertex in view space, with the texture coordinates that are affine in
// world space and so interpolate linearly here too.
struct PolyVert
{
	s32 vx, vy, vz;		// right, up, forward -- 8.8
	s32 s, t;		// texels, 8.8
	s32 wx, wy, wz;		// WORLD position, 8.8 -- milestone 12
};

// The same vertex projected, with the three quantities that are linear in
// SCREEN space: 1/z, s/z and t/z. Everything the scan converter walks.
struct PolyScreen
{
	s32 x, y;		// screen position, 8.8
	s64 w;			// ( 1 << 30 ) / vz
	s64 s, t;		// s * w, t * w
	s64 wx, wy, wz;		// world * w -- milestone 12
};

// One Sutherland-Hodgman pass. The plane is given as a distance function
// evaluated at each vertex by the caller's kind selector: inside is d >= 0.
// Returns the new vertex count; a pass adds at most one vertex.
static s32 polyPlaneDist( const PolyVert *v, int nPlane, s32 proj,
			  s32 kL, s32 kR, s32 kT, s32 kB )
{
	switch ( nPlane )
	{
	case 0: return v->vz - GPU64_RASTER_NEAR;
	case 1: return (s32)( ( (s64)v->vx * proj - (s64)kL * v->vz ) >> 8 );
	case 2: return (s32)( ( (s64)kR * v->vz - (s64)v->vx * proj ) >> 8 );
	case 3: return (s32)( ( (s64)kT * v->vz - (s64)v->vy * proj ) >> 8 );
	default:return (s32)( ( (s64)v->vy * proj - (s64)kB * v->vz ) >> 8 );
	}
}

static u32 polyClipPlane( const PolyVert *pIn, u32 n, PolyVert *pOut,
			  int nPlane, s32 proj, s32 kL, s32 kR, s32 kT, s32 kB )
{
	u32 m = 0;
	for ( u32 i = 0; i < n; i++ )
	{
		const PolyVert *a = &pIn[ i ];
		const PolyVert *b = &pIn[ ( i + 1 ) % n ];
		const s32 da = polyPlaneDist( a, nPlane, proj, kL, kR, kT, kB );
		const s32 db = polyPlaneDist( b, nPlane, proj, kL, kR, kT, kB );

		if ( da >= 0 )
			pOut[ m++ ] = *a;

		if ( ( da >= 0 ) != ( db >= 0 ) )
		{
			const s64 num = -da, den = (s64)db - da;
			PolyVert *o = &pOut[ m++ ];
			o->vx = a->vx + (s32)idiv64( (s64)( b->vx - a->vx ) * num, den );
			o->vy = a->vy + (s32)idiv64( (s64)( b->vy - a->vy ) * num, den );
			o->vz = a->vz + (s32)idiv64( (s64)( b->vz - a->vz ) * num, den );
			o->s  = a->s  + (s32)idiv64( (s64)( b->s  - a->s  ) * num, den );
			o->t  = a->t  + (s32)idiv64( (s64)( b->t  - a->t  ) * num, den );
			o->wx = a->wx + (s32)idiv64( (s64)( b->wx - a->wx ) * num, den );
			o->wy = a->wy + (s32)idiv64( (s64)( b->wy - a->wy ) * num, den );
			o->wz = a->wz + (s32)idiv64( (s64)( b->wz - a->wz ) * num, den );
		}
	}
	return m;
}

// One row's worth of interpolated values, at the point an edge crosses it.
struct PolyEdgeHit
{
	s32 x;
	s64 w, s, t;
	s64 wx, wy, wz;		// world * w -- milestone 12
};

void gpu64_rasterPolys( const Gpu64RasterState *pState,
			const Gpu64RasterTarget *pTarget,
			const u8 *pRecs, u32 nCount, u8 nKey,
			Gpu64RasterLookupFn pLookup, void *pCtx,
			Gpu64RasterBatchResult *pResult )
{
	const int vx0 = viewX0( pState ), vx1 = viewX1( pState );
	const int vy0 = viewY0( pState ), vy1 = viewY1( pState );

	zbufReady();

	// No camera, or no vertex pool, and there is nothing a record could
	// mean. The wire layer says the same thing; this is the second line,
	// because the core is also called directly by tools/rastercheck.
	if ( pState->cam3Proj == 0 || pState->pVerts == 0 || pState->verts == 0 )
	{
		pResult->rejected += nCount;
		return;
	}

	const s32 proj = pState->cam3Proj;
	const int centreX = vx0 + (int)pState->viewW / 2;
	const int centreY = vy0 + (int)pState->viewH / 2;

	// The four side planes, as the constants polyPlaneDist() needs. They
	// are the view rectangle expressed in the projection's own units.
	const s32 kL = (s32)( vx0 - centreX ) * 256;
	const s32 kR = (s32)( vx1 - centreX ) * 256;
	const s32 kT = (s32)( centreY - vy0 ) * 256;
	const s32 kB = (s32)( centreY - vy1 ) * 256;

	const s32 cyaw = fcos( pState->cam3Yaw ), syaw = fsin( pState->cam3Yaw );
	const s32 cpit = fcos( (u8)pState->cam3Pitch ), spit = fsin( (u8)pState->cam3Pitch );
	const unsigned pitch = pTarget->pitch;

	PolyVert   bufA[ GPU64_RASTER_POLY_CLIP_VERTS + 8 ];
	PolyVert   bufB[ GPU64_RASTER_POLY_CLIP_VERTS + 8 ];
	PolyScreen scr [ GPU64_RASTER_POLY_CLIP_VERTS + 8 ];

	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_POLY_BYTES;

		const u16 first  = recU16( r, 0 );
		const u8  nVerts = r[ 2 ];
		const u8  tinfo  = r[ 3 ];
		const u8  texid  = r[ 4 ];
		const u8  colour = r[ 5 ];
		const u8  light  = r[ 6 ];
		const u8  flags  = r[ 7 ];

		if ( nVerts < 3 || nVerts > GPU64_RASTER_MAX_POLY_VERTS )
		{
			pResult->rejected++;
			continue;
		}
		if ( (u32)first + nVerts > pState->verts )
		{
			pResult->rejected++;
			continue;
		}

		// A textured face needs both a live texture and the texinfo
		// that says where its texels land. Either missing is the same
		// kind of mistake an unknown texture id is anywhere else in
		// class 2: the record is rejected, the batch carries on.
		const Gpu64RasterTexture *pTex = 0;
		const Gpu64RasterTexinfo *pTI = 0;
		if ( texid != 0 )
		{
			pTex = pLookup( pCtx, texid );
			if ( pTex == 0 || !pTex->wIsPow2 )
			{
				// Both dimensions are masked per pixel here, so
				// a width that is not a power of two is a
				// rejected record and not an upload error -- the
				// same texture is legal for DRAW_COLUMNS.
				pResult->rejected++;
				continue;
			}
			if ( tinfo == 0 || tinfo > pState->texinfos
			     || pState->pTexinfo == 0 )
			{
				pResult->rejected++;
				continue;
			}
			pTI = &pState->pTexinfo[ tinfo - 1 ];
		}

		// --- gather and transform ------------------------------
		for ( u32 k = 0; k < nVerts; k++ )
		{
			const Gpu64RasterVertex *pv = &pState->pVerts[ first + k ];
			const s32 dx = (s32)pv->x - pState->cam3X;
			const s32 dy = (s32)pv->y - pState->cam3Y;
			const s32 dz = (s32)pv->z - pState->cam3Z;

			// Yaw first: forward is ( cos, sin ), right is a
			// quarter turn clockwise from it -- toView()'s
			// convention, so the two layers agree about north.
			const s32 fwd = ( dx * cyaw + dy * syaw ) >> 8;
			const s32 rgt = ( dx * syaw - dy * cyaw ) >> 8;

			// Then pitch, in the forward/up plane. Positive pitch
			// looks up, which moves the world down the screen.
			bufA[ k ].vx = rgt;
			bufA[ k ].vz = ( fwd * cpit + dz * spit ) >> 8;
			bufA[ k ].vy = ( dz * cpit - fwd * spit ) >> 8;

			// World position, carried through the clipper and the
			// scan converter untransformed. Dynamic lights live in
			// world space, so this is what they are evaluated at;
			// carrying it costs nothing when no light is set,
			// because bLit gates every use of it below.
			bufA[ k ].wx = pv->x;
			bufA[ k ].wy = pv->y;
			bufA[ k ].wz = pv->z;

			if ( pTI )
			{
				const s64 sd = (s64)pv->x * pTI->sx
					     + (s64)pv->y * pTI->sy
					     + (s64)pv->z * pTI->sz;
				const s64 td = (s64)pv->x * pTI->tx
					     + (s64)pv->y * pTI->ty
					     + (s64)pv->z * pTI->tz;
				bufA[ k ].s = (s32)( sd >> 8 ) + pTI->sOff;
				bufA[ k ].t = (s32)( td >> 8 ) + pTI->tOff;
			} else
			{
				bufA[ k ].s = 0;
				bufA[ k ].t = 0;
			}
		}

		// --- clip ----------------------------------------------
		u32 n = polyClipPlane( bufA, nVerts, bufB, 0, proj, kL, kR, kT, kB );
		if ( n < 3 )
		{
			// Entirely behind the near plane. Rejected, the same
			// verdict a wall with both endpoints behind it earns.
			pResult->rejected++;
			continue;
		}
		n = polyClipPlane( bufB, n, bufA, 1, proj, kL, kR, kT, kB );
		if ( n >= 3 ) n = polyClipPlane( bufA, n, bufB, 2, proj, kL, kR, kT, kB );
		if ( n >= 3 ) n = polyClipPlane( bufB, n, bufA, 3, proj, kL, kR, kT, kB );
		if ( n >= 3 ) n = polyClipPlane( bufA, n, bufB, 4, proj, kL, kR, kT, kB );
		if ( n < 3 )
		{
			// Off the side of the view. Well formed, drew nothing:
			// accepted, which is the distinction the counters exist
			// to make.
			pResult->accepted++;
			continue;
		}

		// --- project -------------------------------------------
		for ( u32 k = 0; k < n; k++ )
		{
			const PolyVert *v = &bufB[ k ];
			const s32 vz = v->vz < GPU64_RASTER_NEAR
					? GPU64_RASTER_NEAR : v->vz;

			scr[ k ].x = ( (s32)centreX << 8 )
				   + (s32)idiv64( (s64)v->vx * proj, vz );
			scr[ k ].y = ( (s32)centreY << 8 )
				   - (s32)idiv64( (s64)v->vy * proj, vz );
			scr[ k ].w = idiv64( (s64)1 << 30, vz );
			scr[ k ].s = (s64)v->s * scr[ k ].w;
			scr[ k ].t = (s64)v->t * scr[ k ].w;

			// world * w, so the span walk interpolates it in
			// screen space and the per-pixel divide by w recovers
			// a perspective-correct world position -- the same
			// trick, and the same three divides, that s and t use.
			scr[ k ].wx = (s64)v->wx * scr[ k ].w;
			scr[ k ].wy = (s64)v->wy * scr[ k ].w;
			scr[ k ].wz = (s64)v->wz * scr[ k ].w;
		}

		// --- backface ------------------------------------------
		// Clockwise on screen, y downwards, is the front -- which is
		// the same convention DRAW_WALLS states as "drawn only from the
		// side that projects it left to right".
		s64 area = 0;
		for ( u32 k = 0; k < n; k++ )
		{
			const PolyScreen *a = &scr[ k ];
			const PolyScreen *b = &scr[ ( k + 1 ) % n ];
			area += (s64)a->x * b->y - (s64)b->x * a->y;
		}
		if ( area == 0 )
		{
			pResult->accepted++;		// edge-on, no pixels
			continue;
		}
		if ( area < 0 && !( flags & GPU64_RASTER_POLY_TWOSIDED ) )
		{
			pResult->rejected++;
			continue;
		}

		pResult->accepted++;

		// --- scan ----------------------------------------------
		s32 minY = scr[ 0 ].y, maxY = scr[ 0 ].y;
		for ( u32 k = 1; k < n; k++ )
		{
			if ( scr[ k ].y < minY ) minY = scr[ k ].y;
			if ( scr[ k ].y > maxY ) maxY = scr[ k ].y;
		}

		int yTop = ceilPix( minY ), yBot = ceilPix( maxY );
		if ( yTop < vy0 ) yTop = vy0;
		if ( yBot > vy1 ) yBot = vy1;

		const u16 wMask = pTex ? (u16)( pTex->w - 1 ) : 0;
		const u16 hMask = pTex ? pTex->hMask : 0;
		const unsigned texH = pTex ? pTex->h : 0;
		const boolean bFlatLit = ( flags & GPU64_RASTER_POLY_FLATLIT ) != 0;
		const boolean bMasked  = ( flags & GPU64_RASTER_POLY_MASKED ) != 0;
		const boolean bLit     = lightsLive( pState );

		for ( int y = yTop; y < yBot; y++ )
		{
			const s32 Y = (s32)y << 8;

			PolyEdgeHit hitL = { 0, 0, 0, 0, 0, 0, 0 };
			PolyEdgeHit hitR = { 0, 0, 0, 0, 0, 0, 0 };
			boolean bAny = FALSE;

			for ( u32 k = 0; k < n; k++ )
			{
				const PolyScreen *a = &scr[ k ];
				const PolyScreen *b = &scr[ ( k + 1 ) % n ];

				// Half-open in y: the row belongs to the edge
				// that contains it at its top, never to both.
				const boolean bDown = ( a->y <= Y && Y < b->y );
				const boolean bUp   = ( b->y <= Y && Y < a->y );
				if ( !bDown && !bUp )
					continue;

				const s64 num = (s64)Y - a->y;
				const s64 den = (s64)b->y - a->y;

				PolyEdgeHit h;
				h.x = a->x + (s32)idiv64( (s64)( b->x - a->x ) * num, den );
				h.w = a->w + idiv64( ( b->w - a->w ) * num, den );
				h.s = a->s + idiv64( ( b->s - a->s ) * num, den );
				h.t = a->t + idiv64( ( b->t - a->t ) * num, den );
				if ( bLit )
				{
					h.wx = a->wx + idiv64( ( b->wx - a->wx ) * num, den );
					h.wy = a->wy + idiv64( ( b->wy - a->wy ) * num, den );
					h.wz = a->wz + idiv64( ( b->wz - a->wz ) * num, den );
				} else
				{
					h.wx = h.wy = h.wz = 0;
				}

				if ( !bAny )
				{
					hitL = hitR = h;
					bAny = TRUE;
				}
				else if ( h.x < hitL.x ) hitL = h;
				else if ( h.x > hitR.x ) hitR = h;
			}

			if ( !bAny || hitR.x <= hitL.x )
				continue;

			int xs = ceilPix( hitL.x ), xe = ceilPix( hitR.x );
			if ( xs < vx0 ) xs = vx0;
			if ( xe > vx1 ) xe = vx1;
			if ( xs >= xe )
				continue;

			const s64 dx88 = (s64)hitR.x - hitL.x;
			const s64 off  = ( (s64)xs << 8 ) - hitL.x;

			s64 w = hitL.w + idiv64( ( hitR.w - hitL.w ) * off, dx88 );
			s64 s = hitL.s + idiv64( ( hitR.s - hitL.s ) * off, dx88 );
			s64 t = hitL.t + idiv64( ( hitR.t - hitL.t ) * off, dx88 );
			const s64 dw = idiv64( ( hitR.w - hitL.w ) << 8, dx88 );
			const s64 ds = idiv64( ( hitR.s - hitL.s ) << 8, dx88 );
			const s64 dt = idiv64( ( hitR.t - hitL.t ) << 8, dx88 );

			s64 lx = 0, ly = 0, lz = 0, dlx = 0, dly = 0, dlz = 0;
			if ( bLit )
			{
				lx = hitL.wx + idiv64( ( hitR.wx - hitL.wx ) * off, dx88 );
				ly = hitL.wy + idiv64( ( hitR.wy - hitL.wy ) * off, dx88 );
				lz = hitL.wz + idiv64( ( hitR.wz - hitL.wz ) * off, dx88 );
				dlx = idiv64( ( hitR.wx - hitL.wx ) << 8, dx88 );
				dly = idiv64( ( hitR.wy - hitL.wy ) << 8, dx88 );
				dlz = idiv64( ( hitR.wz - hitL.wz ) << 8, dx88 );
			}

			u8  *pDst = pTarget->pPixels + (unsigned)y * pitch + xs;
			u16 *pZ	  = s_ZBuf + (unsigned)y * GPU64_RASTER_SURFACE_W + xs;

			for ( int x = xs; x < xe;
			      x++, pDst++, pZ++, w += dw, s += ds, t += dt,
			      lx += dlx, ly += dly, lz += dlz )
			{
				if ( w <= 0 )
					continue;

				const s32 z = (s32)idiv64( (s64)1 << 30, w );
				const u16 zs = zStore( z );
				if ( zs >= *pZ )
					continue;

				u8 c;
				if ( pTex )
				{
					const s32 uu = (s32)idiv64( s, w );
					const s32 vv = (s32)idiv64( t, w );
					const unsigned ui = (unsigned)( ( uu >> 8 ) & wMask );
					const unsigned vi = (unsigned)( ( vv >> 8 ) & hMask );
					c = pTex->pTexels[ ui * texH + vi ];
					if ( bMasked && c == nKey )
						continue;
				} else
				{
					c = colour;
				}

				u8 lvl = litLevel( light, z, bFlatLit );
				if ( bLit )
					lvl = lightAdjust( pState, lvl,
							   (s32)idiv64( lx, w ),
							   (s32)idiv64( ly, w ),
							   (s32)idiv64( lz, w ) );
				const u8 *pMap = lightRow( pState, lvl );
				*pZ = zs;
				*pDst = pMap ? pMap[ c ] : c;
				pResult->pixels++;
			}
		}
	}
}

// --- UPLOAD_VERTS / UPLOAD_TEXINFO --------------------------------------

void gpu64_rasterBuildVerts( Gpu64RasterVertex *pDst, const u8 *pRecs, u32 nCount )
{
	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_VERT_BYTES;
		pDst[ i ].x = (s16)recS16( r, 0 );
		pDst[ i ].y = (s16)recS16( r, 2 );
		pDst[ i ].z = (s16)recS16( r, 4 );
		pDst[ i ].pad = 0;
	}
}

void gpu64_rasterBuildTexinfo( Gpu64RasterTexinfo *pDst, const u8 *pRecs, u32 nCount )
{
	for ( u32 i = 0; i < nCount; i++ )
	{
		const u8 *r = pRecs + i * GPU64_RASTER_TEXINFO_BYTES;
		pDst[ i ].sx   = (s16)recS16( r, 0 );
		pDst[ i ].sy   = (s16)recS16( r, 2 );
		pDst[ i ].sz   = (s16)recS16( r, 4 );
		pDst[ i ].sOff = (s16)recS16( r, 6 );
		pDst[ i ].tx   = (s16)recS16( r, 8 );
		pDst[ i ].ty   = (s16)recS16( r, 10 );
		pDst[ i ].tz   = (s16)recS16( r, 12 );
		pDst[ i ].tOff = (s16)recS16( r, 14 );
	}
}

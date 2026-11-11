// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- mesh and texture construction, and the per-mesh draw:
 transform, cull, light, near-clip, project, rasterise.

 Portable: compiled unchanged by the firmware and by tools/hostsim.

 Coordinates are left-handed -- +x right, +y up, +z away from the camera --
 and a face is front-facing when its normal points back towards the camera.
 Winding is therefore clockwise as seen from outside the model, which is what
 an exporter has to emit; get it backwards and every face of a closed mesh is
 culled, which looks exactly like "the upload did not arrive". DRAW_MESH's
 return count is what tells those two apart.
*/
#include "gpu64_3d_render.h"

// --- mesh construction --------------------------------------------------

u8 gpu64_3dBuildMesh( Gpu64_3dMesh *pMesh,
		      const u8 *pVertBlob, u32 nVertLen,
		      const u8 *pFaceBlob, u32 nFaceLen,
		      Gpu64_3dAllocFn pAlloc, void *pAllocCtx )
{
	if ( nVertLen == 0 || ( nVertLen % GPU64_3D_VERT_STRIDE ) != 0 )
		return GPU64_3D_BAD_ARGS;
	if ( nFaceLen == 0 || ( nFaceLen % GPU64_3D_FACE_STRIDE ) != 0 )
		return GPU64_3D_BAD_ARGS;

	const u32 nVerts = nVertLen / GPU64_3D_VERT_STRIDE;
	const u32 nFaces = nFaceLen / GPU64_3D_FACE_STRIDE;

	if ( nVerts > GPU64_3D_MAX_VERTS )
		return GPU64_3D_BAD_ARGS;		// the 1-byte face index

	s16 *pVerts = (s16 *)pAlloc( pAllocCtx, nVerts * 3 * sizeof( s16 ) );
	if ( !pVerts )
		return GPU64_3D_OUT_OF_MEMORY;

	Gpu64_3dFace *pFaces = (Gpu64_3dFace *)pAlloc( pAllocCtx, nFaces * sizeof( Gpu64_3dFace ) );
	if ( !pFaces )
		return GPU64_3D_OUT_OF_MEMORY;

	s32 minv[ 3 ] = {  32767,  32767,  32767 };
	s32 maxv[ 3 ] = { -32768, -32768, -32768 };

	for ( u32 i = 0; i < nVerts; i++ )
		for ( unsigned k = 0; k < 3; k++ )
		{
			const s16 c = (s16)( pVertBlob[ i * 6 + k * 2 ] | ( pVertBlob[ i * 6 + k * 2 + 1 ] << 8 ) );
			pVerts[ i * 3 + k ] = c;
			if ( c < minv[ k ] ) minv[ k ] = c;
			if ( c > maxv[ k ] ) maxv[ k ] = c;
		}

	for ( u32 f = 0; f < nFaces; f++ )
	{
		const u8 *pW = pFaceBlob + f * GPU64_3D_FACE_STRIDE;
		Gpu64_3dFace *pF = &pFaces[ f ];

		pF->i0 = pW[ 0 ];
		pF->i1 = pW[ 1 ];
		pF->i2 = pW[ 2 ];

		// An out-of-range index is rejected at upload, not clamped at draw
		// time: a mesh that indexes past its own vertex blob is a broken
		// export, and silently drawing something plausible would hide it
		// until it read a texture pointer out of the arena.
		if ( pF->i0 >= nVerts || pF->i1 >= nVerts || pF->i2 >= nVerts )
			return GPU64_3D_BAD_ARGS;

		pF->u[ 0 ] = pW[ 3 ]; pF->v[ 0 ] = pW[ 4 ];
		pF->u[ 1 ] = pW[ 5 ]; pF->v[ 1 ] = pW[ 6 ];
		pF->u[ 2 ] = pW[ 7 ]; pF->v[ 2 ] = pW[ 8 ];
		pF->texid  = pW[ 9 ];
		pF->flags  = pW[ 10 ];

		const s16 *a = &pVerts[ pF->i0 * 3 ];
		const s16 *b = &pVerts[ pF->i1 * 3 ];
		const s16 *c = &pVerts[ pF->i2 * 3 ];

		const s64 e1x = b[ 0 ] - a[ 0 ], e1y = b[ 1 ] - a[ 1 ], e1z = b[ 2 ] - a[ 2 ];
		const s64 e2x = c[ 0 ] - a[ 0 ], e2y = c[ 1 ] - a[ 1 ], e2z = c[ 2 ] - a[ 2 ];

		if ( !gpu64_3dNormalise( pF->n,
					 e1y * e2z - e1z * e2y,
					 e1z * e2x - e1x * e2z,
					 e1x * e2y - e1y * e2x ) )
		{
			// A collapsed face has no normal. It is kept rather than
			// rejected -- exporters emit degenerate triangles for all sorts
			// of reasons -- and given a normal that faces the camera, so it
			// lights sanely on the frames where it is not zero-area.
			pF->n[ 0 ] = 0;
			pF->n[ 1 ] = 0;
			pF->n[ 2 ] = -( GPU64_FX15_ONE - 1 );
		}
	}

	// Bounding sphere over the AABB centre. Not the minimal sphere, which
	// would be a Welzl pass for a couple of percent on a cull test that is
	// already conservative.
	s64 r2 = 0;
	for ( unsigned k = 0; k < 3; k++ )
		pMesh->centre[ k ] = (s16)( ( minv[ k ] + maxv[ k ] ) / 2 );

	for ( u32 i = 0; i < nVerts; i++ )
	{
		s64 d2 = 0;
		for ( unsigned k = 0; k < 3; k++ )
		{
			const s64 d = pVerts[ i * 3 + k ] - pMesh->centre[ k ];
			d2 += d * d;
		}
		if ( d2 > r2 ) r2 = d2;
	}

	u32 r = gpu64_3dSqrt64( (u64)r2 ) + 1;		// +1 covers the rounding down
	if ( r > 65535 ) r = 65535;

	pMesh->pVerts = pVerts;
	pMesh->pFaces = pFaces;
	pMesh->nVerts = (u16)nVerts;
	pMesh->nFaces = (u16)nFaces;
	pMesh->radius = (u16)r;

	return GPU64_3D_OK;
}

u8 gpu64_3dBuildTexture( Gpu64_3dTexture *pTex,
			 const u8 *pBlob, u32 nLen,
			 u8 nWShift, u8 nHShift,
			 Gpu64_3dAllocFn pAlloc, void *pAllocCtx )
{
	// 3..8 on each side, i.e. 8 to 256 texels. Power-of-two is what makes the
	// inner loop's wrap a mask instead of a modulo, and 256 is the ceiling
	// the byte texcoords already imply.
	if ( nWShift < 3 || nWShift > 8 || nHShift < 3 || nHShift > 8 )
		return GPU64_3D_BAD_ARGS;

	const u32 w = 1u << nWShift;
	const u32 h = 1u << nHShift;

	if ( nLen != w * h )
		return GPU64_3D_BAD_ARGS;

	u8 *pTexels = (u8 *)pAlloc( pAllocCtx, nLen );
	if ( !pTexels )
		return GPU64_3D_OUT_OF_MEMORY;

	for ( u32 i = 0; i < nLen; i++ )
		pTexels[ i ] = pBlob[ i ];

	pTex->pTexels = pTexels;
	pTex->wShift  = nWShift;
	pTex->hShift  = nHShift;
	pTex->w       = (u16)w;
	pTex->h       = (u16)h;

	return GPU64_3D_OK;
}

// --- the draw -----------------------------------------------------------

// A vertex mid-pipeline: view-space position plus the texcoords that travel
// with it through the near clip.
struct ClipVert
{
	Gpu64_3dVec	p;
	s32		u, v;				// 16.16 texel coordinates
};

static void lerpVert( ClipVert *pOut, const ClipVert *pA, const ClipVert *pB, s64 num, s64 den )
{
	// t = num/den, computed per component in 64 bits rather than as a
	// precomputed 16.16 t: a triangle nearly parallel to the near plane makes
	// den tiny, and rounding t first puts the clipped vertex visibly off the
	// plane.
	#define LERP( a, b )	( (s32)( (s64)(a) + ( ( (s64)(b) - (s64)(a) ) * num ) / den ) )

	pOut->p.x = LERP( pA->p.x, pB->p.x );
	pOut->p.y = LERP( pA->p.y, pB->p.y );
	pOut->p.z = LERP( pA->p.z, pB->p.z );
	pOut->u   = LERP( pA->u,   pB->u   );
	pOut->v   = LERP( pA->v,   pB->v   );

	#undef LERP
}

// Sutherland-Hodgman against the single near plane, z >= nearZ. Near-plane
// only, with the viewport rect handled by the rasteriser's scissor -- design
// doc, open question 3: the cheap answer, and the one that keeps the clipper
// at a fixed 4-vertex maximum.
static unsigned clipNear( ClipVert *pOut, const ClipVert *pIn, unsigned n, s32 nearZ )
{
	unsigned nOut = 0;

	for ( unsigned i = 0; i < n; i++ )
	{
		const ClipVert *pA = &pIn[ i ];
		const ClipVert *pB = &pIn[ ( i + 1 ) % n ];

		const boolean bAIn = pA->p.z >= nearZ;
		const boolean bBIn = pB->p.z >= nearZ;

		if ( bAIn )
			pOut[ nOut++ ] = *pA;

		if ( bAIn != bBIn )
			lerpVert( &pOut[ nOut++ ], pA, pB, (s64)nearZ - pA->p.z, (s64)pB->p.z - pA->p.z );
	}

	return nOut;
}

// --- the guard band -----------------------------------------------------
//
// clipNear() above handles the one plane that MUST be clipped -- nothing can
// be projected through z <= 0 -- and the design doc's open question 3 argued
// the other four could be left to the rasteriser's scissor. They cannot.
//
// project() divides by z, so a vertex that survives the near clip at
// z == nearZ and sits far to the side lands at a screen coordinate of
// x * focal / nearZ. In E1M1 that reaches 62921 pixels, and past 32767 it
// overflows the s32 sx/sy carry it home in. The vertex then wraps to the
// wrong side of the screen, the triangle it belongs to becomes enormous, and
// gpu64_3dRasterTriangle() fills the whole viewport with it: 35 of 300
// sampled frames of the level demo rendered as one solid colour before this
// existed. That is the "blinking, unsteady" the bench reported on 2026-09-20.
//
// The fix is to clip laterally too. GUARD viewport half-widths out, not at
// the viewport edge, so the value is first of all a bound on the arithmetic:
// at 64 half-widths a projected coordinate cannot exceed 65 * 160 = 10400
// pixels, a factor of three inside the s32 that carries it and a factor of
// four inside the rasteriser's own dx * dy product.
//
// It is not only a bound. Of the 1502 frames in the level demo, 45 differ
// between the mesh reject below on its own and the reject plus this clip, and
// all 45 are this clip removing a wrapped triangle -- five of them are frames
// the reject alone loses completely. The mesh reject removes the *work*; this
// removes the last of the garbage.
//
// A tight guard (1 to 4) is the textbook choice and would also cut the
// rasterised area, but it is NOT a free swap: clipping closer moves the
// vertices the rasteriser takes its plane gradients from, and on a close-up
// wall in the quake3d demo guards of 1, 2, 4 and 16 disagree with each other
// across 50-79% of the frame. That is a separate precision defect -- see
// dev/progress_tracker.md section 63 -- and until there is an oracle that
// says which of those pictures is right, narrowing this number is a change
// with an unknown sign.
#define GPU64_3D_GUARD		64

struct GuardPlane
{
	s64	ax, ay, az;			// keep the a.p >= 0 side
};

static s64 guardDist( const ClipVert *pV, const GuardPlane *pP )
{
	return pP->ax * pV->p.x + pP->ay * pV->p.y + pP->az * pV->p.z;
}

// Sutherland-Hodgman against one guard plane. Each plane adds at most one
// vertex, so a near-clipped triangle (at most 4) stays inside 8 across all
// four of them.
static unsigned clipGuardPlane( ClipVert *pOut, const ClipVert *pIn, unsigned n,
				const GuardPlane *pP )
{
	unsigned nOut = 0;

	for ( unsigned i = 0; i < n; i++ )
	{
		const ClipVert *pA = &pIn[ i ];
		const ClipVert *pB = &pIn[ ( i + 1 ) % n ];

		const s64 dA = guardDist( pA, pP );
		const s64 dB = guardDist( pB, pP );

		if ( dA >= 0 )
			pOut[ nOut++ ] = *pA;

		if ( ( dA >= 0 ) != ( dB >= 0 ) )
		{
			// lerpVert() multiplies a coordinate difference by num
			// before dividing, and a guard-plane distance is a
			// focal-times-coordinate product -- four orders of
			// magnitude larger than the z differences clipNear()
			// feeds it. Shift the ratio down until the product
			// cannot overflow. |den| is |dA| + |dB|, so it is always
			// the larger of the two -- but it is only positive when A
			// is the inside end. When B is, both are negative, the
			// loop below used never to run, and the unshifted product
			// overflowed into a vertex behind the eye (a divide by
			// zero in project() on a PC; a garbage triangle on the Pi,
			// where sdiv by zero returns 0). The stunt demo's long
			// track chunks passing beside the camera found it. Flip
			// both to positive first.
			s64 num = dA, den = dA - dB;
			if ( den < 0 )
			{
				num = -num;
				den = -den;
			}
			while ( den > 0x7fffffff )
			{
				num >>= 1;
				den >>= 1;
			}
			lerpVert( &pOut[ nOut++ ], pA, pB, num, den );
		}
	}

	return nOut;
}

// Returns the polygon clipped to the guard band, in pA or pB -- whichever it
// hands back. Leaves both alone and returns pA untouched when nothing
// violates, which is the case for all but a few thousand of the twenty-two
// million vertices a level run projects.
static unsigned clipGuard( ClipVert **ppOut, ClipVert *pA, ClipVert *pB,
			    unsigned n, const Gpu64_3dState *pState )
{
	const s64 f = pState->focal;
	const s64 gh = (s64)( pState->vpW / 2 ) * GPU64_3D_GUARD << 16;
	const s64 gv = (s64)( pState->vpH / 2 ) * GPU64_3D_GUARD << 16;

	const GuardPlane plane[ 4 ] =
	{
		{ -f,  0, gh },			// right
		{  f,  0, gh },			// left
		{  0, -f, gv },			// up
		{  0,  f, gv },			// down
	};

	ClipVert *pSrc = pA, *pDst = pB;

	for ( unsigned k = 0; k < 4; k++ )
	{
		// The whole point of the guard band: test first, clip only when
		// something is actually outside it.
		boolean bOut = FALSE;
		for ( unsigned i = 0; i < n; i++ )
			if ( guardDist( &pSrc[ i ], &plane[ k ] ) < 0 )
			{
				bOut = TRUE;
				break;
			}
		if ( !bOut )
			continue;

		n = clipGuardPlane( pDst, pSrc, n, &plane[ k ] );
		ClipVert *pSwap = pSrc; pSrc = pDst; pDst = pSwap;

		if ( n < 3 )
			break;
	}

	*ppOut = pSrc;
	return n;
}

static void project( Gpu64_3dRasterVert *pOut, const ClipVert *pIn,
		     const Gpu64_3dState *pState )
{
	const s64 z = pIn->p.z;

	const s64 cx = ( (s64)pState->vpX + pState->vpW / 2 ) << 16;
	const s64 cy = ( (s64)pState->vpY + pState->vpH / 2 ) << 16;

	pOut->sx = (s32)( cx + ( (s64)pIn->p.x * pState->focal ) / z );
	// Screen y grows downwards and world y grows up, so the projection
	// subtracts. Getting this backwards produces a scene that renders
	// perfectly and is upside down, which is a slow thing to notice on a
	// symmetric test cube -- hence the asymmetric cube in tools/hostsim.
	pOut->sy = (s32)( cy - ( (s64)pIn->p.y * pState->focal ) / z );

	// near/z, so the value is 1.0 (65536) at the near plane and falls off
	// with distance. Linear in screen space, which is what makes it
	// interpolable at all -- and what u/z and v/z below are divided by.
	pOut->invZ = (s32)( ( (s64)pState->nearZ << 16 ) / z );

	// u/z and v/z -- what is linear in SCREEN space, and so what the plane
	// gradients may legitimately be taken of. invZ is near/z, a positive
	// multiple of 1/z, and the rasteriser's divide undoes that same
	// constant, so no scale factor has to travel alongside.
	//
	// The shift keeps these in the 16.16 magnitude the bare texel
	// coordinate had (a texcoord byte is at most 255.0 and invZ at most
	// 1.0). Do not widen it to buy precision at the far plane: gradient()
	// truncates its result to s32 and a one-pixel-wide triangle would
	// overflow it.
	pOut->uz = (s32)( ( (s64)pIn->u * pOut->invZ ) >> 16 );
	pOut->vz = (s32)( ( (s64)pIn->v * pOut->invZ ) >> 16 );

	// View-space position, for the point lights. Carried, not recomputed:
	// this is the value the vertex has had since the model transform and
	// that clipNear() has already interpolated for a clipped corner.
	pOut->px = pIn->p.x;
	pOut->py = pIn->p.y;
	pOut->pz = pIn->p.z;
}

// --- billboards ---------------------------------------------------------

// Class 2's octant, unchanged (octant8() in gpu64_raster_core.cpp): which
// eighth of the compass a direction falls in, by comparisons alone.
static unsigned octant8( s64 x, s64 y )
{
	if ( y >= 0 )
	{
		if ( x >= 0 ) return ( y <= x ) ? 0 : 1;
		return ( y >= -x ) ? 2 : 3;
	}
	if ( x < 0 ) return ( -y < -x ) ? 4 : 5;
	return ( -y > x ) ? 6 : 7;
}

// Which of eight views of a sprite the camera is looking at. Class 2's rule
// (gpu64_rasterThings()), moved into class 1's axes and angle unit:
//
//   * the ground plane is x/z here, not x/y -- y is up in class 1
//   * yaw is a u16 binary angle, not a u8, so the half-octant bias that
//     centres view 0 on the sprite's facing is 65536/16 and not 256/16.
//     Porting the -16 across unchanged would be wrong by a factor of 256,
//     and wrong in a way that only shows at some camera angles.
//
// The vector is sprite -> camera, so view 0 is the sprite facing the player.
static unsigned spriteView( const Gpu64_3dState *pState, const Gpu64_3dVec *pPos, u16 nYaw )
{
	const s64 dx = (s64)pState->viewPos.x - pPos->x;
	const s64 dz = (s64)pState->viewPos.z - pPos->z;

	const u16 g = (u16)( nYaw - ( GPU64_ANGLE_TURN / 16 ) );
	const s64 sg = gpu64_3dSin( g );
	const s64 cg = gpu64_3dCos( g );

	// Into the sprite's own frame: forward is +z at yaw 0 and yaw turns it
	// towards +x, which is gpu64_3dMatFromEuler()'s convention.
	const s64 f = ( dz * cg + dx * sg ) >> GPU64_FX15_SHIFT;
	const s64 r = ( dx * cg - dz * sg ) >> GPU64_FX15_SHIFT;

	return octant8( f, r );
}

unsigned gpu64_3dDrawSprite( const Gpu64_3dState *pState,
			     Gpu64_3dTarget *pTarget,
			     const Gpu64_3dVec *pPos,
			     u16 nW, u16 nH,
			     u16 nTexId,
			     u16 nYaw,
			     u8 nFlags,
			     Gpu64_3dTextureLookup pLookup,
			     void *pLookupCtx )
{
	if ( pLookup == 0 || nW == 0 || nH == 0 || pState->focal <= 0 )
		return 0;

	Gpu64_3dVec rel, v;
	rel.x = pPos->x - pState->viewPos.x;
	rel.y = pPos->y - pState->viewPos.y;
	rel.z = pPos->z - pState->viewPos.z;
	gpu64_3dVecRotate( &v, &pState->viewRot, &rel );

	// Culled on its feet: a card is at one depth, so there is no clipping
	// case to handle and nothing to salvage from a sprite behind the near
	// plane.
	if ( v.z < pState->nearZ || v.z > pState->farZ )
		return 0;

	u16 texid = nTexId;
	if ( nFlags & GPU64_3D_SPRITE_DIRECTIONAL )
		texid = (u16)( nTexId + spriteView( pState, pPos, nYaw ) );

	const Gpu64_3dTexture *pTex = pLookup( pLookupCtx, texid );
	if ( pTex == 0 )
		// A sprite has no flat-colour fallback: index 0 is its transparency
		// and a solid rectangle in place of a missing view would be worse
		// than nothing. One missing view of eight is skipped, exactly like a
		// mesh a node points at that no longer resolves.
		return 0;

	const s64 z = v.z;

	const s64 cx = ( (s64)pState->vpX + pState->vpW / 2 ) << 16;
	const s64 cy = ( (s64)pState->vpY + pState->vpH / 2 ) << 16;

	const s32 sxc   = (s32)( cx + ( (s64)v.x * pState->focal ) / z );
	const s32 syFeet = (s32)( cy - ( (s64)v.y * pState->focal ) / z );

	const s64 w16 = (s64)nW << 8;			// 8.8 world -> 16.16
	const s64 h16 = (s64)nH << 8;

	const s32 halfW = (s32)( ( ( w16 * pState->focal ) / z ) / 2 );
	const s32 hPix  = (s32)( ( h16 * pState->focal ) / z );

	// The position is where the sprite's feet are -- class 2's convention
	// for a thing -- and it stands up the screen from there, so it is not
	// half-buried in the floor it is standing on.
	const s32 x0 = sxc - halfW, x1 = sxc + halfW;
	const s32 y1 = syFeet,      y0 = syFeet - hPix;

	u8 light = GPU64_3D_LIGHT_LEVELS - 1;
	if ( !( nFlags & GPU64_3D_SPRITE_UNLIT ) )
	{
		// A card always faces the camera, so its normal in view space is
		// (0,0,-1) and N.L is just the light's own -z component. That is
		// what makes a sprite respond to SET_LIGHT like every face around
		// it rather than being the one flat-bright thing in the room.
		s16 lightView[ 3 ];
		gpu64_3dNormalRotate( lightView, &pState->viewRot, pState->lightDir );

		s32 ndotl = -(s32)lightView[ 2 ];
		if ( ndotl < 0 ) ndotl = 0;

		const u32 span = ( GPU64_3D_LIGHT_LEVELS - 1 ) - pState->ambient;
		u32 lit = pState->ambient + ( ( (u32)ndotl * span ) >> GPU64_FX15_SHIFT );
		if ( lit > GPU64_3D_LIGHT_LEVELS - 1 )
			lit = GPU64_3D_LIGHT_LEVELS - 1;

		// The point lights once, at the middle of the card. Per record, not
		// per pixel: a billboard is small and near-planar, and class 2 made
		// the same call for the same reason.
		light = gpu64_3dLightAt( pState, (u8)lit,
					 v.x >> 8, ( v.y + (s32)( h16 / 2 ) ) >> 8, v.z >> 8 );
	}

	const s32 invZ = (s32)( ( (s64)pState->nearZ << 16 ) / z );

	gpu64_3dRasterSprite( pState, pTarget, x0, y0, x1, y1, invZ, pTex, nFlags, light );
	return 1;
}

unsigned gpu64_3dDrawMesh( const Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   Gpu64_3dScratch *pScratch,
			   const Gpu64_3dMesh *pMesh,
			   const Gpu64_3dVec *pPos,
			   const Gpu64_3dMat *pRot,
			   u16 nScale,
			   Gpu64_3dTextureLookup pLookup,
			   void *pLookupCtx )
{
	unsigned nDrawn = 0;

	if ( pMesh->nVerts == 0 || pMesh->nFaces == 0 || pState->focal <= 0 )
		return 0;

	// One rotation for the whole draw: view * model. Composing here rather
	// than rotating twice per vertex halves the per-vertex cost and is the
	// only reason the view transform is kept as a rotation plus an offset
	// instead of a 4x4.
	Gpu64_3dMat total;
	gpu64_3dMatMul( &total, &pState->viewRot, pRot );

	// The model's origin in view space.
	Gpu64_3dVec rel, origin;
	rel.x = pPos->x - pState->viewPos.x;
	rel.y = pPos->y - pState->viewPos.y;
	rel.z = pPos->z - pState->viewPos.z;
	gpu64_3dVecRotate( &origin, &pState->viewRot, &rel );

	// Bounding-sphere reject against all six frustum planes. Cheap, and it
	// is what makes splitting large geometry across meshes pay -- design doc,
	// Mesh format.
	//
	// The four side planes matter as much as near/far and were missing until
	// 2026-09-21. A level converted from Quake is scaled so the whole map
	// fits inside the 128-unit far plane (tools/gen_quakelevel.py's
	// QU_PER_WU), which means near/far alone rejects almost nothing: standing
	// in one room of E1M1, all 82 meshes passed, and every vertex of all
	// 15253 triangles was rotated into view space before the backface test
	// threw the far side of the map away.
	{
		const s64 radius = ( (s64)pMesh->radius * nScale ) >> 8;	// 8.8 * 8.8 -> 8.8
		Gpu64_3dVec c, cv;
		c.x = ( (s32)pMesh->centre[ 0 ] * nScale );		// 8.8 * 8.8 -> 16.16
		c.y = ( (s32)pMesh->centre[ 1 ] * nScale );
		c.z = ( (s32)pMesh->centre[ 2 ] * nScale );
		gpu64_3dVecRotate( &cv, &total, &c );

		const s64 cx = (s64)cv.x + origin.x;
		const s64 cy = (s64)cv.y + origin.y;
		const s64 cz = (s64)cv.z + origin.z;
		const s64 r16 = radius << 8;				// 8.8 -> 16.16
		if ( cz + r16 < pState->nearZ )
			return 0;
		if ( cz - r16 > pState->farZ )
			return 0;

		// The side planes, derived straight from project(): a vertex lands at
		// sx = centre + x * focal / z, so the right edge of the viewport is
		// the half-space focal * x <= ( vpW / 2 ) * z, a plane through the
		// view-space origin with normal ( focal, 0, -vpW/2 ). Reject when the
		// whole sphere is on the far side of it, i.e. when the signed
		// distance exceeds the radius.
		//
		// Everything is scaled into 8.8 pixels so the products stay inside
		// s64 with room to spare: focal is 16.16, the half-extents are whole
		// pixels, and the common factor cancels out of an inequality. The
		// norms are two sqrts per mesh, against a per-vertex cost this
		// removes entirely -- and they are exact, so the test rejects as much
		// as the sphere allows rather than being loosened by an
		// ( a + b ) >= sqrt( a*a + b*b ) shortcut.
		s64 f8 = pState->focal >> 8;				// 16.16 -> 8.8
		if ( f8 < 1 ) f8 = 1;					// never a degenerate plane
		const s64 h8 = (s64)( pState->vpW / 2 ) << 8;		// pixels -> 8.8
		const s64 v8 = (s64)( pState->vpH / 2 ) << 8;

		const s64 nx = (s64)gpu64_3dSqrt64( (u64)( f8 * f8 + h8 * h8 ) );
		const s64 ny = (s64)gpu64_3dSqrt64( (u64)( f8 * f8 + v8 * v8 ) );

		const s64 dx = f8 * cx, dy = f8 * cy, dh = h8 * cz, dv = v8 * cz;
		if (  dx - dh > r16 * nx )		// right of the viewport
			return 0;
		if ( -dx - dh > r16 * nx )		// left
			return 0;
		if (  dy - dv > r16 * ny )		// above
			return 0;
		if ( -dy - dv > r16 * ny )		// below
			return 0;
	}

	for ( unsigned i = 0; i < pMesh->nVerts; i++ )
	{
		Gpu64_3dVec m, r;

		// 8.8 model coordinate times 8.8 scale is exactly 16.16, with no
		// shift and no rounding -- the one place in this pipeline where the
		// fixed-point formats line up for free.
		m.x = (s32)pMesh->pVerts[ i * 3 + 0 ] * nScale;
		m.y = (s32)pMesh->pVerts[ i * 3 + 1 ] * nScale;
		m.z = (s32)pMesh->pVerts[ i * 3 + 2 ] * nScale;

		gpu64_3dVecRotate( &r, &total, &m );

		pScratch->view[ i ].x = r.x + origin.x;
		pScratch->view[ i ].y = r.y + origin.y;
		pScratch->view[ i ].z = r.z + origin.z;
	}



	// The light lives in world space; the pipeline works in view space, so
	// rotate it once here rather than rotating every normal back.
	s16 lightView[ 3 ];
	gpu64_3dNormalRotate( lightView, &pState->viewRot, pState->lightDir );

	for ( unsigned f = 0; f < pMesh->nFaces; f++ )
	{
		const Gpu64_3dFace *pF = &pMesh->pFaces[ f ];

		s16 n[ 3 ];
		gpu64_3dNormalRotate( n, &total, pF->n );

		const Gpu64_3dVec *pV0 = &pScratch->view[ pF->i0 ];

		// Backface test against the vector from the camera (at the view-space
		// origin) to the face. Not against the z axis: with a wide fov a face
		// at the edge of the viewport can face away from the axis and still
		// be visible.
		const s64 facing = ( (s64)n[ 0 ] * pV0->x + (s64)n[ 1 ] * pV0->y + (s64)n[ 2 ] * pV0->z );

		boolean bFlip = FALSE;
		if ( facing >= 0 )
		{
			if ( !( pF->flags & GPU64_3D_FACE_DOUBLE_SIDED ) )
				continue;
			bFlip = TRUE;			// light the side actually being seen
		}

		u8 light = GPU64_3D_LIGHT_LEVELS - 1;
		if ( !( pF->flags & GPU64_3D_FACE_UNLIT ) )
		{
			s16 ln[ 3 ] = { n[ 0 ], n[ 1 ], n[ 2 ] };
			if ( bFlip )
			{
				ln[ 0 ] = (s16)-ln[ 0 ];
				ln[ 1 ] = (s16)-ln[ 1 ];
				ln[ 2 ] = (s16)-ln[ 2 ];
			}

			s32 ndotl = gpu64_3dDot15( ln, lightView );
			if ( ndotl < 0 ) ndotl = 0;

			const u32 span = ( GPU64_3D_LIGHT_LEVELS - 1 ) - pState->ambient;
			u32 lit = pState->ambient + ( ( (u32)ndotl * span ) >> GPU64_FX15_SHIFT );
			if ( lit > GPU64_3D_LIGHT_LEVELS - 1 )
				lit = GPU64_3D_LIGHT_LEVELS - 1;
			light = (u8)lit;
		}

		ClipVert in[ 3 ], out[ 9 ], out2[ 9 ];
		const u8 idx[ 3 ] = { pF->i0, pF->i1, pF->i2 };

		for ( unsigned k = 0; k < 3; k++ )
		{
			in[ k ].p = pScratch->view[ idx[ k ] ];
			// A texcoord byte is a texel coordinate directly, wrapped by the
			// rasteriser's mask -- so 0..255 addresses a 256-wide texture
			// exactly and tiles a narrower one.
			in[ k ].u = (s32)pF->u[ k ] << 16;
			in[ k ].v = (s32)pF->v[ k ] << 16;
		}

		unsigned nOut = clipNear( out, in, 3, pState->nearZ );
		if ( nOut < 3 )
			continue;

		ClipVert *pPoly;
		nOut = clipGuard( &pPoly, out, out2, nOut, pState );
		if ( nOut < 3 )
			continue;

		const Gpu64_3dTexture *pTex = 0;
		u8 flat = pF->texid;

		if ( !( pF->flags & GPU64_3D_FACE_FLAT_COLOUR ) && pLookup )
			pTex = pLookup( pLookupCtx, pF->texid );

		// Fan the clipped polygon. At most 4 vertices come out of a single
		// plane clip of a triangle, so this is one or two triangles.
		for ( unsigned t = 1; t + 1 < nOut; t++ )
		{
			Gpu64_3dRasterVert rv[ 3 ];
			project( &rv[ 0 ], &pPoly[ 0 ],     pState );
			project( &rv[ 1 ], &pPoly[ t ],     pState );
			project( &rv[ 2 ], &pPoly[ t + 1 ], pState );

			gpu64_3dRasterTriangle( pState, pTarget, rv, pTex, flat, light );
			nDrawn++;
		}
	}

	return nDrawn;
}

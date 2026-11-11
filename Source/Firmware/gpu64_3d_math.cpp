// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- fixed-point maths. See gpu64_3d_math.h.
*/
#include "gpu64_3d_math.h"
#include "gpu64_3d_sintab.h"

// --- scalars ------------------------------------------------------------

s16 gpu64_3dSin( u16 nAngle )
{
	// 65536 angles onto 1024 entries: the table index is the top 10 bits.
	// No interpolation -- see the header on why 0.35 degrees is enough.
	return gpu64_3dSinTab[ nAngle >> 6 ];
}

s16 gpu64_3dCos( u16 nAngle )
{
	// cos(a) = sin(a + quarter turn), and the quarter turn wraps for free in
	// u16 arithmetic. That is the whole reason the wire format is a binary
	// angle.
	return gpu64_3dSinTab[ (u16)( nAngle + 16384 ) >> 6 ];
}

u32 gpu64_3dSqrt64( u64 v )
{
	// Bit-by-bit restoring square root: no division, no floating point, and
	// bounded at 32 iterations. Not in an inner loop -- normals are computed
	// at upload and bounding spheres once per mesh.
	u64 rem = 0, root = 0;

	for ( int i = 0; i < 32; i++ )
	{
		root <<= 1;
		rem = ( rem << 2 ) | ( v >> 62 );
		v <<= 2;
		if ( root < rem )
		{
			root++;
			rem -= root;
			root++;
		}
	}

	return (u32)( root >> 1 );
}

// --- matrices -----------------------------------------------------------

void gpu64_3dMatIdentity( Gpu64_3dMat *pOut )
{
	for ( int i = 0; i < 9; i++ )
		pOut->m[ i ] = 0;
	pOut->m[ 0 ] = pOut->m[ 4 ] = pOut->m[ 8 ] = GPU64_FX15_ONE - 1;
}

// gpu64: 1.15 cannot represent +1.0 exactly (32768 overflows s16), so an
// identity entry is 32767 and every product is short by one part in 32768.
// That is a third of a pixel over the whole 320-pixel width and is invisible;
// the alternative -- widening every matrix to s32 -- costs the register
// pressure the rasteriser's inner loop is trying to keep.

void gpu64_3dMatFromEuler( Gpu64_3dMat *pOut, u16 nYaw, u16 nPitch, u16 nRoll )
{
	const s32 sy = gpu64_3dSin( nYaw ),   cy = gpu64_3dCos( nYaw );
	const s32 sp = gpu64_3dSin( nPitch ), cp = gpu64_3dCos( nPitch );
	const s32 sr = gpu64_3dSin( nRoll ),  cr = gpu64_3dCos( nRoll );

	// R = Ry(yaw) * Rx(pitch) * Rz(roll), written out rather than composed
	// from three matrix multiplies: two of the three are mostly zeros, and
	// multiplying through them would cost 54 multiplies and two extra
	// roundings to 1.15 for a result this reaches in 16.
	#define M15( a, b )	(s16)( ( (s32)(a) * (s32)(b) ) >> GPU64_FX15_SHIFT )
	#define M15_3( a, b, c ) (s16)( ( ( ( (s32)(a) * (s32)(b) ) >> GPU64_FX15_SHIFT ) * (s32)(c) ) >> GPU64_FX15_SHIFT )

	// R = Ry(yaw) * Rx(pitch) * Rz(roll), multiplied out. Yaw is the OUTER
	// factor, so pitch turns about the node's own already-yawed x axis. It
	// used to be the inner one (R = Rz * Rx * Ry, every axis a world axis),
	// and that is exactly the bug this replaced: a camera's pitch was then
	// scaled by cos(yaw) and degenerated into pure roll at a quarter turn.
	// A rotation about one axis alone is the same rotation either way --
	// only which of the two sp terms carries it exactly and which picks up
	// the extra 1.15 truncation swapped over, worth 2 parts in 32768. It is
	// combinations of two axes that genuinely moved.
	//
	// The four terms carrying both sr/cr and sp are the ones whose signs are
	// easy to get wrong, and a wrong sign here does not look like a wrong
	// sign: it makes the matrix non-orthogonal, so the model *shears* as it
	// turns, which reads as a perspective artefact rather than as a maths
	// bug. Caught by tools/hostsim, on a face that should have been visible
	// and was not.
	pOut->m[ 0 ] = (s16)( M15( cy, cr ) + M15_3( sy, sp, sr ) );
	pOut->m[ 1 ] = (s16)( -M15( cy, sr ) + M15_3( sy, sp, cr ) );
	pOut->m[ 2 ] = (s16)( M15( sy, cp ) );

	pOut->m[ 3 ] = (s16)( M15( cp, sr ) );
	pOut->m[ 4 ] = (s16)( M15( cp, cr ) );
	pOut->m[ 5 ] = (s16)( -sp );

	pOut->m[ 6 ] = (s16)( -M15( sy, cr ) + M15_3( cy, sp, sr ) );
	pOut->m[ 7 ] = (s16)( M15( sy, sr ) + M15_3( cy, sp, cr ) );
	pOut->m[ 8 ] = (s16)( M15( cy, cp ) );

	#undef M15
	#undef M15_3
}

void gpu64_3dMatMul( Gpu64_3dMat *pOut, const Gpu64_3dMat *pA, const Gpu64_3dMat *pB )
{
	s16 t[ 9 ];

	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 3; c++ )
		{
			s32 acc = 0;
			for ( int k = 0; k < 3; k++ )
				acc += (s32)pA->m[ r * 3 + k ] * (s32)pB->m[ k * 3 + c ];
			t[ r * 3 + c ] = (s16)( acc >> GPU64_FX15_SHIFT );
		}

	// Written through a temporary so pOut may alias either input -- composing
	// a rotation onto itself (ROTATE_LOCAL) is the common case, not the
	// exotic one.
	for ( int i = 0; i < 9; i++ )
		pOut->m[ i ] = t[ i ];
}

void gpu64_3dMatTranspose( Gpu64_3dMat *pOut, const Gpu64_3dMat *pIn )
{
	s16 t[ 9 ];

	for ( int r = 0; r < 3; r++ )
		for ( int c = 0; c < 3; c++ )
			t[ r * 3 + c ] = pIn->m[ c * 3 + r ];

	for ( int i = 0; i < 9; i++ )
		pOut->m[ i ] = t[ i ];
}

// --- vectors ------------------------------------------------------------

void gpu64_3dVecRotate( Gpu64_3dVec *pOut, const Gpu64_3dMat *pM, const Gpu64_3dVec *pV )
{
	const s64 x = pV->x, y = pV->y, z = pV->z;

	const s64 rx = ( x * pM->m[ 0 ] + y * pM->m[ 1 ] + z * pM->m[ 2 ] ) >> GPU64_FX15_SHIFT;
	const s64 ry = ( x * pM->m[ 3 ] + y * pM->m[ 4 ] + z * pM->m[ 5 ] ) >> GPU64_FX15_SHIFT;
	const s64 rz = ( x * pM->m[ 6 ] + y * pM->m[ 7 ] + z * pM->m[ 8 ] ) >> GPU64_FX15_SHIFT;

	pOut->x = (s32)rx;
	pOut->y = (s32)ry;
	pOut->z = (s32)rz;
}

void gpu64_3dNormalRotate( s16 *pOut, const Gpu64_3dMat *pM, const s16 *pN )
{
	const s32 x = pN[ 0 ], y = pN[ 1 ], z = pN[ 2 ];

	// s32 is sufficient here in a way it is not for gpu64_3dVecRotate(): both
	// operands are 1.15, so the sum of three products cannot pass 3 << 30.
	pOut[ 0 ] = (s16)( ( x * pM->m[ 0 ] + y * pM->m[ 1 ] + z * pM->m[ 2 ] ) >> GPU64_FX15_SHIFT );
	pOut[ 1 ] = (s16)( ( x * pM->m[ 3 ] + y * pM->m[ 4 ] + z * pM->m[ 5 ] ) >> GPU64_FX15_SHIFT );
	pOut[ 2 ] = (s16)( ( x * pM->m[ 6 ] + y * pM->m[ 7 ] + z * pM->m[ 8 ] ) >> GPU64_FX15_SHIFT );
}

boolean gpu64_3dNormalise( s16 *pOut, s64 x, s64 y, s64 z )
{
	// The inputs arrive as a cross product of model-space edges and can be
	// enormous, so scale down to something whose square fits before squaring.
	s64 ax = x < 0 ? -x : x;
	s64 ay = y < 0 ? -y : y;
	s64 az = z < 0 ? -z : z;
	s64 max = ax > ay ? ax : ay;
	if ( az > max ) max = az;

	if ( max == 0 )
		return FALSE;				// collapsed face

	while ( max > 0x3FFFFFFF )
	{
		x >>= 1; y >>= 1; z >>= 1;
		max >>= 1;
	}

	const u64 sq = (u64)( x * x + y * y + z * z );
	const u32 len = gpu64_3dSqrt64( sq );
	if ( len == 0 )
		return FALSE;

	pOut[ 0 ] = (s16)( ( x * ( GPU64_FX15_ONE - 1 ) ) / (s64)len );
	pOut[ 1 ] = (s16)( ( y * ( GPU64_FX15_ONE - 1 ) ) / (s64)len );
	pOut[ 2 ] = (s16)( ( z * ( GPU64_FX15_ONE - 1 ) ) / (s64)len );
	return TRUE;
}

s32 gpu64_3dDot15( const s16 *pA, const s16 *pB )
{
	return ( (s32)pA[ 0 ] * pB[ 0 ] + (s32)pA[ 1 ] * pB[ 1 ] + (s32)pA[ 2 ] * pB[ 2 ] ) >> GPU64_FX15_SHIFT;
}

// --- projection ---------------------------------------------------------

s32 gpu64_3dFocalFromFov( u16 nFov, u32 nWidth )
{
	if ( nFov == 0 || nFov >= 32768 )
		return 0;					// >= half a turn: no projection exists

	const s32 s = gpu64_3dSin( (u16)( nFov / 2 ) );
	const s32 c = gpu64_3dCos( (u16)( nFov / 2 ) );

	if ( s <= 0 )
		return 0;

	// focal = (w/2) * cos(fov/2) / sin(fov/2), in 16.16. The numerator is
	// built at 16.16 before the divide so a wide fov -- where cos/sin is well
	// under one -- does not quantise to zero.
	const s64 num = (s64)( nWidth / 2 ) * c * GPU64_FX16_ONE;
	return (s32)( num / ( s ? s : 1 ) );
}

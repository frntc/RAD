// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- fixed-point maths for the 3D pipeline.

 Wire formats are the ones dev/milestone6_3d_design.md settled on and this
 file does not restate the reasoning:

	angles		u16 binary, 65536 = one turn
	positions	s32 16.16
	deltas/verts	s16 8.8
	scale		u16 unsigned 8.8

 Internally rotation matrices are signed 1.15 (32768 = 1.0). 8.8 was the
 obvious choice and is wrong for this job: a matrix entry quantised to 1/256
 puts a rotating cube's edge a whole pixel away from where it belongs at
 320 px wide, and the error is systematic, so it reads as wobble rather than
 as noise. 1.15 costs the same s16 and the same multiply.

 Portable by construction: this compiles for the firmware and for
 tools/hostsim with no change. Nothing here allocates, branches on state, or
 touches hardware.
*/
#ifndef _gpu64_3d_math_h
#define _gpu64_3d_math_h

#include <circle/types.h>

#define GPU64_FX16_SHIFT	16
#define GPU64_FX16_ONE		( 1 << GPU64_FX16_SHIFT )		// 16.16
#define GPU64_FX15_SHIFT	15
#define GPU64_FX15_ONE		( 1 << GPU64_FX15_SHIFT )		// 1.15
#define GPU64_FX8_SHIFT		8
#define GPU64_FX8_ONE		( 1 << GPU64_FX8_SHIFT )		// 8.8

// One turn, in the wire's binary angle.
#define GPU64_ANGLE_TURN	65536

struct Gpu64_3dVec					// 16.16, world or view space
{
	s32	x, y, z;
};

// Row-major 3x3 rotation, entries in 1.15. Rotation only -- translation is
// carried separately, because every place that composes a transform here
// wants rotate-then-translate and never a general 4x4.
struct Gpu64_3dMat
{
	s16	m[ 9 ];
};

// --- scalars ------------------------------------------------------------

// 1.15 sine/cosine of a binary angle.
s16 gpu64_3dSin( u16 nAngle );
s16 gpu64_3dCos( u16 nAngle );

// Integer square root of a 64-bit value, rounded down. Used by the
// normaliser and by UPLOAD_MESH's bounding sphere; not in any inner loop.
u32 gpu64_3dSqrt64( u64 v );

// --- matrices -----------------------------------------------------------

void gpu64_3dMatIdentity( Gpu64_3dMat *pOut );

// Intrinsic yaw, then pitch, then roll -- the order the opcode's arguments
// are listed in: yaw about the world's y axis, then pitch about the node's
// own (already yawed) x axis, then roll about its own z. R = Ry * Rx * Rz.
// Pitch is about the node's own axis and not about world x, which is what
// keeps a yawed camera pitching instead of rolling.
// Left-handed: +x right, +y up, +z into the screen.
void gpu64_3dMatFromEuler( Gpu64_3dMat *pOut, u16 nYaw, u16 nPitch, u16 nRoll );

// pOut = a * b. Safe to alias either input.
void gpu64_3dMatMul( Gpu64_3dMat *pOut, const Gpu64_3dMat *pA, const Gpu64_3dMat *pB );

// The inverse of a rotation, i.e. its transpose. Safe to alias.
void gpu64_3dMatTranspose( Gpu64_3dMat *pOut, const Gpu64_3dMat *pIn );

// --- vectors ------------------------------------------------------------

// Rotates a 16.16 vector. The intermediate is s64: a 16.16 coordinate near
// the +/-32768 limit times a 1.15 entry overflows s32 by a factor of 32768,
// and truncating early would fold a distant object back on top of the camera.
void gpu64_3dVecRotate( Gpu64_3dVec *pOut, const Gpu64_3dMat *pM, const Gpu64_3dVec *pV );

// Rotates a 1.15 direction (a face normal), staying in 1.15.
void gpu64_3dNormalRotate( s16 *pOut, const Gpu64_3dMat *pM, const s16 *pN );

// Scales a normalised-length triple to 1.15. Returns FALSE for a degenerate
// (zero-length) input, which is a mesh with a collapsed face and is the
// caller's problem, not something to divide by.
boolean gpu64_3dNormalise( s16 *pOut, s64 x, s64 y, s64 z );

// Dot product of two 1.15 triples, result 1.15.
s32 gpu64_3dDot15( const s16 *pA, const s16 *pB );

// --- projection ---------------------------------------------------------

// Focal length in 16.16 pixels for a horizontal field of view and a
// viewport width: focal = (w/2) / tan(fov/2). Returns 0 for a fov at or past
// half a turn, which SET_PERSPECTIVE rejects as BAD_ARGS.
s32 gpu64_3dFocalFromFov( u16 nFov, u32 nWidth );

#endif

// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- resources, render state, and the rasteriser.

 This is the portable half of class 1: given a byte pointer to draw into, it
 transforms, lights, clips and rasterises. It knows nothing about IO2, the
 REU, the ring buffer, or which core it runs on, and it is compiled unchanged
 by tools/hostsim -- which is the point. Every pixel-level bug in the
 pipeline is findable on a PC.

 Formats and semantics: dev/milestone6_3d_design.md. Not restated here.
*/
#ifndef _gpu64_3d_render_h
#define _gpu64_3d_render_h

#include <circle/types.h>
#include "gpu64_3d_math.h"
#include "gpu64_3d_span.h"

// --- limits -------------------------------------------------------------

#define GPU64_3D_MAX_VERTS	256		// the 1-byte face index
#define GPU64_3D_VERT_STRIDE	6		// x,y,z as 8.8 on the wire
#define GPU64_3D_FACE_STRIDE	12		// see Gpu64_3dFaceWire below

// The drawing surface, which the 3D layer shares with class 0.
#define GPU64_3D_SURFACE_W	320
#define GPU64_3D_SURFACE_H	200

// SET_VIEWPORT's ceiling: w*h*3 -- one colour byte plus two z bytes per
// pixel, which is what the render loop *writes*. Provisional per the design
// doc; the full surface (192000) fits under it.
#define GPU64_3D_BUDGET		196608

// Lighting levels the colormap carries. Level 15 is full brightness.
#define GPU64_3D_LIGHT_LEVELS	16
#define GPU64_3D_COLORMAP_BYTES	( GPU64_3D_LIGHT_LEVELS * 256 )

// Stage 17: point lights, eight of them, the same cap class 2 has. Eight is
// what a byte-wide mask makes free to test.
#define GPU64_3D_MAX_LIGHTS	8

// The shift `fall` is scaled by, and class 2's number for the same reason
// (gpu64_raster_core.cpp): r2 reaches 2^32 at the largest radius, and a
// smaller shift would round a wide, gentle light's `fall` to zero -- it
// would silently stop existing.
#define GPU64_3D_LIGHT_SHIFT	40

// --- wire formats -------------------------------------------------------

// Face blob element, exactly as it arrives. Byte 11 is padding to a stride
// the design fixed at 12: the eleven meaningful bytes would leave every
// other face straddling a cache line, and an exporter emitting a power-of-two
// stride is easier to write than one that does not.
struct Gpu64_3dFaceWire
{
	u8	i0, i1, i2;			// vertex indices into the vertex blob
	u8	u0, v0, u1, v1, u2, v2;		// per-corner texcoords
	u8	texid;				// low byte of a texture resource ID
	u8	flags;
	u8	pad;
};

#define GPU64_3D_FACE_DOUBLE_SIDED	0x01
#define GPU64_3D_FACE_FLAT_COLOUR	0x02	// texid is a palette index
#define GPU64_3D_FACE_UNLIT		0x04

// Sprite flags -- SET_SPRITE's third argument. Masked and depth-writing are
// the defaults because they are what a monster standing in a room needs;
// the flags turn each of those off for the cases that do not (a solid card,
// a muzzle flare that must not occlude what is behind it).
#define GPU64_3D_SPRITE_DIRECTIONAL	0x01	// texid names 8 consecutive views
#define GPU64_3D_SPRITE_NODEPTH		0x02	// test z, do not write it
#define GPU64_3D_SPRITE_OPAQUE		0x04	// index 0 is a colour, not a hole
#define GPU64_3D_SPRITE_UNLIT		0x08

// Unpacked face, as it is stored in the arena. The normal is computed once
// at upload from winding order (design doc, Mesh format) -- per frame it is
// only rotated, never recomputed, which is the difference between three
// multiplies and a cross product plus a square root per face per frame.
struct Gpu64_3dFace
{
	u8	i0, i1, i2;
	u8	u[ 3 ], v[ 3 ];
	u8	texid;
	u8	flags;
	s16	n[ 3 ];				// model-space normal, 1.15
};

struct Gpu64_3dMesh
{
	s16	*pVerts;			// 3 per vertex, 8.8 model space
	Gpu64_3dFace *pFaces;
	u16	nVerts;
	u16	nFaces;
	s16	centre[ 3 ];			// bounding sphere, 8.8 model space
	u16	radius;				// 8.8, unsigned
};

struct Gpu64_3dTexture
{
	u8	*pTexels;
	u8	wShift, hShift;			// dimensions are 1 << shift, 3..8
	u16	w, h;
};

// --- the resource table -------------------------------------------------
//
// One flat 16-bit ID namespace across every resource type, IDs assigned by
// the C64 (design doc, Resource lifecycle). A linear array rather than a hash:
// a lookup happens once per DRAW_MESH, not once per pixel, and a hash would
// add a failure mode -- a collision path -- to a structure whose whole job is
// to be inspectable when a mesh does not appear.

#define GPU64_3D_MAX_RESOURCES	512

#define GPU64_3D_RES_NONE	0
#define GPU64_3D_RES_MESH	1
#define GPU64_3D_RES_TEXTURE	2

struct Gpu64_3dResource
{
	u16	id;
	u8	type;
	u8	pad;
	Gpu64_3dMesh	mesh;
	Gpu64_3dTexture	tex;
};

// --- point lights -------------------------------------------------------
//
// Stage 17. The arithmetic is class 2's, slot for slot -- see
// gpu64_rasterSetLight() and lightAdjust() in gpu64_raster_core.cpp -- so
// that a program porting a level from one layer to the other gets the same
// falloff from the same strength and radius. Two differences, both forced by
// the layer:
//
//   * the position is in VIEW space, not world space, and 8.8. Class 1 has
//     no world-space vertex to compare against: gpu64_3dDrawMesh() fuses the
//     view and model rotations into one matrix and transforms straight into
//     view space, which is the whole reason a vertex costs one rotation and
//     not two. So the light is what moves into view space -- once per frame,
//     in gpu64_3dSceneApplyLights(), for at most eight of them -- rather than
//     every pixel moving back out of it.
//   * the light ADDS. Class 2's colormap runs dark-wards, so a light there
//     subtracts; class 1's runs the other way (level 15 is full brightness,
//     see gpu64_3dBuildColormap), so here it adds and clamps at 15.
struct Gpu64_3dPointLight
{
	s32	x, y, z;			// 8.8 VIEW space
	s64	r2;				// radius squared, 16.16 (8.8 squared)
	s64	fall;				// ( strength << 40 ) / r2
};

// --- render state -------------------------------------------------------

struct Gpu64_3dState
{
	// viewport, in surface coordinates
	u16	vpX, vpY, vpW, vpH;

	// projection
	u16	fov;
	s32	focal;				// 16.16 pixels
	s32	nearZ, farZ;			// 16.16 view-space depths

	// the single directional light. Stored pointing *towards* the light,
	// which is the convention N.L assumes.
	s16	lightDir[ 3 ];			// 1.15
	u8	ambient;			// 0..15

	u8	background;

	// COLORMAP[level][index] -> index. 4 KB, so it stays L1-resident and
	// lighting costs one indexed byte load per pixel and zero DRAM traffic.
	u8	colormap[ GPU64_3D_COLORMAP_BYTES ];
	boolean	bColormapValid;

	// The point lights, in view space, rebuilt from the scene's LIGHT nodes
	// once per frame by gpu64_3dSceneApplyLights() -- derived state, exactly
	// like viewRot/viewPos below, and never set directly by an opcode.
	// lightMask is the fast "any lights at all" test the rasteriser's inner
	// loop branches on; a slot with fall == 0 is off.
	Gpu64_3dPointLight lights[ GPU64_3D_MAX_LIGHTS ];
	u8		lightMask;

	// camera, as a view transform: rotate by this, then add this offset.
	Gpu64_3dMat	viewRot;
	Gpu64_3dVec	viewPos;		// camera position, world space, 16.16
	boolean		bHaveCamera;
};

// The surface being rendered into, plus the z-buffer that covers it.
struct Gpu64_3dTarget
{
	u8	*pPixels;			// top-left of the 320x200 surface
	unsigned pitch;
	u16	*pDepth;			// vpW * vpH, indexed viewport-relative
};

// --- entry points -------------------------------------------------------

// Puts the state back to its power-on defaults: full-surface viewport, 60
// degree fov, light from the front, ambient 4, background 0, colormap
// invalid. Called at boot and from every session reset.
void gpu64_3dStateDefaults( Gpu64_3dState *pState );

// Rebuilds the colormap from a 256-entry RGB palette (3 bytes an entry).
// This is BUILD_COLORMAP: 256 x 16 nearest-colour searches, a few
// milliseconds. Never call it per frame.
void gpu64_3dBuildColormap( Gpu64_3dState *pState, const u8 *pPaletteRGB );

// Fills the viewport with the background index and resets the z-buffer.
void gpu64_3dClearViewport( const Gpu64_3dState *pState, Gpu64_3dTarget *pTarget );

// Resets the z-buffer alone, leaving the pixels. Milestone 20's view-space
// pass: what is drawn after it lies in front of everything before it.
void gpu64_3dClearDepth( const Gpu64_3dState *pState, Gpu64_3dTarget *pTarget );

// Draws one mesh at a model transform. Returns the number of faces actually
// rasterised, which is the bring-up signal that says "the geometry arrived
// and was not entirely culled" -- distinguishable from "nothing was drawn"
// in a way a blank viewport is not.
//
// pLookup is how the rasteriser reaches the texture table without this file
// owning it; passing 0 renders every face as flat colour.
typedef const Gpu64_3dTexture *( *Gpu64_3dTextureLookup )( void *pCtx, u16 nId );

// Per-call scratch: the transformed vertices, one entry per mesh vertex. The
// caller owns it rather than this file holding a static, so that the day the
// render loop moves to core 1 there is no shared buffer to discover -- the
// two callers simply have two scratches. 3 KB.
struct Gpu64_3dScratch
{
	Gpu64_3dVec	view[ GPU64_3D_MAX_VERTS ];
};

// Draws one billboard at a world position: transform, cull, pick the view,
// light it, project it, rasterise. Returns 1 if it put anything on the
// screen and 0 if it was culled or its texture did not resolve.
//
// nYaw is the direction the sprite faces, used only when nFlags has
// GPU64_3D_SPRITE_DIRECTIONAL: then nTexId names the first of eight
// consecutive views and which one is drawn depends on where the camera is
// standing, the same rule DRAW_THINGS has had since milestone 11.
unsigned gpu64_3dDrawSprite( const Gpu64_3dState *pState,
			     Gpu64_3dTarget *pTarget,
			     const Gpu64_3dVec *pPos,	// 16.16 world, at its feet
			     u16 nW, u16 nH,		// 8.8 world units
			     u16 nTexId,
			     u16 nYaw,
			     u8 nFlags,
			     Gpu64_3dTextureLookup pLookup,
			     void *pLookupCtx );

unsigned gpu64_3dDrawMesh( const Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   Gpu64_3dScratch *pScratch,
			   const Gpu64_3dMesh *pMesh,
			   const Gpu64_3dVec *pPos,	// 16.16 world space
			   const Gpu64_3dMat *pRot,
			   u16 nScale,			// unsigned 8.8
			   Gpu64_3dTextureLookup pLookup,
			   void *pLookupCtx );

// --- the rasteriser -----------------------------------------------------
// Exposed because the host sim exercises it directly, without a mesh.

struct Gpu64_3dRasterVert
{
	s32	sx, sy;				// 16.16 surface coordinates
	s32	invZ;				// 16.16, near/z -- 1.0 at the near plane
	// Texel coordinates times invZ, 16.16. u and v themselves are NOT
	// linear in screen space and interpolating them directly is what made
	// every large face crease along its quad's diagonal; u/z and v/z are,
	// so those are what travel here and the rasteriser divides them back
	// out. Same 16.16 magnitude as the bare texel coordinate had (invZ is
	// at most 1.0), so gradient()'s headroom is unchanged.
	s32	uz, vz;

	// View-space position, 16.16, carried through only for the point
	// lights: interpolated across the triangle affinely -- unlike u and v,
	// which are not -- and read per pixel by lightAdjust(). Costs nothing
	// when no light is live -- the whole lit path is behind one branch
	// outside the pixel loop -- and is left uninitialised by callers that
	// set no lights.
	s32	px, py, pz;
};

// Perspective-correct, z-tested, single light level for the whole triangle
// (flat shading, per the design's chosen look). pTex == 0 means flat colour,
// and nFlat is then the palette index.
void gpu64_3dRasterTriangle( const Gpu64_3dState *pState,
			     Gpu64_3dTarget *pTarget,
			     const Gpu64_3dRasterVert *pV,	// 3 of them
			     const Gpu64_3dTexture *pTex,
			     u8 nFlat,
			     u8 nLight );

// The point lights evaluated at one view-space point, 8.8 -- what a sprite
// (and anything else lit per record rather than per pixel) uses. Returns
// nLevel unchanged when no light reaches the point.
u8 gpu64_3dLightAt( const Gpu64_3dState *pState, u8 nLevel, s32 x, s32 y, s32 z );

// A screen-aligned billboard: an axis-aligned rectangle at one constant
// depth, drawn from a texture whose index 0 is see-through unless
// GPU64_3D_SPRITE_OPAQUE says otherwise. Separate from the triangle
// rasteriser rather than two triangles through it, because everything the
// triangle path spends its setup on -- three plane gradients, an edge walk,
// a per-pixel z step -- is a constant here.
//
// Coordinates are 16.16 surface coordinates, x0/y0 inclusive-ish and x1/y1
// exclusive-ish in the same pixel-centre sense the triangle rasteriser uses.
void gpu64_3dRasterSprite( const Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   s32 x0, s32 y0, s32 x1, s32 y1,
			   s32 nInvZ,
			   const Gpu64_3dTexture *pTex,
			   u8 nFlags,
			   u8 nLight );

// --- mesh and texture construction --------------------------------------
// Parse a blob pair / a texture blob into an arena block. pAlloc is the
// arena's bump allocator, passed in rather than called directly so the host
// sim can hand over plain malloc.
typedef void *( *Gpu64_3dAllocFn )( void *pCtx, u32 nBytes );

// Return values are GPU64_ERR_* codes from gpu64_api.h, repeated here so
// this file needs no firmware header. Kept numerically identical on purpose.
#define GPU64_3D_OK		0x00
#define GPU64_3D_BAD_ARGS	0x04
#define GPU64_3D_OUT_OF_MEMORY	0x08

u8 gpu64_3dBuildMesh( Gpu64_3dMesh *pMesh,
		      const u8 *pVertBlob, u32 nVertLen,
		      const u8 *pFaceBlob, u32 nFaceLen,
		      Gpu64_3dAllocFn pAlloc, void *pAllocCtx );

u8 gpu64_3dBuildTexture( Gpu64_3dTexture *pTex,
			 const u8 *pBlob, u32 nLen,
			 u8 nWShift, u8 nHShift,
			 Gpu64_3dAllocFn pAlloc, void *pAllocCtx );

#endif

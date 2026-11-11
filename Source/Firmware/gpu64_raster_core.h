// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 8 -- the class 2 raster core: columns, spans and scaled
 sprites.

 This is the portable half of class 2. Given a byte pointer to draw into and
 a batch of records exactly as they arrived off the bus, it clips, wraps,
 lights and writes pixels. It knows nothing about IO2, the REU, the arena or
 the framebuffer object, and it is compiled unchanged by tools/rastercheck --
 which is the point: every pixel-level disagreement with the reference model
 is findable on a PC.

 Wire formats and semantics: docs/api_design.md (class 2). Rationale:
 dev/milestone8_raster_design.md. Neither is restated here.
*/
#ifndef _gpu64_raster_core_h
#define _gpu64_raster_core_h

#include <circle/types.h>

// The drawing surface, shared with class 0 and class 1.
#define GPU64_RASTER_SURFACE_W	320
#define GPU64_RASTER_SURFACE_H	200

// Record stride, both batch kinds. Fixed at a power of two so the 6502 side
// indexes a record with a shift -- see the design doc.
#define GPU64_RASTER_REC_BYTES	16

// DRAW_SECTORS' wall record. Twice the stride, still a shift on the 6502.
// It has to be: a two-sided wall names two sectors and three textures, and
// none of that fits beside the geometry in sixteen bytes. It costs nothing
// per frame -- a level is uploaded once and its checksum computed once.
#define GPU64_RASTER_WALL2_BYTES	32

// DRAW_POLYS' face record, and the two tables it indexes. Milestone 9: a
// convex polygon in world space, which is the primitive a wall record and a
// sector both are special cases of -- see dev/milestone9_poly_design.md.
#define GPU64_RASTER_POLY_BYTES		16
#define GPU64_RASTER_VERT_BYTES		8
#define GPU64_RASTER_TEXINFO_BYTES	16

#define GPU64_RASTER_MAX_VERTS		4096
#define GPU64_RASTER_MAX_TEXINFO	255
// The resident world: poly records that live in gpu64 across frames, so a
// Quake frame costs ten register writes instead of kilobytes of DMA.
#define GPU64_RASTER_MAX_WORLD_POLYS	2048

// Vertices in one face. Quake's own limit before it subdivides; the near
// clip can add one, hence the working buffer being larger.
#define GPU64_RASTER_MAX_POLY_VERTS	16
#define GPU64_RASTER_POLY_CLIP_VERTS	20

// A sector record, SET_SECTORS.
#define GPU64_RASTER_SEC_BYTES		8
#define GPU64_RASTER_MAX_SECTORS	128
#define GPU64_RASTER_NO_SECTOR		0xff	// backSec: the wall is solid

// Texture limits. h is masked per pixel and so must be a power of two; w is
// wrapped once per column record and need not be. Both cap at 1024, which is
// four times the tallest thing Doom has.
#define GPU64_RASTER_MAX_DIM	1024

// Colormap: levels x 256 bytes, colour = map[ light * 256 + c ].
#define GPU64_RASTER_MAX_LEVELS	64

// Dynamic point lights, SET_LIGHT. Eight slots: enough for a muzzle flash,
// a rocket in flight, two explosions and the level's own torches without
// the per-pixel loop growing a length a game has to think about.
#define GPU64_RASTER_MAX_LIGHTS		8

// --- record flags -------------------------------------------------------
// Column records only; span records have no flags byte.
#define GPU64_RASTER_COL_MASKED	0x01	// skip source texels equal to the key

// DRAW_SPRITE flags.
#define GPU64_RASTER_SPR_MASKED	0x01
#define GPU64_RASTER_SPR_FLIPX	0x02

// DRAW_WALLS flags.
#define GPU64_RASTER_WALL_MASKED  0x01	// skip texels equal to the key
#define GPU64_RASTER_WALL_FLATLIT 0x02	// use the record's light unchanged,
					// with no darkening by distance

// DRAW_THINGS flags. A separate set and not DRAW_SPRITE's, because a thing
// needs four bits and FLIPX and FLATLIT would otherwise collide at 0x02.
#define GPU64_RASTER_THING_MASKED  0x01	// skip texels equal to the batch key
#define GPU64_RASTER_THING_FLIPX   0x02
#define GPU64_RASTER_THING_NODEPTH 0x04	// ignore the depth buffer entirely:
					// neither tested nor written, so the
					// thing is painted over everything
#define GPU64_RASTER_THING_FLATLIT 0x08	// the record's light unchanged, with
					// no darkening by distance
#define GPU64_RASTER_THING_DIRECTIONAL 0x10 // the thing has eight views. Byte
					// 13 is the direction it FACES (0 =
					// +x, 256 to the circle) and the id in
					// byte 10 is the first of eight
					// consecutive ones; gpu64 picks the
					// view from where the camera stands.
					// Milestone 11.

// DRAW_THINGS batch flags (ARG8), beside GPU64_RASTER_BATCH_CHECKSUM. The
// camera is a property of the batch and not of a record: a frame's things all
// belong to one world, and a game that has moved to SET_CAMERA3D has moved
// every one of them. Milestone 10.
#define GPU64_RASTER_BATCH_CAM3D   0x02	// project through SET_CAMERA3D
					// instead of SET_CAMERA

// DRAW_SECTORS wall flags. MASKED and FLATLIT above are shared.
#define GPU64_RASTER_WALL_NOFLATS 0x04	// this wall's columns paint no floor
					// and no ceiling, whatever the camera says

// DRAW_POLYS face flags.
#define GPU64_RASTER_POLY_MASKED   0x01	// skip texels equal to the batch key
#define GPU64_RASTER_POLY_FLATLIT  0x02	// the record's light unchanged
#define GPU64_RASTER_POLY_TWOSIDED 0x04	// no backface cull: draw it from
					// either side

// Sector flags.
#define GPU64_RASTER_SEC_SKY	0x01	// the ceiling is not painted and no
					// upper texture is drawn against it

// SET_CAMERA flags.
#define GPU64_RASTER_CAM_PAINT	0x01	// walls also paint the floor and the
					// ceiling of the columns they cover

// The near plane, in world units as 8.8. A wall closer than this is clipped
// against it rather than divided by, which is the only reason the projection
// needs no special case for a divisor of zero.
#define GPU64_RASTER_NEAR	0x0040	// 0.25

// --- a texture ----------------------------------------------------------
// Column-major: texel( u, v ) is pTexels[ u * h + v ]. See the design doc on
// why this order and not the row-major one class 1 uses.
struct Gpu64RasterTexture
{
	const u8 *pTexels;
	u16	w, h;
	u16	hMask;			// h - 1; h is always a power of two
	u8	wIsPow2;		// DRAW_SPANS needs it; DRAW_COLUMNS does not
	u8	pad;
};

// --- the draw target ----------------------------------------------------
struct Gpu64RasterTarget
{
	u8	 *pPixels;		// top-left of the 320x200 surface
	unsigned  pitch;		// bytes per row, >= 320
};

// One sector: the floor and ceiling planes a wall record refers to by id.
// Heights are ABSOLUTE world 8.8, and for DRAW_SECTORS so is camEyeH --
// which is the one semantic difference from DRAW_WALLS, where camEyeH is
// measured from a floor at zero. A level with steps has no such floor.
struct Gpu64RasterSector
{
	s16	floorH, ceilH;
	u8	floorCol, ceilCol;
	u8	light;			// base light for this sector's flats
	u8	flags;
};

// One vertex of the pool, UPLOAD_VERTS. Eight bytes rather than six so the
// 6502 indexes the table with a shift, the same reason every batch record is
// a power of two long.
struct Gpu64RasterVertex
{
	s16	x, y, z;		// 8.8 world units, z is up
	s16	pad;
};

// Quake's texinfo: the two axes that turn a world position into texture
// coordinates. s = ( P . sAxis ) >> 8 + sOff, in 8.8 texels. Shared by every
// face on a surface, which is what makes two faces align for free.
struct Gpu64RasterTexinfo
{
	s16	sx, sy, sz, sOff;
	s16	tx, ty, tz, tOff;
};

// One dynamic point light, SET_LIGHT. The position and radius are world
// 8.8; everything the per-pixel loop needs is precomputed here so that the
// inner loop is two multiplies and a shift per light and no divide at all.
//
// Falloff is linear in the SQUARED distance -- ( r2 - d2 ) / r2 -- not in the
// distance. That is not physics and not what Quake does, but it needs no
// square root, it reaches zero exactly at the radius so a light never leaves
// a hard edge, and its shape (bright core, long soft skirt) reads on a
// 256-colour ramp much the way an inverse square does. The alternative cost
// a sqrt or a divide per pixel per light, which is the whole budget.
struct Gpu64RasterLight
{
	s32	x, y, z;		// world 8.8
	s64	r2;			// radius squared, 16.16
	s64	fall;			// ( strength << 40 ) / r2
};

// --- render state -------------------------------------------------------
struct Gpu64RasterState
{
	// The clip rectangle every class 2 primitive obeys, in surface
	// coordinates. Defaults to the whole surface.
	u16	viewX, viewY, viewW, viewH;

	// levels x 256 bytes, or 0 for "identity" -- no lighting, the record's
	// light byte ignored. Not owned: the caller keeps it alive.
	const u8 *pColormap;
	u16	levels;

	// --- the camera, SET_CAMERA ------------------------------------
	//
	// Why this is firmware state and not part of a wall record: it is the
	// same for every wall in the frame, and it is the ONLY thing about a
	// static level that changes between frames. A C64 that has uploaded
	// its walls once then spends ten bytes a frame on the whole view.
	s16	camX, camY;		// 8.8 world units
	u8	camAng;			// binary angle, 256 to the circle
	u8	camFlags;
	s16	camEyeH;		// eye above the floor, 8.8
	s16	camCeilH;		// ceiling above the floor, 8.8
	u16	camProj;		// projection distance in pixels, 8.8
	u8	camFloorCol, camCeilCol;
	s16	camHorizon;		// pixels from the view's centre row

	// --- the 3D camera, SET_CAMERA3D -------------------------------
	//
	// Deliberately not the same fields as SET_CAMERA above: this one has a
	// world z and a real pitch rotation, and no floor/ceiling colours,
	// because a polygon carries its own surface. camProj3 == 0 means it
	// was never set, and DRAW_POLYS rejects the batch.
	s16	cam3X, cam3Y, cam3Z;	// 8.8 world units, z is up
	u8	cam3Yaw;		// 0 looks along +x, 256 to the circle
	s8	cam3Pitch;		// positive looks up, same units
	u16	cam3Proj;		// projection distance in pixels, 8.8
	u8	cam3Flags;		// reserved, 0

	// --- the vertex pool and the texinfo table ---------------------
	// Uploaded once per level; a face record names both by index. Neither
	// is owned: the caller keeps them alive.
	const struct Gpu64RasterVertex  *pVerts;
	u16	verts;
	const struct Gpu64RasterTexinfo *pTexinfo;
	u16	texinfos;

	// --- dynamic lights, SET_LIGHT ---------------------------------
	//
	// Embedded, not a pointer: eight slots is 160 bytes, they are set from
	// inline argument bytes rather than uploaded, and a copy in the state
	// means the core needs no lifetime rule for them. lightMask has a bit
	// per live slot purely so the unlit path -- which is every existing
	// program -- can be taken with one test.
	struct Gpu64RasterLight lights[ GPU64_RASTER_MAX_LIGHTS ];
	u8	lightMask;

	// --- the sector table, SET_SECTORS -----------------------------
	//
	// DRAW_SECTORS reads heights, flat colours and light from here by id,
	// so a wall record carries two one-byte ids instead of four heights.
	// Not owned: the caller keeps it alive.
	const Gpu64RasterSector *pSectors;
	u16	sectors;
};


// Batch outcome, which RASTER_STATS reports back to the C64. `rejected` is
// the number that matters: a batch that lost bytes in transit shows up here.
struct Gpu64RasterBatchResult
{
	u16	accepted;
	u16	rejected;
	u32	pixels;
};

// Texture lookup, so the core does not need to know how the wire layer
// stores its table. Returns 0 for an unknown id -- the caller's records
// naming it are rejected.
typedef const Gpu64RasterTexture *( *Gpu64RasterLookupFn )( void *pCtx, u8 nId );

#ifdef __cplusplus
extern "C++" {
#endif

// Power-on / RASTER_RESET state: full-surface view, identity lighting.
void gpu64_rasterStateDefaults( Gpu64RasterState *pState );

// SET_LIGHT: put a dynamic point light in a slot, or turn the slot off with
// a zero radius or a zero strength. FALSE only for a slot out of range --
// the arithmetic itself cannot fail. nStrength is in colormap levels of
// brightening at the light's centre; nRadius is world 8.8.
boolean gpu64_rasterSetLight( Gpu64RasterState *pState, unsigned nSlot,
			      s32 x, s32 y, s32 z, u16 nRadius, u8 nStrength );

// Fills a Gpu64RasterTexture from raw bytes already in their final storage.
// nSrcRowMajor transposes on the way in. Returns FALSE if the dimensions are
// not acceptable (h not a power of two, either dim 0 or over the cap).
// pDst must have room for w * h bytes.
boolean gpu64_rasterBuildTexture( Gpu64RasterTexture *pTex, u8 *pDst,
				  const u8 *pSrc, u32 nSrcLen,
				  u16 w, u16 h, boolean bSrcRowMajor );

// FILL_VIEW: the view rectangle, one raw palette index, no colormap.
void gpu64_rasterFillView( const Gpu64RasterState *pState,
			   const Gpu64RasterTarget *pTarget, u8 nColour );

// DRAW_COLUMNS / DRAW_SPANS. pRecs points at nCount * 16 bytes.
void gpu64_rasterColumns( const Gpu64RasterState *pState,
			  const Gpu64RasterTarget *pTarget,
			  const u8 *pRecs, u32 nCount, u8 nKey,
			  Gpu64RasterLookupFn pLookup, void *pCtx,
			  Gpu64RasterBatchResult *pResult );

void gpu64_rasterSpans( const Gpu64RasterState *pState,
			const Gpu64RasterTarget *pTarget,
			const u8 *pRecs, u32 nCount,
			Gpu64RasterLookupFn pLookup, void *pCtx,
			Gpu64RasterBatchResult *pResult );

// DRAW_WALLS: wall segments in WORLD space, projected by the camera in
// pState. One record is one wall and covers as many screen columns as it
// happens to; a batch is a whole level. Depth-tested per column against a
// buffer this call clears itself, so the C64 need not sort them.
void gpu64_rasterWalls( const Gpu64RasterState *pState,
			const Gpu64RasterTarget *pTarget,
			const u8 *pRecs, u32 nCount, u8 nKey,
			Gpu64RasterLookupFn pLookup, void *pCtx,
			Gpu64RasterBatchResult *pResult );

// SET_SECTORS' table build: nCount records of 8 bytes, straight off the bus.
// Returns FALSE if any sector is impossible (ceiling at or below floor).
boolean gpu64_rasterBuildSectors( Gpu64RasterSector *pDst, const u8 *pRecs,
				  u32 nCount );

// DRAW_SECTORS: walls with per-sector floor and ceiling heights, two-sided
// portals with upper and lower bands, and flats painted per column. Depth is
// per PIXEL here, not per column as DRAW_WALLS does it -- a portal leaves a
// see-through gap in the middle of its own column, so a column can no longer
// have one depth. That is also what lets records arrive in any order.
void gpu64_rasterSectors( const Gpu64RasterState *pState,
			  const Gpu64RasterTarget *pTarget,
			  const u8 *pRecs, u32 nCount, u8 nKey,
			  Gpu64RasterLookupFn pLookup, void *pCtx,
			  Gpu64RasterBatchResult *pResult );

// RASTER_RESET's half of the depth buffer's lifecycle: the buffer is empty
// again on the next use. DRAW_SECTORS clears the view every batch, so this
// matters only to a program that sends things without sectors.
void gpu64_rasterZReset( void );

// DRAW_THINGS: billboard sprites in WORLD space, projected by the camera in
// pState and depth-tested per PIXEL against the buffer DRAW_SECTORS filled --
// which is the whole point, and the one thing DRAW_SPRITE cannot do. Records
// are 16 bytes. Depth is written as well as tested, at drawn pixels only, so
// a batch of things is order-independent exactly as a batch of walls is.
//
// It does NOT clear the depth buffer: DRAW_SECTORS owns that, and DRAW_THINGS
// is meant to run after it in the same frame.
//
// nBatchFlags selects the camera: GPU64_RASTER_BATCH_CAM3D projects through
// SET_CAMERA3D, so the same records serve a Quake game with a real pitch.
// Without it the batch goes through SET_CAMERA, unchanged from milestone 8d.
//
// GPU64_RASTER_THING_DIRECTIONAL turns a record into an eight-view sprite:
// view n covers the arc where the camera stands at the thing's facing angle
// plus n*45 degrees, give or take 22.5, so view 0 is the thing seen head on,
// view 2 is its left side and view 4 is its back. The texture drawn is
// byte 10 plus n. Milestone 11.
void gpu64_rasterThings( const Gpu64RasterState *pState,
			 const Gpu64RasterTarget *pTarget,
			 const u8 *pRecs, u32 nCount, u8 nKey, u8 nBatchFlags,
			 Gpu64RasterLookupFn pLookup, void *pCtx,
			 Gpu64RasterBatchResult *pResult );

// UPLOAD_VERTS / UPLOAD_TEXINFO table builds: nCount records straight off
// the bus. Neither can fail on content -- there is no such thing as an
// impossible vertex -- so neither returns a verdict; the wire layer has
// already checked the count and the length.
void gpu64_rasterBuildVerts( Gpu64RasterVertex *pDst, const u8 *pRecs, u32 nCount );
void gpu64_rasterBuildTexinfo( Gpu64RasterTexinfo *pDst, const u8 *pRecs, u32 nCount );

// DRAW_POLYS: convex polygons in WORLD space, projected through the 3D
// camera in pState, perspective-texture-mapped and depth-tested per pixel
// against the same buffer DRAW_SECTORS and DRAW_THINGS use. Records are 16
// bytes and index the vertex pool and the texinfo table.
//
// It does NOT clear the depth buffer: a Quake frame may be several batches,
// and FILL_VIEW is the op that owns the clear.
void gpu64_rasterPolys( const Gpu64RasterState *pState,
			const Gpu64RasterTarget *pTarget,
			const u8 *pRecs, u32 nCount, u8 nKey,
			Gpu64RasterLookupFn pLookup, void *pCtx,
			Gpu64RasterBatchResult *pResult );

// DRAW_SPRITE: the texture scaled into the screen rectangle x,y,w,h, clipped
// to the view and to the inclusive row range clipY0..clipY1.
void gpu64_rasterSprite( const Gpu64RasterState *pState,
			 const Gpu64RasterTarget *pTarget,
			 const Gpu64RasterTexture *pTex,
			 int x, int y, unsigned w, unsigned h,
			 u8 nLight, u8 nKey, int nClipY0, int nClipY1, u8 nFlags,
			 Gpu64RasterBatchResult *pResult );

// The optional batch checksum: a plain 16-bit sum of the record bytes, which
// is what a 6502 can afford to compute. Deliberately not a CRC.
u16 gpu64_rasterChecksum( const u8 *p, u32 nLen );

#ifdef __cplusplus
}
#endif

#endif

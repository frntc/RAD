// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6, stage 14 -- the retained scene graph: nodes, transforms,
 and the active camera.

 Portable, like gpu64_3d_render.h and for the same reason: every transform bug
 -- ROTATE_LOCAL composing the wrong way, MOVE_LOCAL walking sideways instead
 of forward, a camera whose view is not actually the inverse of its pose -- is
 findable by tools/hostsim on a PC. What is deliberately NOT here is anything
 that resolves a mesh resource ID to a Gpu64_3dMesh: that table lives in
 gpu64_3d_class1.cpp because only core 0 can pull an upload off the C64 bus.
 DRAW_NODE therefore stays in gpu64_3d_class1.cpp too -- it is a resource
 lookup plus a call to the already-portable gpu64_3dDrawMesh(), not new
 rendering logic, exactly the way opDrawMesh() is a thin IO2 wrapper today.

 Wire formats and opcode semantics: dev/milestone6_3d_design.md,
 "Transforms: no matrices cross the bus", and docs/class1-3d-mesh-reference.md's
 Scene nodes / Transforms tables. Not restated here.
*/
#ifndef _gpu64_3d_scene_h
#define _gpu64_3d_scene_h

#include <circle/types.h>
#include "gpu64_3d_math.h"
#include "gpu64_3d_render.h"		// Gpu64_3dState

// 256 object instances, per the design doc's own limit -- "the scene table,
// sized against the SCENE_COMMIT copy". There is no SCENE_COMMIT yet (stage
// 16), but the cap is part of the design, not of the loop, so it applies now.
#define GPU64_3D_MAX_NODES	256

#define GPU64_3D_NODE_NONE	0
#define GPU64_3D_NODE_OBJECT	1
#define GPU64_3D_NODE_CAMERA	2
#define GPU64_3D_NODE_SPRITE	3	// stage 17: a billboard in the world
#define GPU64_3D_NODE_LIGHT	4	// stage 17: a point light

struct Gpu64_3dNode
{
	u16	id;
	u8	type;
	u8	visible;		// SET_VISIBLE; nonzero = drawn

	// milestone 20: the pose is relative to the active camera, not to the
	// world. Only WORLD_TICK sets it; every CREATE_* clears it. OBJECT and
	// SPRITE nodes with it set draw in a pass of their own after the world,
	// over a cleared depth buffer, so a gun or a status bar is never
	// cut by a wall it is standing in. A LIGHT with it set lights from a
	// point fixed to the eye -- the muzzle flash. Ignored on a CAMERA.
	u8	viewSpace;

	u16	meshId;			// OBJECT only -- resolved by the caller

	// SPRITE only. texId is the base texture; with
	// GPU64_3D_SPRITE_DIRECTIONAL it is the first of eight consecutive
	// views and the node's own yaw picks between them.
	u16	texId;
	u16	spriteW, spriteH;	// 8.8 world units
	u8	spriteFlags;		// GPU64_3D_SPRITE_*

	// LIGHT only. Either being zero turns the light off without destroying
	// the node -- the same "twelve stores end a muzzle flash" convention
	// class 2's SET_LIGHT has, and the reason a flash does not need
	// CREATE/DESTROY every time it fires.
	u8	lightStrength;		// colormap levels at the centre
	u16	lightRadius;		// 8.8 world units

	Gpu64_3dVec	pos;		// 16.16 world space

	// Orientation is stored as the three absolute angles, not just the
	// matrix built from them. Two reasons: GET_TRANSFORM has to report
	// back exactly the yaw/pitch/roll a program set (or accumulated via
	// ROTATE_LOCAL), and there is no arcsin/atan2 in gpu64_3d_math.h to
	// recover angles from an arbitrary rotation matrix -- only the
	// forward direction, sin/cos-of-angle to matrix. ROTATE_LOCAL is
	// therefore plain wrapping addition on each angle independently, per
	// the design doc's own words for the wire format: "add a turn rate
	// and let it wrap: no clamp, no modulo". The matrix is a cache,
	// rebuilt whenever any of the three change.
	u16	yaw, pitch, roll;

	u16	scale;			// unsigned 8.8, uniform

	Gpu64_3dMat	rot;		// gpu64_3dMatFromEuler( yaw, pitch, roll )
};

struct Gpu64_3dScene
{
	Gpu64_3dNode	node[ GPU64_3D_MAX_NODES ];
	u16		activeCameraId;
	boolean		bHaveActiveCamera;
};

// Power-on / session-reset state: every node freed, no active camera. Mirrors
// gpu64_3dReset()'s treatment of the resource table -- a RUN/STOP+RESTORE
// must not leave the next program looking at a dead program's nodes.
void gpu64_3dSceneReset( Gpu64_3dScene *pScene );

// Finds a live node by ID. nType == GPU64_3D_NODE_NONE matches either type,
// same convention resFind() in gpu64_3d_class1.cpp uses for resources.
Gpu64_3dNode *gpu64_3dSceneFind( Gpu64_3dScene *pScene, u16 nId, u8 nType );

// --- lifecycle ($20-$24) -------------------------------------------------
// Every one of these returns a GPU64_3D_OK/GPU64_3D_BAD_ARGS/-style code from
// gpu64_3d_render.h's GPU64_3D_* set, repeated here so this file needs no
// firmware header -- same convention gpu64_3d_render.h's own mesh/texture
// builders use. GPU64_3D_OUT_OF_MEMORY here means the node table, not the
// resource arena.
#define GPU64_3D_BAD_ID		0x0A	// numerically identical to GPU64_ERR_BAD_ID

u8 gpu64_3dSceneCreateObject( Gpu64_3dScene *pScene, u16 nId, u16 nMeshId );
u8 gpu64_3dSceneCreateCamera( Gpu64_3dScene *pScene, u16 nId );
u8 gpu64_3dSceneCreateSprite( Gpu64_3dScene *pScene, u16 nId, u16 nTexId );
u8 gpu64_3dSceneCreateLight( Gpu64_3dScene *pScene, u16 nId );
u8 gpu64_3dSceneDestroyNode( Gpu64_3dScene *pScene, u16 nId );
u8 gpu64_3dSceneSetActiveCamera( Gpu64_3dScene *pScene, u16 nId );
u8 gpu64_3dSceneSetVisible( Gpu64_3dScene *pScene, u16 nId, boolean bVisible );

// --- transforms ($30-$36) -------------------------------------------------
// All take world-space/absolute or local/delta quantities already widened to
// this file's internal units (16.16 positions, u16 binary angles) -- the
// caller (gpu64_3d_class1.cpp's opcode handlers) does the 8.8->16.16 and
// wire-byte unpacking, exactly as opDrawMesh() already does for DRAW_MESH's
// own position and rotation arguments.

u8 gpu64_3dSceneSetPosition( Gpu64_3dScene *pScene, u16 nId, const Gpu64_3dVec *pPos );
u8 gpu64_3dSceneSetOrientation( Gpu64_3dScene *pScene, u16 nId, u16 nYaw, u16 nPitch, u16 nRoll );

// dx/dy/dz already widened to 16.16 (the wire is 8.8; see above).
u8 gpu64_3dSceneMoveLocal( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz );
u8 gpu64_3dSceneMoveWorld( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz );

u8 gpu64_3dSceneRotateLocal( Gpu64_3dScene *pScene, u16 nId, u16 dYaw, u16 dPitch, u16 dRoll );
u8 gpu64_3dSceneSetScale( Gpu64_3dScene *pScene, u16 nId, u16 nScale );

// --- per-type properties ($27-$28) ----------------------------------------
// Position and orientation are NOT here: a sprite and a light are placed
// with the same SET_POSITION/ROTATE_LOCAL every other node uses, which is
// the invariant that makes the transform opcodes worth having. These two
// carry only what is specific to the type.

u8 gpu64_3dSceneSetSprite( Gpu64_3dScene *pScene, u16 nId, u16 nW, u16 nH, u8 nFlags );
u8 gpu64_3dSceneSetPointLight( Gpu64_3dScene *pScene, u16 nId, u8 nStrength, u16 nRadius );

u8 gpu64_3dSceneGetTransform( Gpu64_3dScene *pScene, u16 nId,
			       Gpu64_3dVec *pPos, u16 *pYaw, u16 *pPitch, u16 *pRoll );

// --- the active camera -----------------------------------------------------
// Derives pState->viewRot/viewPos from the active camera node's pose -- the
// view transform is the inverse of the camera's world transform: rotate by
// the transpose after subtracting the camera's position. Exactly what
// tools/hostsim/hostsim.cpp's own setCamera() has done by hand since before
// CREATE_CAMERA existed (see that file's "camera" section), which is what
// proved this is enough before any firmware node existed to drive it.
//
// With no active camera, sets the identity/zero pair -- draw in world space,
// same as DRAW_MESH always has. Deterministic either way: never leaves
// whatever was there before.
void gpu64_3dSceneApplyCamera( const Gpu64_3dScene *pScene, Gpu64_3dState *pState );

// --- the point lights -------------------------------------------------------
// Gathers the scene's live LIGHT nodes into pState->lights[], transforming
// each into view space, and sets pState->lightMask. Derived state, rebuilt
// every frame exactly like the camera above -- an opcode never writes
// pState->lights[] directly.
//
// MUST be called after gpu64_3dSceneApplyCamera(): it reads the view
// transform that call produces. Call it the other way round and the lights
// are placed by the *previous* frame's camera, which looks like lights that
// lag the player by one frame -- visible only while moving.
//
// At most GPU64_3D_MAX_LIGHTS are taken, in scene order. A light is skipped
// when it is hidden (SET_VISIBLE 0) or inert (strength or radius zero), so
// a node created and not yet given parameters cannot occupy one of the eight
// slots a real light needs.
void gpu64_3dSceneApplyLights( const Gpu64_3dScene *pScene, Gpu64_3dState *pState );

// --- one whole frame -------------------------------------------------------
// Clear the viewport, apply the active camera, apply the lights, then draw
// every visible OBJECT node followed by every visible SPRITE node, in scene
// order. This is the body of the autonomous loop's frame, and it lives here
// -- portable, with no resource table of its own -- so that tools/hostsim
// renders a class-1 frame through *this* code rather than through a
// re-implementation of it. Stage 17: a demo verified on the host is only
// evidence about the bench if the node loop is literally the same one.
//
// The two lookups are how this reaches tables it must not own: meshes and
// textures both live in gpu64_3d_class1.cpp on the firmware and in the host
// harness on a PC. A node whose mesh does not resolve is skipped, not a
// failure -- one stale id must not blank an otherwise-good frame.
//
// Does NOT touch the framebuffer's page bookkeeping or any cache
// maintenance: the caller owns the target and what has to be flushed after.
typedef const Gpu64_3dMesh *( *Gpu64_3dMeshLookup )( void *pCtx, u16 nId );

// Occlusion culling for a loaded level (gpu64_level.h, gpu64_levelVisRow()).
// Called once per frame, after the camera is applied, with the camera's
// world position. A non-zero answer is a bit row over *pnNodes consecutive
// node ids from *pFirstId: an OBJECT node in that range whose bit is clear
// is not drawn -- provided its mesh is still *pFirstMesh + its offset in the
// range, i.e. it is still the level chunk the row was computed for and not
// something a program later created under a reused id. 0 culls nothing.
//
// Invisible in the picture by construction: the row is Quake's own PVS, and
// a chunk it clears is behind walls from anywhere in the camera's leaf. It
// changes only how long the frame takes.
typedef const u8 *( *Gpu64_3dVisLookup )( void *pCtx, const Gpu64_3dVec *pEye,
					   u16 *pFirstId, u16 *pnNodes,
					   u16 *pFirstMesh );

void gpu64_3dSceneRender( const Gpu64_3dScene *pScene,
			   Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   Gpu64_3dScratch *pScratch,
			   Gpu64_3dMeshLookup pMeshLookup, void *pMeshCtx,
			   Gpu64_3dTextureLookup pTexLookup, void *pTexCtx,
			   Gpu64_3dVisLookup pVisLookup = 0, void *pVisCtx = 0 );

#endif

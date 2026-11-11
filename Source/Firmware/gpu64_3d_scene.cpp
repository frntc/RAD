// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6, stage 14 -- the retained scene graph, as built.

 Portable: compiled unchanged by the firmware and by tools/hostsim. See
 gpu64_3d_scene.h for what is deliberately not here (mesh resolution).
*/
#include "gpu64_3d_scene.h"
#include "gpu64_3d_span.h"		// GPU64_3D_YIELD
#include <circle/util.h>

static void rebuildRot( Gpu64_3dNode *pN )
{
	gpu64_3dMatFromEuler( &pN->rot, pN->yaw, pN->pitch, pN->roll );
}

static void nodeDefaults( Gpu64_3dNode *pN, u16 nId, u8 nType )
{
	memset( pN, 0, sizeof( *pN ) );
	pN->id      = nId;
	pN->type    = nType;
	pN->visible = TRUE;			// a fresh node is drawn, not hidden --
						// same "visible over hidden" default the
						// colormap and the light use.
	pN->scale   = GPU64_FX8_ONE;		// 1.0
	gpu64_3dMatIdentity( &pN->rot );
}

void gpu64_3dSceneReset( Gpu64_3dScene *pScene )
{
	memset( pScene, 0, sizeof( *pScene ) );
}

Gpu64_3dNode *gpu64_3dSceneFind( Gpu64_3dScene *pScene, u16 nId, u8 nType )
{
	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
		if ( pScene->node[ i ].type != GPU64_3D_NODE_NONE && pScene->node[ i ].id == nId )
			return ( nType == GPU64_3D_NODE_NONE || pScene->node[ i ].type == nType )
				? &pScene->node[ i ] : 0;
	return 0;
}

// Finds the slot for an ID, reusing it if the ID is already live -- same
// convention resSlot() in gpu64_3d_class1.cpp uses for resources: re-creating
// over a live ID replaces it rather than erroring, so a program that gets its
// own bookkeeping wrong at least gets a well-defined result instead of a
// second node nothing ever addresses again.
static Gpu64_3dNode *nodeSlot( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pFree = 0;

	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
	{
		if ( pScene->node[ i ].type != GPU64_3D_NODE_NONE && pScene->node[ i ].id == nId )
			return &pScene->node[ i ];
		if ( pScene->node[ i ].type == GPU64_3D_NODE_NONE && !pFree )
			pFree = &pScene->node[ i ];
	}

	return pFree;
}

// --- lifecycle ------------------------------------------------------------

u8 gpu64_3dSceneCreateObject( Gpu64_3dScene *pScene, u16 nId, u16 nMeshId )
{
	Gpu64_3dNode *pN = nodeSlot( pScene, nId );
	if ( pN == 0 )
		return GPU64_3D_OUT_OF_MEMORY;		// the node table, not the arena

	nodeDefaults( pN, nId, GPU64_3D_NODE_OBJECT );
	pN->meshId = nMeshId;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneCreateCamera( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pN = nodeSlot( pScene, nId );
	if ( pN == 0 )
		return GPU64_3D_OUT_OF_MEMORY;

	nodeDefaults( pN, nId, GPU64_3D_NODE_CAMERA );
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneCreateSprite( Gpu64_3dScene *pScene, u16 nId, u16 nTexId )
{
	Gpu64_3dNode *pN = nodeSlot( pScene, nId );
	if ( pN == 0 )
		return GPU64_3D_OUT_OF_MEMORY;

	nodeDefaults( pN, nId, GPU64_3D_NODE_SPRITE );
	pN->texId = nTexId;

	// One world unit square, masked, screen-upright: a sprite created and
	// never given a size is still a sprite you can see, which is what makes
	// SET_SPRITE optional rather than a second half of CREATE_SPRITE.
	pN->spriteW = GPU64_FX8_ONE;
	pN->spriteH = GPU64_FX8_ONE;
	pN->spriteFlags = 0;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneCreateLight( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pN = nodeSlot( pScene, nId );
	if ( pN == 0 )
		return GPU64_3D_OUT_OF_MEMORY;

	// Strength and radius are left at zero by nodeDefaults(), i.e. the light
	// exists and is off. It lights nothing until SET_POINT_LIGHT gives it
	// both, and gpu64_3dSceneApplyLights() will not spend a slot on it in
	// the meantime.
	nodeDefaults( pN, nId, GPU64_3D_NODE_LIGHT );
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneDestroyNode( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	// A destroyed active camera must not linger as a dangling ID: if the
	// slot is later reused by CREATE_OBJECT for an unrelated node,
	// ApplyCamera must not start treating it as a camera it was never
	// told about.
	if ( pScene->bHaveActiveCamera && pScene->activeCameraId == nId )
		pScene->bHaveActiveCamera = FALSE;

	pN->type = GPU64_3D_NODE_NONE;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetActiveCamera( Gpu64_3dScene *pScene, u16 nId )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_CAMERA );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pScene->activeCameraId    = nId;
	pScene->bHaveActiveCamera = TRUE;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetVisible( Gpu64_3dScene *pScene, u16 nId, boolean bVisible )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->visible = bVisible ? TRUE : FALSE;
	return GPU64_3D_OK;
}

// --- transforms -------------------------------------------------------------

u8 gpu64_3dSceneSetPosition( Gpu64_3dScene *pScene, u16 nId, const Gpu64_3dVec *pPos )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->pos = *pPos;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetOrientation( Gpu64_3dScene *pScene, u16 nId, u16 nYaw, u16 nPitch, u16 nRoll )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->yaw = nYaw; pN->pitch = nPitch; pN->roll = nRoll;
	rebuildRot( pN );
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneMoveLocal( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	// "Forward 1.5" has to mean forward for *this* node, whatever it is
	// currently facing -- the delta is in the node's own axes, so it is
	// rotated by the node's current orientation before being added.
	Gpu64_3dVec delta, rotated;
	delta.x = dx; delta.y = dy; delta.z = dz;
	gpu64_3dVecRotate( &rotated, &pN->rot, &delta );

	pN->pos.x += rotated.x;
	pN->pos.y += rotated.y;
	pN->pos.z += rotated.z;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneMoveWorld( Gpu64_3dScene *pScene, u16 nId, s32 dx, s32 dy, s32 dz )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->pos.x += dx;
	pN->pos.y += dy;
	pN->pos.z += dz;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneRotateLocal( Gpu64_3dScene *pScene, u16 nId, u16 dYaw, u16 dPitch, u16 dRoll )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	// Plain wrapping addition on each angle independently -- see the "why"
	// in gpu64_3d_scene.h. u16 arithmetic wraps mod 65536 for free, which
	// is exactly the design doc's "add a turn rate and let it wrap".
	pN->yaw   = (u16)( pN->yaw   + dYaw );
	pN->pitch = (u16)( pN->pitch + dPitch );
	pN->roll  = (u16)( pN->roll  + dRoll );
	rebuildRot( pN );
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetScale( Gpu64_3dScene *pScene, u16 nId, u16 nScale )
{
	if ( nScale == 0 )
		return GPU64_3D_BAD_ARGS;	// same rejection DRAW_MESH already
						// makes at draw time; catching it
						// here fails at the opcode that
						// caused it, not three calls later.

	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->scale = nScale;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneGetTransform( Gpu64_3dScene *pScene, u16 nId,
			       Gpu64_3dVec *pPos, u16 *pYaw, u16 *pPitch, u16 *pRoll )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_NONE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	*pPos = pN->pos;
	*pYaw = pN->yaw; *pPitch = pN->pitch; *pRoll = pN->roll;
	return GPU64_3D_OK;
}

// --- per-type properties ------------------------------------------------------

u8 gpu64_3dSceneSetSprite( Gpu64_3dScene *pScene, u16 nId, u16 nW, u16 nH, u8 nFlags )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_SPRITE );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->spriteW = nW;
	pN->spriteH = nH;
	pN->spriteFlags = nFlags;
	return GPU64_3D_OK;
}

u8 gpu64_3dSceneSetPointLight( Gpu64_3dScene *pScene, u16 nId, u8 nStrength, u16 nRadius )
{
	Gpu64_3dNode *pN = gpu64_3dSceneFind( pScene, nId, GPU64_3D_NODE_LIGHT );
	if ( pN == 0 )
		return GPU64_3D_BAD_ID;

	pN->lightStrength = nStrength;
	pN->lightRadius = nRadius;
	return GPU64_3D_OK;
}

// --- the active camera ------------------------------------------------------

void gpu64_3dSceneApplyCamera( const Gpu64_3dScene *pScene, Gpu64_3dState *pState )
{
	const Gpu64_3dNode *pCam = 0;
	if ( pScene->bHaveActiveCamera )
	{
		// Not gpu64_3dSceneFind(): that takes a non-const scene, and this
		// path must not mutate one. The scan is the same either way.
		for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
			if ( pScene->node[ i ].type == GPU64_3D_NODE_CAMERA &&
			     pScene->node[ i ].id == pScene->activeCameraId )
			{
				pCam = &pScene->node[ i ];
				break;
			}
	}

	if ( pCam == 0 )
	{
		// No active camera: draw in world space, exactly as DRAW_MESH
		// always has. Deterministic -- never leaves whatever the state
		// carried from a previous call.
		gpu64_3dMatIdentity( &pState->viewRot );
		pState->viewPos.x = pState->viewPos.y = pState->viewPos.z = 0;
		pState->bHaveCamera = FALSE;
		return;
	}

	// View = rotate by the inverse of the camera's rotation (its
	// transpose, since it is a pure rotation), after subtracting the
	// camera's position. tools/hostsim/hostsim.cpp's setCamera() has done
	// exactly this by hand since before CREATE_CAMERA existed in
	// firmware, which is what proved it was enough.
	gpu64_3dMatTranspose( &pState->viewRot, &pCam->rot );
	pState->viewPos = pCam->pos;
	pState->bHaveCamera = TRUE;
}

// --- the point lights ---------------------------------------------------------

void gpu64_3dSceneApplyLights( const Gpu64_3dScene *pScene, Gpu64_3dState *pState )
{
	unsigned n = 0;

	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES && n < GPU64_3D_MAX_LIGHTS; i++ )
	{
		const Gpu64_3dNode *pN = &pScene->node[ i ];

		if ( pN->type != GPU64_3D_NODE_LIGHT || !pN->visible )
			continue;
		if ( pN->lightRadius == 0 || pN->lightStrength == 0 )
			continue;

		// Into view space, once, here -- the reason Gpu64_3dPointLight is
		// documented as view space and not world space. gpu64_3dDrawMesh()
		// fuses the view and model rotations into one matrix, so there is no
		// world-space vertex anywhere downstream for a world-space light to
		// be compared against.
		// A view-space light (milestone 20's muzzle flash) is there already.
		Gpu64_3dVec rel, v;
		if ( pN->viewSpace )
			v = pN->pos;
		else
		{
			rel.x = pN->pos.x - pState->viewPos.x;
			rel.y = pN->pos.y - pState->viewPos.y;
			rel.z = pN->pos.z - pState->viewPos.z;
			gpu64_3dVecRotate( &v, &pState->viewRot, &rel );
		}

		Gpu64_3dPointLight *pL = &pState->lights[ n ];
		pL->x = v.x >> 8;			// 16.16 -> 8.8
		pL->y = v.y >> 8;
		pL->z = v.z >> 8;
		pL->r2 = (s64)pN->lightRadius * (s64)pN->lightRadius;

		// ( strength << SHIFT ) / r2, so fall * ( r2 - d2 ) >> SHIFT is the
		// strength at the centre and zero at the radius. Class 2's
		// gpu64_rasterSetLight() to the letter.
		pL->fall = ( (s64)pN->lightStrength << GPU64_3D_LIGHT_SHIFT ) / pL->r2;
		n++;
	}

	pState->lightMask = (u8)( ( 1u << n ) - 1 );

	for ( unsigned i = n; i < GPU64_3D_MAX_LIGHTS; i++ )
	{
		pState->lights[ i ].r2 = 0;
		pState->lights[ i ].fall = 0;
	}
}

// --- one whole frame -------------------------------------------------------
// Contract in gpu64_3d_scene.h. Extracted verbatim from stage 16's
// gpu64_3dExecuteRenderScene() so the firmware and tools/hostsim run the
// same node loop rather than two that agree by inspection.
// Milestone 20: the nodes whose pose is relative to the eye -- the gun, the
// status bar. Drawn last, over a z-buffer cleared for them, so nothing in the
// world can cut into them however close the player stands to a wall; among
// themselves they still depth-test, which is what lets the bar's digits sit
// on its background tiles. The view transform is swapped for the identity
// for the length of the pass and put back, rather than copying the state:
// Gpu64_3dState carries a 4 KB colormap, and a copy of it would be a store
// burst of its own on core 1. The point lights stay as they are -- they are
// already in view space, which is exactly the space these nodes are in.
//
// The near plane is the state's own, so a view model's parked muzzle flash,
// which sits behind the eye on its idle frames, is clipped away rather than
// projected through the camera.
static void renderViewSpace( const Gpu64_3dScene *pScene,
			     Gpu64_3dState *pState,
			     Gpu64_3dTarget *pTarget,
			     Gpu64_3dScratch *pScratch,
			     Gpu64_3dMeshLookup pMeshLookup, void *pMeshCtx,
			     Gpu64_3dTextureLookup pTexLookup, void *pTexCtx )
{
	unsigned i;
	for ( i = 0; i < GPU64_3D_MAX_NODES; i++ )
	{
		const Gpu64_3dNode *pN = &pScene->node[ i ];
		if ( pN->viewSpace && pN->visible &&
		     ( pN->type == GPU64_3D_NODE_OBJECT || pN->type == GPU64_3D_NODE_SPRITE ) )
			break;
	}
	if ( i == GPU64_3D_MAX_NODES )
		return;				// the common case costs one scan

	gpu64_3dClearDepth( pState, pTarget );

	const Gpu64_3dMat viewRot = pState->viewRot;
	const Gpu64_3dVec viewPos = pState->viewPos;
	gpu64_3dMatIdentity( &pState->viewRot );
	pState->viewPos.x = pState->viewPos.y = pState->viewPos.z = 0;

	for ( ; i < GPU64_3D_MAX_NODES; i++ )
	{
		const Gpu64_3dNode *pN = &pScene->node[ i ];
		if ( pN->type != GPU64_3D_NODE_OBJECT || !pN->visible || !pN->viewSpace )
			continue;

		const Gpu64_3dMesh *pMesh =
			pMeshLookup ? pMeshLookup( pMeshCtx, pN->meshId ) : 0;
		if ( pMesh == 0 )
			continue;

		gpu64_3dDrawMesh( pState, pTarget, pScratch, pMesh,
				   &pN->pos, &pN->rot, pN->scale,
				   pTexLookup, pTexCtx );
		GPU64_3D_YIELD();
	}

	for ( i = 0; i < GPU64_3D_MAX_NODES; i++ )
	{
		const Gpu64_3dNode *pN = &pScene->node[ i ];
		if ( pN->type != GPU64_3D_NODE_SPRITE || !pN->visible || !pN->viewSpace )
			continue;

		gpu64_3dDrawSprite( pState, pTarget, &pN->pos,
				     pN->spriteW, pN->spriteH, pN->texId,
				     pN->yaw, pN->spriteFlags,
				     pTexLookup, pTexCtx );
		GPU64_3D_YIELD();
	}

	pState->viewRot = viewRot;
	pState->viewPos = viewPos;
}

void gpu64_3dSceneRender( const Gpu64_3dScene *pScene,
			   Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   Gpu64_3dScratch *pScratch,
			   Gpu64_3dMeshLookup pMeshLookup, void *pMeshCtx,
			   Gpu64_3dTextureLookup pTexLookup, void *pTexCtx,
			   Gpu64_3dVisLookup pVisLookup, void *pVisCtx )
{
	gpu64_3dClearViewport( pState, pTarget );

	// Fresh every frame, same rule DRAW_NODE's own comment gives: a program
	// that moves the camera between two commits must see the next frame
	// rendered from where the camera is now.
	gpu64_3dSceneApplyCamera( pScene, pState );

	// After the camera, never before: the light slots this fills are in view
	// space, so they are only meaningful against the view matrix the camera
	// just set. Once per frame, not once per node.
	gpu64_3dSceneApplyLights( pScene, pState );

	// After the camera too: the row is for the leaf the camera is in now.
	u16 nVisFirst = 0, nVisCount = 0, nVisMesh = 0;
	const u8 *pVisRow = pVisLookup
		? pVisLookup( pVisCtx, &pState->viewPos, &nVisFirst, &nVisCount, &nVisMesh )
		: 0;

	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
	{
		const Gpu64_3dNode *pN = &pScene->node[ i ];
		if ( pN->type != GPU64_3D_NODE_OBJECT || !pN->visible || pN->viewSpace )
			continue;

		if ( pVisRow != 0 )
		{
			const u16 k = (u16)( pN->id - nVisFirst );	// wraps below the range
			if ( k < nVisCount
			     && pN->meshId == (u16)( nVisMesh + k )
			     && ( pVisRow[ k >> 3 ] & ( 1 << ( k & 7 ) ) ) == 0 )
				continue;
		}

		const Gpu64_3dMesh *pMesh =
			pMeshLookup ? pMeshLookup( pMeshCtx, pN->meshId ) : 0;
		if ( pMesh == 0 )
			continue;

		gpu64_3dDrawMesh( pState, pTarget, pScratch, pMesh,
				   &pN->pos, &pN->rot, pN->scale,
				   pTexLookup, pTexCtx );

		// Store-burst discipline (CLAUDE.md multicore rules):
		// gpu64_3d_span.h's own yield covers the rasteriser's inner loop,
		// but a scene of many small meshes can otherwise chain draw after
		// draw with no yield between them at all. One here per node closes
		// that gap.
		GPU64_3D_YIELD();
	}

	// Sprites after the objects, in scene order. They are depth-tested like
	// everything else, so this is not a painter's-algorithm ordering
	// requirement -- it is only that a sprite is cheap and a mesh is not, so
	// drawing the geometry first gives the sprite's z-test the most to reject
	// against.
	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
	{
		const Gpu64_3dNode *pN = &pScene->node[ i ];
		if ( pN->type != GPU64_3D_NODE_SPRITE || !pN->visible || pN->viewSpace )
			continue;

		gpu64_3dDrawSprite( pState, pTarget, &pN->pos,
				     pN->spriteW, pN->spriteH, pN->texId,
				     pN->yaw, pN->spriteFlags,
				     pTexLookup, pTexCtx );

		GPU64_3D_YIELD();
	}

	renderViewSpace( pScene, pState, pTarget, pScratch,
			 pMeshLookup, pMeshCtx, pTexLookup, pTexCtx );
}

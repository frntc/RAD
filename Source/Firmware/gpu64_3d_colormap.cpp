/*
 gpu64 milestone 6 -- BUILD_COLORMAP, and the render state's defaults.

 The mechanism is Doom's: the palette stays 256 freely-chosen colours and
 lighting goes through a generated table, so textures can use every entry and
 no ramp layout is imposed on the artist. Rationale in
 dev/milestone6_3d_design.md, "Lighting: a generated colormap".

 Portable: compiled unchanged by the firmware and by tools/hostsim.
*/
#include "gpu64_3d_render.h"

void gpu64_3dStateDefaults( Gpu64_3dState *pState )
{
	pState->vpX = 0;
	pState->vpY = 0;
	pState->vpW = GPU64_3D_SURFACE_W;
	pState->vpH = GPU64_3D_SURFACE_H;

	pState->fov = 10923;				// 60 degrees, as a binary angle
	pState->focal = gpu64_3dFocalFromFov( pState->fov, pState->vpW );
	pState->nearZ = GPU64_FX16_ONE;			// 1.0 model unit
	pState->farZ  = 256 * GPU64_FX16_ONE;

	// Pointing back out of the screen towards the viewer, so an unrotated
	// face aimed at the camera is fully lit. A scene that sets no light at
	// all is then visible, which matters more during bring-up than realism.
	pState->lightDir[ 0 ] = 0;
	pState->lightDir[ 1 ] = 0;
	pState->lightDir[ 2 ] = -( GPU64_FX15_ONE - 1 );
	pState->ambient = 4;

	pState->background = 0;

	// A colormap that has never been built is the identity at every level:
	// unlit, but visible. The alternative -- refusing to draw until
	// BUILD_COLORMAP has run -- turns a forgotten setup call into a blank
	// screen, which is the least informative failure this layer can produce.
	for ( unsigned l = 0; l < GPU64_3D_LIGHT_LEVELS; l++ )
		for ( unsigned i = 0; i < 256; i++ )
			pState->colormap[ l * 256 + i ] = (u8)i;
	pState->bColormapValid = FALSE;

	// No point lights. Set here field by field rather than left to a memset
	// this function has never done: every other field above is explicit, and
	// a light that survived a session reset would light the next program's
	// scene from a position its own scene graph has no node for.
	for ( unsigned i = 0; i < GPU64_3D_MAX_LIGHTS; i++ )
	{
		pState->lights[ i ].x = 0;
		pState->lights[ i ].y = 0;
		pState->lights[ i ].z = 0;
		pState->lights[ i ].r2 = 0;
		pState->lights[ i ].fall = 0;
	}
	pState->lightMask = 0;

	gpu64_3dMatIdentity( &pState->viewRot );
	pState->viewPos.x = pState->viewPos.y = pState->viewPos.z = 0;
	pState->bHaveCamera = FALSE;
}

void gpu64_3dBuildColormap( Gpu64_3dState *pState, const u8 *pPaletteRGB )
{
	for ( unsigned level = 0; level < GPU64_3D_LIGHT_LEVELS; level++ )
	{
		// Level 15 is full brightness and resolves to the identity, since the
		// exact colour is always its own nearest match. Level 0 is 1/15 of
		// the way up, not black: a face turned fully away still reads as its
		// own colour rather than as a hole in the geometry.
		const u32 num = level;
		const u32 den = GPU64_3D_LIGHT_LEVELS - 1;

		for ( unsigned i = 0; i < 256; i++ )
		{
			const int r = (int)( (u32)pPaletteRGB[ i * 3 + 0 ] * num / den );
			const int g = (int)( (u32)pPaletteRGB[ i * 3 + 1 ] * num / den );
			const int b = (int)( (u32)pPaletteRGB[ i * 3 + 2 ] * num / den );

			unsigned best = 0;
			u32 bestDist = 0xFFFFFFFF;

			for ( unsigned j = 0; j < 256; j++ )
			{
				const int dr = r - (int)pPaletteRGB[ j * 3 + 0 ];
				const int dg = g - (int)pPaletteRGB[ j * 3 + 1 ];
				const int db = b - (int)pPaletteRGB[ j * 3 + 2 ];

				// Plain squared euclidean distance in RGB. Weighting the
				// channels by luminance was considered and left alone: it
				// biases towards preserving brightness over hue, which is the
				// wrong trade when the whole point of a level is that it *is*
				// darker.
				const u32 dist = (u32)( dr * dr + dg * dg + db * db );
				if ( dist < bestDist )
				{
					bestDist = dist;
					best = j;
					if ( dist == 0 )
						break;
				}
			}

			pState->colormap[ level * 256 + i ] = (u8)best;
		}
	}

	pState->bColormapValid = TRUE;
}

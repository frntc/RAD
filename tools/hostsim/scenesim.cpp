// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 class-1 scene sim -- renders the frame stream tools/prgsim emits, with
 the firmware's own renderer.

 The split, and why it is this way round: tools/prgsim runs the demo's actual
 6502 code against a reference model of the command API, so what reaches this
 program is the scene the C64 really built, command by command, including
 every BUSY and every dropped-frame decision the handshake protocol made. But
 the model deliberately does NOT rasterise class 1 -- a second perspective/
 texture/lighting implementation in Python would be one more thing to be
 wrong, not an oracle. So the picture comes from here, where
 gpu64_3dSceneRender() is the same function gpu64_3dExecuteRenderScene()
 calls on core 1. A frame verified here is evidence about the bench because
 the node loop, the projection and the rasteriser are literally the firmware's.

 What is still NOT covered, and has to be found at the bench: bus timing,
 core-1 yielding, cache maintenance, and the page the display is actually
 scanning out. This program says what the frame should look like, not that
 the hardware can produce it in time.

 Usage: ./scenesim <stream.txt> [outdir] [--ppm-every=N] [--ppm-frame=N]...
                   [--time]

 --time adds each frame's render time on THIS machine to its line, plus a
 summary. It is a relative measure only -- an x86 core is not the Pi's A53 --
 so compare two streams rendered on the same PC (a new scene against one the
 bench already ran at a known rate), never a number against a frame period.
*/
#include "gpu64_3d_render.h"
#include "gpu64_3d_scene.h"
#include "gpu64_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_RES		512
#define MAX_PPM_PICKS	64

static void *hostAlloc( void *, u32 nBytes )
{
	return malloc( nBytes );
}

static u8  g_Pixels[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ];
static u16 g_Depth[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ];
static u8  g_Palette[ 256 * 3 ];

// --- the resource table -------------------------------------------------
// Same shape as gpu64_3d_class1.cpp's s_Res[], for the same reason: the
// portable renderer asks for a mesh or a texture by ID and must not know
// where they live.

struct Res
{
	u16			id;
	u8			type;		// 0 none, 1 mesh, 2 texture
	Gpu64_3dMesh		mesh;
	Gpu64_3dTexture		tex;
};

static Res g_Res[ MAX_RES ];

static Res *resSlot( u16 nId )
{
	Res *pFree = 0;
	for ( unsigned i = 0; i < MAX_RES; i++ )
	{
		if ( g_Res[ i ].type != 0 && g_Res[ i ].id == nId )
			return &g_Res[ i ];
		if ( g_Res[ i ].type == 0 && !pFree )
			pFree = &g_Res[ i ];
	}
	return pFree;
}

static Res *resFind( u16 nId, u8 nType )
{
	for ( unsigned i = 0; i < MAX_RES; i++ )
		if ( g_Res[ i ].type != 0 && g_Res[ i ].id == nId )
			return g_Res[ i ].type == nType ? &g_Res[ i ] : 0;
	return 0;
}

static const Gpu64_3dMesh *lookupMesh( void *, u16 nId )
{
	Res *pR = resFind( nId, 1 );
	return pR ? &pR->mesh : 0;
}

static const Gpu64_3dTexture *lookupTexture( void *, u16 nId )
{
	Res *pR = resFind( nId, 2 );
	return pR ? &pR->tex : 0;
}

// --- files beside the stream --------------------------------------------

static char g_Dir[ 512 ];

static u8 *readFile( const char *pName, u32 *pLen )
{
	char path[ 1024 ];
	snprintf( path, sizeof( path ), "%s/%s", g_Dir, pName );
	FILE *f = fopen( path, "rb" );
	if ( !f )
	{
		fprintf( stderr, "scenesim: cannot open %s\n", path );
		return 0;
	}
	fseek( f, 0, SEEK_END );
	long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	u8 *p = (u8 *)malloc( n ? n : 1 );
	if ( fread( p, 1, n, f ) != (size_t)n )
	{
		fprintf( stderr, "scenesim: short read on %s\n", path );
		fclose( f );
		free( p );
		return 0;
	}
	fclose( f );
	*pLen = (u32)n;
	return p;
}

static void writePPM( const char *pPath )
{
	FILE *f = fopen( pPath, "wb" );
	if ( !f )
	{
		fprintf( stderr, "scenesim: cannot write %s\n", pPath );
		return;
	}
	fprintf( f, "P6\n%d %d\n255\n", GPU64_3D_SURFACE_W, GPU64_3D_SURFACE_H );
	for ( unsigned i = 0; i < GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H; i++ )
		fwrite( &g_Palette[ g_Pixels[ i ] * 3 ], 1, 3, f );
	fclose( f );
}

static u32 checksum( void )
{
	u32 h = 2166136261u;
	for ( unsigned i = 0; i < sizeof( g_Pixels ); i++ )
	{
		h ^= g_Pixels[ i ];
		h *= 16777619u;
	}
	return h;
}

// --- the state the stream carries ---------------------------------------
//
// SET_VIEWPORT/SET_PERSPECTIVE/SET_LIGHT are re-applied here rather than
// having their results shipped in the stream, so that the derived quantities
// -- focal length and the normalised light direction -- come out of the same
// firmware functions the Pi runs. Only the raw opcode arguments cross.

static Gpu64_3dState  g_State;
static Gpu64_3dScene  g_Scene;
static Gpu64_3dScratch g_Scratch;

// --- level visibility --------------------------------------------------
// The LEVEL line prgsim emits when a LOAD_LEVEL finishes: the same file and
// the same two id bases gpu64_3d_class1.cpp's levelVisLookup() works from,
// through the same gpu64_levelVisRow(). A RESET ends it, as
// levelLoadAbort() does on the Pi.
static Gpu64_Level g_Level;
static u8 *g_pLevelFile;
static boolean g_bLevel;
static u16 g_LevelNodeBase, g_LevelMeshBase;
static unsigned long g_VisCulled;

static const u8 *visLookup( void *, const Gpu64_3dVec *pEye,
			    u16 *pFirstId, u16 *pnNodes, u16 *pFirstMesh )
{
	if ( !g_bLevel )
		return 0;
	Gpu64_LevelVec e;
	e.v[ 0 ] = pEye->x;
	e.v[ 1 ] = pEye->y;
	e.v[ 2 ] = pEye->z;
	const u8 *pRow = gpu64_levelVisRow( &g_Level, &e, pnNodes );
	if ( pRow == 0 )
		return 0;
	*pFirstId   = g_LevelNodeBase;
	*pFirstMesh = (u16)( g_LevelMeshBase + gpu64_levelRd16( g_Level.pNodeTab ) );
	for ( unsigned k = 0; k < *pnNodes; k++ )
		if ( !( pRow[ k >> 3 ] & ( 1 << ( k & 7 ) ) ) )
			g_VisCulled++;
	return pRow;
}

int main( int argc, char **argv )
{
	// The firmware's rad.cfg GPU64_BURST_BYTES, for proving a budget
	// changes only where the rasteriser yields, never a pixel.
	if ( const char *pBurst = getenv( "GPU64_BURST_BYTES" ) )
		gpu64_3dSetSpanBytes( (unsigned)atoi( pBurst ) );

	const char *pStream = 0;
	const char *pOut = "out";
	unsigned nPpmEvery = 0;
	unsigned ppmPick[ MAX_PPM_PICKS ];
	unsigned nPicks = 0;
	boolean bQuiet = FALSE;
	boolean bTime = FALSE;
	boolean bNoVis = FALSE;		// A/B: the picture must not change
	double tSum = 0, tMax = 0;

	for ( int i = 1; i < argc; i++ )
	{
		if ( !strncmp( argv[ i ], "--ppm-every=", 12 ) )
			nPpmEvery = (unsigned)atoi( argv[ i ] + 12 );
		else if ( !strncmp( argv[ i ], "--ppm-frame=", 12 ) )
		{
			if ( nPicks < MAX_PPM_PICKS )
				ppmPick[ nPicks++ ] = (unsigned)atoi( argv[ i ] + 12 );
		}
		else if ( !strcmp( argv[ i ], "--quiet" ) )
			bQuiet = TRUE;
		else if ( !strcmp( argv[ i ], "--time" ) )
			bTime = TRUE;
		else if ( !strcmp( argv[ i ], "--no-vis" ) )
			bNoVis = TRUE;
		else if ( !pStream )
			pStream = argv[ i ];
		else
			pOut = argv[ i ];
	}

	if ( !pStream )
	{
		fprintf( stderr, "usage: scenesim <stream.txt> [outdir]"
				  " [--ppm-every=N] [--ppm-frame=N] [--quiet]\n" );
		return 2;
	}

	// Blobs are named relative to the stream, which is where prgsim wrote
	// them.
	snprintf( g_Dir, sizeof( g_Dir ), "%s", pStream );
	char *pSlash = strrchr( g_Dir, '/' );
	if ( pSlash )
		*pSlash = 0;
	else
		strcpy( g_Dir, "." );

	FILE *f = fopen( pStream, "r" );
	if ( !f )
	{
		fprintf( stderr, "scenesim: cannot open %s\n", pStream );
		return 1;
	}

	gpu64_3dStateDefaults( &g_State );
	gpu64_3dSceneReset( &g_Scene );
	memset( g_Res, 0, sizeof( g_Res ) );
	memset( g_Palette, 0, sizeof( g_Palette ) );

	Gpu64_3dTarget target;
	target.pPixels = g_Pixels;
	target.pitch   = GPU64_3D_SURFACE_W;
	target.pDepth  = g_Depth;

	char line[ 1024 ];
	unsigned nFrames = 0, nWritten = 0;
	boolean bFail = FALSE;

	while ( fgets( line, sizeof( line ), f ) )
	{
		if ( line[ 0 ] == '#' || line[ 0 ] == '\n' )
			continue;

		char name[ 256 ], name2[ 256 ];
		unsigned id, a, b;
		int n[ 20 ];

		if ( sscanf( line, "PAL %255s", name ) == 1 )
		{
			u32 len;
			u8 *p = readFile( name, &len );
			if ( !p ) { bFail = TRUE; break; }
			memcpy( g_Palette, p, len < sizeof( g_Palette ) ? len : sizeof( g_Palette ) );
			free( p );
			continue;
		}

		if ( !strncmp( line, "COLORMAP", 8 ) )
		{
			gpu64_3dBuildColormap( &g_State, g_Palette );
			continue;
		}

		if ( !strncmp( line, "RESET", 5 ) )
		{
			// SCENE_RESET leaves uploaded resources alone -- see
			// docs/class1-3d-mesh-reference.md's Resource lifecycle.
			gpu64_3dStateDefaults( &g_State );
			gpu64_3dSceneReset( &g_Scene );
			g_bLevel = FALSE;
			continue;
		}

		if ( sscanf( line, "LEVEL %u %u %255s", &a, &b, name ) == 3 )
		{
			u32 len;
			free( g_pLevelFile );
			g_pLevelFile = readFile( name, &len );
			if ( !g_pLevelFile
			     || !gpu64_levelParse( &g_Level, g_pLevelFile, len ) )
			{
				fprintf( stderr, "scenesim: level %s does not parse\n", name );
				bFail = TRUE;
				break;
			}
			g_LevelNodeBase = (u16)a;
			g_LevelMeshBase = (u16)b;
			g_bLevel = !bNoVis;
			continue;
		}

		if ( sscanf( line, "MESH %u %255s %255s", &id, name, name2 ) == 3 )
		{
			u32 lv, lf;
			u8 *pV = readFile( name, &lv );
			u8 *pF = pV ? readFile( name2, &lf ) : 0;
			if ( !pV || !pF ) { bFail = TRUE; break; }
			Res *pR = resSlot( (u16)id );
			if ( !pR || gpu64_3dBuildMesh( &pR->mesh, pV, lv, pF, lf,
						        hostAlloc, 0 ) != GPU64_3D_OK )
			{
				fprintf( stderr, "scenesim: mesh %u failed to build\n", id );
				bFail = TRUE;
				break;
			}
			pR->id = (u16)id;
			pR->type = 1;
			free( pV );
			free( pF );
			continue;
		}

		if ( sscanf( line, "TEX %u %255s %u %u", &id, name, &a, &b ) == 4 )
		{
			u32 len;
			u8 *p = readFile( name, &len );
			if ( !p ) { bFail = TRUE; break; }
			Res *pR = resSlot( (u16)id );
			if ( !pR || gpu64_3dBuildTexture( &pR->tex, p, len, (u8)a, (u8)b,
							   hostAlloc, 0 ) != GPU64_3D_OK )
			{
				fprintf( stderr, "scenesim: texture %u failed to build\n", id );
				bFail = TRUE;
				break;
			}
			pR->id = (u16)id;
			pR->type = 2;
			free( p );
			continue;
		}

		if ( sscanf( line, "FREE %u", &id ) == 1 )
		{
			Res *pR = resFind( (u16)id, 1 );
			if ( !pR ) pR = resFind( (u16)id, 2 );
			if ( pR ) pR->type = 0;
			continue;
		}

		if ( sscanf( line, "ST vp %d %d %d %d", &n[0], &n[1], &n[2], &n[3] ) == 4 )
		{
			g_State.vpX = (u16)n[0];
			g_State.vpY = (u16)n[1];
			g_State.vpW = (u16)n[2];
			g_State.vpH = (u16)n[3];
			continue;
		}

		if ( sscanf( line, "ST persp %d %d %d", &n[0], &n[1], &n[2] ) == 3 )
		{
			g_State.fov   = (u16)n[0];
			g_State.nearZ = n[1];
			g_State.farZ  = n[2];
			// Derived, not shipped: same call SET_VIEWPORT and
			// SET_PERSPECTIVE both make on the Pi.
			g_State.focal = gpu64_3dFocalFromFov( g_State.fov, g_State.vpW );
			continue;
		}

		if ( sscanf( line, "ST light %d %d %d %d", &n[0], &n[1], &n[2], &n[3] ) == 4 )
		{
			gpu64_3dNormalise( g_State.lightDir, n[0], n[1], n[2] );
			g_State.ambient = (u8)n[3];
			continue;
		}

		if ( sscanf( line, "ST bg %d %d", &n[0], &n[1] ) == 2 )
		{
			g_State.background = (u8)n[0];
			g_State.bColormapValid = n[1] ? TRUE : FALSE;
			continue;
		}

		if ( sscanf( line, "CAM %d", &n[0] ) == 1 )
		{
			g_Scene.bHaveActiveCamera = n[0] >= 0 ? TRUE : FALSE;
			g_Scene.activeCameraId = (u16)( n[0] >= 0 ? n[0] : 0 );
			continue;
		}

		if ( !strncmp( line, "FRAME ", 6 ) )
		{
			// Each frame carries its whole scene, so the node table
			// starts empty: a node destroyed between two commits must
			// not survive here as a slot nothing overwrites.
			for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
				g_Scene.node[ i ].type = GPU64_3D_NODE_NONE;
			continue;
		}

		// The 19th field, viewspace (milestone 20), is optional so a
		// stream written before it still reads.
		int nFields;
		n[18] = 0;
		if ( ( nFields = sscanf( line, "N %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d",
			      &n[0], &n[1], &n[2], &n[3], &n[4], &n[5], &n[6], &n[7],
			      &n[8], &n[9], &n[10], &n[11], &n[12], &n[13], &n[14],
			      &n[15], &n[16], &n[17], &n[18] ) ) >= 18 )
		{
			if ( n[0] < 0 || n[0] >= (int)GPU64_3D_MAX_NODES )
				continue;
			Gpu64_3dNode *pN = &g_Scene.node[ n[0] ];
			memset( pN, 0, sizeof( *pN ) );
			// n[1] is the node's own id, which is what the scene
			// looks nodes up by -- CAM names a camera by id, not by
			// slot. Dropping it here made every node id 0, so no
			// camera ever resolved and the room rendered from the
			// origin in world space.
			pN->id      = (u16)n[1];
			pN->type    = (u8)n[2];
			pN->visible = n[3] ? TRUE : FALSE;
			pN->pos.x   = n[4];
			pN->pos.y   = n[5];
			pN->pos.z   = n[6];
			pN->yaw     = (u16)n[7];
			pN->pitch   = (u16)n[8];
			pN->roll    = (u16)n[9];
			pN->scale   = (u16)n[10];
			pN->meshId  = (u16)n[11];
			pN->texId   = (u16)n[12];
			pN->spriteW = (u16)n[13];
			pN->spriteH = (u16)n[14];
			pN->spriteFlags   = (u8)n[15];
			pN->lightStrength = (u8)n[16];
			pN->lightRadius   = (u16)n[17];
			pN->viewSpace     = n[18] ? 1 : 0;
			// rot is derived from the angles, exactly as
			// gpu64_3dSceneSetOrientation() does it -- the stream
			// carries angles, never a matrix.
			gpu64_3dMatFromEuler( &pN->rot, pN->yaw, pN->pitch, pN->roll );
			continue;
		}

		if ( !strncmp( line, "ENDFRAME", 8 ) )
		{
			nFrames++;
			struct timespec t0, t1;
			const unsigned long nYield0 = gpu64_3dYieldCount;
			clock_gettime( CLOCK_MONOTONIC, &t0 );
			gpu64_3dSceneRender( &g_Scene, &g_State, &target, &g_Scratch,
					      lookupMesh, 0, lookupTexture, 0,
					      visLookup, 0 );
			clock_gettime( CLOCK_MONOTONIC, &t1 );
			double us = ( t1.tv_sec - t0.tv_sec ) * 1e6
				  + ( t1.tv_nsec - t0.tv_nsec ) / 1e3;
			tSum += us;
			if ( us > tMax )
				tMax = us;

			boolean bWrite = nPpmEvery && ( nFrames % nPpmEvery ) == 0;
			for ( unsigned i = 0; i < nPicks; i++ )
				if ( ppmPick[ i ] == nFrames )
					bWrite = TRUE;
			if ( bWrite )
			{
				char path[ 1024 ];
				snprintf( path, sizeof( path ), "%s/frame%04u.ppm", pOut, nFrames );
				writePPM( path );
				nWritten++;
			}

			// One line per frame, not a total: a defect that starts
			// at frame 98 shows up as a checksum that stops changing,
			// or starts changing wildly, and a sum cannot show that.
			if ( !bQuiet )
			{
				unsigned ink = 0;
				for ( unsigned i = 0; i < sizeof( g_Pixels ); i++ )
					if ( g_Pixels[ i ] != g_State.background )
						ink++;
				if ( bTime )
					printf( "frame %4u  ink %6u  checksum %08x  render %7.0f us  bursts %5lu\n",
						nFrames, ink, checksum(), us,
						gpu64_3dYieldCount - nYield0 );
				else
					printf( "frame %4u  ink %6u  checksum %08x\n",
						nFrames, ink, checksum() );
			}
			continue;
		}
	}

	fclose( f );

	if ( bFail )
		return 1;

#ifdef GPU64_PROJ_AUDIT
	{
		extern long long g_projOver, g_projTotal, g_projWorst;
		printf( "scenesim: projected %lld verts, %lld overflowed s32, worst |coord| %lld px\n",
			g_projTotal, g_projOver, g_projWorst >> 16 );
	}
#endif
	if ( bTime && nFrames )
		printf( "scenesim: render mean %.0f us, max %.0f us (this machine)\n",
			tSum / nFrames, tMax );
	if ( nFrames )
		printf( "scenesim: level vis culled %.1f world chunks/frame%s\n",
			(double)g_VisCulled / nFrames, bNoVis ? " (--no-vis)" : "" );
	printf( "scenesim: %u frames, %u PPMs in %s/\n", nFrames, nWritten, pOut );
	return nFrames ? 0 : 1;
}

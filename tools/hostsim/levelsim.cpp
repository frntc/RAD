// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 level sim -- renders a .g64lev level file with the firmware's own
 renderer, on a PC.

 Why this exists, and how it differs from the two sims beside it: hostsim
 drives the immediate-mode path, scenesim replays the command stream a demo's
 real 6502 code produced. Neither can say whether tools/gen_quakelevel.py's
 output is a *level* -- a stream from a demo that does not exist yet cannot be
 replayed, and the converter's own round-trip check (tools/check_g64lev.py)
 only proves the file's shape, not that the geometry is a room you can stand
 in. This program closes that gap the cheapest way available: it does what
 the firmware's loader will do -- build every mesh and texture, create one
 OBJECT node per chunk, put a camera at info_player_start -- and renders
 through gpu64_3dSceneRender(), the same function core 1 runs.

 So a picture from here is evidence about the converter and about the scene
 graph's capacity, and evidence about nothing else. Bus timing, core-1
 yielding and the displayed page are still bench questions.

 Usage:
   ./levelsim <level.g64lev> [outdir] [--frames=N] [--turn] [--quiet]
              [--pos=x,y,z] [--yaw=DEG] [--pitch=DEG] [--fov=DEG]
*/
#include "gpu64_3d_render.h"
#include "gpu64_3d_scene.h"
#include "gpu64_level.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RES		512
#define MESH_ID_BASE	1000		// textures take 0..255; see build_mesh()
#define NODE_ID_BASE	100
#define CAMERA_ID	1

// --- the level file ------------------------------------------------------
// The layout tools/gen_quakelevel.py writes. Read field by field rather than
// by casting a struct over the buffer: the file is little-endian and packed,
// and the firmware loader will have to do it this way too.

#define LEVEL_MAGIC	0x4c343647u	// 'G64L'
#define LEVEL_VERSION	7

#define TEX_STRIDE	12
#define MESH_STRIDE	16
#define NODE_STRIDE	16
#define ENT_STRIDE	44
#define PLANE_STRIDE	12
#define HULL_STRIDE	36
#define CLIP_STRIDE	8

static u8 *g_Lev;
static u32 g_LevLen;

static u16 rd16( u32 o ) { return (u16)( g_Lev[ o ] | ( g_Lev[ o + 1 ] << 8 ) ); }
static u32 rd32( u32 o )
{
	return (u32)g_Lev[ o ] | ( (u32)g_Lev[ o + 1 ] << 8 )
	     | ( (u32)g_Lev[ o + 2 ] << 16 ) | ( (u32)g_Lev[ o + 3 ] << 24 );
}
static s32 rds32( u32 o ) { return (s32)rd32( o ); }

struct Level
{
	u16	nTex, nMesh, nNode, nEnt, nPlane, nHull;
	u32	nClip;
	u16	scale;			// Quake units per world unit
	u32	base;			// where the blob area starts
	u32	palOff, strOff;		// offsets within the blob area
	u32	texTab, meshTab, nodeTab, entTab, planeTab, hullTab, clipTab;
};

static Level g_L;

static void *hostAlloc( void *, u32 nBytes )
{
	return malloc( nBytes );
}

static u8  g_Pixels[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ];
static u16 g_Depth[ GPU64_3D_SURFACE_W * GPU64_3D_SURFACE_H ];
static u8  g_Palette[ 256 * 3 ];

// --- the resource table, same shape as gpu64_3d_class1.cpp's s_Res[] -----

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
	for ( unsigned i = 0; i < MAX_RES; i++ )
		if ( g_Res[ i ].type == 0 )
		{
			g_Res[ i ].id = nId;
			return &g_Res[ i ];
		}
	return 0;
}

static Res *resFind( u16 nId, u8 nType )
{
	for ( unsigned i = 0; i < MAX_RES; i++ )
		if ( g_Res[ i ].type == nType && g_Res[ i ].id == nId )
			return &g_Res[ i ];
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

static void writePPM( const char *pPath )
{
	FILE *f = fopen( pPath, "wb" );
	if ( !f )
	{
		fprintf( stderr, "levelsim: cannot write %s\n", pPath );
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

// --- loading -------------------------------------------------------------

static boolean loadFile( const char *pPath )
{
	FILE *f = fopen( pPath, "rb" );
	if ( !f )
	{
		fprintf( stderr, "levelsim: cannot open %s\n", pPath );
		return FALSE;
	}
	fseek( f, 0, SEEK_END );
	long n = ftell( f );
	fseek( f, 0, SEEK_SET );
	g_Lev = (u8 *)malloc( n ? n : 1 );
	if ( fread( g_Lev, 1, n, f ) != (size_t)n )
	{
		fprintf( stderr, "levelsim: short read on %s\n", pPath );
		fclose( f );
		return FALSE;
	}
	fclose( f );
	g_LevLen = (u32)n;
	return TRUE;
}

static const char *levStr( u32 nOff );	// defined with the blob accessors below

// The firmware runs Source/Firmware/gpu64_level.cpp's parser, not the one
// above. Two readers of one packed binary format is exactly how an offset
// typo reaches the bench -- the magic constant in this file and the one in
// gpu64_level.h disagreed on first writing, and nothing but a comparison
// would have said so. So every field is read twice, by both, and a
// disagreement is fatal here rather than a wrong room there.
//
// A differential rather than collapsing to one parser on purpose: the second
// reader is the test. Deleting it would leave the firmware's offsets
// unchecked by anything except a photograph of a CRT.
#define DIFF( field, mine, theirs )					\
	if ( (u64)( mine ) != (u64)( theirs ) )				\
	{								\
		fprintf( stderr, "levelsim: %s -- this file reads %llu,"	\
				  " gpu64_level.cpp reads %llu\n",	\
			 field, (unsigned long long)( mine ),		\
			 (unsigned long long)( theirs ) );		\
		return FALSE;						\
	}

static boolean checkAgainstFirmwareParser( void )
{
	Gpu64_Level fw;
	if ( !gpu64_levelParse( &fw, g_Lev, g_LevLen ) )
	{
		fprintf( stderr, "levelsim: gpu64_level.cpp refuses this file"
				  " even though the reader in levelsim.cpp accepts it\n" );
		return FALSE;
	}

	DIFF( "scale",  g_L.scale,  fw.nScale )
	DIFF( "ntex",   g_L.nTex,   fw.nTex )
	DIFF( "nmesh",  g_L.nMesh,  fw.nMesh )
	DIFF( "nnode",  g_L.nNode,  fw.nNode )
	DIFF( "nent",   g_L.nEnt,   fw.nEnt )
	DIFF( "nplane", g_L.nPlane, fw.nPlane )
	DIFF( "nhull",  g_L.nHull,  fw.nHull )
	DIFF( "nclip",  g_L.nClip,  fw.nClip )
	DIFF( "palOff", g_L.palOff, fw.nPalOff )
	DIFF( "strOff", g_L.strOff, fw.nStrOff )

	// The table pointers, compared as offsets from the start of the file.
	DIFF( "texTab",   g_L.texTab,   (u32)( fw.pTexTab   - g_Lev ) )
	DIFF( "meshTab",  g_L.meshTab,  (u32)( fw.pMeshTab  - g_Lev ) )
	DIFF( "nodeTab",  g_L.nodeTab,  (u32)( fw.pNodeTab  - g_Lev ) )
	DIFF( "entTab",   g_L.entTab,   (u32)( fw.pEntTab   - g_Lev ) )
	DIFF( "planeTab", g_L.planeTab, (u32)( fw.pPlaneTab - g_Lev ) )
	DIFF( "hullTab",  g_L.hullTab,  (u32)( fw.pHullTab  - g_Lev ) )
	DIFF( "clipTab",  g_L.clipTab,  (u32)( fw.pClipTab  - g_Lev ) )
	DIFF( "base",     g_L.base,     (u32)( fw.pBlob     - g_Lev ) )

	// And the two accessors, which have their own arithmetic: the blob
	// bound and the string pool. Entity 0 exercises gpu64_levelEnt()'s
	// whole record layout, which the loader's player-start search depends
	// on and which nothing else here reads through the firmware's eyes.
	if ( g_L.nEnt )
	{
		Gpu64_LevelEnt e;
		if ( !gpu64_levelEnt( &fw, 0, &e ) )
		{
			fprintf( stderr, "levelsim: gpu64_levelEnt() rejects entity 0\n" );
			return FALSE;
		}
		const u32 r = g_L.entTab;
		DIFF( "ent0 yaw",   rd16( r + 2 ),  e.nYaw )
		DIFF( "ent0 x",     rds32( r + 16 ), e.x )
		DIFF( "ent0 y",     rds32( r + 20 ), e.y )
		DIFF( "ent0 z",     rds32( r + 24 ), e.z )
		if ( strcmp( levStr( rd16( r ) ), e.pClassName ) )
		{
			fprintf( stderr, "levelsim: ent0 classname -- this file reads"
					  " \"%s\", gpu64_level.cpp reads \"%s\"\n",
				 levStr( rd16( r ) ), e.pClassName );
			return FALSE;
		}
	}
	return TRUE;
}
#undef DIFF

static boolean parseHeader( void )
{
	if ( g_LevLen < 52 || rd32( 0 ) != LEVEL_MAGIC )
	{
		fprintf( stderr, "levelsim: not a G64L file\n" );
		return FALSE;
	}
	if ( rd16( 4 ) != LEVEL_VERSION )
	{
		fprintf( stderr, "levelsim: level version %u, expected %u\n",
			 rd16( 4 ), LEVEL_VERSION );
		return FALSE;
	}
	g_L.scale  = rd16( 6 );
	g_L.nTex   = rd16( 8 );
	g_L.nMesh  = rd16( 10 );
	g_L.nNode  = rd16( 12 );
	g_L.nEnt   = rd16( 14 );
	g_L.nPlane = rd16( 16 );
	g_L.nHull  = rd16( 18 );
	g_L.nClip  = rd32( 20 );
	g_L.base   = rd32( 24 );
	g_L.palOff = rd32( 28 );
	g_L.strOff = rd32( 32 );

	g_L.texTab   = 52;		// 36-51: visibility and pictures, unused here
	g_L.meshTab  = g_L.texTab   + (u32)g_L.nTex   * TEX_STRIDE;
	g_L.nodeTab  = g_L.meshTab  + (u32)g_L.nMesh  * MESH_STRIDE;
	g_L.entTab   = g_L.nodeTab  + (u32)g_L.nNode  * NODE_STRIDE;
	g_L.planeTab = g_L.entTab   + (u32)g_L.nEnt   * ENT_STRIDE;
	g_L.hullTab  = g_L.planeTab + (u32)g_L.nPlane * PLANE_STRIDE;
	g_L.clipTab  = g_L.hullTab  + (u32)g_L.nHull  * HULL_STRIDE;

	const u32 end = g_L.clipTab + g_L.nClip * CLIP_STRIDE;
	if ( end != g_L.base )
	{
		fprintf( stderr, "levelsim: tables end at %u, header says base %u\n",
			 end, g_L.base );
		return FALSE;
	}
	if ( g_L.base > g_LevLen )
	{
		fprintf( stderr, "levelsim: blob area starts past the file end\n" );
		return FALSE;
	}
	return checkAgainstFirmwareParser();
}

static u8 *blob( u32 nOff, u32 nLen, const char *pWhat )
{
	if ( (u64)g_L.base + nOff + nLen > g_LevLen )
	{
		fprintf( stderr, "levelsim: %s runs past the blob area\n", pWhat );
		return 0;
	}
	return g_Lev + g_L.base + nOff;
}

static const char *levStr( u32 nOff )
{
	return (const char *)( g_Lev + g_L.base + g_L.strOff + nOff );
}

// --- the scene -----------------------------------------------------------

static Gpu64_3dState   g_State;
static Gpu64_3dScene   g_Scene;
static Gpu64_3dScratch g_Scratch;

static boolean buildResources( void )
{
	for ( unsigned i = 0; i < g_L.nTex; i++ )
	{
		const u32 r = g_L.texTab + i * TEX_STRIDE;
		const u8 ws = g_Lev[ r + 2 ], hs = g_Lev[ r + 3 ];
		const u32 off = rd32( r + 4 ), len = rd32( r + 8 );
		u8 *p = blob( off, len, "a texture" );
		if ( !p ) return FALSE;
		Res *pR = resSlot( (u16)i );
		if ( !pR || gpu64_3dBuildTexture( &pR->tex, p, len, ws, hs,
						   hostAlloc, 0 ) != GPU64_3D_OK )
		{
			fprintf( stderr, "levelsim: texture %u (%ux%u) failed to build\n",
				 i, 1u << ws, 1u << hs );
			return FALSE;
		}
		pR->type = 2;
	}

	for ( unsigned i = 0; i < g_L.nMesh; i++ )
	{
		const u32 r = g_L.meshTab + i * MESH_STRIDE;
		const u32 vo = rd32( r ), vl = rd32( r + 4 );
		const u32 fo = rd32( r + 8 ), fl = rd32( r + 12 );
		u8 *pV = blob( vo, vl, "a vertex blob" );
		u8 *pF = blob( fo, fl, "a face blob" );
		if ( !pV || !pF ) return FALSE;
		Res *pR = resSlot( (u16)( MESH_ID_BASE + i ) );
		if ( !pR || gpu64_3dBuildMesh( &pR->mesh, pV, vl, pF, fl,
						hostAlloc, 0 ) != GPU64_3D_OK )
		{
			fprintf( stderr, "levelsim: mesh %u (%u verts, %u faces)"
					  " failed to build\n",
				 i, vl / 6, fl / 12 );
			return FALSE;
		}
		pR->type = 1;
	}
	return TRUE;
}

static boolean buildScene( void )
{
	for ( unsigned i = 0; i < g_L.nNode; i++ )
	{
		const u32 r = g_L.nodeTab + i * NODE_STRIDE;
		const u16 mesh = rd16( r );
		Gpu64_3dVec pos;
		pos.x = rds32( r + 2 );
		pos.y = rds32( r + 6 );
		pos.z = rds32( r + 10 );

		const u16 id = (u16)( NODE_ID_BASE + i );
		u8 res = gpu64_3dSceneCreateObject( &g_Scene, id,
						     (u16)( MESH_ID_BASE + mesh ) );
		if ( res != GPU64_3D_OK )
		{
			// The node table is 256 entries and a level can want
			// more chunks than that. Saying so is the point: it is
			// a capacity answer the converter needs, not a crash.
			fprintf( stderr, "levelsim: node %u of %u rejected (err %02x)"
					  " -- the scene table holds %u\n",
				 i, g_L.nNode, res, (unsigned)GPU64_3D_MAX_NODES );
			return FALSE;
		}
		gpu64_3dSceneSetPosition( &g_Scene, id, &pos );
	}
	return TRUE;
}

// Milestone 20's showcase: extra nodes from the command line, placed in the
// level (or on an empty background with --no-level) so a model's axes,
// winding and animation can be looked at before any firmware or 6502 code
// exists to place it. An actor is FIRST[+COUNT]@x,y,z[,yawdeg] in level mesh
// indices; with COUNT it steps through COUNT frames, one per rendered frame,
// which is exactly the resource change WORLD_TICK will make. A sprite is
// TEXID@x,y,z,w,h in world units. --view-actor= and --view-sprite= take the
// same forms and set the node's viewSpace flag, as WORLD_TICK does for the
// gun and the status bar: the position is then relative to the camera
// (x right, y up, z forward) and the node is drawn over the level.
#define MAX_EXTRA	64
#define EXTRA_ID_BASE	400

struct Extra
{
	boolean	bSprite, bView;
	unsigned first, count;
	double	x, y, z, yaw, w, h;
};
static Extra g_Extra[ MAX_EXTRA ];
static unsigned g_nExtra;

static boolean parseExtra( const char *p, boolean bSprite )
{
	if ( g_nExtra >= MAX_EXTRA )
		return FALSE;
	Extra &e = g_Extra[ g_nExtra ];
	memset( &e, 0, sizeof( e ) );
	e.bSprite = bSprite;
	e.count = 1;
	if ( bSprite )
	{
		if ( sscanf( p, "%u@%lf,%lf,%lf,%lf,%lf", &e.first, &e.x, &e.y, &e.z,
			     &e.w, &e.h ) != 6 )
			return FALSE;
	}
	else
	{
		const int n = strchr( p, '+' ) && strchr( p, '+' ) < strchr( p, '@' )
			? sscanf( p, "%u+%u@%lf,%lf,%lf,%lf", &e.first, &e.count,
				  &e.x, &e.y, &e.z, &e.yaw )
			: sscanf( p, "%u@%lf,%lf,%lf,%lf", &e.first,
				  &e.x, &e.y, &e.z, &e.yaw ) + 1;
		if ( n < 5 || e.count == 0 )
			return FALSE;
	}
	g_nExtra++;
	return TRUE;
}

static Gpu64_3dNode *findNode( u16 nId )
{
	for ( unsigned i = 0; i < GPU64_3D_MAX_NODES; i++ )
		if ( g_Scene.node[ i ].type != GPU64_3D_NODE_NONE
		  && g_Scene.node[ i ].id == nId )
			return &g_Scene.node[ i ];
	return 0;
}

static Gpu64_3dNode *findNode( u16 nId );

static boolean buildExtras( void )
{
	for ( unsigned i = 0; i < g_nExtra; i++ )
	{
		const Extra &e = g_Extra[ i ];
		const u16 id = (u16)( EXTRA_ID_BASE + i );
		u8 res = e.bSprite
			? gpu64_3dSceneCreateSprite( &g_Scene, id, (u16)e.first )
			: gpu64_3dSceneCreateObject( &g_Scene, id,
						     (u16)( MESH_ID_BASE + e.first ) );
		if ( res != GPU64_3D_OK )
		{
			fprintf( stderr, "levelsim: extra node %u rejected (err %02x)\n",
				 i, res );
			return FALSE;
		}
		Gpu64_3dVec pos;
		pos.x = (s32)( e.x * 65536.0 );
		pos.y = (s32)( e.y * 65536.0 );
		pos.z = (s32)( e.z * 65536.0 );
		gpu64_3dSceneSetPosition( &g_Scene, id, &pos );
		if ( e.bView )
			findNode( id )->viewSpace = 1;
		if ( e.bSprite )
			gpu64_3dSceneSetSprite( &g_Scene, id, (u16)( e.w * 256.0 ),
						(u16)( e.h * 256.0 ), 0 );
		else
			gpu64_3dSceneSetOrientation( &g_Scene, id,
				(u16)(int)( e.yaw * 65536.0 / 360.0 ), 0, 0 );
	}
	return TRUE;
}

static void animateExtras( unsigned frame )
{
	for ( unsigned i = 0; i < g_nExtra; i++ )
		if ( !g_Extra[ i ].bSprite && g_Extra[ i ].count > 1 )
			if ( Gpu64_3dNode *pN = findNode( (u16)( EXTRA_ID_BASE + i ) ) )
				pN->meshId = (u16)( MESH_ID_BASE + g_Extra[ i ].first
						    + frame % g_Extra[ i ].count );
}

// Finds info_player_start and returns its eye position and yaw. The eye is
// 22 Quake units above the entity origin -- Quake's own view_ofs -- which at
// 32 units per world unit is not negligible: without it the camera sits at
// knee height and the floor fills half the frame.
static boolean playerStart( Gpu64_3dVec *pPos, u16 *pYaw )
{
	for ( unsigned i = 0; i < g_L.nEnt; i++ )
	{
		const u32 r = g_L.entTab + i * ENT_STRIDE;
		if ( strcmp( levStr( rd16( r ) ), "info_player_start" ) )
			continue;
		pPos->x = rds32( r + 16 );
		pPos->y = rds32( r + 20 ) + (s32)( 22 * 65536 / g_L.scale );
		pPos->z = rds32( r + 24 );
		*pYaw = rd16( r + 2 );
		return TRUE;
	}
	return FALSE;
}

static void countEnts( void )
{
	// Prints the classnames a game will have to do something with. A
	// converter that silently lost the monsters would still render.
	const char *pWant[] = { "monster_army", "monster_dog", "func_door",
				 "func_plat", "func_button", "trigger_once",
				 "trigger_multiple", "light", "item_health",
				 "info_player_start" };
	for ( unsigned k = 0; k < sizeof( pWant ) / sizeof( pWant[ 0 ] ); k++ )
	{
		unsigned n = 0;
		for ( unsigned i = 0; i < g_L.nEnt; i++ )
			if ( !strcmp( levStr( rd16( g_L.entTab + i * ENT_STRIDE ) ),
				       pWant[ k ] ) )
				n++;
		if ( n )
			printf( "  %-20s %u\n", pWant[ k ], n );
	}
}

int main( int argc, char **argv )
{
	// The firmware's rad.cfg GPU64_BURST_BYTES, for proving a budget
	// changes only where the rasteriser yields, never a pixel.
	if ( const char *pBurst = getenv( "GPU64_BURST_BYTES" ) )
		gpu64_3dSetSpanBytes( (unsigned)atoi( pBurst ) );

	const char *pLevel = 0;
	const char *pOut = "out";
	unsigned nFrames = 1;
	boolean bTurn = FALSE, bQuiet = FALSE, bHavePos = FALSE;
	Gpu64_3dVec camPos = { 0, 0, 0 };
	u16 camYaw = 0, camPitch = 0;
	int nFovDeg = 0, nBg = 0;
	boolean bNoLevel = FALSE;
	double px = 0, py = 0, pz = 0;

	for ( int i = 1; i < argc; i++ )
	{
		if ( !strncmp( argv[ i ], "--frames=", 9 ) )
			nFrames = (unsigned)atoi( argv[ i ] + 9 );
		else if ( !strcmp( argv[ i ], "--turn" ) )
			bTurn = TRUE;
		else if ( !strcmp( argv[ i ], "--quiet" ) )
			bQuiet = TRUE;
		else if ( !strncmp( argv[ i ], "--pos=", 6 ) )
		{
			if ( sscanf( argv[ i ] + 6, "%lf,%lf,%lf", &px, &py, &pz ) == 3 )
				bHavePos = TRUE;
		}
		else if ( !strncmp( argv[ i ], "--yaw=", 6 ) )
			camYaw = (u16)(int)( atof( argv[ i ] + 6 ) * 65536.0 / 360.0 );
		else if ( !strncmp( argv[ i ], "--pitch=", 8 ) )
			camPitch = (u16)(int)( atof( argv[ i ] + 8 ) * 65536.0 / 360.0 );
		else if ( !strncmp( argv[ i ], "--fov=", 6 ) )
			nFovDeg = atoi( argv[ i ] + 6 );
		else if ( !strcmp( argv[ i ], "--no-level" ) )
			bNoLevel = TRUE;
		else if ( !strncmp( argv[ i ], "--bg=", 5 ) )
			nBg = atoi( argv[ i ] + 5 );
		else if ( !strncmp( argv[ i ], "--actor=", 8 )
		       || !strncmp( argv[ i ], "--sprite=", 9 )
		       || !strncmp( argv[ i ], "--view-actor=", 13 )
		       || !strncmp( argv[ i ], "--view-sprite=", 14 ) )
		{
			const boolean bView = argv[ i ][ 2 ] == 'v';
			const char *p = argv[ i ] + ( bView ? 7 : 2 );
			const boolean bSpr = p[ 0 ] == 's';
			if ( !parseExtra( p + ( bSpr ? 7 : 6 ), bSpr ) )
			{
				fprintf( stderr, "levelsim: bad %s\n", argv[ i ] );
				return 2;
			}
			g_Extra[ g_nExtra - 1 ].bView = bView;
		}
		else if ( !pLevel )
			pLevel = argv[ i ];
		else
			pOut = argv[ i ];
	}

	if ( !pLevel )
	{
		fprintf( stderr, "usage: levelsim <level.g64lev> [outdir]"
				  " [--frames=N] [--turn] [--pos=x,y,z] [--yaw=DEG]"
				  " [--pitch=DEG] [--fov=DEG] [--quiet] [--no-level] [--bg=IDX]"
				  " [--actor=MESH[+N]@x,y,z[,yaw]] [--sprite=TEX@x,y,z,w,h]"
				  " [--view-actor=...] [--view-sprite=...]\n" );
		return 2;
	}

	if ( !loadFile( pLevel ) || !parseHeader() )
		return 1;

	printf( "%s: %u textures, %u meshes, %u nodes, %u entities,"
		 " %u Quake units per world unit\n",
		 pLevel, g_L.nTex, g_L.nMesh, g_L.nNode, g_L.nEnt, g_L.scale );
	printf( "  collision  %u planes, %u hulls, %u clipnodes\n",
		 g_L.nPlane, g_L.nHull, g_L.nClip );

	u8 *pPal = blob( g_L.palOff, 768, "the palette" );
	if ( !pPal )
		return 1;
	memcpy( g_Palette, pPal, 768 );

	gpu64_3dStateDefaults( &g_State );
	gpu64_3dSceneReset( &g_Scene );
	memset( g_Res, 0, sizeof( g_Res ) );

	if ( !buildResources() || ( !bNoLevel && !buildScene() ) || !buildExtras() )
		return 1;

	// The colormap is what makes the level lit rather than flat, and it is
	// derived from the level's own palette -- same call BUILD_COLORMAP makes.
	gpu64_3dBuildColormap( &g_State, g_Palette );
	g_State.background = (u8)nBg;

	if ( nFovDeg )
	{
		g_State.fov = (u16)(int)( nFovDeg * 65536.0 / 360.0 );
		g_State.focal = gpu64_3dFocalFromFov( g_State.fov, g_State.vpW );
	}

	// The far plane: a Quake room is tens of world units across at 32
	// units per world unit, and the default 256 is generous. Kept explicit
	// so the number a bench build will need is visible here.
	g_State.nearZ = GPU64_FX16_ONE / 4;
	g_State.farZ  = 256 * GPU64_FX16_ONE;

	u8 res = gpu64_3dSceneCreateCamera( &g_Scene, CAMERA_ID );
	if ( res != GPU64_3D_OK )
	{
		fprintf( stderr, "levelsim: the camera did not fit (err %02x)\n", res );
		return 1;
	}
	gpu64_3dSceneSetActiveCamera( &g_Scene, CAMERA_ID );

	if ( bHavePos )
	{
		camPos.x = (s32)( px * 65536.0 );
		camPos.y = (s32)( py * 65536.0 );
		camPos.z = (s32)( pz * 65536.0 );
		printf( "  camera  %.2f %.2f %.2f (given)\n", px, py, pz );
	}
	else if ( playerStart( &camPos, &camYaw ) )
	{
		printf( "  camera  %.2f %.2f %.2f  yaw %04x (info_player_start)\n",
			camPos.x / 65536.0, camPos.y / 65536.0, camPos.z / 65536.0,
			camYaw );
	}
	else
	{
		fprintf( stderr, "levelsim: no info_player_start; use --pos\n" );
		return 1;
	}
	countEnts();

	Gpu64_3dTarget target;
	target.pPixels = g_Pixels;
	target.pitch   = GPU64_3D_SURFACE_W;
	target.pDepth  = g_Depth;

	unsigned nBlank = 0;
	for ( unsigned frame = 0; frame < nFrames; frame++ )
	{
		const u16 yaw = bTurn
			? (u16)( camYaw + (u16)( (u32)frame * 65536u / ( nFrames ? nFrames : 1 ) ) )
			: camYaw;
		gpu64_3dSceneSetPosition( &g_Scene, CAMERA_ID, &camPos );
		gpu64_3dSceneSetOrientation( &g_Scene, CAMERA_ID, yaw, camPitch, 0 );
		animateExtras( frame );

		gpu64_3dSceneRender( &g_Scene, &g_State, &target, &g_Scratch,
				      lookupMesh, 0, lookupTexture, 0 );

		unsigned ink = 0;
		for ( unsigned i = 0; i < sizeof( g_Pixels ); i++ )
			if ( g_Pixels[ i ] != g_State.background )
				ink++;
		if ( !ink )
			nBlank++;

		char path[ 1024 ];
		snprintf( path, sizeof( path ), "%s/level%04u.ppm", pOut, frame );
		writePPM( path );

		// Per frame, never a total: "the room is there" and "the room is
		// there in frame 1 and blank from frame 2" are different answers
		// and a sum cannot tell them apart.
		if ( !bQuiet )
			printf( "frame %4u  yaw %04x  ink %6u (%2u%%)  checksum %08x\n",
				frame, yaw, ink,
				ink * 100 / (unsigned)sizeof( g_Pixels ), checksum() );
	}

	printf( "levelsim: %u frames, %u PPMs in %s/", nFrames, nFrames, pOut );
	if ( nBlank )
		printf( "  -- %u BLANK", nBlank );
	printf( "\n" );
	return nBlank == nFrames ? 1 : 0;
}

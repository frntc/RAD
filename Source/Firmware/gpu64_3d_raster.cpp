// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 6 -- the perspective-correct, z-tested triangle rasteriser,
 and the viewport clear. See gpu64_3d_render.h.

 Portable: compiled unchanged by the firmware and by tools/hostsim.
*/
#include "gpu64_3d_render.h"

// gpu64: three bytes leave the core per pixel -- one colour, two z -- so the
// milestone 6a burst budget is this many pixels, not this many bytes. The
// yield sits inside the span loop rather than at the end of a scanline
// because a full-width span is 960 bytes, nearly four times the budget.
//
// The budget is a runtime value (gpu64_3d_span.h), so the pixel count is
// derived once by the setter rather than divided per chunk.
unsigned gpu64_3dSpanBytes = GPU64_3D_SPAN_BYTES_DEFAULT;
static int s_nSpanPixels   = GPU64_3D_SPAN_BYTES_DEFAULT / 3;

#ifdef GPU64_HOSTSIM
unsigned long gpu64_3dYieldCount = 0;
#endif

#define GPU64_3D_SPAN_PIXELS	s_nSpanPixels

unsigned gpu64_3dSetSpanBytes( unsigned nBytes )
{
	if ( nBytes < GPU64_3D_SPAN_BYTES_MIN ) nBytes = GPU64_3D_SPAN_BYTES_MIN;
	if ( nBytes > GPU64_3D_SPAN_BYTES_MAX ) nBytes = GPU64_3D_SPAN_BYTES_MAX;
	nBytes &= ~63u;

	gpu64_3dSpanBytes = nBytes;
	s_nSpanPixels     = (int)( nBytes / 3 );
	return nBytes;
}

// Pixels between the divides that recover u and v from u/z and v/z. Quake's
// own number, for Quake's own reason: the error a linear step accumulates
// over sixteen pixels is far below a texel, and a divide per pixel is not
// worth paying for it. Class 2's gpu64_rasterPolys() does divide per pixel,
// but only because tools/rastercheck diffs it texel-for-texel against a
// Python model; class 1 has no such model and is free to subdivide.
#define GPU64_3D_PERSP_RUN	16

// The triangle loop's chunk, which is also a whole number of perspective
// runs (below). The runs restart at every chunk boundary, so a chunk that is
// not a multiple of the run length moves the divides -- and with them the
// texel rounding -- whenever the budget changes. Rounding the chunk down
// keeps the picture identical at every GPU64_BURST_BYTES; tools/hostsim
// proves it (GPU64_BURST_BYTES=64 vs 448, byte-identical PPMs).
static int triSpanPixels( void )
{
	const int n = s_nSpanPixels & ~( GPU64_3D_PERSP_RUN - 1 );
	return n > GPU64_3D_PERSP_RUN ? n : GPU64_3D_PERSP_RUN;
}

typedef __int128 s128;

// --- point lights -------------------------------------------------------
//
// Stage 17. Class 2's arithmetic, in class 1's direction: its colormap runs
// dark-wards so a light there subtracts, and this one runs bright-wards
// (level 15 is the unattenuated palette) so a light here adds. Cost per
// light is three subtracts, three multiplies, a compare and -- only inside
// the radius -- one more multiply and a shift. No square root, no divide:
// the divide by r2 happened once, in gpu64_3dSceneApplyLights().

// The lights that can actually reach one triangle. Filling this in at
// triangle setup is what makes a per-pixel evaluation affordable: a scene
// may have eight lights live and still leave most of its triangles with an
// empty list, and an empty list takes the unlit inner loop -- the same one
// byte for byte that every pre-Stage-17 program has always run.
struct Gpu64_3dLitSet
{
	const Gpu64_3dPointLight *pL[ GPU64_3D_MAX_LIGHTS ];
	unsigned n;
};

static inline u8 lightAdd( const Gpu64_3dLitSet *pSet, u8 nLevel, s32 x, s32 y, s32 z )
{
	int add = 0;

	for ( unsigned i = 0; i < pSet->n; i++ )
	{
		const Gpu64_3dPointLight *pL = pSet->pL[ i ];

		// 8.8 differences square to 16.16, and three of them summed pass
		// what an s32 holds at the far plane -- hence 64 bits throughout,
		// the same as class 2.
		const s64 dx = (s64)x - pL->x;
		const s64 dy = (s64)y - pL->y;
		const s64 dz = (s64)z - pL->z;
		const s64 d2 = dx * dx + dy * dy + dz * dz;
		if ( d2 >= pL->r2 )
			continue;

		add += (int)( ( pL->fall * ( pL->r2 - d2 ) ) >> GPU64_3D_LIGHT_SHIFT );
	}

	if ( add <= 0 )
		return nLevel;

	const int lvl = (int)nLevel + add;
	return lvl >= GPU64_3D_LIGHT_LEVELS ? (u8)( GPU64_3D_LIGHT_LEVELS - 1 ) : (u8)lvl;
}

u8 gpu64_3dLightAt( const Gpu64_3dState *pState, u8 nLevel, s32 x, s32 y, s32 z )
{
	Gpu64_3dLitSet set;
	set.n = 0;

	for ( unsigned i = 0; i < GPU64_3D_MAX_LIGHTS; i++ )
		if ( pState->lights[ i ].fall != 0 )
			set.pL[ set.n++ ] = &pState->lights[ i ];

	return lightAdd( &set, nLevel, x, y, z );
}

// Which of the live lights can reach the box the triangle sits in. The test
// is the standard closest-point-on-box one, in 8.8, with a one-unit margin:
// the rasteriser samples pixel centres, which can sit a fraction of a pixel
// outside the triangle itself, and a light that flicked off exactly at a
// triangle's edge would show as a seam along it.
#define GPU64_3D_LIT_MARGIN	( 1 * GPU64_FX8_ONE )

static void gatherLights( Gpu64_3dLitSet *pSet, const Gpu64_3dState *pState,
			  const Gpu64_3dRasterVert *pV )
{
	pSet->n = 0;

	if ( pState->lightMask == 0 )
		return;

	s32 lo[ 3 ], hi[ 3 ];
	for ( unsigned k = 0; k < 3; k++ )
	{
		const s32 a = ( k == 0 ? pV[ 0 ].px : k == 1 ? pV[ 0 ].py : pV[ 0 ].pz ) >> 8;
		const s32 b = ( k == 0 ? pV[ 1 ].px : k == 1 ? pV[ 1 ].py : pV[ 1 ].pz ) >> 8;
		const s32 c = ( k == 0 ? pV[ 2 ].px : k == 1 ? pV[ 2 ].py : pV[ 2 ].pz ) >> 8;

		lo[ k ] = a < b ? ( a < c ? a : c ) : ( b < c ? b : c );
		hi[ k ] = a > b ? ( a > c ? a : c ) : ( b > c ? b : c );
		lo[ k ] -= GPU64_3D_LIT_MARGIN;
		hi[ k ] += GPU64_3D_LIT_MARGIN;
	}

	for ( unsigned i = 0; i < GPU64_3D_MAX_LIGHTS; i++ )
	{
		const Gpu64_3dPointLight *pL = &pState->lights[ i ];
		if ( pL->fall == 0 )
			continue;

		const s32 p[ 3 ] = { pL->x, pL->y, pL->z };
		s64 d2 = 0;
		for ( unsigned k = 0; k < 3; k++ )
		{
			const s64 d = p[ k ] < lo[ k ] ? (s64)lo[ k ] - p[ k ]
				    : p[ k ] > hi[ k ] ? (s64)p[ k ] - hi[ k ] : 0;
			d2 += d * d;
		}

		if ( d2 < pL->r2 )
			pSet->pL[ pSet->n++ ] = pL;
	}
}

// --- viewport clear -----------------------------------------------------

void gpu64_3dClearViewport( const Gpu64_3dState *pState, Gpu64_3dTarget *pTarget )
{
	const unsigned w = pState->vpW;
	const unsigned h = pState->vpH;

	for ( unsigned y = 0; y < h; y++ )
	{
		u8  *pRow = pTarget->pPixels + (size_t)( pState->vpY + y ) * pTarget->pitch + pState->vpX;
		u16 *pZ   = pTarget->pDepth + (size_t)y * w;

		unsigned x = 0;
		while ( x < w )
		{
			unsigned n = w - x;
			if ( n > (unsigned)GPU64_3D_SPAN_PIXELS )
				n = (unsigned)GPU64_3D_SPAN_PIXELS;

			for ( unsigned i = 0; i < n; i++ )
			{
				pRow[ x + i ] = pState->background;
				// 0 is "infinitely far": the buffer holds near/z, so bigger
				// is nearer and every real fragment beats an untouched pixel.
				pZ[ x + i ] = 0;
			}

			x += n;
			GPU64_3D_YIELD();
		}
	}
}

void gpu64_3dClearDepth( const Gpu64_3dState *pState, Gpu64_3dTarget *pTarget )
{
	const unsigned w = pState->vpW;
	const unsigned h = pState->vpH;

	// The same span-sized bursts as gpu64_3dClearViewport(), for the same
	// reason: it runs on core 1 and must yield inside CLAUDE.md's store
	// burst limit. A span here stores two bytes a pixel against the three
	// the full clear stores, so it is inside whatever that one is tuned to.
	for ( unsigned y = 0; y < h; y++ )
	{
		u16 *pZ = pTarget->pDepth + (size_t)y * w;

		unsigned x = 0;
		while ( x < w )
		{
			unsigned n = w - x;
			if ( n > (unsigned)GPU64_3D_SPAN_PIXELS )
				n = (unsigned)GPU64_3D_SPAN_PIXELS;

			for ( unsigned i = 0; i < n; i++ )
				pZ[ x + i ] = 0;

			x += n;
			GPU64_3D_YIELD();
		}
	}
}

// --- the rasteriser -----------------------------------------------------

struct Gpu64_3dGrad
{
	// Attribute change per pixel, at 16.32 -- sixteen fractional bits MORE
	// than the attribute itself carries. That surplus is not luxury, it is
	// the whole correctness of the plane: the span setup below reaches its
	// starting value by extrapolating from vertex 0, and vertex 0 of a wall
	// seen edge-on sits hundreds of pixels off the side of the screen. At
	// 16.16 the gradient of 1/z along such a wall is a number like 58.85
	// truncated to 58, and 0.85 lost per pixel across a 520-pixel
	// extrapolation is a 442-unit error in a value whose whole range at the
	// far plane is 1927 -- a 20% depth error, and, because u/z rides the
	// same plane, a texture that slides along the wall as the camera walks
	// past it. Measured with the oracle in the milestone-17 notes; do not
	// narrow these back to s32.
	s64	dx, dy;				// attribute change per pixel, 16.32
};

// Plane gradients of one attribute over the triangle. Done in 128-bit
// because the numerator is a 16.16 attribute times a 16.16 coordinate
// difference and the result then has to be scaled back up by 65536 before
// the divide -- doing that in 64 bits overflows for any triangle bigger than
// a few pixels, and the symptom is a texture that shears only on large
// polygons, which is a miserable thing to chase.
static void gradient( Gpu64_3dGrad *pG, s64 d,
		      s64 a0, s64 a1, s64 a2,
		      s64 dx1, s64 dy1, s64 dx2, s64 dy2 )
{
	const s64 da1 = a1 - a0;
	const s64 da2 = a2 - a0;

	const s128 numX = ( (s128)da1 * dy2 - (s128)da2 * dy1 ) << 32;
	const s128 numY = ( (s128)da2 * dx1 - (s128)da1 * dx2 ) << 32;

	pG->dx = (s64)( numX / d );
	pG->dy = (s64)( numY / d );
}

void gpu64_3dRasterTriangle( const Gpu64_3dState *pState,
			     Gpu64_3dTarget *pTarget,
			     const Gpu64_3dRasterVert *pV,
			     const Gpu64_3dTexture *pTex,
			     u8 nFlat,
			     u8 nLight )
{
	const s64 dx1 = pV[ 1 ].sx - pV[ 0 ].sx;
	const s64 dy1 = pV[ 1 ].sy - pV[ 0 ].sy;
	const s64 dx2 = pV[ 2 ].sx - pV[ 0 ].sx;
	const s64 dy2 = pV[ 2 ].sy - pV[ 0 ].sy;

	const s64 d = dx1 * dy2 - dx2 * dy1;
	if ( d == 0 )
		return;					// zero area: nothing to fill

	Gpu64_3dGrad gz, gu, gv;
	gradient( &gz, d, pV[ 0 ].invZ, pV[ 1 ].invZ, pV[ 2 ].invZ, dx1, dy1, dx2, dy2 );
	gradient( &gu, d, pV[ 0 ].uz, pV[ 1 ].uz, pV[ 2 ].uz, dx1, dy1, dx2, dy2 );
	gradient( &gv, d, pV[ 0 ].vz, pV[ 1 ].vz, pV[ 2 ].vz, dx1, dy1, dx2, dy2 );

	// The lights that reach this triangle, and -- only if there are any --
	// the three extra plane gradients that carry view-space position across
	// it. A triangle no light reaches pays one box test per live light here
	// and nothing at all per pixel.
	Gpu64_3dLitSet lit;
	gatherLights( &lit, pState, pV );

	Gpu64_3dGrad gpx = { 0, 0 }, gpy = { 0, 0 }, gpz = { 0, 0 };
	if ( lit.n )
	{
		gradient( &gpx, d, pV[ 0 ].px, pV[ 1 ].px, pV[ 2 ].px, dx1, dy1, dx2, dy2 );
		gradient( &gpy, d, pV[ 0 ].py, pV[ 1 ].py, pV[ 2 ].py, dx1, dy1, dx2, dy2 );
		gradient( &gpz, d, pV[ 0 ].pz, pV[ 1 ].pz, pV[ 2 ].pz, dx1, dy1, dx2, dy2 );
	}

	// Vertices sorted by y, as three indices -- the vertex data itself is
	// never moved, so the gradients above stay keyed to vertex 0.
	unsigned i0 = 0, i1 = 1, i2 = 2;
	if ( pV[ i0 ].sy > pV[ i1 ].sy ) { unsigned t = i0; i0 = i1; i1 = t; }
	if ( pV[ i1 ].sy > pV[ i2 ].sy ) { unsigned t = i1; i1 = i2; i2 = t; }
	if ( pV[ i0 ].sy > pV[ i1 ].sy ) { unsigned t = i0; i0 = i1; i1 = t; }

	const int vpX0 = pState->vpX;
	const int vpY0 = pState->vpY;
	const int vpX1 = pState->vpX + pState->vpW;
	const int vpY1 = pState->vpY + pState->vpH;

	// Pixel centres are at (x + 0.5, y + 0.5); a scanline is covered when its
	// centre falls inside the edge span. (v + 32767) >> 16 is the ceiling of
	// (v/65536 - 0.5), i.e. the first covered pixel.
	int y0 = (int)( ( pV[ i0 ].sy + 32767 ) >> 16 );
	int y1 = (int)( ( pV[ i2 ].sy + 32767 ) >> 16 );

	if ( y0 < vpY0 ) y0 = vpY0;
	if ( y1 > vpY1 ) y1 = vpY1;

	const u8 *pColormap = pState->colormap + (size_t)nLight * 256;
	const u8 flatIndex  = pColormap[ nFlat ];

	const u32 uMask = pTex ? (u32)( pTex->w - 1 ) : 0;
	const u32 vMask = pTex ? (u32)( pTex->h - 1 ) : 0;
	const u8  wShift = pTex ? pTex->wShift : 0;

	for ( int y = y0; y < y1; y++ )
	{
		const s64 yc = ( (s64)y << 16 ) + 32768;

		// The long edge i0->i2 always spans the whole triangle; which short
		// edge is active depends on whether we are above or below vertex i1.
		s64 xa, xb;

		{
			const s64 den = pV[ i2 ].sy - pV[ i0 ].sy;
			if ( den == 0 )
				continue;
			xa = pV[ i0 ].sx + ( ( pV[ i2 ].sx - pV[ i0 ].sx ) * ( yc - pV[ i0 ].sy ) ) / den;
		}

		if ( yc < pV[ i1 ].sy )
		{
			const s64 den = pV[ i1 ].sy - pV[ i0 ].sy;
			if ( den == 0 )
				continue;
			xb = pV[ i0 ].sx + ( ( pV[ i1 ].sx - pV[ i0 ].sx ) * ( yc - pV[ i0 ].sy ) ) / den;
		} else
		{
			const s64 den = pV[ i2 ].sy - pV[ i1 ].sy;
			if ( den == 0 )
				continue;
			xb = pV[ i1 ].sx + ( ( pV[ i2 ].sx - pV[ i1 ].sx ) * ( yc - pV[ i1 ].sy ) ) / den;
		}

		s64 xl = xa < xb ? xa : xb;
		s64 xr = xa < xb ? xb : xa;

		int x0 = (int)( ( xl + 32767 ) >> 16 );
		int x1 = (int)( ( xr + 32767 ) >> 16 );

		if ( x0 < vpX0 ) x0 = vpX0;
		if ( x1 > vpX1 ) x1 = vpX1;
		if ( x0 >= x1 )
			continue;

		// Attribute values at the centre of the span's first pixel, then a
		// plain add per pixel. All three of these ARE linear in screen
		// space -- that is the whole point of carrying u/z and v/z
		// rather than u and v, which are not, and whose affine
		// interpolation creased every large face along its quad's
		// diagonal. The divide that turns uz/vz back into texels is
		// paid once per GPU64_3D_PERSP_RUN pixels, below.
		const s64 fx = ( ( (s64)x0 << 16 ) + 32768 ) - pV[ 0 ].sx;
		const s64 fy = yc - pV[ 0 ].sy;

		// The accumulators are 16.32, matching the gradients: fx is a
		// pixel count in 16.16 and the product is taken in 128 bits
		// because a steep gradient times a long extrapolation passes
		// s64 on its own.
		#define AT0( a0, g ) \
			( ( (s64)(a0) << 16 ) + \
			  (s64)( ( (s128)(g).dx * fx + (s128)(g).dy * fy ) >> 16 ) )

		s64 z  = AT0( pV[ 0 ].invZ, gz );
		s64 uz = AT0( pV[ 0 ].uz,   gu );
		s64 vz = AT0( pV[ 0 ].vz,   gv );

		s64 px = 0, py = 0, pz = 0;
		if ( lit.n )
		{
			px = AT0( pV[ 0 ].px, gpx );
			py = AT0( pV[ 0 ].py, gpy );
			pz = AT0( pV[ 0 ].pz, gpz );
		}

		#undef AT0

		u8  *pRow = pTarget->pPixels + (size_t)y * pTarget->pitch;
		u16 *pZ   = pTarget->pDepth + (size_t)( y - vpY0 ) * pState->vpW - vpX0;

		const int nTriSpan = triSpanPixels();

		int x = x0;
		while ( x < x1 )
		{
			int n = x1 - x;
			if ( n > nTriSpan )
				n = nTriSpan;

			const int xChunk = x + n;

			// The perspective runs sit INSIDE the burst chunk rather
			// than replacing it. triSpanPixels() is milestone
			// 6a's store-burst budget and nothing else may set it;
			// yielding every sixteen pixels instead would multiply
			// the DSBs by five and buy nothing.
			while ( x < xChunk )
			{
				int m = xChunk - x;
				if ( m > GPU64_3D_PERSP_RUN )
					m = GPU64_3D_PERSP_RUN;

				// u and v exact at both ends of the run, a plain
				// add between. Both ends come down from the 16.32
				// the accumulators carry to the 16.16 the divides
				// want. z is positive everywhere inside a
				// near-clipped triangle, but the run's far end is
				// one past the last covered pixel and the plane
				// extrapolates freely there -- hence the clamps,
				// which cost a handful of compares per sixteen
				// pixels and remove a divide-by-zero that would
				// otherwise depend on the geometry. The low clamp
				// is on the SHIFTED value: a z of a few hundred is
				// a legitimate 16.16 depth and a zero 16.16 one.
				const s64 z0 = z >> 16;
				const s64 z1 = ( z + gz.dx * m ) >> 16;

				const s32 zNear = z0 > 0
					? (s32)( z0 < 0x7fffffff ? z0 : 0x7fffffff ) : 1;
				const s32 zEnd = z1 > 0
					? (s32)( z1 < 0x7fffffff ? z1 : 0x7fffffff ) : 1;

				// uz is 16.32 and zNear 16.16, so the quotient is
				// already the 16.16 texel coordinate -- the shift
				// the old s32 form needed is now the format.
				s32 u = (s32)( uz / zNear );
				s32 v = (s32)( vz / zNear );

				const s64 uEnd = ( uz + gu.dx * m ) / zEnd;
				const s64 vEnd = ( vz + gv.dx * m ) / zEnd;

				const s32 du = (s32)( ( uEnd - u ) / m );
				const s32 dv = (s32)( ( vEnd - v ) / m );

				// Two loop bodies, not an `if` per pixel: the unlit one is
				// what every program before Stage 17 runs and it is the same
				// loop it has always been -- the light test is hoisted out
				// here, where it costs one branch per run.
				if ( lit.n == 0 )
				{
					for ( int i = 0; i < m; i++, x++ )
					{
						// invZ is 1.0 at the near plane and falls off with distance,
						// so the test is >, not <, and an untouched pixel (0) always
						// loses.
						const s64 zi = z >> 16;
						const u32 depth = zi <= 0 ? 0
							: ( zi > 65535 ? 65535 : (u32)zi );

						if ( depth > pZ[ x ] )
						{
							if ( pTex )
							{
								const u32 tu = ( (u32)( u >> 16 ) ) & uMask;
								const u32 tv = ( (u32)( v >> 16 ) ) & vMask;
								pRow[ x ] = pColormap[ pTex->pTexels[ ( tv << wShift ) | tu ] ];
							} else
								pRow[ x ] = flatIndex;

							pZ[ x ] = (u16)depth;
						}

						z  += gz.dx;
						uz += gu.dx;
						vz += gv.dx;
						u  += du;
						v  += dv;
					}
				} else
				{
					for ( int i = 0; i < m; i++, x++ )
					{
						const s64 zi = z >> 16;
						const u32 depth = zi <= 0 ? 0
							: ( zi > 65535 ? 65535 : (u32)zi );

						if ( depth > pZ[ x ] )
						{
							// The interpolated position is 16.16 view space and
							// the lights are 8.8: one shift, per pixel, rather
							// than eight lights held in the wider format.
							const u8 lvl = lightAdd( &lit, nLight, (s32)( px >> 24 ), (s32)( py >> 24 ), (s32)( pz >> 24 ) );
							const u8 *pRowMap = pState->colormap + (size_t)lvl * 256;

							if ( pTex )
							{
								const u32 tu = ( (u32)( u >> 16 ) ) & uMask;
								const u32 tv = ( (u32)( v >> 16 ) ) & vMask;
								pRow[ x ] = pRowMap[ pTex->pTexels[ ( tv << wShift ) | tu ] ];
							} else
								pRow[ x ] = pRowMap[ nFlat ];

							pZ[ x ] = (u16)depth;
						}

						z  += gz.dx;
						uz += gu.dx;
						vz += gv.dx;
						u  += du;
						v  += dv;
						px += gpx.dx;
						py += gpy.dx;
						pz += gpz.dx;
					}
				}
			}

			GPU64_3D_YIELD();
		}
	}
}

// --- the sprite ---------------------------------------------------------
//
// Stage 17. A billboard is an axis-aligned rectangle at one depth, so
// everything gpu64_3dRasterTriangle() spends its setup on -- three plane
// gradients, two edge walks, a per-pixel z step -- collapses to a constant
// here. It gets its own loop rather than being fanned into two triangles
// through that one for the same reason DRAW_THINGS is not DRAW_POLYS.
//
// The yield is the triangle rasteriser's, unchanged and for the same reason
// (CLAUDE.md's 7-cache-line burst limit): a sprite that fills the screen
// writes a full-width span, and 320 pixels is 960 bytes.
void gpu64_3dRasterSprite( const Gpu64_3dState *pState,
			   Gpu64_3dTarget *pTarget,
			   s32 x0, s32 y0, s32 x1, s32 y1,
			   s32 nInvZ,
			   const Gpu64_3dTexture *pTex,
			   u8 nFlags,
			   u8 nLight )
{
	if ( pTex == 0 || x1 <= x0 || y1 <= y0 )
		return;

	const int vpX0 = pState->vpX;
	const int vpY0 = pState->vpY;
	const int vpX1 = pState->vpX + pState->vpW;
	const int vpY1 = pState->vpY + pState->vpH;

	int ix0 = (int)( ( x0 + 32767 ) >> 16 );
	int ix1 = (int)( ( x1 + 32767 ) >> 16 );
	int iy0 = (int)( ( y0 + 32767 ) >> 16 );
	int iy1 = (int)( ( y1 + 32767 ) >> 16 );

	if ( ix0 < vpX0 ) ix0 = vpX0;
	if ( ix1 > vpX1 ) ix1 = vpX1;
	if ( iy0 < vpY0 ) iy0 = vpY0;
	if ( iy1 > vpY1 ) iy1 = vpY1;
	if ( ix0 >= ix1 || iy0 >= iy1 )
		return;

	// Texel step per pixel, 16.16. The numerator is a texture dimension
	// (at most 256) shifted left 32, which is 2^40 -- inside an s64 with
	// room to spare, and the reason this is not done in 32 bits.
	const s32 du = (s32)( ( (s64)pTex->w << 32 ) / ( (s64)x1 - x0 ) );
	const s32 dv = (s32)( ( (s64)pTex->h << 32 ) / ( (s64)y1 - y0 ) );

	const s32 uStart = (s32)( ( ( ( ( (s64)ix0 << 16 ) + 32768 ) - x0 ) * du ) >> 16 );
	const s32 vStart = (s32)( ( ( ( ( (s64)iy0 << 16 ) + 32768 ) - y0 ) * dv ) >> 16 );

	u32 depth = (u32)( nInvZ < 0 ? 0 : nInvZ );
	if ( depth > 65535 ) depth = 65535;

	const u8 *pColormap = pState->colormap + (size_t)nLight * 256;

	const u32 uMask = (u32)( pTex->w - 1 );
	const u32 vMask = (u32)( pTex->h - 1 );
	const u8  wShift = pTex->wShift;

	const boolean bMasked = ( nFlags & GPU64_3D_SPRITE_OPAQUE ) == 0;
	const boolean bWriteZ = ( nFlags & GPU64_3D_SPRITE_NODEPTH ) == 0;

	s32 v = vStart;

	for ( int y = iy0; y < iy1; y++, v += dv )
	{
		const u32 tv = ( (u32)( v >> 16 ) ) & vMask;
		const u8 *pTexRow = pTex->pTexels + ( tv << wShift );

		u8  *pRow = pTarget->pPixels + (size_t)y * pTarget->pitch;
		u16 *pZ   = pTarget->pDepth + (size_t)( y - vpY0 ) * pState->vpW - vpX0;

		s32 u = uStart;
		int x = ix0;

		while ( x < ix1 )
		{
			int n = ix1 - x;
			if ( n > GPU64_3D_SPAN_PIXELS )
				n = GPU64_3D_SPAN_PIXELS;

			for ( int i = 0; i < n; i++, x++, u += du )
			{
				if ( depth <= pZ[ x ] )
					continue;

				const u8 t = pTexRow[ ( (u32)( u >> 16 ) ) & uMask ];
				// Index 0 is the hole in the card, which is what lets a
				// monster be a monster shape and not a rectangle. An opaque
				// sprite is the opt-out, for a card that really is solid.
				if ( t == 0 && bMasked )
					continue;

				pRow[ x ] = pColormap[ t ];
				if ( bWriteZ )
					pZ[ x ] = (u16)depth;
			}

			GPU64_3D_YIELD();
		}
	}
}

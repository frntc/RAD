// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
//
// gpu64_sniff.cpp - the IO2 sniffer's state and its HDMI report
//
// See gpu64_sniff.h for what this is for and how to read it.
//

#include "gpu64_sniff.h"
#include "gpu64_fb.h"
#include "gpu64_busstats.h"
#include "gpu64_holdgap.h"

GPU64SNIFF gpu64Sniff;

// ARM cycles after the CPU half-cycle anchor. The fifth is RAD's default
// WAIT_CYCLE_WRITEDATA. A PAL C64 cycle is ~1420 of these at 1.4GHz, so the
// half-cycle ends near 710; the last slot is still inside it.
const u32 gpu64SniffT[ GPU64_SNIFF_SLOTS ] =
	{ 250, 300, 350, 420, 470, 520, 570, 620, 670 };

const u8 gpu64SniffPattern[ GPU64_SNIFF_PATTERN_LEN ] = GPU64_SNIFF_PATTERN;

#ifdef GPU64_SNIFF_ENABLED

static char *sDec( char *p, u32 v )
{
	char t[ 12 ];
	int n = 0;
	do { t[ n++ ] = '0' + v % 10; v /= 10; } while ( v );
	while ( n ) *p++ = t[ --n ];
	return p;
}

static char *sDecW( char *p, u32 v, int w )
{
	char t[ 12 ];
	int n = 0;
	do { t[ n++ ] = '0' + v % 10; v /= 10; } while ( v );
	for ( int i = n; i < w; i++ ) *p++ = ' ';
	while ( n ) *p++ = t[ --n ];
	return p;
}

static char *sHex2( char *p, u8 v )
{
	static const char h[] = "0123456789ABCDEF";
	*p++ = h[ v >> 4 ];
	*p++ = h[ v & 15 ];
	return p;
}

static char *sStr( char *p, const char *s )
{
	while ( *s ) *p++ = *s++;
	return p;
}

// Share of n in d, 0-100, or 999 when there is nothing to divide by.
static u32 sPct( u32 n, u32 d )
{
	return d ? (u32)( ( (u64)n * 100 + d / 2 ) / d ) : 999;
}

// "win 1234 oth 56 51:300 67:300 56:299" -- how much of the traffic was in
// the API's window ($DF50-$DF6F), and the three busiest addresses overall.
static char *sHist( char *p, const char *tag, const u16 *h )
{
	u32 win = 0, oth = 0;
	for ( u32 a = 0; a < 256; a++ )
		if ( a >= 0x50 && a <= 0x6F ) win += h[ a ]; else oth += h[ a ];

	p = sStr( p, tag );
	p = sStr( p, " win " ); p = sDec( p, win );
	p = sStr( p, " oth " ); p = sDec( p, oth );

	u8 used[ 256 ] = { 0 };
	for ( int k = 0; k < 3; k++ )
	{
		u32 best = 256, bestN = 0;
		for ( u32 a = 0; a < 256; a++ )
			if ( !used[ a ] && h[ a ] > bestN ) { best = a; bestN = h[ a ]; }
		if ( best == 256 )
			break;
		used[ best ] = 1;
		*p++ = ' ';
		p = sHex2( p, (u8)best );
		*p++ = ':';
		p = sDec( p, bestN );
	}
	*p++ = '\n';
	return p;
}

static char *sRow( char *p, const char *tag, const u32 *v, u32 d )
{
	p = sStr( p, tag );
	for ( u32 i = 0; i < GPU64_SNIFF_SLOTS; i++ )
	{
		u32 pc = sPct( v[ i ], d );
		if ( pc == 999 ) p = sStr( p, "   -" );
		else p = sDecW( p, pc, 4 );
	}
	*p++ = '\n';
	return p;
}

void gpu64_sniffReport( void )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	// Only what the C64 did counts as a change; the vector fetches that
	// clock this report move every time.
	u32 sig = gpu64Sniff.io2Writes * 3 + gpu64Sniff.io2Reads * 7 +
		  gpu64Sniff.datas * 11 + gpu64Sniff.syncs * 13;
	// The first report always prints, even at all zeroes: "SNIFF W 0 R 0
	// V n" is itself the answer that REU mode runs and the IRQ vector is
	// seen, but nothing reaches IO2. (Run 1 on the C64U showed nothing at
	// all, which could not tell the two apart.)
	if ( gpu64Sniff.reports && sig == gpu64Sniff.lastSig )
		return;
	gpu64Sniff.reports++;
	gpu64Sniff.lastSig = sig;

	static char line[ 1024 ];
	char *p = line;

	p = sStr( p, "SNIFF#" ); p = sDec( p, gpu64Sniff.reports );
	p = sStr( p, " W " ); p = sDec( p, gpu64Sniff.io2Writes );
	p = sStr( p, " R " ); p = sDec( p, gpu64Sniff.io2Reads );
	p = sStr( p, " V " ); p = sDec( p, gpu64Sniff.vectorFetches );
	p = sStr( p, " VX " ); p = sDec( p, gpu64Sniff.vectorFakes );
	*p++ = '\n';

	p = sStr( p, "DISP " ); p = sDec( p, gpu64BusStats.dispatches );
	p = sStr( p, " BADW " ); p = sDec( p, gpu64BusStats.writesBad );
	p = sStr( p, " BADR " ); p = sDec( p, gpu64BusStats.readsBad );
	p = sStr( p, " MUX " ); p = sDec( p, gpu64BusStats.muxMiss );
	*p++ = '/'; p = sDec( p, gpu64BusStats.muxFixed );
	*p++ = '/'; p = sDec( p, gpu64BusStats.muxUnfixed );
	*p++ = '\n';

	p = sHist( p, "WR", gpu64Sniff.histW );
	p = sHist( p, "RD", gpu64Sniff.histR );

	// The first sixteen IO2 accesses since power-on, raw: w/r, address low
	// byte, data. A healthy API program opens with writes at $56-$67 and
	// $51.
	for ( u32 i = 0; i < gpu64Sniff.ringN; i++ )
	{
		*p++ = gpu64Sniff.ringWrite[ i ] ? 'w' : 'r';
		p = sHex2( p, gpu64Sniff.ringAddr[ i ] );
		p = sHex2( p, gpu64Sniff.ringData[ i ] );
		*p++ = ( i % 6 == 5 || i + 1 == gpu64Sniff.ringN ) ? '\n' : ' ';
	}

	if ( gpu64Sniff.datas )
	{
		p = sStr( p, "SWEEP sync " ); p = sDec( p, gpu64Sniff.syncs );
		p = sStr( p, " n " ); p = sDec( p, gpu64Sniff.scored );
		p = sStr( p, "/" ); p = sDec( p, gpu64Sniff.datas );
		p = sStr( p, " C64=" ); p = sDec( p, gpu64HoldGap.armPerC64 );
		*p++ = '\n';

		p = sStr( p, "T " );
		for ( u32 i = 0; i < GPU64_SNIFF_SLOTS; i++ )
			p = sDecW( p, gpu64SniffT[ i ], 4 );
		*p++ = '\n';

		p = sRow( p, "OK", gpu64Sniff.ok, gpu64Sniff.scored );

		p = sStr( p, "XB" );
		for ( u32 i = 0; i < GPU64_SNIFF_SLOTS; i++ )
		{
			*p++ = ' '; *p++ = ' ';
			p = sHex2( p, gpu64Sniff.badBits[ i ] );
		}
		*p++ = '\n';

		p = sRow( p, "IO", gpu64Sniff.io2Low, gpu64Sniff.scored );
		p = sRow( p, "PH", gpu64Sniff.phiHigh, gpu64Sniff.scored );
	}

	p = sStr( p, "--\n" );
	pFB->LogWrite( line, (unsigned)( p - line ) );
}

#endif

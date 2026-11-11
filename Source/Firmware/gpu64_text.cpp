// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: 80x50 text mode implementation -- see gpu64_text.h.
*/
#include "gpu64_text.h"
#include "gpu64_c64font.h"
#include <circle/util.h>

Gpu64TextState gpu64Text;

// Power-on colours. Light blue on blue is the C64's own, and picking it here
// means a program that switches to text mode and types gets a screen that
// looks like the machine it is plugged into without setting any colour at
// all.
#define GPU64_TXT_DEF_FG	14
#define GPU64_TXT_DEF_BG	6

void gpu64_textReset( void )
{
	memset( gpu64Text.screen, 0x20, GPU64_TXT_CELLS );	// screen code 32 = space
	memset( gpu64Text.fg, GPU64_TXT_DEF_FG, GPU64_TXT_CELLS );
	memset( gpu64Text.bg, GPU64_TXT_DEF_BG, GPU64_TXT_CELLS );
	gpu64Text.charset = GPU64_CHARSET_LOWER;
}

// --- rendering ----------------------------------------------------------
//
// One cell is 8 rows of 8 pixels written straight into the scanout buffer.
// There is no page to flip and no back buffer: the surface the VideoCore is
// scanning is the only one, so a repaint is visible as it happens. That is
// fine here and would not be in graphics mode -- text changes are cell-sized
// and the C64 is halted for the dispatch that caused them, so there is no
// frame being composed for a viewer to catch half-finished.

static void renderCell( u8 *pSurf, unsigned nPitch, unsigned nCol, unsigned nRow )
{
	unsigned nCell = nRow * GPU64_TXT_COLS + nCol;

	u8 nCode = gpu64Text.screen[ nCell ];
	u8 nFg   = gpu64Text.fg[ nCell ];
	u8 nBg   = gpu64Text.bg[ nCell ];

	// Screen code bit 7 is reverse video, as on the C64's own screen. The
	// ROM does carry a reversed copy of every glyph at $80-$FF, but this
	// renders the plain glyph with the two colours swapped instead:
	// identical pixels, and it keeps a reversed cell's ink and paper both
	// under the program's control, which indexing the reversed half would
	// not.
	if ( nCode & 0x80 )
	{
		u8 t = nFg; nFg = nBg; nBg = t;
		nCode &= 0x7F;
	}

	const u8 *pGlyph = gpu64C64Font[ gpu64Text.charset & 1 ][ nCode ];
	u8 *pDst = pSurf + (size_t)nRow * GPU64_TXT_CHAR_H * nPitch
			 + (size_t)nCol * GPU64_TXT_CHAR_W;

	for ( unsigned gy = 0; gy < GPU64_TXT_CHAR_H; gy++, pDst += nPitch )
	{
		u8 bits = pGlyph[ gy ];
		for ( unsigned gx = 0; gx < GPU64_TXT_CHAR_W; gx++ )
			pDst[ gx ] = ( bits & ( 0x80 >> gx ) ) ? nFg : nBg;
	}
}

void gpu64_textRenderRows( unsigned nRow0, unsigned nRow1 )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	u8 *pSurf = pFB->TextSurface();
	if ( pSurf == 0 )				// not in text mode: planes only
		return;

	if ( nRow1 > GPU64_TXT_ROWS ) nRow1 = GPU64_TXT_ROWS;
	if ( nRow0 >= nRow1 )
		return;

	unsigned nPitch = pFB->GetTextPitch();
	for ( unsigned row = nRow0; row < nRow1; row++ )
		for ( unsigned col = 0; col < GPU64_TXT_COLS; col++ )
			renderCell( pSurf, nPitch, col, row );

	pFB->CleanTextRows( nRow0 * GPU64_TXT_CHAR_H, nRow1 * GPU64_TXT_CHAR_H );
}

// A run inside one row -- what every write-a-string or set-a-cell op
// actually dirties. Kept separate from the whole-row path so a TEXT_PUT
// costs 64 pixels rather than 640.
static void renderRun( unsigned nCol, unsigned nRow, unsigned nCount )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return;

	u8 *pSurf = pFB->TextSurface();
	if ( pSurf == 0 )
		return;

	unsigned nPitch = pFB->GetTextPitch();
	for ( unsigned i = 0; i < nCount; i++ )
		renderCell( pSurf, nPitch, nCol + i, nRow );

	pFB->CleanTextRows( nRow * GPU64_TXT_CHAR_H, ( nRow + 1 ) * GPU64_TXT_CHAR_H );
}

// --- mode ---------------------------------------------------------------

boolean gpu64_textSetMode( u8 nMode )
{
	CGpu64FrameBuffer *pFB = g_pGpu64FB;
	if ( pFB == 0 )
		return FALSE;

	if ( !pFB->SetMode( nMode ) )
		return FALSE;

	// Entering text mode publishes whatever the planes already hold, so a
	// program can compose a screen with the graphics display still up and
	// have it appear complete at the switch.
	if ( nMode == GPU64_MODE_TEXT )
		gpu64_textRenderRows( 0, GPU64_TXT_ROWS );

	return TRUE;
}

// --- plane operations ---------------------------------------------------

void gpu64_textClear( u8 nFg, u8 nBg )
{
	memset( gpu64Text.screen, 0x20, GPU64_TXT_CELLS );
	memset( gpu64Text.fg, nFg, GPU64_TXT_CELLS );
	memset( gpu64Text.bg, nBg, GPU64_TXT_CELLS );
	gpu64_textRenderRows( 0, GPU64_TXT_ROWS );
}

void gpu64_textPut( unsigned nCol, unsigned nRow, u8 nCode, u8 nFg, u8 nBg )
{
	if ( nCol >= GPU64_TXT_COLS || nRow >= GPU64_TXT_ROWS )
		return;

	unsigned nCell = nRow * GPU64_TXT_COLS + nCol;
	gpu64Text.screen[ nCell ] = nCode;
	gpu64Text.fg[ nCell ] = nFg;
	gpu64Text.bg[ nCell ] = nBg;

	renderRun( nCol, nRow, 1 );
}

void gpu64_textFill( int nCol, int nRow, int nW, int nH, u8 nCode, u8 nFg, u8 nBg )
{
	// Clipped, not rejected -- the same contract RECT_FILL has in graphics
	// mode, so a program can slide a panel off the edge of the screen.
	if ( nW <= 0 || nH <= 0 )
		return;
	if ( nCol < 0 ) { nW += nCol; nCol = 0; }
	if ( nRow < 0 ) { nH += nRow; nRow = 0; }
	if ( nCol + nW > GPU64_TXT_COLS ) nW = GPU64_TXT_COLS - nCol;
	if ( nRow + nH > GPU64_TXT_ROWS ) nH = GPU64_TXT_ROWS - nRow;
	if ( nW <= 0 || nH <= 0 )
		return;

	for ( int y = 0; y < nH; y++ )
	{
		unsigned nCell = (unsigned)( nRow + y ) * GPU64_TXT_COLS + (unsigned)nCol;
		memset( gpu64Text.screen + nCell, nCode, (size_t)nW );
		memset( gpu64Text.fg + nCell, nFg, (size_t)nW );
		memset( gpu64Text.bg + nCell, nBg, (size_t)nW );
		renderRun( (unsigned)nCol, (unsigned)( nRow + y ), (unsigned)nW );
	}
}

void gpu64_textWrite( unsigned nCol, unsigned nRow, u8 nFg, u8 nBg,
		      const u8 *pText, unsigned nLen, u8 nFlags )
{
	if ( nRow >= GPU64_TXT_ROWS || nCol >= GPU64_TXT_COLS )
		return;

	// Truncated at the end of the row rather than wrapped. Wrapping would
	// need a scroll rule for the last row and a cursor to carry between
	// commands; a program that wants either can issue one call per row.
	unsigned nMax = GPU64_TXT_COLS - nCol;
	if ( nLen > nMax )
		nLen = nMax;

	unsigned nCell = nRow * GPU64_TXT_COLS + nCol;
	for ( unsigned i = 0; i < nLen; i++ )
	{
		u8 b = pText[ i ];
		if ( nFlags & GPU64_TXT_WRITE_ASCII )
			b = gpu64_c64ScreenCode( (char)b, gpu64Text.charset );
		gpu64Text.screen[ nCell + i ] = b;
		gpu64Text.fg[ nCell + i ] = nFg;
		gpu64Text.bg[ nCell + i ] = nBg;
	}

	renderRun( nCol, nRow, nLen );
}

void gpu64_textScroll( unsigned nRows, u8 nFg, u8 nBg )
{
	if ( nRows == 0 )
		return;
	if ( nRows > GPU64_TXT_ROWS )
		nRows = GPU64_TXT_ROWS;

	unsigned nKept = ( GPU64_TXT_ROWS - nRows ) * GPU64_TXT_COLS;
	unsigned nMoved = nRows * GPU64_TXT_COLS;

	if ( nKept )
	{
		memmove( gpu64Text.screen, gpu64Text.screen + nMoved, nKept );
		memmove( gpu64Text.fg, gpu64Text.fg + nMoved, nKept );
		memmove( gpu64Text.bg, gpu64Text.bg + nMoved, nKept );
	}

	memset( gpu64Text.screen + nKept, 0x20, nMoved );
	memset( gpu64Text.fg + nKept, nFg, nMoved );
	memset( gpu64Text.bg + nKept, nBg, nMoved );

	gpu64_textRenderRows( 0, GPU64_TXT_ROWS );
}

void gpu64_textSetCharset( u8 nCharset )
{
	nCharset &= 1;
	if ( nCharset == gpu64Text.charset )
		return;
	gpu64Text.charset = nCharset;
	// Every glyph on screen changes meaning, so this is one of the two
	// operations that genuinely costs a full repaint.
	gpu64_textRenderRows( 0, GPU64_TXT_ROWS );
}

void gpu64_textUpload( u8 nPlane, unsigned nFirst, const u8 *pSrc, unsigned nLen )
{
	u8 *pPlane;
	switch ( nPlane )
	{
	case GPU64_TXT_PLANE_SCREEN:	pPlane = gpu64Text.screen; break;
	case GPU64_TXT_PLANE_FG:	pPlane = gpu64Text.fg; break;
	default:			pPlane = gpu64Text.bg; break;
	}

	memcpy( pPlane + nFirst, pSrc, nLen );

	// Repaint whole rows: an upload that starts mid-row still has to redraw
	// that row's untouched cells, because a cell's pixels come from all
	// three planes and only one of them changed.
	gpu64_textRenderRows( nFirst / GPU64_TXT_COLS,
			      ( nFirst + nLen + GPU64_TXT_COLS - 1 ) / GPU64_TXT_COLS );
}

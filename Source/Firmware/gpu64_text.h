// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: the 80x50 text mode -- class 0 opcodes $50-$5F.

 A second display *mode*, not a layer over the graphics one: entering it
 re-programs the VideoCore for a 640x400 surface (768x544 with the border)
 and leaving it puts the 320x200 paged display back. See
 CGpu64FrameBuffer::SetMode() for why that is a whole framebuffer switch and
 not a viewport change.

 The model is deliberately the C64's own, so a 6502 programmer already knows
 it: three parallel 4000-byte planes -- screen codes, foreground colour,
 background colour -- indexed row-major, and glyphs that are the machine's
 own character ROM (gpu64_c64font.h). Screen code bit 7 is reverse video,
 exactly as it is on a C64 screen. What the C64 does not have, and this does,
 is a per-cell *background* colour and 256 colours in both planes.

 Rendering is immediate and per cell: text mode has one page and a fixed
 scanout address, so a cell is repainted where it stands and there is no
 flip. That is why none of the paging machinery applies here, and why the
 API refuses PAGE_FLIP and the class 1/2 render paths while this mode is up.
*/
#ifndef _gpu64_text_h
#define _gpu64_text_h

#include "gpu64_fb.h"
#include <circle/types.h>

#define GPU64_TXT_CELLS		( GPU64_TXT_COLS * GPU64_TXT_ROWS )	// 4000

// Plane selectors for TEXT_UPLOAD ($54).
#define GPU64_TXT_PLANE_SCREEN	0
#define GPU64_TXT_PLANE_FG	1
#define GPU64_TXT_PLANE_BG	2

// TEXT_WRITE ($53) flags.
// bit 0: the payload is ASCII and is translated to screen codes on the way
//        in. Clear means the bytes are screen codes already -- which is the
//        only way to reach the graphics half of the ROM or to set bit 7 for
//        reverse video.
#define GPU64_TXT_WRITE_ASCII	0x01

struct Gpu64TextState
{
	u8	screen[ GPU64_TXT_CELLS ];
	u8	fg[ GPU64_TXT_CELLS ];
	u8	bg[ GPU64_TXT_CELLS ];
	u8	charset;			// GPU64_CHARSET_UPPER / _LOWER
};

extern Gpu64TextState gpu64Text;

// Back to the power-on contents: every cell a space, light blue on blue,
// charset 1. Does not touch the display -- RESET_STATE calls this whether or
// not text mode is up.
void gpu64_textReset( void );

// TEXT_MODE ($50). Switches the display and paints the current plane
// contents. FALSE if the mode could not be programmed.
boolean gpu64_textSetMode( u8 nMode );

// The plane operations. All clip to the 80x50 grid, all repaint exactly the
// cells they changed, and all are no-ops (bar the plane update) when the
// display is not in text mode -- so a program may stage a screen before
// switching to it.
void gpu64_textClear( u8 nFg, u8 nBg );
void gpu64_textPut( unsigned nCol, unsigned nRow, u8 nCode, u8 nFg, u8 nBg );
void gpu64_textFill( int nCol, int nRow, int nW, int nH, u8 nCode, u8 nFg, u8 nBg );
void gpu64_textWrite( unsigned nCol, unsigned nRow, u8 nFg, u8 nBg,
		      const u8 *pText, unsigned nLen, u8 nFlags );
void gpu64_textScroll( unsigned nRows, u8 nFg, u8 nBg );
void gpu64_textSetCharset( u8 nCharset );
// Copies len bytes into one plane starting at cell nFirst. The caller has
// already bounds-checked nFirst + nLen against GPU64_TXT_CELLS.
void gpu64_textUpload( u8 nPlane, unsigned nFirst, const u8 *pSrc, unsigned nLen );

// Repaints rows [r0, r1) from the planes. Public because a mode switch and
// TEXT_REFRESH ($58) both need the whole screen.
void gpu64_textRenderRows( unsigned nRow0, unsigned nRow1 );

#endif

// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64: the 320x200x8 framebuffer the command API draws into.

 This owns the HDMI display outright ("architecture A", see
 dev/milestone4_2d_api_design.md#display-architecture): one CBcmFrameBuffer
 at 320x200, depth 8, virtual height 400, so the VideoCore gives us a real
 256-entry 24-bit palette, a real hardware page flip (SetVirtualOffset), and
 upscaling to whatever HDMI mode is attached -- none of which costs ARM time
 inside the bus-watch loop.

 That replaces the previous arrangement, where CScreenDevice's COLOR16
 framebuffer was the display and gpu64 drew into it via SetPixel(). Only one
 framebuffer can own the display, so CScreenDevice is out of the picture
 entirely; the on-screen log now lives here too, as an overlay drawn in one
 reserved palette entry (see GPU64_LOG_INK).
*/
#ifndef _gpu64_fb_h
#define _gpu64_fb_h

#include <circle/bcmframebuffer.h>
#include <circle/types.h>

// The drawing surface. All API coordinates are in this space, with (0,0) at
// its top-left -- the border below is outside it and is not addressable by
// any draw op, exactly like the C64's.
#define GPU64_FB_WIDTH		320
#define GPU64_FB_HEIGHT		200
// Three, not two. A deferred flip only *posts* the new virtual offset (see
// gpu64_flip.h); the VideoCore applies it at its own next vsync, so for up to
// a whole display frame after CommitFlip() the previously visible page is
// still being scanned out. With two pages that page is immediately handed
// back as the draw page, and the next frame is painted on screen as it is
// built -- which is exactly what made raycast show four background-only
// frames in five. A third page gives CommitFlip() somewhere provably idle to
// draw into.
#define GPU64_FB_PAGES		3

// gpu64: the border. The physical framebuffer is larger than the drawing
// surface and the surface sits centred in it, so SET_BORDER can paint the
// frame the way $D020 does. Sized like the C64's own visible border rather
// than the full PAL overscan: 384x272 total is the geometry emulators
// conventionally use.
#define GPU64_BORDER_W		32
#define GPU64_BORDER_H		36
#define GPU64_FB_TOTAL_W	( GPU64_FB_WIDTH + 2 * GPU64_BORDER_W )		// 384
#define GPU64_FB_TOTAL_H	( GPU64_FB_HEIGHT + 2 * GPU64_BORDER_H )	// 272

// gpu64: reserved palette entry the on-screen log draws its glyph pixels in.
// The API deliberately does not forbid a program from using index 255 as an
// ordinary colour or repointing it with PAL_SET -- the log just draws in
// whatever that entry holds. See docs/api_design.md.
#define GPU64_LOG_INK		255

// Log overlay geometry. This used to use Circle's CCharGenerator, whose only
// face is 8x16 -- 40 columns by 12 rows on a 200-pixel-tall surface, which
// made the log unreadable on a real display: boot messages wrapped, and 12
// lines of scrollback is nothing. The C64 character ROM (gpu64_c64font.h) is
// 8x8, so the same text now gets 25 rows -- and the log is drawn in the same
// face as the boot wordmark and the 80x50 text mode.
#define GPU64_LOG_CHAR_W	8
#define GPU64_LOG_CHAR_H	8
#define GPU64_LOG_COLS		( GPU64_FB_WIDTH / GPU64_LOG_CHAR_W )	// 40
#define GPU64_LOG_ROWS		( GPU64_FB_HEIGHT / GPU64_LOG_CHAR_H )	// 25

// --- 80x50 text mode ----------------------------------------------------
// A second, separate display geometry: 80 columns by 50 rows of the C64's
// own 8x8 glyphs, i.e. a 640x400 surface. It cannot be a window onto the
// 320x200 graphics surface -- it is four times the area -- so entering it
// re-programs the VideoCore for a different framebuffer, and leaving it
// re-programs it back. The two modes are mutually exclusive and share
// nothing but the palette; see CGpu64FrameBuffer::SetMode().
//
// The border keeps the same *proportion* it has in graphics mode (exactly
// double, 64x72 against 32x36), so the picture the display shows is framed
// the same way in both and switching modes does not appear to change the
// size of the screen.
#define GPU64_MODE_GRAPHICS	0
#define GPU64_MODE_TEXT		1

#define GPU64_TXT_COLS		80
#define GPU64_TXT_ROWS		50
#define GPU64_TXT_CHAR_W	8
#define GPU64_TXT_CHAR_H	8
#define GPU64_TXT_WIDTH		( GPU64_TXT_COLS * GPU64_TXT_CHAR_W )	// 640
#define GPU64_TXT_HEIGHT	( GPU64_TXT_ROWS * GPU64_TXT_CHAR_H )	// 400
#define GPU64_TXT_BORDER_W	64
#define GPU64_TXT_BORDER_H	72
#define GPU64_TXT_TOTAL_W	( GPU64_TXT_WIDTH + 2 * GPU64_TXT_BORDER_W )	// 768
#define GPU64_TXT_TOTAL_H	( GPU64_TXT_HEIGHT + 2 * GPU64_TXT_BORDER_H )	// 544

class CGpu64FrameBuffer
{
public:
	CGpu64FrameBuffer( void );
	~CGpu64FrameBuffer( void );

	boolean Initialize( void );
	boolean IsInitialized( void ) const	{ return m_bInitialized; }

	// --- pages ---------------------------------------------------------
	// Top-left of the *drawing surface* within the page, so every caller
	// can keep indexing p[ y * pitch + x ] in 320x200 coordinates and never
	// think about the border.
	u8 *PageBuffer( unsigned nPage );
	// Top-left of the whole physical page, border included.
	u8 *PageBase( unsigned nPage );
	unsigned GetPitch( void ) const		{ return m_nPitch; }
	u8 GetDrawPage( void ) const		{ return m_nDrawPage; }
	u8 GetVisiblePage( void ) const		{ return m_nVisiblePage; }
	void SetDrawPage( u8 nPage );
	// Swaps draw and visible page. Returns FALSE if the mailbox call failed.
	boolean Flip( void );
	// The two halves of Flip(), for the vblank-deferred case. PrepareFlip()
	// does everything expensive -- the log overlay and the cache clean --
	// and runs during the command dispatch, where the C64 is halted anyway.
	// CommitFlip() is then just the SetVirtualOffset the frame boundary is
	// actually waiting for, which is what the bus-watch loop runs.
	void PrepareFlip( void );
	boolean CommitFlip( void );
	// Whether the page PrepareFlip() would hand over as the next draw page
	// has really left the screen. CommitFlip() only *posts* the new scanout
	// offset; the VideoCore applies it at its own next vsync, which the
	// frame clock only approximates, so the page it replaced keeps being
	// scanned for up to a frame after m_nVisiblePage has moved on. Anything
	// that renders asynchronously into the handed-over page (SCENE_COMMIT's
	// core 1 frame) must ask this first and answer BUSY while it is FALSE.
	boolean NextDrawPageIdle( void );
	// Back to the reset arrangement: page 0 drawn and visible.
	void ResetPages( void );
	// Every page to black, border band included, and cleaned out to DRAM.
	void ClearAllPages( void );

	// Blocks until the display's next vertical sync. A mailbox round-trip
	// to the VideoCore, so this is boot- and setup-time only -- see
	// gpu64_vsync.h. Exposed because the frame clock is calibrated against
	// it and m_pFB is private.
	boolean WaitForVSync( void );

	// --- drawing (all act on the draw page, all clip) -------------------
	void Clear( u8 nColor );
	void SetPixel( int x, int y, u8 nColor );
	void Line( int x0, int y0, int x1, int y1, u8 nColor );
	void Rect( int x, int y, int w, int h, u8 nColor );
	void RectFill( int x, int y, int w, int h, u8 nColor );
	// nKey < 0 = opaque blit; nKey >= 0 = source pixels equal to it are skipped
	void Blit( const u8 *pSrc, int x, int y, unsigned w, unsigned h, int nKey );
	// Not clipped -- the caller must have validated the rectangle (READ_RECT
	// is specified as an error, not a clip, when it runs off the page).
	void ReadRect( u8 *pDst, unsigned x, unsigned y, unsigned w, unsigned h );

	// --- display mode ---------------------------------------------------
	// GPU64_MODE_GRAPHICS (320x200, paged, everything above) or
	// GPU64_MODE_TEXT (640x400, single page, gpu64_text.cpp draws into it).
	// Switching re-initialises the VideoCore framebuffer, which reallocates
	// it: every cached PageBuffer()/GetPitch() value is stale afterwards, so
	// nothing may hold one across a call to this. Returns FALSE if the new
	// mode could not be programmed, in which case the old one is still up.
	u8 GetMode( void ) const		{ return m_nMode; }
	boolean SetMode( u8 nMode );
	// Graphics mode re-sent to the VideoCore whole, pages cleared.
	boolean Reprogram( void );

	// Top-left of the 640x400 text surface, border excluded -- 0 unless the
	// display is in text mode. Text mode has one page and no flip: the
	// scanout address never moves, so a cell can be repainted in place.
	u8 *TextSurface( void );
	unsigned GetTextPitch( void ) const	{ return m_nTextPitch; }
	// Pushes text-surface rows out to DRAM, as CleanRows() does for a page.
	void CleanTextRows( unsigned y0, unsigned y1 );

	// --- border ---------------------------------------------------------
	// Paints the frame around the drawing surface on *both* pages: the
	// border is a property of the display, not of the page being animated,
	// so a PAGE_FLIP must never make it change colour.
	void SetBorder( u8 nColor );
	u8 GetBorder( void ) const		{ return m_nBorder; }

	// --- palette --------------------------------------------------------
	void SetPaletteEntry( u8 nIndex, u8 r, u8 g, u8 b );
	boolean CommitPalette( void );
	// The boot palette: the C64 16 in 0-15, black through 254, white at
	// GPU64_LOG_INK. Commits it unless the display is not up yet.
	void ResetPalette( void );
	// gpu64: the palette as the ARM last set it, 3 bytes an entry. Circle's
	// CBcmFrameBuffer takes palette writes and never gives them back, and
	// milestone 6's BUILD_COLORMAP has to search the palette for nearest
	// matches -- so this class keeps a shadow copy. Always 256 entries.
	const u8 *GetPaletteRGB( void ) const	{ return m_Palette; }

	// --- firmware-drawn text --------------------------------------------
	// 8x8 glyphs from the C64 character ROM onto the draw page, addressed in
	// 40x25 character cells and clipped to that grid.
	void DrawC64Text( unsigned nCol, unsigned nRow, const char *pString,
			  u8 nInk, u8 nPaper );
	// What HDMI shows while the C64 has no clock -- see gpu64_deadClockEnter()
	// in rad_reu.cpp.
	void ShowHoldingScreen( void );

	// --- on-screen log overlay -----------------------------------------
	void LogWrite( const char *pString, unsigned nLength );
	void LogEnable( boolean bEnable );
	boolean LogEnabled( void ) const	{ return m_bLogEnabled; }

	// Pushes CPU-side writes out to DRAM. The VideoCore scans out DRAM
	// directly and is not cache-coherent with the ARM core, so nothing drawn
	// is visible until this runs. Rows are relative to the drawing surface,
	// not to the physical page; whole physical rows get cleaned either way,
	// which costs nothing extra and keeps the border covered.
	void CleanRows( unsigned nPage, unsigned y0, unsigned y1 );
	// One row's [x0, x1) only -- lets a caller split a clean to fit the
	// core-1 store-burst budget (gpu64_3d_span.h).
	void CleanRowSpan( unsigned nPage, unsigned y, unsigned x0, unsigned x1 );
	// Whole physical page, border included.
	void CleanPage( unsigned nPage );

private:
	void DrawLogOverlay( unsigned nPage );
	void LogChar( char c );
	// Programs the VideoCore for whichever of the two framebuffers is
	// named, re-reads its base address and pitch, and repaints the border.
	boolean ActivateGraphics( void );
	void FreshGraphics( void );
	boolean ActivateText( void );
	void PaintTextBorder( void );

	CBcmFrameBuffer	*m_pFB;
	u8		*m_pBuffer;
	unsigned	m_nPitch;
	boolean		m_bInitialized;

	// The text-mode display. A second CBcmFrameBuffer rather than a
	// reshaped first one: the graphics object's geometry, pitch and page
	// stride are what every hardware-verified class 1/2 path is written
	// against, and mode switching must not perturb them. Created on the
	// first switch into text mode and kept afterwards -- the object is a
	// few hundred bytes of tag block, and the VideoCore's own buffer is
	// reallocated on each Initialize() either way.
	CBcmFrameBuffer	*m_pFBText;
	u8		*m_pTextBuffer;
	unsigned	m_nTextPitch;
	u8		m_nMode;

	u8		m_nDrawPage;
	u8		m_nVisiblePage;
	// Page PrepareFlip() readied and CommitFlip() will make visible.
	u8		m_nPendingVisible;
	// Pages CommitFlip() moved off the screen whose scanout may still be
	// running (bit per page), and the system-timer stamp of each post. See
	// NextDrawPageIdle().
	u8		m_nRetiringMask;
	u32		m_nRetiredAt[ GPU64_FB_PAGES ];

	u8		m_nBorder;

	u8		m_Palette[ 256 * 3 ];

	boolean		m_bLogEnabled;
	char		m_LogText[ GPU64_LOG_ROWS ][ GPU64_LOG_COLS ];
	unsigned	m_nLogRow;
	unsigned	m_nLogCol;
};

// gpu64: single instance, owned by CRAD (rad_main.h), reachable from the
// bus-watch loop and the API dispatcher without dragging in rad_main.h.
extern CGpu64FrameBuffer *g_pGpu64FB;

#endif

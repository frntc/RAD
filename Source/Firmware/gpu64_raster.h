// Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
/*
 gpu64 milestone 8 -- the class 2 (raster) subsystem, public interface.

 Wire protocol: docs/api_design.md, "Class 2 opcodes". Rationale and as-built
 record: dev/milestone8_raster_design.md.

 Class 2 is the column/span/sprite layer a Doom-shaped renderer needs: the
 C64 decides what to draw and emits batches of 16-byte records, gpu64 turns
 them into pixels. It runs synchronously on core 0 inside the dispatch
 window, exactly like a class 0 draw op -- see the design doc on why it is
 neither a class 0 opcode nor part of class 1.
*/
#ifndef _gpu64_raster_h
#define _gpu64_raster_h

#include <circle/types.h>

// gpu64: the whole class 2 subsystem behind one toggle, the same way class 1
// and the ladder are. Off = gpu64_apiDispatch() answers class 2 with
// BAD_CLASS and nothing below is linked in. This is the bisect handle if
// anything in class 0 or class 1 regresses.
#define GPU64_RASTER_ENABLED

// --- class 2 opcodes ----------------------------------------------------
// System -- $00-$0F
#define GPU64_RASTER_OP_RESET		0x00
#define GPU64_RASTER_OP_SET_VIEW	0x01
#define GPU64_RASTER_OP_SET_COLORMAP	0x02
#define GPU64_RASTER_OP_STATS		0x03
#define GPU64_RASTER_OP_FILL_VIEW	0x04
#define GPU64_RASTER_OP_SET_CAMERA	0x05
#define GPU64_RASTER_OP_SET_SECTORS	0x06
#define GPU64_RASTER_OP_SET_CAMERA3D	0x07
#define GPU64_RASTER_OP_SET_LIGHT	0x08

// Resources -- $10-$1F
#define GPU64_RASTER_OP_UPLOAD_TEXTURE	0x10
#define GPU64_RASTER_OP_FREE_TEXTURE	0x11
#define GPU64_RASTER_OP_UPLOAD_VERTS	0x12
#define GPU64_RASTER_OP_UPLOAD_TEXINFO	0x13
#define GPU64_RASTER_OP_UPLOAD_POLYS	0x14

// Batches -- $20-$2F
#define GPU64_RASTER_OP_DRAW_COLUMNS	0x20
#define GPU64_RASTER_OP_DRAW_SPANS	0x21
#define GPU64_RASTER_OP_DRAW_SPRITE	0x22
#define GPU64_RASTER_OP_DRAW_WALLS	0x23
#define GPU64_RASTER_OP_DRAW_SECTORS	0x24
#define GPU64_RASTER_OP_DRAW_THINGS	0x25
#define GPU64_RASTER_OP_DRAW_POLYS	0x26
#define GPU64_RASTER_OP_DRAW_WORLD	0x27

// DRAW_COLUMNS / DRAW_SPANS flag bits (ARG8).
#define GPU64_RASTER_BATCH_CHECKSUM	0x01

#ifdef GPU64_RASTER_ENABLED

// Session reset -- called from gpu64_apiReset(), i.e. on every resetREU().
// Frees every class 2 texture, returns the view to the full surface and the
// colormap to identity. Same lifecycle rule class 1 follows: a RUN/STOP must
// not leave the next program running against stale IDs.
void gpu64_rasterReset( void );

// Executes one class 2 command on core 0 and returns a GPU64_ERR_* code.
u8 gpu64_rasterDispatch( u8 op );

// Formats the subsystem's counters for the on-screen log, on the same
// LOG_ENABLE(1) hook the flip stats and class 1's counters ride.
char *gpu64_rasterReport( char *p );

#endif	// GPU64_RASTER_ENABLED

#endif

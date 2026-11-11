;
;       {_______            {_          {______
;             {__          {_ __               {__
;             {__         {_  {__               {__
;          {__           {__   {__               {__
;      {______          {__     {__              {__
;            {__       {__       {__            {__
;              {_________         {______________		Expansion Unit
;
;      gpu64 -- equates and macros shared by the demonstration programs.
;
;      This file is not a program. It is the part of a gpu64 program that is
;      the same in every gpu64 program: the register window, the opcode
;      numbers, the error codes, and four macros for staging an argument
;      block. Copy it into your own project and you can start at the first
;      interesting line.
;
;      Everything here comes straight from docs/api_design.md. Nothing in it
;      is cleverer than that document -- if the two ever disagree, the
;      document is right.
;
;      THE WHOLE PROTOCOL, IN FOUR LINES
;
;              lda #0
;              sta CMD_HI      ; class 0 -- sticky, so once per program
;              #argb 0, 6      ; stage arguments, any order
;              #cmd OP_CLEAR   ; the write to CMD_LO fires the command
;
;      The write to CMD_LO halts the C64 until the command has finished, so
;      there is nothing to poll and nothing to wait for. Read ERRCODE after
;      it if you want to know how it went; every dispatch writes it, so a
;      stale value can never look like a fresh failure.
;
;      Copyright (c) 2026 Honza Slesinger
;
;      This program is free software: you can redistribute it and/or modify
;      it under the terms of the GNU General Public License as published by
;      the Free Software Foundation, either version 3 of the License, or
;      (at your option) any later version.
;

; ----------------------------------------------------------------------
; The register window. gpu64 owns $DF50-$DFFF in IO2; the REU keeps
; $DF00-$DF0A and IO1 ($DE00-$DEFF) is untouched.
; ----------------------------------------------------------------------

CMD_HI   = $df50		; class selector, sticky
CMD_LO   = $df51		; opcode -- WRITING THIS FIRES THE COMMAND
STATUS   = $df52		; bit0 busy, bit1 error, bit2 vblank, bit3 armed
ERRCODE  = $df53		; result of the last dispatch
ID_LO    = $df54
ID_HI    = $df55
ARG      = $df56		; ARG0..ARG15 = $df56..$df65
SEQ      = $df67		; command sequence number (see docs)
SEQACK   = $df68		; echoes SEQ once the command has dispatched
RESULT   = $df66		; readable; the only way to check an ARG landed

ST_BUSY		= $01
ST_ERROR	= $02
ST_VB_PEND	= $04
ST_VB_ARMED	= $08
ST_FRAME_READY	= $10		; class 1 loop: a finished frame is waiting

; --- class 0 opcodes ---------------------------------------------------
OP_NOP		= $00
OP_RESET_STATE	= $01
OP_VBLANK_ARM	= $02
OP_VBLANK_ACK	= $03
OP_SET_DRAW_PAGE = $04
OP_PAGE_FLIP	= $05
OP_GET_INFO	= $06
OP_LOG_ENABLE	= $07
OP_SET_BORDER	= $08
OP_VBLANK_SYNC	= $09
OP_GET_HEALTH	= $0a
OP_FULL_RESET	= $0b
OP_SET_DMA_WINDOW = $0c	; where a readback is allowed to write
OP_CLEAR	= $10
OP_SET_PIXEL	= $20
OP_LINE		= $21
OP_RECT		= $22
OP_RECT_FILL	= $23
OP_PAL_SET	= $30
OP_PAL_LOAD	= $31
OP_BLIT		= $40
OP_BLIT_KEYED	= $41
OP_READ_RECT	= $42

; --- class 0 text mode -- $50-$5F --------------------------------------
; The 80x50 character screen. It is a DISPLAY MODE, not a drawing op: while
; it is up the framebuffer commands above have nothing to draw into, a page
; flip answers UNSUPPORTED, and so does every class 1 and class 2 command.
; TEXT_MODE 0 goes back to graphics, and costs nothing if you are already
; there -- which is why dmInit issues it.
OP_TEXT_MODE	= $50
OP_TEXT_CLEAR	= $51
OP_TEXT_PUT	= $52
OP_TEXT_WRITE	= $53
OP_TEXT_UPLOAD	= $54
OP_TEXT_CHARSET	= $55
OP_TEXT_SCROLL	= $56
OP_TEXT_FILL	= $57
OP_TEXT_REFRESH	= $58

; Fixed point 8.8 ($8x) and IEEE float32 ($9x) are the same operations on
; different element formats, so they share a low nibble.
OP_MAT_MUL	= $80
OP_MAT_ADD	= $81
OP_MAT_SUB	= $82
OP_MAT_SCALE	= $83
OP_MAT_TRANSPOSE = $84
OP_MAT_IDENTITY	= $85
OP_MAT_INVERSE	= $86
OP_FLT_MUL	= $90
OP_FLT_ADD	= $91
OP_FLT_SUB	= $92
OP_FLT_SCALE	= $93
OP_FLT_TRANSPOSE = $94
OP_FLT_IDENTITY	= $95
OP_FLT_INVERSE	= $96

; --- class 2 opcodes -- the raster layer (CMD_HI = 2) ------------------
; Columns, spans and scaled sprites: the primitives a Doom-shaped renderer
; needs. See docs/api_design.md, "Class 2 opcodes".
R2_RESET	= $00
R2_SET_VIEW	= $01
R2_SET_COLORMAP	= $02
R2_STATS	= $03
R2_FILL_VIEW	= $04
R2_SET_CAMERA	= $05
R2_SET_SECTORS	= $06
R2_SET_CAMERA3D	= $07
R2_SET_LIGHT	= $08
R2_UPLOAD_TEX	= $10
R2_FREE_TEX	= $11
R2_UPLOAD_VERTS	= $12
R2_UPLOAD_TEXINFO = $13
R2_UPLOAD_POLYS	= $14
R2_DRAW_COLUMNS	= $20
R2_DRAW_SPANS	= $21
R2_DRAW_SPRITE	= $22
R2_DRAW_WALLS	= $23
R2_DRAW_SECTORS	= $24
R2_DRAW_THINGS	= $25
R2_DRAW_POLYS	= $26
R2_DRAW_WORLD	= $27

; Record stride, both batch kinds -- a power of two so indexing record n is
; a shift and not a multiply.
R2_REC		= 16

; DRAW_SECTORS' own stride, and the sector record. Thirty-two bytes because a
; two-sided wall names two sectors and three textures and none of that fits
; beside the geometry in sixteen -- and because a level is uploaded once, so
; the extra bytes are never paid for again.
R2_WALL2	= 32
R2_SEC		= 8
R2_NO_SECTOR	= $ff		; backSec: this wall is solid

; Column record flags (byte 14), sprite flags (ARG14), batch flags (ARG8).
R2_COL_MASKED	= $01
R2_SPR_MASKED	= $01
R2_SPR_FLIPX	= $02
R2_BATCH_SUM	= $01
R2_BATCH_CAM3D	= $02		; DRAW_THINGS: project through SET_CAMERA3D
				; instead of SET_CAMERA

; Wall record flags (byte 14) and SET_CAMERA flags (ARG5).
R2_WALL_MASKED	= $01
R2_WALL_FLATLIT	= $02
R2_CAM_PAINT	= $01

; DRAW_SECTORS wall flags (byte 15) and sector flags (byte 7).
R2_WALL_NOFLATS	= $04		; this wall's columns paint no flats
R2_SEC_SKY	= $01		; ceiling not painted, no upper band drawn

; DRAW_THINGS record flags (byte 12). A separate set from the DRAW_SPRITE
; ones because a thing needs four bits and FLIPX and FLATLIT would otherwise
; both want $02.
R2_THING_MASKED	 = $01
R2_THING_FLIPX	 = $02
R2_THING_NODEPTH = $04		; ignore the depth buffer: neither tested
				; nor written, so the thing is on top
R2_THING_FLATLIT = $08
R2_THING_DIR	 = $10		; eight views: tex is the FIRST of eight
				; consecutive ids and byte 13 is the
				; direction the thing faces. gpu64 picks
				; the view from where the camera stands.

; DRAW_POLYS. A face record is 16 bytes and indexes the vertex pool and the
; texinfo table; a vertex is 8 bytes and a texinfo 16.
R2_POLY		 = 16
R2_VERT		 = 8
R2_TEXINFO	 = 16
R2_POLY_MASKED	 = $01
R2_POLY_FLATLIT	 = $02
R2_POLY_TWOSIDED = $04		; no backface cull: drawn from either side

; --- class 1 opcodes -- the 3D scene (CMD_HI = 1) ----------------------
; Prefixed C1_ because class 2 reuses the same numbers for other things.
; The destructive four (SCENE_RESET, LOOP_STOP, DESTROY_NODE, FREE_RESOURCE)
; need ARG15 = C1_KEY on the same dispatch; see docs/class1-3d-mesh-reference.md.
C1_KEY			= $a5
C1_SCENE_RESET		= $00
C1_SET_VIEWPORT		= $01
C1_SET_PERSPECTIVE	= $02
C1_SET_LIGHT		= $03
C1_BUILD_COLORMAP	= $04
C1_SET_BACKGROUND	= $05
C1_LOOP_START		= $06
C1_LOOP_STOP		= $07
C1_SCENE_COMMIT		= $08
C1_ARENA_STATUS		= $09
C1_UPLOAD_MESH		= $10
C1_UPLOAD_TEXTURE	= $11
C1_FREE_RESOURCE	= $12
C1_CREATE_OBJECT	= $20
C1_CREATE_CAMERA	= $21
C1_DESTROY_NODE		= $22
C1_SET_ACTIVE_CAMERA	= $23
C1_SET_VISIBLE		= $24
C1_CREATE_SPRITE	= $25
C1_CREATE_LIGHT		= $26
C1_SET_SPRITE		= $27
C1_SET_POINT_LIGHT	= $28
C1_SET_POSITION		= $30
C1_SET_ORIENTATION	= $31
C1_MOVE_LOCAL		= $32
C1_MOVE_WORLD		= $33
C1_ROTATE_LOCAL		= $34
C1_SET_SCALE		= $35
C1_GET_TRANSFORM	= $36
C1_CLEAR_VIEWPORT	= $40
C1_DRAW_MESH		= $41
C1_DRAW_NODE		= $42

; Fire a class 1 command, selecting the class first.
cmdc1	.macro	op
	lda #1
	sta CMD_HI
	lda #\op
	sta CMD_LO
	.endm

; texid 0 in a record is not an error: it means "solid colour", the palette
; index being the low byte of the record's u field.
R2_SOLID	= 0

; --- error codes -------------------------------------------------------
ERR_OK		= $00
ERR_BAD_OPCODE	= $01
ERR_BAD_CLASS	= $02
ERR_OUT_OF_RANGE = $03
ERR_BAD_ARGS	= $04
ERR_SINGULAR	= $05
ERR_UNSUPPORTED	= $06
ERR_BUSY	= $07
ERR_OUT_OF_MEMORY = $08		; class 2: the texture arena is full
ERR_QUEUE_FULL	= $09
ERR_BAD_ID	= $0a		; class 2: a texture id of 0, past 255, or dead
ERR_NO_CAMERA	= $0b		; class 1: the scene has no active camera
ERR_WORKER_TIMEOUT = $0c

; --- geometry and spaces -----------------------------------------------
FB_W		= 320
FB_H		= 200

; The text screen: 80 columns by 50 rows of 8x8 ROM glyphs, 640x400 pixels.
; A cell is addressed as row * TXT_COLS + col wherever a plane is uploaded.
TXT_COLS	= 80
TXT_ROWS	= 50
TXT_CELLS	= TXT_COLS * TXT_ROWS

; The three parallel planes, one byte per cell, which TEXT_UPLOAD writes.
TXT_PLANE_SCREEN = 0
TXT_PLANE_FG	 = 1
TXT_PLANE_BG	 = 2

; TEXT_WRITE flags: take the bytes as ASCII and translate them, rather than
; as screen codes that are already what the ROM is indexed by.
TXT_ASCII	= $01

; Screen code bit 7 is reverse video, exactly as it is at $0400.
TXT_REVERSE	= $80

; The two halves of the character ROM, as the C64 itself numbers them.
TXT_SET_UPPER	= 0		; upper case and graphics -- the boot set
TXT_SET_LOWER	= 1		; lower case and upper case
SPACE_C64	= 0
SPACE_REU	= 1

; --- C64 colour numbers, which are also gpu64's reset palette ----------
BLACK = 0
WHITE = 1
RED = 2
CYAN = 3
PURPLE = 4
GREEN = 5
BLUE = 6
YELLOW = 7
ORANGE = 8
BROWN = 9
LTRED = 10
DKGREY = 11
GREY = 12
LTGREEN = 13
LTBLUE = 14
LTGREY = 15

SCREEN   = $0400

; Zero page. $f7-$fe is the tape/RS232 area, free while a PRG runs.
SRC      = $fb
DST      = $fd

; ----------------------------------------------------------------------
; Argument staging.
;
; ARG registers are write-only and are NOT cleared between commands. Every
; opcode reads exactly the byte count its table row lists and never looks
; past it -- so leftovers cannot leak in, but you must stage every byte the
; opcode reads, every time. These macros make an argument block read top to
; bottom in the same order the reference lists it.
; ----------------------------------------------------------------------

; One byte of argument.
argb	.macro	off, val
	lda #\val
	sta ARG+\off
	.endm

; A 16-bit argument, low byte first as a 6502 expects. Coordinates are
; signed; w/h are unsigned.
argw	.macro	off, val
	lda #<(\val)
	sta ARG+\off
	lda #>(\val)
	sta ARG+\off+1
	.endm

; The 6-byte blob descriptor -- space, 24-bit address, 16-bit length. Bulk
; data never crosses the register window byte by byte: you say where the
; bytes already live and gpu64 fetches them itself. The same descriptor is
; also a destination, for the commands that hand data back.
argblob	.macro	off, space, addr, len
	lda #\space
	sta ARG+\off
	lda #<(\addr)
	sta ARG+\off+1
	lda #>(\addr)
	sta ARG+\off+2
	lda #0
	sta ARG+\off+3
	lda #<(\len)
	sta ARG+\off+4
	lda #>(\len)
	sta ARG+\off+5
	.endm

; The 4-byte compact descriptor the math opcodes use: no length, because
; the dimensions and the element size already imply it.
argcd	.macro	off, space, addr
	lda #\space
	sta ARG+\off
	lda #<(\addr)
	sta ARG+\off+1
	lda #>(\addr)
	sta ARG+\off+2
	lda #0
	sta ARG+\off+3
	.endm



; ----------------------------------------------------------------------
; Class 1 staging.
;
; Class 1 addresses a node or a resource through ID_LO/ID_HI, which is a
; 16-bit register pair of its own and NOT part of the ARG window -- so an
; id is staged with #id16 and everything else with the macros above.
; ----------------------------------------------------------------------

id16	.macro	val
	lda #<(\val)
	sta ID_LO
	lda #>(\val)
	sta ID_HI
	.endm

; A 16.16 world coordinate staged from an 8.8 CONSTANT, which is how a
; program that thinks in 8.8 talks to SET_POSITION. SET_POSITION is 16.16
; and not 8.8 because a scene node may stand anywhere in a 65536-unit
; world, where DRAW_MESH's immediate-mode offset only ever had to reach
; across one model.
arg1616	.macro	off, val
	lda #0
	sta ARG+\off
	lda #<(\val)
	sta ARG+\off+1
	lda #>(\val)
	sta ARG+\off+2
	lda #(((\val) < 0) ? $ff : $00)
	sta ARG+\off+3
	.endm

; ----------------------------------------------------------------------
; Text mode staging.
;
; Unlike the class 2 record macros below, these FIRE the command as well as
; staging it: a text screen is built out of whole commands, not out of
; records. None of them is legal until TEXT_MODE has been accepted -- but
; they are all safe to issue with the graphics screen still on the display,
; because the planes are updated either way and appear the moment the mode
; changes. That is how a program composes a screen before showing it.
; ----------------------------------------------------------------------

; One cell. Off-grid col/row is clipped, not an error -- SET_PIXEL's
; contract, for the same reason: a moving cursor should not need edge cases.
txtput	.macro	col, row, code, fg, bg
	#argb 0, \col
	#argb 1, \row
	#argb 2, \code
	#argb 3, \fg
	#argb 4, \bg
	#cmd OP_TEXT_PUT
	.endm

; A rectangle of cells, one code and one colour pair. col and row are
; signed, w and h unsigned, and the rectangle clips exactly as RECT_FILL
; does. Code $20 with the colours you want is how a panel gets painted.
txtfill	.macro	col, row, w, h, code, fg, bg
	#argb 0, \col
	#argb 1, \row
	#argb 2, \w
	#argb 3, \h
	#argb 4, \code
	#argb 5, \fg
	#argb 6, \bg
	#cmd OP_TEXT_FILL
	.endm

; A string at col,row, from C64 RAM. The bytes are ASCII by default and
; gpu64 translates them for whichever character set is up; clear \flags to
; send screen codes instead. The line is truncated at the right-hand edge,
; never wrapped, and at most one row (80 bytes) ever lands.
txtw	.macro	col, row, addr, len, fg, bg, flags=TXT_ASCII
	#argblob 0, SPACE_C64, \addr, \len
	#argb 6, \col
	#argb 7, \row
	#argb 8, \fg
	#argb 9, \bg
	#argb 10, \flags
	#cmd OP_TEXT_WRITE
	.endm

; A run of raw plane bytes starting at cell \first, which is row*80+col.
; This is the bulk path -- screen codes, foregrounds or backgrounds
; straight out of C64 RAM or the REU, with no per-cell dispatch. The run is
; linear across the whole 4000-cell plane, so it wraps from the end of one
; row into the start of the next.
txtup	.macro	plane, first, addr, len, space=SPACE_C64
	#argblob 0, \space, \addr, \len
	#argb 6, \plane
	#argw 7, \first
	#cmd OP_TEXT_UPLOAD
	.endm

; ----------------------------------------------------------------------
; Class 2 record staging.
;
; These write a 16-byte record into a batch array in your own RAM -- they
; do NOT touch the ARG window. A batch is built once and then handed to
; gpu64 as a single command, which is the entire point of the class: 800
; primitives cost one dispatch, not 800.
;
; \rec is the address of the record. Use them for a fixed batch; a real
; renderer writes the same bytes from a loop.
; ----------------------------------------------------------------------

; A wall column: screen column \x, rows \y0..\y1 inclusive, sampling
; texture column \u of texture \tex from v = \v stepping \dv per row.
r2col	.macro	rec, x, y0, y1, tex, light, u, v, dv, flags=0
	lda #<(\x)
	sta \rec+0
	lda #>(\x)
	sta \rec+1
	lda #<(\y0)
	sta \rec+2
	lda #>(\y0)
	sta \rec+3
	lda #<(\y1)
	sta \rec+4
	lda #>(\y1)
	sta \rec+5
	lda #\tex
	sta \rec+6
	lda #\light
	sta \rec+7
	lda #<(\u)
	sta \rec+8
	lda #>(\u)
	sta \rec+9
	lda #<(\v)
	sta \rec+10
	lda #>(\v)
	sta \rec+11
	lda #<(\dv)
	sta \rec+12
	lda #>(\dv)
	sta \rec+13
	lda #\flags
	sta \rec+14
	lda #0
	sta \rec+15
	.endm

; A floor/ceiling span: screen row \y, columns \x0..\x1 inclusive,
; sampling from u,v stepping du,dv per column. All four are 8.8.
r2span	.macro	rec, y, x0, x1, tex, light, u, v, du, dv
	lda #<(\y)
	sta \rec+0
	lda #>(\y)
	sta \rec+1
	lda #<(\x0)
	sta \rec+2
	lda #>(\x0)
	sta \rec+3
	lda #<(\x1)
	sta \rec+4
	lda #>(\x1)
	sta \rec+5
	lda #\tex
	sta \rec+6
	lda #\light
	sta \rec+7
	lda #<(\u)
	sta \rec+8
	lda #>(\u)
	sta \rec+9
	lda #<(\v)
	sta \rec+10
	lda #>(\v)
	sta \rec+11
	lda #<(\du)
	sta \rec+12
	lda #>(\du)
	sta \rec+13
	lda #<(\dv)
	sta \rec+14
	lda #>(\dv)
	sta \rec+15
	.endm

; The batch command itself: where the records are, how many, and whether
; the last two bytes are a checksum.
; The camera, for DRAW_WALLS. Angle is binary -- 256 to the circle, 0 looking
; along +x. Heights and the projection distance are 8.8; the projection
; distance is in pixels, so 160.0 across a 320-wide view is a 90-degree field
; of view.
r2cam	.macro	x, y, ang, eye, ceil, proj, flags=0, floorc=0, ceilc=0, horizon=0
	#argw 0, \x
	#argw 2, \y
	#argb 4, \ang
	#argb 5, \flags
	#argw 6, \eye
	#argw 8, \ceil
	#argw 10, \proj
	#argb 12, \floorc
	#argb 13, \ceilc
	#argb 14, \horizon
	.endm

; A wall segment in WORLD space -- it does not change when the player moves,
; which is the whole point of it. u1 and u2 are the texture coordinate at
; each endpoint, so no length and no square root is ever needed. Winding
; decides which side is the front: a wall is only drawn from the side that
; projects it left to right.
r2wall	.macro	rec, x1, y1, x2, y2, tex, light, u1, u2, flags=0
	lda #<(\x1)
	sta \rec+0
	lda #>(\x1)
	sta \rec+1
	lda #<(\y1)
	sta \rec+2
	lda #>(\y1)
	sta \rec+3
	lda #<(\x2)
	sta \rec+4
	lda #>(\x2)
	sta \rec+5
	lda #<(\y2)
	sta \rec+6
	lda #>(\y2)
	sta \rec+7
	lda #\tex
	sta \rec+8
	lda #\light
	sta \rec+9
	lda #<(\u1)
	sta \rec+10
	lda #>(\u1)
	sta \rec+11
	lda #<(\u2)
	sta \rec+12
	lda #>(\u2)
	sta \rec+13
	lda #\flags
	sta \rec+14
	lda #0
	sta \rec+15
	.endm

; --- DRAW_SECTORS -----------------------------------------------------
; A level with steps in it. Heights come from a sector table instead of from
; the camera, and a wall may be two-sided: it then draws a band above the far
; ceiling and a band below the far floor and leaves the middle see-through.
;
; These two emit DATA, not stores, because a level does not change. Build it
; once at assembly time, upload it once, and a frame costs the camera alone.
;
; For DRAW_SECTORS the camera's eye height is an ABSOLUTE world height, not
; a height above a floor -- a level with steps has no one floor to measure
; from. SET_CAMERA still validates eye > 0 and ceil > eye, so send the
; level's tallest ceiling as ceil; DRAW_SECTORS does not read it.
r2secd	.macro	floor, ceil, floorc, ceilc, light, flags=0
	.word	\floor, \ceil
	.byte	\floorc, \ceilc, \light, \flags
	.endm

; One wall. front/back are sector ids, back = R2_NO_SECTOR for a solid wall.
; texm/texu/texl are the middle, upper and lower textures, 0 for "the flat
; colour in colm/colu/coll instead". Winding decides the front face, exactly
; as in DRAW_WALLS.
r2walld	.macro	x1, y1, x2, y2, u1, u2, front, back, light, flags=0, texm=0, texu=0, texl=0, colm=0, colu=0, coll=0
	.word	\x1, \y1, \x2, \y2, \u1, \u2
	.byte	\front, \back, \light, \flags
	.byte	\texm, \texu, \texl
	.byte	\colm, \colu, \coll
	.byte	0, 0
	.fill	8, 0			; reserved -- must be zero
	.endm

; SET_SECTORS: where the sector table is and how many sectors it holds.
; count = 0 drops the table.
r2sectors .macro addr, count
	#argblob 0, SPACE_C64, \addr, (\count)*R2_SEC
	#argw 6, \count
	.endm

; One thing: a billboard at a world position, depth-tested against whatever
; DRAW_SECTORS left in the depth buffer -- which is what puts a monster
; behind a wall. Send this batch AFTER the DRAW_SECTORS for the same frame.
;
; x,y are world 8.8 and base is the ABSOLUTE height of the sprite's bottom,
; the same measure the sector table's floor heights use -- so a thing
; standing on the floor of sector n has base = that sector's floor. w and h
; are its size in world units, 8.8: the card always faces the camera, so w is
; measured straight across the view and there is no rotation to send.
;
; A thing is always textured; tex = 0 is a rejected record.
;
; With R2_THING_DIR, tex is the first of EIGHT consecutive texture ids -- the
; thing seen from the front, then every 45 degrees round it -- and face is the
; direction the thing itself is pointing, in the same 256-to-the-circle units
; the camera's angles use. gpu64 works out which of the eight you are looking
; at. Without the flag, face is ignored and the one texture is drawn from
; every side.
r2thingd .macro	x, y, base, w, h, tex, light, flags=0, face=0
	.word	(\x) & $ffff, (\y) & $ffff, (\base) & $ffff, \h, \w
	.byte	\tex, \light, \flags
	.byte	\face			; facing, R2_THING_DIR only
	.byte	0, 0			; reserved -- must be zero
	.endm

; --- DRAW_POLYS -------------------------------------------------------
; A convex polygon anywhere in space at any angle: what a Doom level cannot
; express and a Quake level is made of. These emit DATA for the same reason
; the sector macros do -- a level is built at assembly time, uploaded once,
; and a frame costs the camera and the face list.

; The 3D camera. Position is world 8.8 with z up, measured the way the
; sector table's absolute floor heights are. Yaw is binary, 256 to the
; circle, 0 looking along +x. Pitch is the same units and is a real
; rotation, positive looking up -- not SET_CAMERA's y-shear.
r2cam3	.macro	x, y, z, yaw, pitch, proj, flags=0
	#argw 0, \x
	#argw 2, \y
	#argw 4, \z
	#argb 6, \yaw
	#argb 7, \pitch
	#argw 8, \proj
	#argb 10, \flags
	.endm

; Milestone 12. One dynamic point light, SET_LIGHT. Radius and strength are
; both inline: a muzzle flash has to be sendable every frame without a DMA.
; A radius or a strength of zero turns the slot off.
r2light	.macro	slot, x, y, z, radius, strength
	#argb 0, \slot
	#argw 1, \x
	#argw 3, \y
	#argw 5, \z
	#argw 7, \radius
	#argb 9, \strength
	.endm

; One vertex of the pool. Eight bytes and not six so the 6502 indexes the
; table with a shift.
r2vertd	.macro	x, y, z
	.word	(\x) & $ffff, (\y) & $ffff, (\z) & $ffff, 0
	.endm

; One texinfo: Quake's two texture axes. s = ( P . sAxis ) >> 8 + sOff, in
; 8.8 texels, so two faces naming one texinfo align whatever their shapes.
r2texinfod .macro sx, sy, sz, soff, tx, ty, tz, toff
	.word	(\sx) & $ffff, (\sy) & $ffff, (\sz) & $ffff, (\soff) & $ffff
	.word	(\tx) & $ffff, (\ty) & $ffff, (\tz) & $ffff, (\toff) & $ffff
	.endm

; One face: a convex polygon whose vertices are consecutive in the pool
; starting at `first`, wound clockwise on screen when seen from the front.
; tex = 0 draws the flat colour in `col` and needs no texinfo.
r2polyd	.macro	first, nverts, texinfo, tex, col, light, flags=0
	.word	\first
	.byte	\nverts, \texinfo, \tex, \col, \light, \flags
	.fill	8, 0			; reserved -- must be zero
	.endm

r2verts	.macro	addr, count
	#argblob 0, SPACE_C64, \addr, (\count)*R2_VERT
	#argw 6, \count
	.endm

r2texinfo .macro addr, count
	#argblob 0, SPACE_C64, \addr, (\count)*R2_TEXINFO
	#argw 6, \count
	.endm

; UPLOAD_POLYS / DRAW_WORLD: the resident world. The faces go up once at
; start-up and a frame names a range of them, so a level costs no per-frame
; transfer at all. `first` is the destination slot, so a level bigger than
; the staging buffer arrives in chunks; count = 0 at first = 0 drops it.
r2world	.macro	addr, count, first=0
	#argblob 0, SPACE_C64, \addr, (\count)*R2_POLY
	#argw 6, \count
	#argw 8, \first
	.endm

r2drawworld .macro first, count, key=0
	#argw 0, \first
	#argw 2, \count
	#argb 9, \key
	.endm

r2batchp .macro	addr, count, flags=0, key=0
	#argblob 0, SPACE_C64, \addr, (\count)*R2_POLY+((\flags)&1)*2
	#argw 6, \count
	#argb 8, \flags
	#argb 9, \key
	.endm

; DRAW_SECTORS' batch, which is r2batch with the other stride.
r2batch2 .macro	addr, count, flags=0, key=0
	#argblob 0, SPACE_C64, \addr, (\count)*R2_WALL2+((\flags)&1)*2
	#argw 6, \count
	#argb 8, \flags
	#argb 9, \key
	.endm

r2batch	.macro	addr, count, flags=0, key=0
	#argblob 0, SPACE_C64, \addr, (\count)*R2_REC+((\flags)&1)*2
	#argw 6, \count
	#argb 8, \flags
	#argb 9, \key
	.endm

; Fire the command. This is the write that halts the C64 and does the work.
cmd	.macro	op
	lda #\op
	sta CMD_LO
	.endm

; The same, for a class 2 (raster) command. CMD_HI is sticky, so strictly a
; demo only has to write it when the class changes -- this writes it every
; time, which costs one store and removes a whole category of bookkeeping
; bug from a program that interleaves classes.
cmd2	.macro	op
	lda #2
	sta CMD_HI
	lda #\op
	sta CMD_LO
	.endm

; And back to class 0.
cmd0	.macro	op
	lda #0
	sta CMD_HI
	lda #\op
	sta CMD_LO
	.endm

; A 16-bit value staged from a variable rather than a constant.
argwv	.macro	off, var
	lda \var
	sta ARG+\off
	lda \var+1
	sta ARG+\off+1
	.endm

; ----------------------------------------------------------------------
; The BASIC stub: "10 SYS 2064", so RUN launches the code that follows.
; $0810 is the entry point every gpu64 demo and test uses, which is also
; where the host simulator (tools/prgsim) starts a program.
;
; Assemble WITHOUT --nostart. That option strips the 2-byte load address
; and produces a PRG that loads to the wrong place and silently never runs.
; ----------------------------------------------------------------------
basicStub	.macro
	* = $0801
	.byte $0b, $08, $0a, $00, $9e
	.text "2064"
	.byte $00, $00, $00
	* = $0810
	.endm

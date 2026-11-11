;
;       {_______            {_          {______
;             {__          {_ __               {__
;             {__         {_  {__               {__
;          {__           {__   {__               {__
;      {______          {__     {__              {__
;            {__       {__       {__            {__
;              {_________         {______________		Expansion Unit
;
;      gpu64 -- shared harness for the API conformance suite.
;
;      Every gpu64_test_*.a includes this. It is not a program on its own.
;
;      WHY A HARNESS
;
;      The suite's whole value is that a bench run produces a verdict, not a
;      screenful of numbers to interpret. A tester should be able to look at
;      one line from across the room and know whether to look closer. So the
;      harness owns all of the output and the tests own none of it: a test
;      computes a fail code and calls one macro.
;
;      A fail code of 0 is a pass. Anything else is printed as F<hex> and
;      the convention is that it is *the byte that was wrong* wherever there
;      is one -- almost always the unexpected ERRCODE, so a failure line
;      reads back as "this opcode answered $04 when it should have answered
;      $00" without any lookup table. Codes that cannot be an ERRCODE are
;      used for the failures that are not about ERRCODE at all:
;
;        $E1  a buffer did not match what it was compared against
;        $E2  a buffer was not uniformly filled with the expected byte
;        $E3  a single byte read back from the framebuffer was wrong
;        $E4  a value was in range when it should not have been, or the
;             other way round -- used where there is no byte to show
;        $FE  the command returned OK where an error was required. This one
;             is worth calling out: it means an error path did not fire, and
;             an error path that never fires is not a tested error path.
;
;      USING IT
;
;      Layout of a test program:
;
;              .include "gpu64_testkit.inc"    ; equates + macros
;      start   jsr kitInit
;              #title "GPU64 SOMETHING TEST"
;              ... tests ...
;              jmp kitFinish
;              .include "gpu64_testkit_rt.inc" ; the runtime, once, at the end
;
;      A test is three steps: stage arguments, dispatch, judge.
;
;              #argb 0, 6
;              lda #OP_CLEAR
;              jsr cmd0                ; A = ERRCODE
;              #wantOk                 ; X = fail code
;              #check "CLEAR"          ; prints and counts
;
;      Copyright (c) 2026 Honza Slesinger
;
;      This program is free software: you can redistribute it and/or modify
;      it under the terms of the GNU General Public License as published by
;      the Free Software Foundation, either version 3 of the License, or
;      (at your option) any later version.
;

; ----------------------------------------------------------------------
; The register window. gpu64 owns $DF50-$DFFF; REU keeps $DF00-$DF0A.
; ----------------------------------------------------------------------

CMD_HI   = $df50
CMD_LO   = $df51
STATUS   = $df52
ERRCODE  = $df53
ID_LO    = $df54
ID_HI    = $df55
ARG      = $df56
RESULT   = $df66

ST_BUSY		= $01
ST_ERROR	= $02
ST_VB_PEND	= $04
ST_VB_ARMED	= $08
ST_FRAME_RDY	= $10

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
OP_SET_DMA_WINDOW = $0c
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

; The 80x50 character screen. TEXT_MODE is a DISPLAY mode, not a drawing
; op: while it is up there is no framebuffer page, so PAGE_FLIP and every
; class 1 and class 2 command answer UNSUPPORTED. The plane ops are legal
; in either mode -- with the graphics display up they update the planes and
; paint nothing, which is how a program stages a whole screen and has it
; appear complete at the switch. $59-$5F are undefined.
OP_TEXT_MODE	= $50
OP_TEXT_CLEAR	= $51
OP_TEXT_PUT	= $52
OP_TEXT_WRITE	= $53
OP_TEXT_UPLOAD	= $54
OP_TEXT_CHARSET	= $55
OP_TEXT_SCROLL	= $56
OP_TEXT_FILL	= $57
OP_TEXT_REFRESH	= $58

; Fixed point 8.8 ($8x) and IEEE float32 ($9x) share a low nibble.
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
R2_UPLOAD_POLYS = $14
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

; Milestone 13. The resident world: faces uploaded once and drawn by range,
; so a frame costs no bulk transfer at all. This is how many slots it has.
R2_WORLD_MAX	 = 2048

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
ERR_OUT_OF_MEM	= $08
ERR_QUEUE_FULL	= $09
ERR_BAD_ID	= $0a
ERR_NO_CAMERA	= $0b
ERR_WORKER_TIME	= $0c

; --- harness fail codes ------------------------------------------------
F_CMP		= $e1
F_FILL		= $e2
F_PIXEL		= $e3
F_RANGE		= $e4
F_NOERR		= $fe

; --- framebuffer geometry, as the API reference states it --------------
FB_W		= 320
FB_H		= 200
FB_PAGES	= 3
BORDER_W	= 32
BORDER_H	= 36

; --- text mode geometry ------------------------------------------------
; A second display geometry, not a window: 640x400 inside a 64x72 border,
; which is exactly twice the graphics figures so the framing looks the same
; on the same display. GET_INFO reports these while text mode is up, and
; that is how a program confirms the switch really happened.
TXT_COLS	= 80
TXT_ROWS	= 50
TXT_CELLS	= TXT_COLS * TXT_ROWS		; 4000
TXT_W		= 640
TXT_H		= 400
TXT_PAGES	= 1
TXT_BORDER_W	= 64
TXT_BORDER_H	= 72

; The three parallel planes TEXT_UPLOAD writes, one byte per cell, indexed
; row-major as cell = row * 80 + col.
TXT_PLANE_SCREEN = 0
TXT_PLANE_FG	= 1
TXT_PLANE_BG	= 2

; TEXT_WRITE flags: take the payload as ASCII and translate it to screen
; codes for the character set currently up. Clear it to pass screen codes
; through untouched, which is the only way to reach the graphics half of
; the ROM or to set bit 7.
TXT_ASCII	= $01

; Screen code bit 7 is reverse video, exactly as at $0400.
TXT_REVERSE	= $80

TXT_SET_UPPER	= 0			; upper case and graphics
TXT_SET_LOWER	= 1			; lower case and upper case

SPACE_C64	= 0
SPACE_REU	= 1

SCREEN   = $0400

; Zero page. $f7-$fe is the tape/RS232 area, unused while a PRG runs.
P1       = $f7
P2       = $f9
SRC      = $fb
DST      = $fd

; ----------------------------------------------------------------------
; Argument staging.
;
; ARG registers are write-only and are NOT cleared between commands, so
; every opcode must stage every byte it reads, every time. These macros
; exist so that is obvious at the call site: an opcode's argument block
; reads top to bottom in the same order the reference table lists it.
; ----------------------------------------------------------------------

argb	.macro	off, val
	lda #\val
	sta ARG+\off
	.endm

argw	.macro	off, val
	lda #<(\val)
	sta ARG+\off
	lda #>(\val)
	sta ARG+\off+1
	.endm

; The 6-byte blob descriptor: space, 24-bit addr, 16-bit len. This is how
; every bulk payload crosses -- never byte-by-byte through the window.
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

; The 4-byte compact descriptor the math opcodes use: no len, because the
; dimensions and the element size already imply it.
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

; The 3D camera, for DRAW_POLYS. Position is world 8.8 with z up, measured
; the same way the sector table's absolute floor heights are. Yaw is binary,
; 256 to the circle, 0 looking along +x -- SET_CAMERA's convention. Pitch is
; the same units and is a real rotation, positive looking up.
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

; One vertex of the pool, UPLOAD_VERTS. Eight bytes and not six so the 6502
; indexes the table with a shift.
r2vertd	.macro	x, y, z
	.word	(\x) & $ffff, (\y) & $ffff, (\z) & $ffff, 0
	.endm

; One texinfo, UPLOAD_TEXINFO: Quake's two texture axes. A face's texel
; coordinates are s = ( P . sAxis ) >> 8 + sOff, in 8.8 texels, so two faces
; naming one texinfo are guaranteed to align whatever their shapes.
r2texinfod .macro sx, sy, sz, soff, tx, ty, tz, toff
	.word	(\sx) & $ffff, (\sy) & $ffff, (\sz) & $ffff, (\soff) & $ffff
	.word	(\tx) & $ffff, (\ty) & $ffff, (\tz) & $ffff, (\toff) & $ffff
	.endm

; One face: a convex polygon, its vertices consecutive in the pool starting
; at `first`. tex = 0 draws the flat colour in `col` and needs no texinfo.
r2polyd	.macro	first, nverts, texinfo, tex, col, light, flags=0
	.word	\first
	.byte	\nverts, \texinfo, \tex, \col, \light, \flags
	.fill	8, 0			; reserved -- must be zero
	.endm

; UPLOAD_VERTS / UPLOAD_TEXINFO: where the table is and how many entries.
; count = 0 drops it.
r2verts	.macro	addr, count
	#argblob 0, SPACE_C64, \addr, (\count)*R2_VERT
	#argw 6, \count
	.endm

r2texinfo .macro addr, count
	#argblob 0, SPACE_C64, \addr, (\count)*R2_TEXINFO
	#argw 6, \count
	.endm

; DRAW_POLYS' batch. Same shape as r2batch; the stride happens to match.
r2batchp .macro	addr, count, flags=0, key=0
	#argblob 0, SPACE_C64, \addr, (\count)*R2_POLY+((\flags)&1)*2
	#argw 6, \count
	#argb 8, \flags
	#argb 9, \key
	.endm

; UPLOAD_POLYS: fill part of the resident world with poly records. `first` is
; the destination index, so a level bigger than the staging buffer arrives as
; several commands. count = 0 with first = 0 drops the pool.
r2world	.macro	addr, count, first=0
	#argblob 0, SPACE_C64, \addr, (\count)*R2_POLY
	#argw 6, \count
	#argw 8, \first
	.endm

; DRAW_WORLD: draw a range of the resident world through SET_CAMERA3D. No
; blob at all -- this is the whole point of the resident pool.
r2drawworld .macro first, count, key=0
	#argw 0, \first
	#argw 2, \count
	#argb 9, \key
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

; UPLOAD_TEXTURE, for the several-in-a-row case. Folds its ERRCODE into the
; accumulator, so a run of uploads is one result line rather than four.
uptex	.macro	id, addr, w, h, rowmajor=0
	lda #<(\id)
	sta ID_LO
	lda #>(\id)
	sta ID_HI
	#argblob 0, SPACE_C64, \addr, (\w)*(\h)
	#argw 6, \w
	#argw 8, \h
	#argb 10, \rowmajor
	lda #R2_UPLOAD_TEX
	jsr cmd2
	#accOk
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

; ----------------------------------------------------------------------
; Judging. Each of these leaves the fail code in X, ready for #check.
; ----------------------------------------------------------------------

; A = ERRCODE. Pass if OK; otherwise the fail code IS the errcode.
wantOk	.macro
	tax
	.endm

; A = ERRCODE. Pass only if it equals \err -- and a command that answered
; OK where an error was demanded fails as $FE, never silently as 0.
wantErr	.macro	err
	ldy #\err
	jsr kitExpect
	.endm

; X = fail code already set by hand.
raw	.macro
	.endm

; ----------------------------------------------------------------------
; Reporting. The name is stored inline after the JSR, so a test needs no
; string table and the name sits where the test is.
; ----------------------------------------------------------------------

check	.macro	name
	jsr kitCheck
	.text \name
	.byte 0
	.endm

title	.macro	name
	jsr kitTitle
	.text \name
	.byte 0
	.endm

; A bare note line in the results area -- for a value that is reported
; rather than judged, e.g. the measured frame period.
note	.macro	name
	jsr kitNote
	.text \name
	.byte 0
	.endm

; ----------------------------------------------------------------------
; Checking data.
;
; Most of what this suite asserts is not an error code but a byte that came
; back from the framebuffer or from a math result. A test opens with
; #clracc, makes as many #expectb assertions as it likes, and closes with
; #useacc -- so one result line can stand for a whole shape, and the fail
; code is the first byte that was actually wrong.
; ----------------------------------------------------------------------

clracc	.macro
	lda #0
	sta accFail
	.endm

useacc	.macro
	ldx accFail
	.endm

expectb	.macro	addr, val
	lda \addr
	cmp #\val
	beq +
	jsr kitBad
+
	.endm

; The same, for a value that must NOT be a particular byte -- used where
; the assertion is "this pixel was left alone", and the interesting failure
; is that something painted over it.
rejectb	.macro	addr, val
	lda \addr
	cmp #\val
	bne +
	jsr kitBad
+
	.endm

; A little-endian 16-bit field, which is how every multi-byte value in this
; API crosses -- info block entries, 8.8 matrix elements, coordinates.
expectw	.macro	addr, val
	lda \addr
	cmp #<(\val)
	beq +
	jsr kitBad
+
	lda \addr+1
	cmp #>(\val)
	beq +
	jsr kitBad
+
	.endm

; Length for the kitFill / kitSeq / kitCmp / kitChkFill helpers.
setlen	.macro	n
	lda #<(\n)
	sta kitLen
	lda #>(\n)
	sta kitLen+1
	.endm

setptr	.macro	p, addr
	lda #<(\addr)
	sta \p
	lda #>(\addr)
	sta \p+1
	.endm

; A slot that shows a 16-bit measurement rather than a verdict.
; After a failed #check on a kitCmp, this reports what actually differed:
; the offset of the first mismatching byte, and the two bytes themselves
; as "wantgot" -- expected (from P1) in the high nibble pair, actual (from
; P2) in the low. Two extra result lines that turn $E1 into a diagnosis.
cmpdiff	.macro	name, name2, name3
	#showw kitOff, \name
	#showw kitDiffN, \name3
	lda kitDiffB
	sta kitVal16
	lda kitDiffA
	sta kitVal16+1
	jsr kitShow
	.text \name2
	.byte 0
	.endm

; The second and third mismatches from the same kitCmp: their offsets, and
; their two actual bytes packed into one word (2nd high, 3rd low). Together
; with #cmpdiff this is three offsets and three got-values from a single
; run, which is what it takes to tell scattered damage from one bad byte.
cmpmore	.macro	name2, name3, namegot
	#showw kitOff2, \name2
	#showw kitOff3, \name3
	lda kitGot3
	sta kitVal16
	lda kitGot2
	sta kitVal16+1
	jsr kitShow
	.text \namegot
	.byte 0
	.endm

; The offsets alone, for a pass whose want/got values are already known from
; another pass. Four slots instead of six -- the screen has exactly 21 rows
; per column and three transfer passes do not otherwise fit.
cmpwhere .macro	nameat, namen, name2, name3
	#showw kitOff, \nameat
	#showw kitDiffN, \namen
	#showw kitOff2, \name2
	#showw kitOff3, \name3
	.endm

showw	.macro	addr, name
	lda \addr
	sta kitVal16
	lda \addr+1
	sta kitVal16+1
	jsr kitShow
	.text \name
	.byte 0
	.endm

; ----------------------------------------------------------------------
; Staging the drawing opcodes.
;
; These only lay out ARG bytes -- they never dispatch. Keeping the two
; apart is deliberate: a test that stages a shape and then fires the wrong
; opcode is a bug the suite must be able to express, because that is what a
; real program's bug looks like.
; ----------------------------------------------------------------------

stgPixel .macro	x, y, c
	#argw 0, \x
	#argw 2, \y
	#argb 4, \c
	.endm

stgLine	.macro	x0, y0, x1, y1, c
	#argw 0, \x0
	#argw 2, \y0
	#argw 4, \x1
	#argw 6, \y1
	#argb 8, \c
	.endm

; Serves RECT and RECT_FILL, which take the same nine bytes.
stgRect	.macro	x, y, w, h, c
	#argw 0, \x
	#argw 2, \y
	#argw 4, \w
	#argw 6, \h
	#argb 8, \c
	.endm

; BLIT and BLIT_KEYED; the key byte is ARG14 and is staged separately, so
; a keyed blit reads as "the same blit, plus a key".
stgBlit	.macro	src, x, y, w, h
	#argblob 0, SPACE_C64, \src, (\w)*(\h)
	#argw 6, \x
	#argw 8, \y
	#argw 10, \w
	#argw 12, \h
	.endm

stgRead	.macro	dst, x, y, w, h
	#argblob 0, SPACE_C64, \dst, (\w)*(\h)
	#argw 6, \x
	#argw 8, \y
	#argw 10, \w
	#argw 12, \h
	.endm

; Fill a readback buffer with a byte no test ever expects, so a READ_RECT
; that quietly wrote nothing cannot pass on leftovers. $AA is chosen for
; being outside every colour index the suite draws with.
; Fills a destination with a byte that is not a plausible result, so a
; command that wrote nothing at all cannot be mistaken for one that wrote
; the right thing. When two buffers are filled and then compared against
; each other, they MUST be given different bytes -- otherwise "nothing
; happened" compares equal and the check passes vacuously.
poison	.macro	addr, len, val=$aa
	#setptr P1, \addr
	#setlen \len
	lda #\val
	sta kitVal
	jsr kitFill
	.endm

; Fold a dispatch's ERRCODE into the accumulated fail code, for a test that
; fires several commands and judges them as one line. Must follow a cmd0 /
; cmd1 directly, whose Z flag still reflects the ERRCODE it returned.
accOk	.macro
	beq +
	jsr kitBad
+
	.endm

; A little-endian 32-bit field -- an IEEE 754 single, for the $9x math set.
expectd	.macro	addr, val
	lda \addr
	cmp #<(\val)
	beq +
	jsr kitBad
+
	lda \addr+1
	cmp #>(\val)
	beq +
	jsr kitBad
+
	lda \addr+2
	cmp #`(\val)
	beq +
	jsr kitBad
+
	lda \addr+3
	cmp #((\val) >> 24) & $ff
	beq +
	jsr kitBad
+
	.endm

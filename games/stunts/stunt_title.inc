; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; stunt_title.inc -- what the game shows before the first race: the
; HONDANI logo with its light sweep, then the start screen with the
; credits. Included by gpu64_demo_stunt.a at $c000; `title` is called
; once, after dmInit and before anything of the race is set up.
;
; THE PICTURES COME FROM STUNTS.REU
;
; stunts.reu is the REU image the player selects in the RAD menu with
; this PRG; gpu64 reads levels and pictures from nowhere else, and knows
; nothing about this game. LEVEL_PICTURE draws out of it (stunt_pics.py
; makes it). Nothing crosses the bus but the command.
;
; The logo is games/quake/gpu64_game_title.inc's, on both screens at once and at
; the same place: the C64 shows the hires original (cell rows 6-17) out of
; VIC bank 3, and the HDMI screen shows PIC_LOGO, the same 296x96 crop, at
; 16,48. Every lit HDMI pixel is palette index LOGO_BASE + its C64 cell
; column, so the sweep is one colour per column on both screens: the C64
; rewrites the screen-RAM nibbles of the columns that changed, and the
; HDMI screen gets the same forty colours as one PAL_LOAD.
;
; The start screen is two HDMI pictures in turn -- PIC_START, after the box
; art, and PIC_CREDS -- with PIC_FIRE blinking over them; each tick draws
; the whole page and flips, so a lost command lasts one tick. The C64
; shows the credits as text. The user plays with HDMI only.
;
; If the logo cannot be drawn -- the REU image is not stunts.reu -- the
; HDMI side falls back to the C64 text screen mirrored into HDMI's text
; mode, with a line saying which image to select.
;
; THE BUS IS NOT RELIABLE
;
; Every command here goes through stage0/stage2, checked, so a flipped
; byte is refused and sent again. The logo is drawn twice; the sweep's
; PAL_LOAD repeats every step; the start screen is drawn again every tick.
; A command the bus lost entirely is repaired by the next one.
;
; MUSIC
;
; Tim Follin's Led Storm (C64 version by Michal Hoffmann), moved to $a000
; by tools/sidreloc.py, plays from the KERNAL's IRQ at 50 Hz until the
; race starts. BASIC is banked out ($01 = $36) for the whole title. The
; tune's zero page is $fc-$fe, inside SRC/DST: the IRQ swaps $fa-$ff in
; and out around every call into it, as Quake's does.
;
; runsim.py pokes SIM_TITLE_ON into SIM_TITLE unless given --title, and
; the title is then skipped.
; ======================================================================

SIM_TITLE    = $02ab
SIM_TITLE_ON = $ab

TUNE_INIT    = $a000		; Led Storm
TUNE_PLAY    = $a003
LOGO_RLE     = $b490
LOGO_SCREEN  = $cc00		; VIC bank 3: screen $cc00, bitmap $e000
LOGO_BITMAP  = $e000
LOGO_ROW0    = 6		; cell rows 6..17 hold the logo
LOGO_ROWS    = 12
LOGO_COL0    = 2		; cell columns 2..38
LOGO_COL1    = 38
LOGO_X       = LOGO_COL0 * 8	; 16, 48: where both screens draw it
LOGO_Y       = LOGO_ROW0 * 8
LOGO_BASE    = 192		; palette index of C64 column 0
RLE_ESC      = $e5
OP_LEVEL_PICTURE = $1a

SWEEP_MS    = 20		; one column of light per step
SWEEP_STEPS = 46		; the ramp's 7 columns cross 2..38
SWEEP_HOLD  = 50		; steps of plain red before and after
TICK_MS     = 100		; the start screen's refresh
BLINK_TICKS = 4			; "PRESS FIRE" on/off period, in ticks
PAGE_TICKS  = 64		; ticks per HDMI page: start art, credits
FIRE_X      = (320 - PIC_FIRE_W) / 2
FIRE_Y_ART  = 180		; under the scene on PIC_START
FIRE_Y_CRED = 158		; in the gap above the copyright on PIC_CREDS

HDMI_COL0   = 20		; the C64's 40 columns, centred in 80

; ----------------------------------------------------------------------
title
	lda SIM_TITLE
	cmp #SIM_TITLE_ON
	bne tiRun
	lda #0
	sta SIM_TITLE
	inc simRun			; and the old race loop, not the menus
	rts
tiRun
	lda #$36			; BASIC out: the tune and the logo live under it
	sta $01
	jsr clkInit
	jsr tiSaveVic
	jsr tiMusicOn
	jsr tiLogo
	jsr tiStart
	jsr tiMusicOff
	jsr tiRestoreVic
	lda #$37
	sta $01
	rts

; ======================================================================
; The logo.
; ======================================================================
tiLogo
	; The C64 copy first, built with the screen blanked: the HDMI copy is
	; made from it.
	lda #$0b
	sta $d011
	lda #0
	sta $d020
	sta $d021
	sta $d015
	jsr tiUnpackLogo

	; HDMI, on the page nobody is looking at: dmInit's FULL_RESET left
	; page 0 drawn and visible, so flip once to move the drawing
	; elsewhere, then build the logo there in its initial red.
	jsr dmFlip
	#argb 0, 0
	lda #OP_CLEAR
	jsr cmdClass0
	lda #$ff			; the sweep before it starts: all red
	sta tiStep
	jsr tiSweepColours
	jsr tiPalLoad
	; The logo, twice: a lost command is not retried. Either one drawing
	; is also how the title learns the right REU image is selected.
	lda #0
	sta tiPack
	jsr tiHdmiLogo
	jsr tiPackOk
	jsr tiHdmiLogo
	jsr tiPackOk

	lda $dd00
	and #$fc			; bank 3
	sta $dd00
	lda #$38			; screen $cc00, bitmap $e000
	sta $d018
	lda #$08
	sta $d016

	; Both at once.
	jsr dmFlip
	lda #$3b
	sta $d011

	jsr tiMark
	jsr tiScan			; a key still down from RUN is not a press

	; Hold, sweep, hold. A select press skips what is left.
	ldx #SWEEP_HOLD
	jsr tiHold
	bcs tiLogoDone
	lda #0
	sta tiStep
tiSweepLoop
	jsr tiSweepColours
	jsr tiSweepC64
	jsr tiPalLoad
	lda #SWEEP_MS
	jsr tiWait
	jsr tiScan
	lda tiNew
	and #4
	bne tiLogoDone
	inc tiStep
	lda tiStep
	cmp #SWEEP_STEPS
	bne tiSweepLoop
	ldx #SWEEP_HOLD
	jsr tiHold
tiLogoDone
	; Back to a text screen for everything after.
	lda #$0b
	sta $d011
	lda $dd00
	ora #$03			; bank 0
	sta $dd00
	lda tiVic+2
	sta $d018
	lda #$c8
	sta $d016
	rts

; X = steps of SWEEP_MS. C set if select was pressed.
tiHold
	stx tiCnt
-	lda #SWEEP_MS
	jsr tiWait
	jsr tiScan
	lda tiNew
	and #4
	bne +
	dec tiCnt
	bne -
	clc
	rts
+	sec
	rts

; tiCol[cx] = the colour column cx has at step tiStep: ramp[tiStep - cx]
; inside the light's seven columns, red elsewhere. tiStep = $ff is all red.
; tiPal = the same forty colours as RGB, for PAL_LOAD at LOGO_BASE.
tiSweepColours
	ldx #39
tscLoop
	lda #RED
	ldy tiStep
	cpy #$ff
	beq tscSet
	txa
	eor #$ff			; tiStep - cx = tiStep + ~cx + 1
	sec
	adc tiStep
	bcc tscRed			; borrow: the light has not reached cx
	cmp #7
	bcs tscRed
	tay
	lda tiRamp,y
	bne tscSet
tscRed
	lda #RED
tscSet
	sta tiCol,x
	; RGB into tiPal[cx * 3]
	sta tiT
	asl a
	adc tiT				; colour * 3 (< 48, no carry out)
	tay
	txa
	sta tiT
	asl a
	adc tiT
	sta tiT				; cx * 3
	txa
	pha
	ldx tiT
	lda tiPepto,y
	sta tiPal,x
	lda tiPepto+1,y
	sta tiPal+1,x
	lda tiPepto+2,y
	sta tiPal+2,x
	pla
	tax
	dex
	bpl tscLoop
	rts

; The C64 side: recolour the columns whose colour changed since the last
; step. Every lit nibble of the logo is non-zero and stays non-zero, so a
; cell's lit nibbles are found from the cell itself.
tiSweepC64
	ldy #LOGO_COL0
tswCol
	lda tiCol,y
	cmp tiColShown,y
	beq tswNext
	sta tiColShown,y
	sta tiT
	asl a
	asl a
	asl a
	asl a
	ora tiT
	sta tiCC			; colour in both nibbles
	ldx #LOGO_ROWS - 1
tswRow
	lda tiRowLo,x
	sta DST
	lda tiRowHi,x
	sta DST+1
	lda (DST),y
	beq tswCell
	pha
	and #$f0
	beq +
	lda #$f0
+	sta tiT
	pla
	and #$0f
	beq +
	lda #$0f
+	ora tiT
	and tiCC
	sta (DST),y
tswCell
	dex
	bpl tswRow
tswNext
	iny
	cpy #LOGO_COL1 + 1
	bne tswCol
	rts

tiPalLoad
	lda #SPACE_C64
	sta stBuf+0
	lda #<tiPal
	sta stBuf+1
	lda #>tiPal
	sta stBuf+2
	lda #0
	sta stBuf+3
	sta stBuf+5
	lda #120
	sta stBuf+4
	lda #LOGO_BASE
	sta stBuf+6
	lda #40
	sta stBuf+7
	lda #OP_PAL_LOAD
	ldx #7
	jmp tiStage0

; PIC_LOGO at the C64's place, as it is: index 0 around the logo.
tiHdmiLogo
	lda #PIC_LOGO
	ldx #LOGO_X
	ldy #LOGO_Y
	jsr tiPicAt
	lda #0
	jmp tiPic

; A = a LEVEL_PICTURE answer: OK sets tiPack.
tiPackOk
	cmp #0
	bne +
	lda #1
	sta tiPack
+	rts

; A = picture, X = x (0-255), Y = y: staged in stBuf for tiPic.
tiPicAt
	sta stBuf+0
	stx stBuf+1
	sty stBuf+3
	lda #0
	sta stBuf+2
	sta stBuf+4
	rts

; A = flags. Sends the staged LEVEL_PICTURE, checked.
tiPic
	sta stBuf+5
	lda #0
	sta stId
	sta stId+1
	lda #OP_LEVEL_PICTURE
	ldx #5
	jmp stage2

; Clear the bitmap and screen in bank 3 and unpack the logo's two spans
; into them (tools/hondani.py's RLE: ESC n v, ESC 0 ends).
tiUnpackLogo
	lda #0
	tay
	ldx #>LOGO_BITMAP
	stx DST+1
	sty DST
-	sta (DST),y
	iny
	bne -
	inc DST+1
	ldx DST+1
	cpx #$ff
	bne -
-	sta $ff00,y			; the last 64 bytes, and not the vectors
	iny
	cpy #$40
	bne -
	ldy #0
-	sta LOGO_SCREEN,y
	sta LOGO_SCREEN+$100,y
	sta LOGO_SCREEN+$200,y
	sta LOGO_SCREEN+$2e8,y
	iny
	bne -
	lda #<LOGO_RLE
	sta SRC
	lda #>LOGO_RLE
	sta SRC+1
	lda #<(LOGO_BITMAP + LOGO_ROW0 * 320)
	sta DST
	lda #>(LOGO_BITMAP + LOGO_ROW0 * 320)
	sta DST+1
	jsr tiUnrle
	lda #<(LOGO_SCREEN + LOGO_ROW0 * 40)
	sta DST
	lda #>(LOGO_SCREEN + LOGO_ROW0 * 40)
	sta DST+1
	lda #RED
	ldx #39
-	sta tiColShown,x		; the file is all red: nothing to redraw
	dex
	bpl -
	; fall through

tiUnrle
	ldy #0
turLoop
	lda (SRC),y
	jsr tiSrcInc
	cmp #RLE_ESC
	beq turEsc
	sta (DST),y
	jsr tiDstInc
	jmp turLoop
turEsc
	lda (SRC),y
	jsr tiSrcInc
	tax
	beq turDone
	lda (SRC),y
	jsr tiSrcInc
-	sta (DST),y
	jsr tiDstInc
	dex
	bne -
	jmp turLoop
turDone
	rts

tiSrcInc
	inc SRC
	bne +
	inc SRC+1
+	rts
tiDstInc
	inc DST
	bne +
	inc DST+1
+	rts

; ======================================================================
; The start screen: the title and the credits, on both screens, until
; fire.
; ======================================================================
tiStart
	jsr tiCls
	jsr tiPrintAll
	lda #$1b
	sta $d011
	lda tiPack
	bne tiStartPics

	; Not stunts.reu: say so, on both screens.
	lda #<tiMissing
	sta tiPtr
	lda #>tiMissing
	sta tiPtr+1
	jsr tpaLine

	; HDMI: blank planes, then text mode. The rows go out every tick.
	lda #LTGREY
	sta stBuf+0
	lda #BLACK
	sta stBuf+1
	lda #OP_TEXT_CLEAR
	ldx #1
	jsr tiStage0

	lda #0
	sta tiTick
	jsr tiMark
	jsr tiScan
tsLoop
	; "PRESS FIRE" blinks: its row's colour, on both screens
	lda tiTick
	and #BLINK_TICKS
	beq +
	lda #ROW_USED | BLACK
	.byte $2c			; bit abs: skip the next lda
+	lda #ROW_USED | YELLOW
	sta tiRowCol + ROW_FIRE
	ldx #ROW_FIRE
	jsr tiColourRow
	jsr tiHdmiScreen
	inc tiTick
	lda #TICK_MS
	jsr tiWait
	jsr tiScan
	lda tiNew
	and #4
	beq tsLoop

	; Graphics back for the race. Asking for the mode it is already in
	; costs nothing, so the extra sends are free insurance.
	ldx #3
-	stx tiCnt
	lda #0
	sta stBuf+0
	lda #OP_TEXT_MODE
	ldx #0
	jsr tiStage0
	ldx tiCnt
	dex
	bne -
	rts

; The pictures: a page and, half the time, PRESS FIRE over it, then a
; flip. The palette goes in with every page (flag bit 1), which costs
; nothing once it is there.
tiStartPics
	lda #BLACK
	sta stBuf+0
	lda #OP_SET_BORDER
	ldx #0
	jsr tiStage0
	lda #0
	sta tiTick
	jsr tiMark
	jsr tiScan
tspLoop
	lda #PIC_START
	ldy #FIRE_Y_ART
	ldx tiTick
	cpx #PAGE_TICKS
	bcc +
	lda #PIC_CREDS
	ldy #FIRE_Y_CRED
+	sty tiFireY
	ldx #0
	ldy #0
	jsr tiPicAt
	lda #2				; install the palette
	jsr tiPic
	lda tiTick
	and #BLINK_TICKS
	bne +
	lda #PIC_FIRE
	ldx #FIRE_X
	ldy tiFireY
	jsr tiPicAt
	lda #1				; keyed
	jsr tiPic
+	jsr dmFlip
	inc tiTick
	lda tiTick
	cmp #PAGE_TICKS * 2
	bcc +
	lda #0
	sta tiTick
+	lda #TICK_MS
	jsr tiWait
	jsr tiScan
	lda tiNew
	and #4
	beq tspLoop
	rts

; The mode, the charset and every row that has text, all checked.
tiHdmiScreen
	lda #BLACK
	sta stBuf+0
	lda #OP_SET_BORDER
	ldx #0
	jsr tiStage0
	lda #0				; upper case and graphics, as the C64 has
	sta stBuf+0
	lda #OP_TEXT_CHARSET
	ldx #0
	jsr tiStage0
	lda #1
	sta stBuf+0
	lda #OP_TEXT_MODE
	ldx #0
	jsr tiStage0
	lda #0
	sta tiI
thsRow
	ldx tiI
	lda tiRowCol,x
	beq thsNext
	and #$0f
	sta stBuf+8			; fg
	lda #SPACE_C64
	sta stBuf+0
	lda tiScrLo,x
	sta stBuf+1
	lda tiScrHi,x
	sta stBuf+2
	lda #0
	sta stBuf+3
	sta stBuf+5
	sta stBuf+9			; bg
	sta stBuf+10			; flags: screen codes, as they are
	lda #40
	sta stBuf+4
	lda #HDMI_COL0
	sta stBuf+6
	txa
	asl a
	sta stBuf+7			; row: every other one of 50
	lda #OP_TEXT_WRITE
	ldx #10
	jsr tiStage0
thsNext
	inc tiI
	lda tiI
	cmp #25
	bne thsRow
	rts

; Print every line of tiLines on the C64 and note each row's colour.
tiPrintAll
	ldx #24
	lda #0
-	sta tiRowCol,x
	dex
	bpl -
	lda #<tiLines
	sta tiPtr
	lda #>tiLines
	sta tiPtr+1
tpaLine
	jsr tiGet			; row, or $ff at the end
	cmp #$ff
	beq tpaDone
	tax
	jsr tiGet			; colour
	ora #ROW_USED
	sta tiRowCol,x
	jsr tiGet			; column
	clc
	adc tiScrLo,x
	sta DST
	lda tiScrHi,x
	adc #0
	sta DST+1
	jsr tiColourRow
	ldy #0
-	jsr tiGet
	beq tpaLine
	cmp #$41			; ASCII upper case to screen codes
	bcc +
	cmp #$5b
	bcs +
	sbc #$3f			; C clear here: - $40
+	sta (DST),y
	iny
	bne -
tpaDone
	rts

; The next byte of tiLines, Z set if it is 0.
tiPtr = tgAddr + 1
tiGet
tgAddr	lda tiLines
	inc tiPtr
	bne +
	inc tiPtr+1
+	cmp #0
	rts

; X = row: its colour RAM to tiRowCol[row]. Leaves DST alone.
tiColourRow
	lda tiColLo,x
	sta tcrA+1
	lda tiColHi,x
	sta tcrA+2
	lda tiRowCol,x
	ldy #39
tcrA	sta $d800,y
	dey
	bpl tcrA
	rts

; ======================================================================
; Helpers.
; ======================================================================

; A = op, X = the last ARG index staged in stBuf: stage0 with no id.
tiStage0
	ldy #0
	sty stId
	sty stId+1
	jmp stage0

; tiIn: bit 0 up, 1 down, 2 select -- joystick 2, W/S, CRSR DOWN, and
; fire/SPACE/RETURN. tiNew = the bits that were not down last scan. A held
; stick ghosts into the keyboard columns, so with it off centre the stick
; alone counts, as in readKeys; port 1's row pulls are masked out the same
; way.
tiScan
	sei
	lda #$ff
	sta $dc02
	lda #0
	sta $dc03
	sta tiK0
	sta tiK1
	sta tiK7
	lda #$ff
	sta CIA_PRA
	lda CIA_PRB
	eor #$ff
	sta tiP1
	lda $dc00
	eor #$ff
	and #$1f
	sta tiJoy
	and #$0f
	bne tisNoKb
	lda #%11111110			; column 0: RETURN row 1, CRSR DOWN row 7
	sta CIA_PRA
	lda CIA_PRB
	ora tiP1
	eor #$ff
	sta tiK0
	lda #%11111101			; column 1: W row 1, S row 5
	sta CIA_PRA
	lda CIA_PRB
	ora tiP1
	eor #$ff
	sta tiK1
	lda #%01111111			; column 7: SPACE row 4
	sta CIA_PRA
	lda CIA_PRB
	ora tiP1
	eor #$ff
	sta tiK7
tisNoKb
	lda #$ff
	sta CIA_PRA
	cli
	lda #0
	sta tiT
	lda tiJoy
	and #$01			; stick up
	bne tisUp
	lda tiK1
	and #$02			; W
	beq tisNoUp
tisUp
	inc tiT
tisNoUp
	lda tiJoy
	and #$02
	bne tisDown
	lda tiK1
	and #$20			; S
	bne tisDown
	lda tiK0
	and #$80			; CRSR DOWN
	beq tisSel
tisDown
	lda tiT
	ora #2
	sta tiT
tisSel
	lda tiJoy
	and #$10
	bne tisFire
	lda tiK0
	and #$02			; RETURN
	bne tisFire
	lda tiK7
	and #$10			; SPACE
	beq tisDone
tisFire
	lda tiT
	ora #4
	sta tiT
tisDone
	lda tiIn
	eor #$ff
	and tiT
	sta tiNew
	lda tiT
	sta tiIn
	rts

; Start a chain of tiWaits from now.
tiMark
	jsr clkNow
	lda msNow
	sta tiT0
	lda msNow+1
	sta tiT0+1
	rts

; A = ms. Waits until A ms after the last mark, and moves the mark on by
; A, so a chain of waits keeps its pace even when one step ran long.
tiWait
	sta tiDur
-	jsr clkNow
	lda tiT0			; the counter counts down
	sec
	sbc msNow
	tax
	lda tiT0+1
	sbc msNow+1
	bne +				; 256 ms or more: long past
	cpx tiDur
	bcc -
+	lda tiT0
	sec
	sbc tiDur
	sta tiT0
	bcs +
	dec tiT0+1
+	rts

; Music from the KERNAL's IRQ, sped up to 50 Hz.
tiMusicOn
	sei
	jsr tiZpIn
	lda #0
	tax
	tay
	jsr TUNE_INIT
	jsr tiZpOut
	lda $0314
	sta tiOldIrq
	lda $0315
	sta tiOldIrq+1
	lda #<tiIrq
	sta $0314
	lda #>tiIrq
	sta $0315
	ldx #<19704			; 985248 Hz / 50
	ldy #>19704
	lda $02a6			; 1 = PAL
	bne +
	ldx #<20454			; 1022727 Hz / 50
	ldy #>20454
+	stx $dc04
	sty $dc05
	cli
	rts

tiMusicOff
	sei
	lda tiOldIrq
	sta $0314
	lda tiOldIrq+1
	sta $0315
	ldx #<$4025			; the KERNAL's own 60 Hz jiffy
	ldy #>$4025
	lda $02a6
	bne +
	ldx #<$4295
	ldy #>$4295
+	stx $dc04
	sty $dc05
	lda #0
	ldx #$18
-	sta $d400,x
	dex
	bpl -
	cli
	rts

tiIrq
	jsr tiZpIn
	jsr TUNE_PLAY
	jsr tiZpOut
	jmp $ea31

; $fa-$ff: the tune's zero page ($fc-$fe) inside the game's SRC/DST.
tiZpIn
	ldx #5
-	lda $fa,x
	sta tiZpGame,x
	lda tiZpTune,x
	sta $fa,x
	dex
	bpl -
	rts
tiZpOut
	ldx #5
-	lda $fa,x
	sta tiZpTune,x
	lda tiZpGame,x
	sta $fa,x
	dex
	bpl -
	rts

tiSaveVic
	lda $d011
	sta tiVic+0
	lda $d016
	sta tiVic+1
	lda $d018
	sta tiVic+2
	lda $d020
	sta tiVic+3
	lda $d021
	sta tiVic+4
	rts

tiRestoreVic
	lda tiVic+3
	sta $d020
	lda tiVic+4
	sta $d021
	jsr tiCls
	lda tiVic+0
	sta $d011
	lda tiVic+1
	sta $d016
	lda tiVic+2
	sta $d018
	rts

; The C64 screen blank, light grey.
tiCls
	ldx #0
-	lda #$20
	sta SCREEN,x
	sta SCREEN+$100,x
	sta SCREEN+$200,x
	sta SCREEN+$2e8,x
	lda #LTGREY
	sta $d800,x
	sta $d900,x
	sta $da00,x
	sta $dae8,x
	inx
	bne -
	rts

; ----------------------------------------------------------------------
; The start screen. Row, colour, then the text centred in 40 columns.
; Upper case and punctuation only: the C64 shows it in its upper-case set.
; ----------------------------------------------------------------------
ROW_USED = $10			; tiRowCol: the row has text (colour RAM ignores it)
ROW_FIRE = 21

tiLine	.macro	row, colour, txt
	.byte \row, \colour, (40 - len(\txt)) / 2
	.text \txt
	.byte 0
	.endm

tiLines
	#tiLine  2, YELLOW,  "S T U N T   C A R   R A C E R"
	#tiLine  4, LTGREY,  "ON THE GPU64 SECOND SCREEN"
	#tiLine  7, CYAN,    "GAME AND GPU64"
	#tiLine  8, WHITE,   "HONDANI"
	#tiLine 10, CYAN,    "MUSIC"
	#tiLine 11, WHITE,   "LED STORM BY TIM FOLLIN"
	#tiLine 12, WHITE,   "C64 VERSION BY MICHAL HOFFMANN"
	#tiLine 13, GREY,    "(SMALLTOWN BOY) - MULTISTYLE LABS 2003"
	#tiLine 15, CYAN,    "INSPIRED BY"
	#tiLine 16, WHITE,   "STUNT CAR RACER BY GEOFF CRAMMOND, 1988"
	#tiLine 18, LTGREY,  "JOYSTICK IN PORT 2, OR W S A D + SPACE"
	#tiLine ROW_FIRE, YELLOW, "PRESS FIRE TO START"
	#tiLine 24, GREY,    "(C) 2026 HONZA / HONDANI - GPL V3"
	.byte $ff
tiMissing
	#tiLine 22, LTRED,   "SELECT STUNTS.REU IN THE RAD MENU"
	.byte $ff

tiRowLo
	.for r = 0, r < LOGO_ROWS, r = r + 1
	.byte <(LOGO_SCREEN + (LOGO_ROW0 + r) * 40)
	.next
tiRowHi
	.for r = 0, r < LOGO_ROWS, r = r + 1
	.byte >(LOGO_SCREEN + (LOGO_ROW0 + r) * 40)
	.next
tiScrLo
	.for r = 0, r < 25, r = r + 1
	.byte <(SCREEN + r * 40)
	.next
tiScrHi
	.for r = 0, r < 25, r = r + 1
	.byte >(SCREEN + r * 40)
	.next
tiColLo
	.for r = 0, r < 25, r = r + 1
	.byte <($d800 + r * 40)
	.next
tiColHi
	.for r = 0, r < 25, r = r + 1
	.byte >($d800 + r * 40)
	.next

; The light: white at its leading edge, then yellow, light red, back to red.
tiRamp	.byte WHITE, WHITE, YELLOW, YELLOW, LTRED, LTRED, RED

; The C64 palette gpu64 boots with (Pepto), three bytes a colour.
tiPepto
	.byte   0,   0,   0,  255, 255, 255,  104,  55,  43,  112, 164, 178
	.byte 111,  61, 134,   88, 141,  67,   53,  40, 121,  184, 199, 111
	.byte 111,  79,  37,   67,  57,   0,  154, 103,  89,   68,  68,  68
	.byte 108, 108, 108,  154, 210, 132,  108,  94, 181,  149, 149, 149

tiStep		.byte 0
tiPack		.byte 0
tiFireY		.byte 0
tiTick		.byte 0
tiCnt		.byte 0
tiI		.byte 0
tiT		.byte 0
tiCC		.byte 0
tiDur		.byte 0
tiT0		.word 0
tiIn		.byte 0
tiNew		.byte 0
tiJoy		.byte 0
tiP1		.byte 0
tiK0		.byte 0
tiK1		.byte 0
tiK7		.byte 0
tiOldIrq	.word 0
tiVic		.fill 5, 0
tiZpGame	.fill 6, 0
tiZpTune	.fill 6, 0
tiRowCol	.fill 25, 0
tiCol		.fill 40, 0
tiColShown	.fill 40, 0
tiPal		.fill 120, 0

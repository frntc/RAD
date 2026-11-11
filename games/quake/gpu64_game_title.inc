; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_title.inc -- what the game shows before E1M1: the HONDANI
; logo with its light sweep, the Quake title, the skill menu and the
; loading plaque. Included by gpu64_demo_game.a; `title` is called once at
; start, before SCENE_RESET.
;
; TWO SCREENS, ONE PICTURE
;
; The logo is on both screens at once, at the same place: the C64 shows the
; hires original (games/quake/hondani.art, cell rows 6-17) out of VIC bank 3, and
; the HDMI screen shows the same crop out of the level file's picture
; section (LEVEL_PICTURE $1A, picture PIC_HONDANI at LOGO_X,LOGO_Y). The
; HDMI copy's logo pixels are palette index LOGO_BASE + their C64 cell
; column, so the sweep is one colour per column on both: the C64 rewrites
; the screen-RAM nibbles of the columns that changed, and the HDMI screen
; gets the same 40 colours as one PAL_LOAD. Nothing is redrawn on the HDMI
; side while the light moves -- the pixels stay, their palette entries
; change -- which is why the two stay in step.
;
; Everything after the logo is Quake's own art out of the pak file
; (tools/quake_pics.py): conback for the title; the faded conback, the
; plaque, "SINGLE PLAYER", four skill labels in the console font and the
; spinning menu dot for the menu, as Quake's M_Draw lays them out. A fifth
; line, CONTROLS, opens the key bindings (gpu64_game_controls.inc). The C64
; carries a text version of each, so either screen alone is enough to
; play the menu.
;
; THE BUS IS NOT RELIABLE
;
; Every LEVEL_PICTURE goes through stage2, checked, so a flipped byte is
; refused and sent again rather than blitting the wrong picture -- and it
; also runs under a strict mode some earlier launch left latched (gpu64
; state is sticky across program launches). The title and the menu are
; redrawn whole every tick, so a command the bus lost entirely is gone by
; the next tick. The logo is drawn once and is the one exception; the
; sweep's PAL_LOAD repeats every step and heals itself.
;
; MUSIC
;
; E-Quake tune 1 plays from the KERNAL's IRQ, sped up to 50 Hz, until the
; loading plaque. It lives at $A000-$A98C (tools/sidreloc.py moved it there
; and proved every SID write of 20000 frames unchanged),
; so BASIC is banked out ($01 = $36) for the whole run and `finish` puts
; it back. The tune's zero page is $FA-$FF, which overlaps SRC/DST: the
; IRQ swaps those six bytes in and out around every call into a tune.
;
; runsim.py skips all of this unless given --title: it pokes SIM_TITLE_ON
; into SIM_TITLE, and SIM_SKILL is the skill the menu would have returned.
; ======================================================================

OP_LEVEL_PICTURE = $1a

SIM_TITLE    = $02ab
SIM_TITLE_ON = $ab
SIM_SKILL    = $02ac
SIM_LEVEL    = $02ae		; --level-slot: the start level the menu would pick

TUNE_INIT       = $a000		; E-Quake tune 1, the only tune
TUNE_PLAY       = $a003
LOGO_RLE        = $a990
LOGO_SCREEN     = $cc00		; VIC bank 3: screen $cc00, bitmap $e000
LOGO_BITMAP     = $e000
LOGO_ROW0       = 6		; cell rows 6..17 hold the logo
LOGO_ROWS       = 12
LOGO_COL0       = 2		; cell columns 2..38
LOGO_COL1       = 38
RLE_ESC         = $e5

SWEEP_MS    = 20		; one column of light per step
SWEEP_STEPS = 46		; the ramp's 7 columns cross 2..38
SWEEP_HOLD  = 50		; steps of plain red before and after
TICK_MS     = 100		; title and menu: Quake's menu dot runs at 10 fps

; The four skills' spawnflags masks, high byte (NOT_IN_EASY 256,
; NOT_IN_MEDIUM 512, NOT_IN_HARD 1024). Nightmare spawns what hard does.
SKILL_NORMAL = 1

; HDMI menu layout, from Quake's M_Draw: plaque at 16,4, title centred at
; y 4, items 20 pixels apart from y 32 at x 72, the dot at x 54.
MENU_X   = 72
MENU_Y   = 32
MENU_DY  = 20
DOT_X    = 54
LOAD_X   = 88			; (320 - 144) / 2
LOAD_Y   = 64			; (200 - 48 - 24) / 2, as SCR_DrawLoading

; ----------------------------------------------------------------------
title
	lda #$36			; BASIC out: the tunes live under it
	sta $01
	lda #SKILL_NORMAL
	sta skill
	lda #0
	sta lvSlot
	lda SIM_TITLE
	cmp #SIM_TITLE_ON
	bne tiRun
	lda #0
	sta SIM_TITLE
	lda SIM_SKILL
	and #3
	sta skill
	lda SIM_LEVEL
	and #7
	sta lvSlot
	jmp tiSkillSet
tiRun
	jsr clkInit
	jsr tiSaveVic
	jsr tiMusicOn
	jsr tiLogo
	jsr tiTitle
	jsr tiMenu
	jsr tiLoading
	jsr tiMusicOff
	jsr tiRestoreVic
tiSkillSet
	ldx skill
	lda tiSkillMasks,x
	sta skillMask
	rts

tiSkillMasks	.byte $01, $02, $04, $04

; ======================================================================
; The logo.
; ======================================================================
tiLogo
	; HDMI first, on the page nobody is looking at: dmInit's FULL_RESET
	; left page 0 drawn and visible, so flip once to move the drawing
	; elsewhere, then build the logo there in its initial red.
	jsr dmFlip
	#argb 0, 0
	lda #OP_CLEAR
	jsr cmdClass0
	lda #PIC_HONDANI
	ldx #<LOGO_X
	ldy #LOGO_Y
	jsr tiPicAt
	lda #2				; install the level palette, opaque
	jsr tiPic
	lda #$ff			; the sweep before it starts: all red
	sta tiStep
	jsr tiSweepColours
	jsr tiPalLoad

	; The C64 copy, built with the screen blanked.
	lda #$0b
	sta $d011
	lda #0
	sta $d020
	sta $d021
	sta $d015
	jsr tiUnpackLogo
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
	jsr tiCls
	lda #$1b
	sta $d011
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
; cell's lit nibbles are found from the cell itself -- no copy of the
; original is needed.
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
	#argblob 0, SPACE_C64, tiPal, 120
	#argb 6, LOGO_BASE
	#argb 7, 40
	lda #OP_PAL_LOAD
	jmp cmdClass0

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
; The Quake title: conback on the HDMI screen, a text card on the C64.
; ======================================================================
tiTitle
	jsr tiMark
	lda #0
	sta tiTick
	jsr tiTextColour
	lda #8
	jsr dmRow
	jsr dmText
	.text "               Q U A K E"
	.byte 0
	lda #10
	jsr dmRow
	jsr dmText
	.text "      E1M1: THE SLIPGATE COMPLEX"
	.byte 0
	lda #13
	jsr dmRow
	jsr dmText
	.text "      ON THE SECOND SCREEN, BY GPU64"
	.byte 0
	lda #18
	jsr dmRow
	jsr dmText
	.text "       PRESS FIRE, SPACE OR RETURN"
	.byte 0
tiTitleTick
	lda #PIC_CONBACK
	ldx #0
	ldy #0
	jsr tiPicAt
	lda #2				; the logo sweep left 192-231 recoloured
	jsr tiPic
	jsr dmFlip
	; The prompt blinks on the C64, half a second each way.
	ldx #WHITE
	lda tiTick
	cmp #5
	bcc +
	ldx #DKGREY
+	txa
	ldx #39
-	sta $d800+18*40,x
	dex
	bpl -
	inc tiTick
	lda tiTick
	cmp #10
	bcc +
	lda #0
	sta tiTick
+	jsr tiTickWait
	lda tiNew
	and #4
	beq tiTitleTick
	rts

; ======================================================================
; The skill menu: four skills, then the start level, then CONTROLS.
; Fire on the level line steps it E1M1 .. E1M8 and stays in the menu (the
; title reads only up, down and fire). A level the Pi does not have -- no
; pack in the REU -- is E1M1 when it comes to LOAD_LEVEL.
; ======================================================================
TI_ITEMS = 6
TI_LEVEL = 4			; the menu line that is the start level
TI_CTRL  = 5
tiMenu
	jsr tiMark
	jsr tiCls
	lda #0
	sta tiTick
	lda #3
	jsr dmRow
	jsr dmText
	.text "             SINGLE PLAYER"
	.byte 0
	lda #5
	jsr dmRow
	jsr dmText
	.text "           CHOOSE YOUR SKILL"
	.byte 0
	ldx #0
-	txa
	pha
	asl a
	adc #9
	jsr dmRow
	lda DST
	clc
	adc #15
	sta DST
	bcc +
	inc DST+1
+	pla
	pha
	jsr tiSkillName
	pla
	tax
	inx
	cpx #TI_ITEMS
	bne -
	lda #21
	jsr dmRow
	jsr dmText
	.text "   W/S OR JOYSTICK, FIRE TO START"
	.byte 0
tiMenuTick
	jsr tiMenuC64
	lda #PIC_CONFADE
	ldx #0
	ldy #0
	jsr tiPicAt
	lda #0
	jsr tiPic
	lda #PIC_QPLAQUE
	ldx #16
	ldy #4
	jsr tiPicAt
	lda #1
	jsr tiPic
	lda #PIC_TTL_SGL
	ldx #96
	ldy #4
	jsr tiPicAt
	lda #1
	jsr tiPic
	ldx #0
-	stx tiI
	lda tiItemPic,x
	cpx #TI_LEVEL
	bne +
	clc
	adc lvSlot
+	pha
	lda tiMenuY,x
	clc
	adc #4				; the labels are 16 high, the dot 24
	tay
	pla
	ldx #MENU_X
	jsr tiPicAt
	lda #1
	jsr tiPic
	ldx tiI
	inx
	cpx #TI_ITEMS
	bne -
	lda tiTick			; (time * 10) % 6, as M_Draw
	clc
	adc #PIC_DOT0
	ldx skill
	ldy tiMenuY,x
	ldx #DOT_X
	jsr tiPicAt
	lda #1
	jsr tiPic
	jsr dmFlip

	inc tiTick
	lda tiTick
	cmp #6
	bcc +
	lda #0
	sta tiTick
+	jsr tiTickWait
	lda tiNew
	lsr a
	bcc +
	dec skill			; up
	bpl +
	lda #TI_ITEMS-1
	sta skill
+	lda tiNew
	and #2
	beq +
	inc skill			; down
	lda skill
	cmp #TI_ITEMS
	bcc +
	lda #0
	sta skill
+	lda tiNew
	and #4
	bne +
	jmp tiMenuTick
+	lda skill			; the last two lines are not skills:
	cmp #TI_LEVEL			; `skill` is the cursor here, and only
	bne +				; 0-3 ever leave
	lda lvSlot
	clc
	adc #1
	and #7
	sta lvSlot
	jmp tiMenu			; the C64 line has to be redrawn
+	cmp #TI_CTRL
	bne +
	jsr ctMenu
	jmp tiMenu
+	rts

tiMenuY	.byte MENU_Y, MENU_Y + MENU_DY, MENU_Y + 2 * MENU_DY, MENU_Y + 3 * MENU_DY
	.byte MENU_Y + 4 * MENU_DY, MENU_Y + 5 * MENU_DY
tiItemPic	.byte PIC_SK_EASY, PIC_SK_NORMAL, PIC_SK_HARD, PIC_SK_NIGHTMARE
	.byte PIC_LV_E1M1, PIC_SK_CONTROLS

; The C64 menu: a cursor and a highlight on the current line.
tiMenuC64
	ldx #0
tmcLoop
	txa
	asl a
	adc #9
	tay
	lda tiScrLo,y
	sta DST
	lda tiScrHi,y
	sta DST+1
	lda tiColLo,y
	sta SRC
	lda tiColHi,y
	sta SRC+1
	lda #$20
	ldy #DKGREY
	cpx skill
	bne +
	lda #$3e			; '>'
	ldy #YELLOW
+	sty tiT
	ldy #13
	sta (DST),y
	lda tiT
-	sta (SRC),y
	iny
	cpy #25
	bne -
	inx
	cpx #TI_ITEMS
	bne tmcLoop
	rts

; A = menu line: its name at the cursor.
tiSkillName
	cmp #TI_LEVEL
	bne +
	jsr dmText			; as the HDMI label
	.text "LEVEL "
	.byte 0
	jmp lvName
+	tax
	lda tiSkillText,x
	tax
	ldy #0
-	lda tiSkillStr,x
	beq +
	jsr dmScreenCode
	sta (DST),y
	inx
	iny
	bne -
+	rts

tiSkillText	.byte 0, 5, 12, 17, 0, 27
tiSkillStr	.text "EASY", 0, "NORMAL", 0, "HARD", 0, "NIGHTMARE", 0, "CONTROLS", 0

; ======================================================================
; The loading plaque: the menu's faded background, Quake's LOADING, and a
; line on the C64. It stays up while the level loads.
; ======================================================================
tiLoading
	jsr tiCls
	lda #12
	jsr dmRow
	jsr dmText
	.text "             LOADING "
	.byte 0
	jsr lvName
	lda #PIC_CONFADE
	ldx #0
	ldy #0
	jsr tiPicAt
	lda #0
	jsr tiPic
	lda #PIC_LOADING
	ldx #LOAD_X
	ldy #LOAD_Y
	jsr tiPicAt
	lda #1
	jsr tiPic
	jmp dmFlip

; ======================================================================
; Helpers.
; ======================================================================

; A = picture, X = x (0-255), Y = y: staged in stBuf for tiPic.
tiPicAt
	sta stBuf+0
	stx stBuf+1
	sty stBuf+3
	lda #0
	sta stBuf+2
	sta stBuf+4
	rts

; A = flags. Sends the staged LEVEL_PICTURE, checked. An error is not
; worth stopping the title for: the next tick draws it again.
tiPic
	sta stBuf+5
	lda #0
	sta stId
	sta stId+1
	lda #OP_LEVEL_PICTURE
	ldx #5
	jmp stage2

; Wait for the rest of the tick, then scan the controls.
tiTickWait
	lda #TICK_MS
	jsr tiWait
	; fall through

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

; Music from the KERNAL's IRQ at 50 Hz: the tune, or with tiMusicAt any
; tune -- A/X = init, Y = the low byte of play, whose high
; byte is init's (every tune's play is init + 3).
tiMusicOn
	lda #<TUNE_INIT
	ldx #>TUNE_INIT
	ldy #<TUNE_PLAY
tiMusicAt
	sta tmInit+1
	stx tmInit+2
	sty tiPlay+1
	stx tiPlay+2
	sei
	jsr tiZpIn
	lda #0
	tax
	tay
tmInit	jsr TUNE_INIT
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
tiPlay	jsr TUNE_PLAY
	jsr tiZpOut
	jmp $ea31

; The six zero-page bytes the tunes use, which are the game's SRC/DST too.
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
	lda $0286
	sta tiVic+5
	rts

tiRestoreVic
	lda tiVic+3
	sta $d020
	lda tiVic+4
	sta $d021
	lda tiVic+5
	sta tiTextCol
	jsr tiClsWith
	lda tiVic+0
	sta $d011
	lda tiVic+1
	sta $d016
	lda tiVic+2
	sta $d018
	rts

tiTextColour
	lda #LTGREY
	sta tiTextCol
	; fall through
; The C64 screen blank and every cell's colour tiTextCol.
tiClsWith
	ldx #0
-	lda #$20
	sta SCREEN,x
	sta SCREEN+$100,x
	sta SCREEN+$200,x
	sta SCREEN+$2e8,x
	lda tiTextCol
	sta $d800,x
	sta $d900,x
	sta $da00,x
	sta $dae8,x
	inx
	bne -
	rts

tiCls
	lda #LTGREY
	sta tiTextCol
	jmp tiClsWith

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

skill		.byte SKILL_NORMAL	; 0 easy .. 3 nightmare
skillMask	.byte $02		; spawnflags high byte that leaves an entity out
tiStep		.byte 0
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
tiTextCol	.byte 0
tiOldIrq	.word 0
tiVic		.fill 6, 0
tiZpGame	.fill 6, 0
tiZpTune	.fill 6, 0
tiCol		.fill 40, 0
tiColShown	.fill 40, 0
tiPal		.fill 120, 0

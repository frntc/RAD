; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_pause.inc -- the pause menu: <- in play, as Quake's Escape.
; Included by gpu64_demo_game.a with the controls menu, under BASIC, which
; is banked out from `title` to `finish` -- the frame loop is inside that.
;
; <- held for PAUSE_HOLD frames stops the render loop (as the intermission
; does: LEVEL_PICTURE answers BUSY under a running loop) and draws Quake's
; menu over the faded console -- the plaque, PAUSED, RESUME, CONTROLS and
; MAIN MENU with the spinning dot -- redrawn whole every tick so a lost
; picture is back a tick later. The C64 shows the same menu in text; its game screen
; is saved to $c400/$c800 first and put back on the way out.
;
; RESUME, or <- pressed again, goes back to the frame loop: LOOP_START,
; and the frame clock re-marked so the pause is neither play time nor one
; enormous frame of motion. Nothing in the retained scene is touched --
; LOOP_STOP only stops the loop, and the next commit draws the level
; exactly where it was. CONTROLS is the skill menu's controls screen.
; MAIN MENU is the skill menu itself, to change the skill or the level:
; it leaves the frame loop for good, as the intermission's next level
; does, and comes back in through levelSetup -- a new game, since
; combatInit resets the player there.
;
; A pick by fire or SPACE waits (at most PM_LEAVE ticks) for the key to be
; let go, so the press that resumes does not also fire or jump; <- is
; latched until it is released, so the press that resumes does not pause
; again three frames later. Both <- edges are debounced over PM_DEB scans:
; a single bad keyboard sample must not open or close the menu.
;
; The waits are bounded twice, like the intermission's (imWait), because
; runsim's --frame-ms clock only moves on page flips. CONTROLS is not:
; its tick waits trust the clock alone, as on the title.
; ======================================================================

PAUSE_HOLD = 3			; frames <- must read down to pause
PM_DEB     = 16			; menu scans in a row that make a <- edge
PM_LEAVE   = 10			; ticks a pick waits for its key's release
PM_SCR     = $c400		; the C64 game screen, saved
PM_COL     = $c800		; ... and its colours
PM_ITEMS   = 3			; RESUME, CONTROLS, MAIN MENU
PM_MAIN    = 2

; ----------------------------------------------------------------------
; Once a frame, after the commit.
; ----------------------------------------------------------------------
pauseCheck
	lda keyD
	and #KD_LARR
	beq +
	lda #0				; up: the latch is off
	sta pmHold
	rts
+	lda pmHold
	bmi +				; still the press that resumed
	inc pmHold
	lda pmHold
	cmp #PAUSE_HOLD
	bcs pauseMenu
+	rts

pauseMenu
	jsr sfxOff
	lda #1
	sta CMD_HI
	ldx #8
pmStop
	lda #KEY_DESTRUCTIVE
	sta ARG+15
	lda #OP_LOOP_STOP
	jsr cmdSeq1
	lda seqMiss
	beq pmStopped
	dex
	bne pmStop
pmStopped
	lda #0
	sta CMD_HI
	jsr pmSave

	lda #0
	sta pmCur
	sta pmArm
	sta pmDeb
	sta pmLeave
	sta tiTick
	lda #$ff			; what is down from play is not a press
	sta tiIn
pmStatic
	jsr pmC64Static
pmTick
	jsr pmC64Lines
	jsr pmHdmi
	inc tiTick			; the dot's six frames
	lda tiTick
	cmp #6
	bcc +
	lda #0
	sta tiTick
+	jsr pmWait
	lda SIM_EXIT			; runsim's --stop-after: the frame loop
	cmp #SIM_EXIT_ON		; ends the run
	beq pmResume
	lda pmBack
	bne pmResume
	lda pmLeave
	beq pmKeys
	lda tiIn			; a pick: out once its key is up
	beq pmResume
	inc pmLeave
	lda pmLeave
	cmp #PM_LEAVE + 1
	bcs pmResume
	bcc pmTick
pmKeys
	lda pmNew
	lsr a
	bcc +
	dec pmCur			; up
	bpl +
	lda #PM_ITEMS-1
	sta pmCur
+	lda pmNew
	and #2
	beq +
	inc pmCur			; down
	lda pmCur
	cmp #PM_ITEMS
	bcc +
	lda #0
	sta pmCur
+	lda pmNew
	and #4
	beq pmTick
	lda pmCur
	bne pmControls
	inc pmLeave			; RESUME
	bne pmTick
pmControls
	cmp #PM_MAIN
	beq pmMainMenu
	jsr ctMenu
	lda #$ff
	sta tiIn
	jmp pmStatic

pmResume
	jsr pmRestore
	jsr sfxInit
	lda #$80			; <- latched until it is seen up
	sta pmHold
	lda #0
	sta ARG+0
	lda #OP_LOOP_START
	jsr cmd1
	jsr clkNow			; the pause was not a frame
	lda msNow
	sta msMark
	sta msFrameMark
	lda msNow+1
	sta msMark+1
	sta msFrameMark+1
	rts

; MAIN MENU: the title's own sequence from the skill menu on, then the
; whole per-level setup. The loop is already stopped (pmStop), and the
; C64 screen save is simply abandoned -- levelSetup draws its own.
pmMainMenu
	pla				; pauseCheck's return into the frame
	pla				; loop: levelSetup ends by entering it
	lda #$80			; <- latched until it is seen up
	sta pmHold
	lda #$ff			; the pick's fire is not a press
	sta tiIn
	jsr tiSaveVic
	jsr tiMusicOn
	jsr tiMenu
	jsr tiLoading
	jsr tiMusicOff
	jsr tiRestoreVic
	jsr tiSkillSet
	jsr sfxInit
	jmp levelSetup

; One tick: IM_TICK_MS, or IM_POLLS scans. pmNew = the menu bits that
; went down during it; pmBack = <- pressed (after being seen released).
pmWait
	lda #0
	sta pmNew
	sta pmBack
	lda #<IM_POLLS
	sta imPolls
	lda #>IM_POLLS
	sta imPolls+1
	jsr tiMark
pmwLoop
	jsr tiScan
	lda tiNew
	ora pmNew
	sta pmNew
	lda tiK7			; 1 = down
	and #KD_LARR
	beq pmwUp
	lda pmArm
	beq pmwEdge			; down before it was ever up: the opener
	inc pmDeb
	lda pmDeb
	cmp #PM_DEB
	bcc pmwClock
	lda #1
	sta pmBack
	bne pmwClock
pmwUp
	lda pmArm
	bne pmwEdge			; up and armed: wait for the press
	inc pmDeb
	lda pmDeb
	cmp #PM_DEB
	bcc pmwClock
	lda #1
	sta pmArm
pmwEdge
	lda #0
	sta pmDeb
pmwClock
	jsr clkNow
	lda tiT0
	sec
	sbc msNow
	tax
	lda tiT0+1
	sbc msNow+1
	bne pmwDone
	cpx #IM_TICK_MS
	bcs pmwDone
	lda imPolls
	bne +
	dec imPolls+1
	bmi pmwDone
+	dec imPolls
	jmp pmwLoop
pmwDone
	rts

; ----------------------------------------------------------------------
; The HDMI screen, whole, then a flip. M_Draw's layout, as the skill menu.
; ----------------------------------------------------------------------
pmHdmi
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
	lda #PIC_PAUSE
	ldx #PAUSE_X
	ldy #4
	jsr tiPicAt
	lda #1
	jsr tiPic
	lda #PIC_PM_RESUME
	ldx #MENU_X
	ldy #MENU_Y + 4			; the labels are 16 high, the dot 24
	jsr tiPicAt
	lda #1
	jsr tiPic
	lda #PIC_SK_CONTROLS
	ldx #MENU_X
	ldy #MENU_Y + MENU_DY + 4
	jsr tiPicAt
	lda #1
	jsr tiPic
	lda #PIC_PM_MAINMENU
	ldx #MENU_X
	ldy #MENU_Y + 2 * MENU_DY + 4
	jsr tiPicAt
	lda #1
	jsr tiPic
	lda tiTick
	clc
	adc #PIC_DOT0
	ldx pmCur
	ldy tiMenuY,x
	ldx #DOT_X
	jsr tiPicAt
	lda #1
	jsr tiPic
	jmp dmFlip

; ----------------------------------------------------------------------
; The C64 copy.
; ----------------------------------------------------------------------
pmC64Static
	jsr tiCls
	lda #3
	jsr dmRow
	jsr dmText
	.text "                PAUSED"
	.byte 0
	lda #5
	jsr dmRow
	jsr dmText
	.text "                 "
	.byte 0
	jsr lvName
	lda #9
	jsr dmRow
	jsr dmText
	.text "               RESUME"
	.byte 0
	lda #11
	jsr dmRow
	jsr dmText
	.text "               CONTROLS"
	.byte 0
	lda #13
	jsr dmRow
	jsr dmText
	.text "               MAIN MENU"
	.byte 0
	lda #20
	jsr dmRow
	jsr dmText
	.text "   W/S OR JOYSTICK, FIRE TO PICK"
	.byte 0
	lda #21
	jsr dmRow
	jsr dmText
	.text "   <- AGAIN RESUMES"
	.byte 0
	lda #23
	jsr dmRow
	jsr dmText
	.text "   IN PLAY, H IS THE HEALTH CHEAT"
	.byte 0
	rts

; The cursor and the highlight, rows 9, 11 and 13.
pmC64Lines
	ldx #0
pclLoop
	lda pmRows,x
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
	cpx pmCur
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
	cpx #PM_ITEMS
	bne pclLoop
	rts

; The game's C64 screen, out and back.
pmSave
	ldx #0
-	lda SCREEN,x
	sta PM_SCR,x
	lda SCREEN+$100,x
	sta PM_SCR+$100,x
	lda SCREEN+$200,x
	sta PM_SCR+$200,x
	lda SCREEN+$2e8,x
	sta PM_SCR+$2e8,x
	lda $d800,x
	sta PM_COL,x
	lda $d900,x
	sta PM_COL+$100,x
	lda $da00,x
	sta PM_COL+$200,x
	lda $dae8,x
	sta PM_COL+$2e8,x
	inx
	bne -
	rts

pmRestore
	ldx #0
-	lda PM_SCR,x
	sta SCREEN,x
	lda PM_SCR+$100,x
	sta SCREEN+$100,x
	lda PM_SCR+$200,x
	sta SCREEN+$200,x
	lda PM_SCR+$2e8,x
	sta SCREEN+$2e8,x
	lda PM_COL,x
	sta $d800,x
	lda PM_COL+$100,x
	sta $d900,x
	lda PM_COL+$200,x
	sta $da00,x
	lda PM_COL+$2e8,x
	sta $dae8,x
	inx
	bne -
	rts

pmRows		.byte 9, 11, 13
pmHold		.byte 0		; frames <- down; bit 7: latched after a resume
pmCur		.byte 0
pmArm		.byte 0		; <- has been seen released in the menu
pmDeb		.byte 0
pmBack		.byte 0
pmNew		.byte 0
pmLeave		.byte 0		; ticks since RESUME was picked, 0 = none

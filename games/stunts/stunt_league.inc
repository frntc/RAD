; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; stunt_league.inc -- the menus and the league, on the HDMI screen.
; Included by gpu64_demo_stunt.a in the main code; `mainMenu` is jumped
; to once the uploads are done, and never returns except through RUN/STOP
; to BASIC.
;
; THE LEAGUE
;
; Twelve drivers in four divisions of three, as in the 1988 original.
; Each division has two tracks; in a season every pair of drivers in a
; division races once on each, so the player races four times -- each
; rival twice -- and the two rivals race each other twice off screen. A
; win is 2 points and the fastest lap of the race 1 more. At the end of
; the season the top of each division goes up and the bottom comes down;
; the top of division 1 is the champion. Off-screen races are decided by
; the two drivers' skills and a dice roll; a rival the player races
; drives with its skill's speeds (setRival).
;
; THE SCREENS
;
; Every screen is PIC_MENU (the chequered frame), a heading picture and
; text, all LEVEL_PICTUREs out of stunts.reu: the text is one 9x9 glyph
; picture per character (stunt_pics.py). A screen is drawn whole and
; flipped when it changes and once a second regardless, so a command the
; bus lost costs at most a second of a missing letter.
; ======================================================================

LG_TICK	= 40			; ms a menu tick
LG_HEAL	= 25			; ticks between whole redraws
LG_RACES = 4			; the player's races in a season
DRV_PRACT = 12			; practice's rival: not in the league

mnAt	.macro px, py
	lda #<\px
	sta mnX
	lda #>\px
	sta mnX+1
	lda #\py
	sta mnY
	.endm
mnSetC	.macro set
	lda #\set
	sta mnSet
	.endm

; ----------------------------------------------------------------------
; Drawing.
; ----------------------------------------------------------------------

; A = picture: at mnX/mnY, flags mnFl. Checked like every class 1 send.
mnPic
	sta stBuf+0
	lda mnX
	sta stBuf+1
	lda mnX+1
	sta stBuf+2
	lda mnY
	sta stBuf+3
	lda #0
	sta stBuf+4
	sta stId
	sta stId+1
	lda mnFl
	sta stBuf+5
	lda #OP_LEVEL_PICTURE
	ldx #5
	jmp stage2

; The frame and its panel, with the game's palette; text keyed from here.
mnBegin
	lda #0
	sta mnX
	sta mnX+1
	sta mnY
	lda #2
	sta mnFl
	lda #PIC_MENU
	jsr mnPic
	lda #1
	sta mnFl
	rts

; A = HD_*: the heading, centred at the top.
mnHead
	pha
	asl a
	tax
	sec
	lda #<320
	sbc headW,x
	sta mnX
	lda #>320
	sbc headW+1,x
	lsr a
	sta mnX+1
	ror mnX
	lda #12
	sta mnY
	pla
	clc
	adc #PIC_HEAD
	jmp mnPic

; A = a character (ASCII upper case): its glyph in mnSet at mnX/mnY, and
; mnX on by 8. A space draws nothing.
mnChar
	and #$3f
	cmp #$20
	beq mcNext
	sta mnG
	lda mnSet
	.rept 6
	asl a
	.endrept
	clc
	adc mnG
	adc #PIC_GLY
	jsr mnPic
mcNext
	clc
	lda mnX
	adc #8
	sta mnX
	bcc +
	inc mnX+1
+	rts

; The 0-terminated string that follows the jsr.
mnStr
	pla
	sta SRC
	pla
	sta SRC+1
	jsr mnInc
	jsr mnPrint
	lda SRC+1
	pha
	lda SRC
	pha
	rts

; A/X = lo/hi of a 0-terminated string.
mnPrintAX
	sta SRC
	stx SRC+1
; SRC = a 0-terminated string; left at its 0.
mnPrint
	ldy #0
	lda (SRC),y
	beq +
	jsr mnChar
	jsr mnInc
	jmp mnPrint
+	rts

mnInc
	inc SRC
	bne +
	inc SRC+1
+	rts

; X = driver: the name.
mnName
	txa
	asl a
	tax
	lda drvName,x
	pha
	lda drvName+1,x
	tax
	pla
	jmp mnPrintAX

; X = track: the name.
mnTrack
	txa
	asl a
	tax
	lda trkName,x
	pha
	lda trkName+1,x
	tax
	pla
	jmp mnPrintAX

; A = 0-255 in decimal, no leading zeros.
mnDec
	sta mnT
	lda #0
	sta mnLead
	ldx #100
	jsr mnDigit
	ldx #10
	jsr mnDigit
	lda mnT
	ora #$30
	jmp mnChar
mnDigit
	stx mnD
	ldx #0
-	lda mnT
	cmp mnD
	bcc +
	sbc mnD
	sta mnT
	inx
	bne -
+	txa
	ora mnLead
	beq +
	sta mnLead
	txa
	ora #$30
	jmp mnChar
+	rts

; mnTw = tenths of a second: "SS.T", or "--.-" for none.
mnTime
	lda mnTw
	ora mnTw+1
	bne +
	jsr mnStr
	.text "--.-"
	.byte 0
	rts
+	lda #0
	ldx #16
-	asl mnTw
	rol mnTw+1
	rol a
	cmp #10
	bcc +
	sbc #10
	inc mnTw
+	dex
	bne -
	pha
	lda mnTw+1
	beq +
	lda #255
	sta mnTw
+	lda mnTw
	jsr mnDec
	lda #'.'
	jsr mnChar
	pla
	ora #$30
	jmp mnChar

; ----------------------------------------------------------------------
; Input and pacing: tiScan's bits (0 up, 1 down, 2 select), plus RUN/STOP.
; ----------------------------------------------------------------------

; A menu's first tick: the pace from now, whatever is held already is
; not a press, and the screen drawn.
mnEnter
	jsr tiMark
	jsr tiScan
	jsr tiScan
	lda tiK7
	and #$80
	sta mnStopIn
	lda #1
	sta mnDirty
	lda #0
	sta mnHealT
	rts

; A tick. A = tiNew; mnStop = RUN/STOP went down this tick. Asks for a
; whole redraw every LG_HEAL ticks.
mnTick
	lda #LG_TICK
	jsr tiWait
	jsr tiScan
	lda tiK7
	and #$80
	tax
	eor mnStopIn
	and #$80
	and tiK7
	sta mnStop
	stx mnStopIn
	inc mnHealT
	lda mnHealT
	cmp #LG_HEAL
	bcc +
	lda #0
	sta mnHealT
	inc mnDirty
+	inc mnBlink
	lda tiNew
	rts

; ----------------------------------------------------------------------
; The main menu.
; ----------------------------------------------------------------------
MM_N	= 4

mainMenu
	lda #$36			; the tune lives under BASIC
	sta $01
	jsr tiMusicOn
mmEnter
	jsr mnEnter
mmLoop
	lda mnDirty
	beq +
	jsr mmDraw
	lda #0
	sta mnDirty
+	jsr mnTick
	lsr a
	bcc +
	dec mmSel
	bpl mmMoved
	lda #MM_N - 1
	sta mmSel
	bpl mmMoved
+	lsr a
	bcc +
	inc mmSel
	lda mmSel
	cmp #MM_N
	bcc mmMoved
	lda #0
	sta mmSel
	beq mmMoved
+	lsr a
	bcs mmSelect
	lda mnStop
	beq mmLoop
	jsr tiMusicOff
	jmp quit
mmMoved
	inc mnDirty
	jmp mmLoop
mmSelect
	lda mmSel
	bne +
	jsr lgNew
	jsr season
	jmp mmEnter
+	cmp #1
	bne +
	lda lgActive
	beq mmLoop
	jsr season
	jmp mmEnter
+	cmp #2
	bne +
	jsr practise
	jmp mmEnter
+	jsr controls
	jmp mmEnter

mmDraw
	jsr mnBegin
	lda #HD_STUNT
	jsr mnHead
	lda #0
	sta mmI
-	lda #<64
	sta mnX
	lda #>64
	sta mnX+1
	lda mmI
	asl a
	asl a
	asl a
	asl a
	clc
	adc #60
	sta mnY
	ldx #GLY_WHITE
	lda mmI
	cmp #1
	bne +
	lda lgActive
	bne +
	ldx #GLY_SKY			; no season to continue
+	lda mmI
	cmp mmSel
	bne +
	ldx #GLY_YELLOW
+	stx mnSet
	lda mmI
	cmp mmSel
	bne +
	lda #<48
	sta mnX
	lda #'>'
	jsr mnChar
	lda #<64
	sta mnX
+	lda mmI
	asl a
	tax
	lda mmText,x
	pha
	lda mmText+1,x
	tax
	pla
	jsr mnPrintAX
	inc mmI
	lda mmI
	cmp #MM_N
	bne -
	#mnSetC GLY_SKY
	#mnAt 32, 160
	jsr mnStr
	.text "UP/DOWN TO CHOOSE, FIRE TO SELECT"
	.byte 0
	#mnAt 96, 174
	jsr mnStr
	.text "RUN/STOP TO QUIT"
	.byte 0
	jmp dmFlip

mmText	.word mmT0, mmT1, mmT2, mmT3
mmT0	.text "START A NEW SEASON"
	.byte 0
mmT1	.text "CONTINUE THE SEASON"
	.byte 0
mmT2	.text "PRACTISE A TRACK"
	.byte 0
mmT3	.text "CONTROLS"
	.byte 0

; ----------------------------------------------------------------------
; Controls.
; ----------------------------------------------------------------------
controls
	jsr mnEnter
-	lda mnDirty
	beq +
	jsr ctDraw
	lda #0
	sta mnDirty
+	jsr mnTick
	and #4
	bne +
	lda mnStop
	beq -
+	rts

ctDraw
	jsr mnBegin
	lda #HD_CONTROLS
	jsr mnHead
	#mnSetC GLY_SKY
	#mnAt 24, 40
	jsr mnStr
	.text "JOYSTICK IN PORT 2, OR THE KEYS"
	.byte 0
	lda #<ctText
	sta SRC
	lda #>ctText
	sta SRC+1
	lda #56
	sta mnY
-	lda #<24
	sta mnX
	lda #>24
	sta mnX+1
	#mnSetC GLY_YELLOW
	jsr mnPrint
	jsr mnInc
	lda #<152
	sta mnX
	#mnSetC GLY_WHITE
	jsr mnPrint
	jsr mnInc
	lda mnY
	clc
	adc #13
	sta mnY
	ldy #0
	lda (SRC),y
	bne -
	#mnSetC GLY_SKY
	#mnAt 112, 174
	jsr mnStr
	.text "PRESS FIRE"
	.byte 0
	jmp dmFlip

ctText	.text "UP  / W", 0, "ACCELERATE", 0
	.text "DOWN / S", 0, "BRAKE", 0
	.text "LEFT / A", 0, "STEER LEFT", 0
	.text "RIGHT / D", 0, "STEER RIGHT", 0
	.text "FIRE / SPACE", 0, "BOOST", 0
	.text "F3 (PRACTISE)", 0, "AUTOPILOT", 0
	.text "HOLD RUN/STOP", 0, "LEAVE THE RACE", 0
	.byte 0

; ----------------------------------------------------------------------
; Practice: any track, against a middling rival, no points.
; ----------------------------------------------------------------------
practise
	lda #1
	sta lgPract
	jsr mnEnter
prLoop
	lda mnDirty
	beq +
	jsr prDraw
	lda #0
	sta mnDirty
+	jsr mnTick
	lsr a
	bcc +
	dec prTrk
	jmp prMoved
+	lsr a
	bcc +
	inc prTrk
	jmp prMoved
+	lsr a
	bcs prRace
	lda mnStop
	beq prLoop
	lda #0
	sta lgPract
	rts
prMoved
	lda prTrk
	and #TRACKS - 1
	sta prTrk
	inc mnDirty
	jmp prLoop
prRace
	lda #DRV_PRACT
	sta lgOpp
	lda prTrk
	sta curTrack
	jsr raceGo
	jsr lgScore
	jsr resultScreen
	jmp practise

prDraw
	jsr mnBegin
	lda #HD_PRACTISE
	jsr mnHead
	#mnSetC GLY_YELLOW
	; the name, centred: 4 px a character either side of 160
	ldx prTrk
	txa
	asl a
	tax
	lda trkName,x
	sta SRC
	lda trkName+1,x
	sta SRC+1
	ldy #0
-	lda (SRC),y
	beq +
	iny
	bne -
+	tya
	asl a
	asl a
	sta mnT
	sec
	lda #160
	sbc mnT
	sta mnX
	lda #0
	sta mnX+1
	lda #42
	sta mnY
	jsr mnPrint
	#mnAt (320 - PIC_TRACK_W) / 2, 58
	lda #0
	sta mnFl
	lda prTrk
	clc
	adc #PIC_TRACK
	jsr mnPic
	lda #1
	sta mnFl
	#mnSetC GLY_SKY
	#mnAt 24, 162
	jsr mnStr
	.text "UP/DOWN TRACK  FIRE RACE"
	.byte 0
	#mnAt 24, 176
	jsr mnStr
	.text "RUN/STOP BACK"
	.byte 0
	jmp dmFlip

; ----------------------------------------------------------------------
; The season.
; ----------------------------------------------------------------------
season
	jsr lgWhere
	lda lgRace
	cmp #LG_RACES
	bcc +
	jsr seasonEnd
	jmp season
+	jsr lgNext
	jsr mnEnter
snLoop
	lda mnDirty
	beq +
	jsr hubDraw
	lda #0
	sta mnDirty
+	jsr mnTick
	and #4
	bne snRace
	lda mnStop
	beq snLoop
	rts
snRace
	jsr raceGo
	jsr lgScore
	jsr resultScreen
	inc lgRace
	lda lgRace
	and #1
	bne +
	; after each track's two: the rivals race each other on it
	lda lgO1
	sta lgA
	lda lgO2
	sta lgB
	jsr lgSim
+	jmp season

; The division's table, the next race, its track.
hubDraw
	jsr mnBegin
	lda plDiv
	clc
	adc #HD_DIV1
	jsr mnHead
	lda plDiv
	jsr lgTable
	#mnSetC GLY_SKY
	#mnAt 24, 90
	jsr mnStr
	.text "RACE "
	.byte 0
	lda lgRace
	clc
	adc #1
	jsr mnDec
	jsr mnStr
	.text " OF 4"
	.byte 0
	#mnSetC GLY_WHITE
	#mnAt 24, 104
	jsr mnStr
	.text "YOU  V"
	.byte 0
	#mnAt 24, 116
	ldx lgOpp
	jsr mnName
	#mnSetC GLY_YELLOW
	#mnAt 16, 136
	ldx lgTrk
	jsr mnTrack
	#mnAt 148, 88
	lda #0
	sta mnFl
	lda lgTrk
	clc
	adc #PIC_TRACK
	jsr mnPic
	lda #1
	sta mnFl
	#mnSetC GLY_SKY
	#mnAt 24, 162
	jsr mnStr
	.text "FIRE TO RACE"
	.byte 0
	#mnAt 24, 176
	jsr mnStr
	.text "RUN/STOP MENU"
	.byte 0
	jmp dmFlip

; A = division: its table, best first, at y 38..80.
lgTable
	jsr lgRank
	#mnSetC GLY_SKY
	#mnAt 24, 38
	jsr mnStr
	.text "DRIVER"
	.byte 0
	#mnAt 168, 38
	jsr mnStr
	.text "RACED WON PTS"
	.byte 0
	lda #0
	sta mmI
-	lda mmI
	asl a
	asl a
	sta mnT
	asl a
	clc
	adc mnT				; 12 a row
	adc #52
	sta mnY
	ldx mmI
	lda lgRk,x
	sta lgA
	ldx #GLY_WHITE
	cmp #0
	bne +
	ldx #GLY_YELLOW
+	stx mnSet
	lda #<24
	sta mnX
	lda #>24
	sta mnX+1
	ldx lgA
	jsr mnName
	lda #<184
	sta mnX
	ldx lgA
	lda drvRaced,x
	jsr mnDec
	lda #<224
	sta mnX
	ldx lgA
	lda drvWon,x
	jsr mnDec
	lda #<256
	sta mnX
	lda #>256
	sta mnX+1
	ldx lgA
	lda drvPts,x
	jsr mnDec
	inc mmI
	lda mmI
	cmp #3
	bne -
	rts

; After a race: who won, the laps, the points. Until fire.
resultScreen
	jsr mnEnter
-	lda mnDirty
	beq +
	jsr rsDraw
	lda #0
	sta mnDirty
+	jsr mnTick
	and #4
	beq -
	rts

rsDraw
	jsr mnBegin
	lda #HD_RACE
	jsr mnHead
	#mnSetC GLY_SKY
	#mnAt 40, 44
	jsr mnStr
	.text "WINNER"
	.byte 0
	#mnSetC GLY_YELLOW
	#mnAt 40, 56
	ldx #0
	lda winner
	beq +
	ldx lgOpp
+	jsr mnName
	#mnSetC GLY_SKY
	#mnAt 40, 80
	jsr mnStr
	.text "BEST LAP"
	.byte 0
	#mnSetC GLY_WHITE
	#mnAt 40, 92
	ldx #0
	jsr mnName
	#mnAt 200, 92
	lda cBestLo+0
	sta mnTw
	lda cBestHi+0
	sta mnTw+1
	jsr mnTime
	#mnAt 40, 104
	ldx lgOpp
	jsr mnName
	#mnAt 200, 104
	lda cBestLo+1
	sta mnTw
	lda cBestHi+1
	sta mnTw+1
	jsr mnTime
	lda lgPract
	beq +
	jmp rsFire
+	#mnSetC GLY_SKY
	#mnAt 40, 128
	jsr mnStr
	.text "POINTS"
	.byte 0
	#mnSetC GLY_WHITE
	#mnAt 40, 140
	ldx #0
	jsr mnName
	#mnAt 200, 140
	lda #'+'
	jsr mnChar
	lda rsPtsP
	jsr mnDec
	#mnAt 40, 152
	ldx lgOpp
	jsr mnName
	#mnAt 200, 152
	lda #'+'
	jsr mnChar
	lda rsPtsO
	jsr mnDec
rsFire
	#mnSetC GLY_SKY
	#mnAt 120, 176
	jsr mnStr
	.text "PRESS FIRE"
	.byte 0
	jmp dmFlip

; The season is over: the other divisions' races, the table and what it
; means for the player, then the moves, and a new season.
seasonEnd
	lda #0
	sta lgD
-	lda lgD
	cmp plDiv
	beq +
	jsr lgSimDiv
+	inc lgD
	lda lgD
	cmp #4
	bne -

	; every division's top and bottom, before anything moves
	lda #3
	sta lgD
-	lda lgD
	jsr lgRank
	ldx lgD
	lda lgRk+0
	sta lgTop,x
	lda lgRk+2
	sta lgBot,x
	dec lgD
	bpl -

	; the verdict: 0 champion, 1 promoted, 2 stays, 3 relegated
	ldx plDiv
	lda #2
	ldy lgTop,x
	bne +
	lda #1
	cpx #0
	bne +
	lda #0
+	ldy lgBot,x
	bne +
	cpx #3
	beq +
	lda #3
+	sta lgVerdict

	jsr mnEnter
-	lda mnDirty
	beq +
	jsr seDraw
	lda #0
	sta mnDirty
+	jsr mnTick
	and #4
	beq -

	; the moves: each division's top swaps with the bottom of the one
	; above
	lda #1
	sta lgD
-	ldx lgD
	lda lgTop,x
	sta lgA
	lda lgBot-1,x
	sta lgB
	jsr lgSwap
	inc lgD
	lda lgD
	cmp #4
	bne -

	; a new season
	jsr lgClear
	inc lgSeason
	rts

seDraw
	jsr mnBegin
	lda #HD_END
	jsr mnHead
	lda plDiv
	jsr lgTable
	#mnSetC GLY_YELLOW
	#mnAt 24, 100
	lda lgVerdict
	asl a
	tax
	lda seText,x
	pha
	lda seText+1,x
	tax
	pla
	jsr mnPrintAX
	lda lgVerdict
	cmp #1
	beq +
	cmp #3
	bne seStays
+	ldx plDiv			; promoted: plDiv - 1 + 1; relegated: + 2
	inx
	lda lgVerdict
	cmp #1
	beq +
	inx
	inx
+	dex
	txa
	jsr mnDec
	jmp seFire
seStays
	cmp #2
	bne seFire
	lda plDiv
	clc
	adc #1
	jsr mnDec
seFire
	#mnSetC GLY_SKY
	#mnAt 120, 176
	jsr mnStr
	.text "PRESS FIRE"
	.byte 0
	jmp dmFlip

seText	.word seT0, seT1, seT2, seT3
seT0	.text "LEAGUE CHAMPION!"
	.byte 0
seT1	.text "PROMOTED TO DIVISION "
	.byte 0
seT2	.text "STAYING IN DIVISION "
	.byte 0
seT3	.text "RELEGATED TO DIVISION "
	.byte 0

; ----------------------------------------------------------------------
; The league's arithmetic.
; ----------------------------------------------------------------------

; A new league: the drivers where they start, no points, race 1.
lgNew
	ldx #11
-	lda divInit,x
	sta divDrv,x
	dex
	bpl -
	jsr clkNow
	lda msNow
	eor msNow+1
	ora #1
	sta rndS
	lda #1
	sta lgActive
	sta lgSeason
	lda #0
	sta lgPract
lgClear
	ldx #12
	lda #0
-	sta drvPts,x
	sta drvWon,x
	sta drvRaced,x
	dex
	bpl -
	sta lgRace
	rts

; plDiv = the player's division, lgO1/lgO2 = the other two in it.
lgWhere
	ldx #11
-	lda divDrv,x
	beq +
	dex
	bpl -
+	lda divOf,x
	sta plDiv
	tay
	lda divBase,y
	tay
	ldx #0
-	lda divDrv,y
	beq +
	sta lgO1,x
	inx
+	iny
	cpx #2
	bne -
	rts

; Race lgRace: lgOpp, lgTrk, and the rival's speeds.
lgNext
	lda lgRace
	and #1
	tax
	lda lgO1,x
	sta lgOpp
	lda #3
	sec
	sbc plDiv
	asl a
	sta lgTrk
	lda lgRace
	lsr a
	ora lgTrk
	sta lgTrk
	sta curTrack
	rts

; The player's race is over: winner, cBest and C (set = abandoned) into
; points, for the table and for the result screen.
lgScore
	lda #0
	sta rsPtsP
	sta rsPtsO
	bcc +
	lda #1				; walked away from it: a loss, no lap
	sta winner
	lda #0
	sta cBestLo+0
	sta cBestHi+0
+	lda winner
	bne +
	lda #2
	sta rsPtsP
	bne ++
+	lda #2
	sta rsPtsO
+	; the fastest lap: a time of 0 is no lap at all
	lda cBestLo+0
	ora cBestHi+0
	beq lsRival
	lda cBestLo+1
	ora cBestHi+1
	beq lsPlayer
	lda cBestLo+1
	cmp cBestLo+0
	lda cBestHi+1
	sbc cBestHi+0
	bcc lsRival			; the rival's is shorter
lsPlayer
	inc rsPtsP
	jmp lsAdd
lsRival
	lda cBestLo+1
	ora cBestHi+1
	beq lsAdd
	inc rsPtsO
lsAdd
	lda lgPract
	bne lgsDone
	ldx #0
	lda rsPtsP
	jsr lgAward
	ldx lgOpp
	lda rsPtsO
	jsr lgAward
	lda winner
	bne +
	inc drvWon+0
	rts
+	ldx lgOpp
	inc drvWon,x
lgsDone
	rts

; X = driver, A = points: a race run.
lgAward
	clc
	adc drvPts,x
	sta drvPts,x
	inc drvRaced,x
	rts

; lgA against lgB, off screen: the skills and the dice.
lgSim
	jsr rnd
	sta mnT
	ldx lgA
	ldy lgB
	lda drvSkill,x
	sec
	sbc drvSkill,y
	asl a
	asl a
	asl a
	clc
	adc #128			; 8..248: lgA's odds in 256
	cmp mnT
	bcs +
	lda lgA				; lgB won: the winner first
	ldx lgB
	stx lgA
	sta lgB
+	ldx lgA
	inc drvWon,x
	lda #2
	jsr lgAward
	ldx lgB
	lda #0
	jsr lgAward
	jsr rnd
	ldx lgA				; the winner has the fastest lap, mostly
	cmp #192
	bcc +
	ldx lgB
+	inc drvPts,x
	rts

; A = division other than the player's: its whole season.
lgSimDiv
	tay
	lda divBase,y
	sta lgI
	lda #2
	sta lgN				; two tracks
lsdLoop
	ldy lgI
	lda divDrv,y
	sta lgA
	lda divDrv+1,y
	sta lgB
	jsr lgSim
	ldy lgI
	lda divDrv,y
	sta lgA
	lda divDrv+2,y
	sta lgB
	jsr lgSim
	ldy lgI
	lda divDrv+1,y
	sta lgA
	lda divDrv+2,y
	sta lgB
	jsr lgSim
	dec lgN
	bne lsdLoop
	rts

; A = division: lgRk = its three drivers, most points first, then most
; wins; a tie stays in table order.
lgRank
	tay
	lda divBase,y
	tay
	ldx #0
-	lda divDrv,y
	sta lgRk,x
	iny
	inx
	cpx #3
	bne -
	ldx #0
	jsr lgCmpSwap
	ldx #1
	jsr lgCmpSwap
	ldx #0
lgCmpSwap
	ldy lgRk+1,x
	lda drvPts,y
	ldy lgRk,x
	cmp drvPts,y
	beq +
	bcc lcsKeep
	bcs lcsSwap
+	ldy lgRk+1,x
	lda drvWon,y
	ldy lgRk,x
	cmp drvWon,y
	beq lcsKeep
	bcc lcsKeep
lcsSwap
	lda lgRk,x
	ldy lgRk+1,x
	sta lgRk+1,x
	tya
	sta lgRk,x
lcsKeep
	rts

; Drivers lgA and lgB change places in divDrv.
lgSwap
	ldx #11
-	lda divDrv,x
	cmp lgA
	bne +
	lda lgB
	sta divDrv,x
	jmp lswNext
+	cmp lgB
	bne lswNext
	lda lgA
	sta divDrv,x
lswNext
	dex
	bpl -
	rts

; A = an 8-bit pseudo-random number.
rnd
	lda rndS
	asl a
	asl a
	clc
	adc rndS
	clc
	adc #17
	sta rndS
	eor frames
	rts

; A = skill 0-15: the rival's speeds, led and chasing. Skill 15 is the
; old fixed rival, $33 and $3c.
setRival
	clc
	adc #$24
	sta rvLead
	adc #9
	sta rvBack
	rts

; Back to BASIC from the main menu: the loop is already stopped.
quit
	lda #$37
	sta $01
	jsr $e544
	rts

; A race from the menus: the music stops for it. C = abandoned.
.weak
LGTEST	= 0
.endweak
raceGo
	; The autopilot is practice's: a league race is driven by hand. Not
	; under runsim ($02aa, its telemetry byte), which has no hands.
	ldx #0
	lda lgPract
	bne +
	lda $02aa
	cmp #$aa
	beq +
	stx autoPilot
	inx
+	stx apLock
	ldx lgOpp
	lda drvSkill,x
	jsr setRival
	jsr tiMusicOff
.if LGTEST
	; test builds (-D LGTEST=1): no race, the player wins 20.0 to 25.0
	lda #0
	sta winner
	lda #<200
	sta cBestLo
	lda #>200
	sta cBestHi
	lda #<250
	sta cBestLo+1
	lda #>250
	sta cBestHi+1
	clc
.else
	jsr runRace
.endif
	php
	lda #$36
	sta $01
	jsr tiMusicOn
	plp
	rts

; ----------------------------------------------------------------------
; The drivers. 0 is the player.
; ----------------------------------------------------------------------
drvName	.word dnYou, dnBaron, dnRocket, dnAce, dnDust, dnWheel, dnLead
	.word dnNed, dnGear, dnCrash, dnSparky, dnSlider, dnRival
drvSkill .byte 0, 15, 14, 13, 11, 10, 9, 7, 6, 5, 3, 1, 8
dnYou	.text "YOU"
	.byte 0
dnBaron	.text "THE BARON"
	.byte 0
dnRocket .text "ROCKET ROD"
	.byte 0
dnAce	.text "FLYING ACE"
	.byte 0
dnDust	.text "DUST DEVIL"
	.byte 0
dnWheel	.text "BIG WHEEL"
	.byte 0
dnLead	.text "LEAD FOOT"
	.byte 0
dnNed	.text "NITRO NED"
	.byte 0
dnGear	.text "GEARBOX"
	.byte 0
dnCrash	.text "CRASH KID"
	.byte 0
dnSparky .text "SPARKY"
	.byte 0
dnSlider .text "SLIDER"
	.byte 0
dnRival	.text "RIVAL"
	.byte 0

divInit	.byte 1, 2, 3,  4, 5, 6,  7, 8, 9,  0, 10, 11
divBase	.byte 0, 3, 6, 9
divOf	.byte 0, 0, 0, 1, 1, 1, 2, 2, 2, 3, 3, 3

; ----------------------------------------------------------------------
mnX	.word 0
mnY	.byte 0
mnFl	.byte 1
mnSet	.byte 0
mnT	.byte 0
mnG	.byte 0
mnD	.byte 0
mnLead	.byte 0
mnTw	.word 0
mnDirty	.byte 0
mnHealT	.byte 0
mnBlink	.byte 0
mnStop	.byte 0
mnStopIn .byte 0
mmSel	.byte 0
mmI	.byte 0
prTrk	.byte 0
lgActive .byte 0
lgPract	.byte 0
lgSeason .byte 0
lgRace	.byte 0
lgOpp	.byte 0
lgTrk	.byte 0
lgO1	.byte 0
lgO2	.byte 0
lgA	.byte 0
lgB	.byte 0
lgD	.byte 0
lgI	.byte 0
lgN	.byte 0
lgVerdict .byte 0
lgRk	.fill 3, 0
lgTop	.fill 4, 0
lgBot	.fill 4, 0
plDiv	.byte 3
rsPtsP	.byte 0
rsPtsO	.byte 0
rndS	.byte 1
apLock	.byte 0			; 1: F3 does nothing (a league race)
simRun	.byte 0			; the title was skipped: runsim's old race loop
rvLead	.byte $33		; the rival's speeds, leading and chasing
rvBack	.byte $3c
raceTen	.word 0			; tenths since the start, for both cars' laps
cStLo	.fill 2, 0		; when each car's lap began
cStHi	.fill 2, 0
cBestLo	.fill 2, 0		; each car's best lap, tenths; 0 = none
cBestHi	.fill 2, 0
ltT	.word 0
divDrv	.fill 12, 0
drvPts	.fill 13, 0
drvWon	.fill 13, 0
drvRaced .fill 13, 0

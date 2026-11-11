; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_exit.inc -- milestone 20 stage F: the end of the level.
;
; Four things Quake's E1M1 needs before it can be finished, and one it
; needs to look like Quake while it is being played:
;
;   trigger_counter  the three buttons behind the slime room. Each fires
;                    t9; the counter (count 3) fires t10, the door, on the
;                    third. It is NOT a touch volume: its model-28 box is
;                    only what the converter had to hand it, and treating
;                    it as one opened t10 without a single button.
;   wait -1          func_door and func_button whose wait is -1 stay open.
;                    A spent trigger_once's door that closed after OPEN_WAIT
;                    was closed for good, and a button that popped back out
;                    could be pressed again -- which a counter would count.
;   the exit         trigger_changelevel: the intermission, then the level
;                    its 'map' names (the converter puts it in p1 as E1Mn's
;                    n, so E1M4's secret exit really leads to E1M8). 'start',
;                    the end of the episode, goes back to E1M1.
;   the stats        kills (combat), secrets (trigger_secret) and the time,
;                    counted in play, shown on both screens at the exit.
;   muzzle flash     one point-light node, lit at the eye for FLASH_FRAMES
;                    after a shot (not the axe).
;
; The intermission is Quake's Sbar_IntermissionOverlay on the HDMI screen:
; LEVEL_PICTURE draws COMPLETE, INTER and the number glyphs over CONFADE,
; with the loop stopped (LEVEL_PICTURE answers BUSY under it), and redraws
; the lot every tick so a dropped picture is back a tick later. The C64
; shows the same numbers in text and plays E-Quake tune 1.
;
; Waits here are bounded twice: by the millisecond clock and by a poll
; count. runsim's --frame-ms locks that clock to page flips, so a wait that
; trusted the clock alone would never end in a simulated run.
; ======================================================================

K_TRIG_CHANGE  = 25		; trigger_changelevel
K_TRIG_COUNTER = 26		; trigger_counter
MAX_CNT        = 4

FLASH_ID     = 90		; free: actors are 2..85, the level 100 up
FLASH_FRAMES = 2
BOOM_FRAMES  = 6		; a grenade's blast
FLASH_S      = 6		; colormap levels at the centre
FLASH_R      = 25 * 256		; 8.8 world units: Quake's 200

IM_TICK_MS = 100
IM_POLLS   = 500		; the second bound on a tick (see above)
IM_ARM     = 20			; ticks before a press counts: ~2 s
MSG_FRAMES = 60			; how long a counter message stays up
MSG_ROW    = 3


; ----------------------------------------------------------------------
; Level load: a trigger_counter record. Called from etTrig with the record
; in entBlk; the counter keeps its own table and is never touched.
; ----------------------------------------------------------------------
cntTake
	ldx nCnt
	cpx #MAX_CNT
	bcc +
	jmp etDrop
+	lda entBlk+ENT_TNAME
	sta cntTnL,x
	lda entBlk+ENT_TNAME+1
	sta cntTnH,x
	lda entBlk+ENT_TARGET
	sta cntTgL,x
	lda entBlk+ENT_TARGET+1
	sta cntTgH,x
	lda entBlk+ENT_P1+1		; count; Quake's default is 2
	bne cntTwo
	lda entBlk+ENT_P1
	bne +
cntTwo
	lda #2
+	sta cntCount,x
	sta cntLeft,x
	inc nCnt
	rts

; X = mover being loaded. mvStay bit 0 = a door or button with wait -1,
; bit 7 = a door with START_OPEN (spawnflags 1), which Quake spawns at its
; open position and moves to the modelled one when fired: mulOfs runs its
; fraction backwards. Preserves X.
exStayTake
	lda entBlk+ENT_P1
	and entBlk+ENT_P1+1
	cmp #$ff			; C: wait -1
	lda #0
	rol a
	ldy mvKind,x
	cpy #K_BUTTON
	beq exStay
	cpy #K_DOOR
	beq +
	lda #0
	beq exStay
+	tay
	lda entBlk+ENT_SPAWN
	lsr a
	tya
	bcc exStay
	ora #$80
	inc mvDirty,x			; its first offset is not zero
exStay
	sta mvStay,x
	rts

; ----------------------------------------------------------------------
; fireTarget's second half: every counter listening for the fired id
; counts one down, and the one that reaches zero fires its own target.
; firePush queues it, so the drain that called this picks it up.
; ----------------------------------------------------------------------
cntFire
	ldx nCnt
cntLoop
	dex
	bpl +
	rts
+	lda cntTnL,x
	cmp fireTL
	bne cntLoop
	lda cntTnH,x
	cmp fireTH
	bne cntLoop
	lda cntLeft,x
	beq cntLoop			; done already
	dec cntLeft,x
	stx cntX
	bne cntMore
	lda cntTgL,x
	ldy cntTgH,x
	jsr firePush
	jsr msgRow
	jsr dmText
	.text "        SEQUENCE COMPLETED!"
	.byte 0
	ldx cntX
	jmp cntLoop
cntMore
	jsr msgRow
	jsr dmText
	.text "        ONLY "
	.byte 0
	ldx cntX
	lda cntLeft,x
	clc
	adc #$30
	jsr imChar
	jsr dmText
	.text " MORE TO GO..."
	.byte 0
	ldx cntX
	jmp cntLoop

; The message row, blanked and with the cursor at its start; the frame
; clears it again MSG_FRAMES later. With telemetry on, rows 2-24 are the
; telemetry's, and the message would be written over within a frame --
; so it goes on the row anyway and simply loses.
msgRow
	lda #MSG_FRAMES
	sta msgT
	lda #MSG_ROW
	jsr dmRow
	ldy #39
	lda #$20
-	sta (DST),y
	dey
	bpl -
	rts

; ----------------------------------------------------------------------
; gtTrg: A = the kind of the trigger just touched, X = its index. Secrets
; and the exit; returns with A = the kind again and X preserved.
; ----------------------------------------------------------------------
exTrig
	cmp #K_TRIG_SECRET
	bne +
	inc secFound
	lda #SFX_SECRET
	jsr sfxPlay
	lda #K_TRIG_SECRET
	rts
+	cmp #K_TRIG_CHANGE
	bne +
	sta exitHit
	lda trDest,x
	sta exitDest
	lda #K_TRIG_CHANGE
	rts
+	cmp #K_TRIG_TELE
	bne +
	ldy trDest,x
	beq +				; inert: an ambush's, or the table was full
	jsr tpGo
	lda #K_TRIG_MULTI		; and it goes on working
+	rts

; ----------------------------------------------------------------------
; trigger_teleport. gen_quakelevel.py puts the destination's eye in the
; travel field, its yaw in p0 and 1 in p1 when the teleport works; etTrig
; has just copied p1 into trDest,x. X is the trigger.
; ----------------------------------------------------------------------
tpTake
	lda entBlk+ENT_KIND
	cmp #K_TRIG_TELE
	bne tpTkDone
	lda trDest,x
	beq tpTkDone
	lda #0
	sta trDest,x
	ldy nTel
	cpy #MAX_TEL
	bcs tpTkDone
	lda entBlk+ENT_TRAVEL+1
	sta tpL,y
	lda entBlk+ENT_TRAVEL+2
	sta tpH,y
	lda entBlk+ENT_TRAVEL+5
	sta tpL+MAX_TEL,y
	lda entBlk+ENT_TRAVEL+6
	sta tpH+MAX_TEL,y
	lda entBlk+ENT_TRAVEL+9
	sta tpL+2*MAX_TEL,y
	lda entBlk+ENT_TRAVEL+10
	sta tpH+2*MAX_TEL,y
	lda entBlk+ENT_P0
	sta tpL+3*MAX_TEL,y
	lda entBlk+ENT_P0+1
	sta tpH+3*MAX_TEL,y
	iny
	sty nTel
	tya
	sta trDest,x
tpTkDone
	rts

; Y = the teleport's slot + 1. The destination becomes the last good place
; and restoreGood takes the player there, standing still, facing its way.
tpGo
	dey
	ldx #0
-	lda #0
	sta lastGood,x
	lda tpL,y
	sta lastGood+1,x
	lda tpH,y
	sta lastGood+2,x
	asl				; the sign, into the top byte
	lda #$ff
	adc #0
	eor #$ff
	sta lastGood+3,x
	tya
	clc
	adc #MAX_TEL
	tay
	inx
	inx
	inx
	inx
	cpx #12
	bne -
	lda tpL,y
	sta camYaw
	lda tpH,y
	sta camYaw+1
	lda #0
	sta camPitch
	jsr restoreGood
	lda #SFX_POWER
	jsr sfxPlay
	ldx gtI
	rts

; ----------------------------------------------------------------------
; Once a frame, after clkFrame: the play time, and the message timer.
; ----------------------------------------------------------------------
exFrame
	lda msCur+MS_PER
	clc
	adc msAcc
	sta msAcc
	bcc exSec
	inc msAcc+1
exSec
	lda msAcc
	sec
	sbc #<1000
	tax
	lda msAcc+1
	sbc #>1000
	bcc exMsg
	sta msAcc+1
	stx msAcc
	inc playSec
	bne exSec
	inc playSec+1
	jmp exSec
exMsg
	lda tick			; MSG_FRAMES are game ticks
	beq +
	lda msgT
	beq +
	dec msgT
	bne +
	lda #MSG_ROW
	jsr dmRow
	ldy #39
	lda #$20
-	sta (DST),y
	dey
	bpl -
+	rts

; restart and start-up: the level's counts from zero.
exReset
	lda #0
	sta secFound
	sta playSec
	sta playSec+1
	sta msAcc
	sta msAcc+1
	sta exitHit
	sta mfT
	ldx nCnt
-	dex
	bmi +
	lda cntCount,x
	sta cntLeft,x
	jmp -
+	rts

; ======================================================================
; The muzzle flash. mfT counts the frames it has left; the light is sent
; only when what it should be differs from what was last acknowledged, so
; a lost command is simply sent again next frame, and a node that has gone
; (BAD_ID: a phantom destroyed it) is created again.
; ======================================================================
; Setup, before LOOP_START.
mfInit
	lda #1
	sta mfLost
	lda #0
	sta mfT
	sta mfOn
	jmp mfCreate

; combat's fire path. X = weapon, preserved.
mfShot
	cpx #W_AXE
	beq +
	lda #FLASH_FRAMES
	sta mfT
	lda mfAtB			; lit at a blast: moved back to the eye
	beq +
	lda #0
	sta mfAtB
	sta mfOn
+	rts

; pjBoom's: the light at pjP, the grenade. Preserves X.
mfBoom
	ldy #11
-	lda pjP,y
	sta mfPos,y
	dey
	bpl -
	lda #1
	sta mfAtB
	lda #0
	sta mfOn
	lda #BOOM_FRAMES
	sta mfT
	rts

; Once a frame, before the commit.
mfFrame
	lda mfLost
	beq +
	jsr mfCreate
	bne mfRts
+	lda mfT
	beq mfWantOff
	ldx tick			; FLASH_FRAMES are game ticks
	beq +
	dec mfT
+
	lda mfOn
	bne mfRts			; lit already; the eye has barely moved
	ldx #11
-	lda cam,x
	ldy mfAtB
	beq +
	lda mfPos,x
+	sta stBuf,x
	dex
	bpl -
	jsr mfId
	lda #OP_SET_POSITION
	ldx #11
	jsr stage2
	bne mfErr
	lda #FLASH_S
	jsr mfSet
	bne mfErr
	lda #1
	sta mfOn
mfRts
	rts
mfWantOff
	lda mfOn
	beq mfRts
	lda #0
	jsr mfSet
	bne mfErr
	lda #0
	sta mfOn
	sta mfAtB
	rts
mfErr
	cmp #ERR_BAD_ID
	bne mfRts			; sent again next frame
	lda #1
	sta mfLost
	rts

; A = strength. SET_POINT_LIGHT, checked. Z set on OK.
mfSet
	sta stBuf+0
	lda #<FLASH_R
	sta stBuf+1
	lda #>FLASH_R
	sta stBuf+2
	jsr mfId
	lda #OP_SET_POINT_LIGHT
	ldx #2
	jmp stage2

; The node, new and off. Z set on OK.
mfCreate
	jsr mfId
	lda #OP_CREATE_LIGHT
	ldx #$ff
	jsr stage2
	bne +
	lda #0
	sta mfLost
	sta mfOn
+	rts

mfId
	lda #<FLASH_ID
	sta stId
	lda #>FLASH_ID
	sta stId+1
	rts

; ======================================================================
; The intermission. Called from the frame loop with exitHit set; returns
; with the level restarted, the loop to be restarted by the next commit.
; ======================================================================
intermission
	jsr sfxOff
	lda #1
	sta CMD_HI
	ldx #8
imStop
	lda #KEY_DESTRUCTIVE
	sta ARG+15
	lda #OP_LOOP_STOP
	jsr cmdSeq1
	lda seqMiss
	beq imStopped
	dex
	bne imStop
imStopped
	lda #0
	sta CMD_HI

	; The stats, from the counts, once.
	lda playSec
	sta imT
	lda playSec+1
	sta imT+1
	lda #0
	sta imMin
-	lda imT
	sec
	sbc #60
	tax
	lda imT+1
	sbc #0
	bcc +
	sta imT+1
	stx imT
	inc imMin
	bne -
	dec imMin			; 255 minutes is plenty
	jmp -
+	lda imT
	sta imSec

	lda #<TUNE_INIT
	ldx #>TUNE_INIT
	ldy #<TUNE_PLAY
	jsr tiMusicAt

	jsr teleShown
	lda #2
	jsr dmRow
	jsr dmText
	.text "                           "
	.byte 0
	lda #6
	jsr dmRow
	jsr dmText
	.text "          "
	.byte 0
	jsr lvName
	jsr dmText
	.text " COMPLETED"
	.byte 0
	lda #10
	jsr dmRow
	jsr dmText
	.text "          TIME      "
	.byte 0
	lda imMin
	jsr dmDecA
	lda #':'
	jsr imChar
	lda imSec
	jsr imDec2
	lda #12
	jsr dmRow
	jsr dmText
	.text "          SECRETS   "
	.byte 0
	lda secFound
	jsr dmDecA
	lda #'/'
	jsr imChar
	lda secTotal
	jsr dmDecA
	lda #14
	jsr dmRow
	jsr dmText
	.text "          KILLS     "
	.byte 0
	lda kills
	jsr dmDecA
	lda #'/'
	jsr imChar
	lda kTotal
	jsr dmDecA

	lda #$ff			; a key still down from play does not count
	sta tiIn
	lda #0
	sta imTick
imLoop
	jsr imDraw
	jsr dmFlip
	jsr imWait
	jsr readKeys			; runsim's --stop-after ends a run
	lda SIM_EXIT			; parked here: straight to finish, while
	cmp #SIM_EXIT_ON		; its first RUN/STOP press is still down
	beq imSimExit			; (finish waits for that press's release)
	lda imTick
	cmp #IM_ARM
	bcs imArmed
	inc imTick
	lda imTick
	cmp #IM_ARM
	bne imLoop
	lda #20
	jsr dmRow
	jsr dmText
	.text "       PRESS FIRE, SPACE OR RETURN"
	.byte 0
	jmp imLoop
imArmed
	lda imPress
	beq imLoop
	bne imLeave
imSimExit
	jsr tiMusicOff
	jsr sfxInit			; finish's sfxOff takes the IRQ back out
	pla				; intermission's own return: finish is
	pla				; the frame loop's, and ends the program
	lda #1
	sta fnQuick
	jmp finish
fnQuick		.byte 0
imLeave
	jsr tiMusicOff
	jsr sfxInit
	; The next level: the one this exit's 'map' names, pack slot n-1 for
	; E1Mn; 0 is the end of the episode, and that is E1M1 again. A slot
	; the Pi has no level for (a smaller pack in the REU) comes back as
	; BAD_ARGS from LOAD_LEVEL, which falls back to E1M1 itself. The whole
	; per-level setup runs again from SCENE_RESET, which LOAD_LEVEL needs
	; anyway -- it replaces every resource and node the Pi holds. The loop
	; is already stopped (imStop).
	lda exitDest
	beq +
	sec
	sbc #1
+	and #7
	sta lvSlot
	ldx #0				; the stats off the C64 screen
	lda #$20
-	sta SCREEN,x
	sta SCREEN+$100,x
	sta SCREEN+$200,x
	sta SCREEN+$2e8,x
	inx
	bne -
	pla				; intermission's return into the frame
	pla				; loop: levelSetup ends by entering it
	jmp levelSetup

; The level's name at the cursor: E1M1 .. E1M8.
lvName
	jsr dmText
	.text "E1M"
	.byte 0
	lda lvSlot
	clc
	adc #'1'
	jmp imChar

; A = character, printed at the cursor.
imChar
	ldy #0
	sta (DST),y
	inc DST
	bne +
	inc DST+1
+	rts

; A = 0-99, two digits.
imDec2
	ldx #$ff
	sec
-	inx
	sbc #10
	bcs -
	adc #10
	pha
	txa
	jsr dmDecDigit
	pla
	jmp dmDecDigit

; One tick: scan until IM_TICK_MS has gone by or IM_POLLS scans have,
; whichever is first. imPress = a select edge seen during it.
imWait
	lda #0
	sta imPress
	lda #<IM_POLLS
	sta imPolls
	lda #>IM_POLLS
	sta imPolls+1
	jsr tiMark
imwLoop
	jsr tiScan
	lda tiNew
	and #4
	ora imPress
	sta imPress
	jsr clkNow
	lda tiT0
	sec
	sbc msNow
	tax
	lda tiT0+1
	sbc msNow+1
	bne imwDone
	cpx #IM_TICK_MS
	bcs imwDone
	lda imPolls
	bne +
	dec imPolls+1
	bmi imwDone
+	dec imPolls
	jmp imwLoop
imwDone
	rts

; The HDMI page: Sbar_IntermissionOverlay's layout, 320x200.
imDraw
	lda #0
	sta imX+1
	sta imX
	ldy #0
	lda #PIC_CONFADE
	ldx #0
	jsr imPic
	lda #64
	sta imX
	ldy #24
	lda #PIC_COMPLETE
	jsr imPicKey
	lda #0
	sta imX
	ldy #56
	lda #PIC_INTER
	jsr imPicKey

	lda #64				; time
	sta imY
	lda #160
	ldx imMin
	jsr imNum
	lda #234
	sta imX
	ldy #64
	lda #PIC_NUM_COLON
	jsr imPicKey
	lda imSec
	ldx #$ff
	sec
-	inx
	sbc #10
	bcs -
	adc #10
	sta imT
	lda #246
	sta imX
	txa
	clc
	adc #PIC_NUM_0
	ldy #64
	jsr imPicKey
	lda #10
	sta imX
	inc imX+1			; 266
	lda imT
	clc
	adc #PIC_NUM_0
	ldy #64
	jsr imPicKey

	lda #104			; secrets
	ldx secFound
	ldy secTotal
	jsr imPair
	lda #144			; kills
	ldx kills
	ldy kTotal
	; fall through

; A = y, X = count, Y = total: "count/total" as Quake lays it out.
imPair
	sta imY
	sty imT+1
	lda #160
	jsr imNum
	lda #232
	sta imX
	lda #0
	sta imX+1
	ldy imY
	lda #PIC_NUM_SLASH
	jsr imPicKey
	lda #240
	ldx imT+1
	; fall through

; A = left x (0-255), X = value, imY = y: IntermissionNumber(x, y, n, 3),
; right-aligned in three 24-pixel cells. The last cell can pass x 255.
imNum
	sta imX
	lda #0
	sta imX+1
	txa
	ldx #$ff
	sec
-	inx
	sbc #100
	bcs -
	adc #100
	stx imD+0
	ldx #$ff
	sec
-	inx
	sbc #10
	bcs -
	adc #10
	stx imD+1
	sta imD+2
	ldx #0				; the first cell drawn
	lda imD+0
	bne imnGo
	inx
	lda imD+1
	bne imnGo
	inx
imnGo
	stx imI
	txa				; x += skipped cells * 24
	beq imnCell
-	lda imX
	clc
	adc #24
	sta imX
	bcc +
	inc imX+1
+	dex
	bne -
imnCell
	ldx imI
	lda imD,x
	clc
	adc #PIC_NUM_0
	ldy imY
	jsr imPicKey
	lda imX
	clc
	adc #24
	sta imX
	bcc +
	inc imX+1
+	inc imI
	lda imI
	cmp #3
	bcc imnCell
	rts

; A = picture, imX = x (16-bit), Y = y. imPicKey keys colour 255 out;
; imPic takes the flags in X.
imPicKey
	ldx #1
imPic
	sta stBuf+0
	lda imX
	sta stBuf+1
	lda imX+1
	sta stBuf+2
	sty stBuf+3
	lda #0
	sta stBuf+4
	txa
	jmp tiPic

; ----------------------------------------------------------------------
nCnt		.byte 0
cntTnL		.fill MAX_CNT, 0
cntTnH		.fill MAX_CNT, 0
cntTgL		.fill MAX_CNT, 0
cntTgH		.fill MAX_CNT, 0
cntCount	.fill MAX_CNT, 0
cntLeft		.fill MAX_CNT, 0
cntX		.byte 0
mvStay		.fill MAX_MOV, 0
secFound	.byte 0
secTotal	.byte 0
exitHit		.byte 0
exitDest	.byte 0		; the touched exit's trDest
playSec		.word 0
msAcc		.word 0
msgT		.byte 0
mfT		.byte 0
mfOn		.byte 0
mfLost		.byte 0
mfAtB		.byte 0			; lit at mfPos, a blast, not the eye
mfPos		.fill 12, 0
imT		.word 0
imMin		.byte 0
imSec		.byte 0
imTick		.byte 0
imPress		.byte 0
imPolls		.word 0
imX		.word 0
imY		.byte 0
imD		.fill 3, 0
imI		.byte 0

; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_env.inc - water, slime, lava and the radiation suit.
;
; Quake's WaterMove, on the C64. What the player is standing in comes back
; from every CLIP_MOVE answer: byte 45 is the contents at the feet of the
; end position, from the level's hull 0 (the only hull that knows liquids),
; and bytes 50/51 are Quake's waterlevel and its complement -- 0 dry,
; 1 feet, 2 waist, 3 eyes. clipMove validates the pair into plWater; a
; firmware too old to send it sends 0/0, which fails the complement test,
; and the player simply never gets wet.
;
; The rules are id's (client.qc's WaterMove), in milliseconds of dtMs so
; they run at wall-clock speed whatever the frame rate:
;   drowning  eyes under for 12 s, then every 1 s a hit of 4, 6, 8 ... 14,
;             and back to 10 past 15. Surfacing restores the air at once.
;   slime     4 x waterlevel every 1 s; nothing at all in the suit.
;   lava      10 x waterlevel every 0.2 s, every 1 s in the suit.
;   suit      30 s, and it also stops the drowning.
; The first touch of a liquid hurts at once, as Quake's does (dmgtime
; starts in the past); the period only paces the ones after.
;
; Swimming itself lives in moveCamera (swimCheck, mcWalk) and clipMove,
; because it changes how the step is asked for, not what happens after.
; ======================================================================

CONT_WATER = 2			; GPU64_CLIP_CONT_* in gpu64_level.h
CONT_SLIME = 3
CONT_LAVA  = 4

AIR_MS     = 12000
DROWN_MS   = 1000
SLIME_MS   = 1000
LAVA_MS    = 200
LAVA_SUIT_MS = 1000
SUIT_MS    = 30000

; Swimming, in velY's units (16.16 world per DT_REF frame, 1 Quake unit
; = $0800). Quake swims at 0.7 of the walk and sinks at 60 units a second
; when nothing is pressed; SPACE is its upmove, 200 x 0.7 = 140 a second.
; At 32 ms a frame that is 4.5 and 1.9 Quake units.
SWIM_UP    = $2400
SWIM_SINK  = $10000 - $0f00

; ----------------------------------------------------------------------
; plReset's half: dry, full air, no suit.
; ----------------------------------------------------------------------
envReset
	lda #0
	sta plWater
	sta swimming
	sta airMs
	sta airMs+1
	sta plSuit
	sta plSuit+1
	lda #2
	sta drownD
	lda #$ff
	sta dmgMs
	sta dmgMs+1
	rts

; ----------------------------------------------------------------------
; Called from moveCamera once the step is set: are we swimming, and what
; does the water do to the step and the vertical speed? Waist deep is
; swimming (Quake's waterlevel >= 2). A water jump still rising belongs
; to the air until it turns; one that falls back is swimming again.
; ----------------------------------------------------------------------
swimCheck
	lda #0
	sta swimming
	lda plWater
	cmp #2
	bcc scRts0
	lda jumping
	beq scSwim
	lda velY+1
	bpl scRts0			; still rising out of the water
	lda #0
	sta jumping
	beq scSwim			; (lda #0 above: always)
scRts0
	rts
scSwim
	lda #1
	sta swimming
	lda #0
	sta onGround

	; 0.6875 of the step: s/2 + s/8 + s/16.
	lda stepLo
	lsr a
	sta scT
	lsr a
	lsr a
	sta scT+1
	lsr a
	clc
	adc scT+1
	adc scT
	sta stepLo

	; The vertical target: up on SPACE, sink when neither W nor S is held
	; (the pitch carries a swim along), otherwise hold depth.
	lda keyD
	and #KD_SPC
	bne scNoUp
	lda #<SWIM_UP
	ldx #>SWIM_UP
	bne scAvg
scNoUp
	lda keyB
	and #KB_W | KB_S
	cmp #KB_W | KB_S
	bne scHold
	lda #<SWIM_SINK
	ldx #>SWIM_SINK
	bne scAvg
scHold
	lda #0
	tax
scAvg
	; velY = (velY + target) / 2, signed. Both are within +-$7000, so the
	; sum fits in 16 bits and the halve is an arithmetic shift.
	clc
	adc velY
	sta velY
	txa
	adc velY+1
	cmp #$80
	ror a
	sta velY+1
	ror velY

	; The water jump: waist deep, against a wall, SPACE -- out onto the
	; ledge in an ordinary jump. The wall is last frame's answer.
	lda plWater
	cmp #2
	bne scRts
	lda keyD
	and #KD_SPC
	bne scRts
	lda clipFlags
	and #CLIP_WALL
	beq scRts
	lda keyB
	and #KB_W
	bne scRts			; only swimming INTO the wall
	lda #<JUMP_V
	sta velY
	lda #>JUMP_V
	sta velY+1
	lda #1
	sta jumping
	lda #0
	sta swimming
scRts
	rts

; ----------------------------------------------------------------------
; A frame, after the move is taken: the suit runs down, the air runs out,
; and the liquid bites.
; ----------------------------------------------------------------------
envFrame
	lda plDead
	beq +
	rts
+
	lda plSuit
	ora plSuit+1
	beq efSuitDone
	sec
	lda plSuit
	sbc dtMs
	sta plSuit
	lda plSuit+1
	sbc #0
	sta plSuit+1
	bcs efSuitDone
	lda #0
	sta plSuit
	sta plSuit+1
efSuitDone

	; Air. Eyes under and no suit: the clock runs.
	lda plWater
	cmp #3
	bne efBreathe
	lda plSuit
	ora plSuit+1
	bne efBreathe
	clc
	lda airMs
	adc dtMs
	sta airMs
	bcc +
	inc airMs+1
+	lda airMs+1
	cmp #>AIR_MS
	bcc efLiquid
	bne +
	lda airMs
	cmp #<AIR_MS
	bcc efLiquid
+	sec				; one second back on the clock
	lda airMs
	sbc #<DROWN_MS
	sta airMs
	lda airMs+1
	sbc #>DROWN_MS
	sta airMs+1
	lda drownD
	clc
	adc #2
	cmp #16
	bcc +
	lda #10
+	sta drownD
	jsr envHurt
	jmp efLiquid
efBreathe
	lda #0
	sta airMs
	sta airMs+1
	lda #2
	sta drownD

efLiquid
	lda plWater
	beq efOut
	lda clipCont
	cmp #CONT_SLIME
	beq efSlime
	cmp #CONT_LAVA
	beq efLava
efOut
	lda #$ff			; the next touch hurts at once
	sta dmgMs
	sta dmgMs+1
	rts
efSlime
	lda plSuit
	ora plSuit+1
	bne efOut
	lda #<SLIME_MS
	sta efPer
	lda #>SLIME_MS
	sta efPer+1
	lda #4
	bne efTick
efLava
	lda #<LAVA_MS
	sta efPer
	lda #>LAVA_MS
	sta efPer+1
	lda plSuit
	ora plSuit+1
	beq +
	lda #<LAVA_SUIT_MS
	sta efPer
	lda #>LAVA_SUIT_MS
	sta efPer+1
+	lda #10
efTick
	sta efK
	lda dmgMs
	and dmgMs+1
	cmp #$ff
	bne efCount
	lda #0
	sta dmgMs
	sta dmgMs+1
	beq efBite
efCount
	clc
	lda dmgMs
	adc dtMs
	sta dmgMs
	bcc +
	inc dmgMs+1
+	lda dmgMs+1
	cmp efPer+1
	bcc efRts
	bne +
	lda dmgMs
	cmp efPer
	bcc efRts
+	sec
	lda dmgMs
	sbc efPer
	sta dmgMs
	lda dmgMs+1
	sbc efPer+1
	sta dmgMs+1
efBite
	; k x waterlevel, waterlevel 1..3: at most 30.
	lda #0
	ldx plWater
-	clc
	adc efK
	dex
	bne -
	; fall through

; A = damage from the environment.
envHurt
	jsr plHurt
	inc envHurts
	bne efRts
	dec envHurts
efRts
	rts

; ----------------------------------------------------------------------
; Row 24: WL waterlevel, CT contents at the feet, SW swimming, AIR and
; SUIT in 256 ms steps (47 is twelve seconds, 117 a fresh suit), EH times
; the environment hurt the player.
; ----------------------------------------------------------------------
showEnv
	lda #24
	jsr dmRow
	jsr dmText
	.text "WL"
	.byte 0
	lda plWater
	jsr dmDigit
	jsr dmText
	.text " CT"
	.byte 0
	lda clipCont
	and #$0f
	jsr dmDigit
	jsr dmText
	.text " SW"
	.byte 0
	lda swimming
	jsr dmDigit
	jsr dmText
	.text " AIR"
	.byte 0
	lda airMs+1
	jsr dmDecA
	jsr dmText
	.text " SUIT"
	.byte 0
	lda plSuit+1
	jsr dmDecA
	jsr dmText
	.text " EH"
	.byte 0
	lda envHurts
	jmp dmDecA

; ----------------------------------------------------------------------
; State.
; ----------------------------------------------------------------------
plWater		.byte 0		; Quake's waterlevel, 0..3
swimming	.byte 0
airMs		.word 0
dmgMs		.word $ffff
drownD		.byte 2
plSuit		.word 0		; ms of suit left
envHurts	.byte 0
efPer		.word 0
efK		.byte 0
scT		.byte 0, 0

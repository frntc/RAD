; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_ai.inc -- milestone 20 stage E: the monsters, for
; gpu64_demo_game.a.
;
; Quake's AI, simplified and split the way stage D split the shot: the Pi
; answers geometry, the C64 decides everything else. A monster asks two
; questions, both riding the frame's WORLD_TICK block:
;
;   a TRACE, its eye to the player's, hull 0 -- can it see the player; and
;   a MOVE, its origin plus one step along its yaw, hull 1, CLIP_MOVE's
;   walk mode -- where does that step actually take it.
;
; Asleep, a monster looks: the player within AI_SIGHT, in the half-space
; it faces, and a clear trace wakes it. A gunshot wakes every sleeper
; inside an AI_NOISE box, and so does being hurt. Awake, it acts every
; other frame (15 Hz at 30 fps; Quake's monsters think at 10), staggered by
; slot so half of them act on each frame: it turns toward the player at
; most AI_TURN a think, takes a double step along its yaw, keeps its line
; of sight fresh, and attacks when Quake's CheckAttack would. A step that
; comes back short marks it blocked, and the next thinks detour 45-135
; degrees off the ideal yaw, alternating sides, before trying straight at
; the player again -- a cheap SV_NewChaseDir.
;
; A move whose answer does not end ON A FLOOR is not taken: the settle
; probes one step height down, so this is Quake's own "monsters do not walk
; off ledges" for free. Nor is one whose end is more than a unit from its
; start on any axis: the block's checksum refuses a corrupted answer, the
; bound refuses a nonsensical one (gpu64-bound-the-answer).
;
; Attacks, at normal skill:
;   grunt  in range and in sight, per think a chance by distance (Quake's
;          SoldierCheckAttack); the shoot animation fires on its fifth frame:
;          four pellets, spread 0.1 on both axes, 4 damage each.
;   dog    in melee range, the bite animation; on its fourth frame, if the
;          player is still within 100 QU, 8 + 0..7 damage. Between 80 and
;          150 QU, in sight, a leap: LEAP_FRAMES fast steps, and touching
;          the player on the way does 10 + 0..7.
;
; Simplifications, deliberately: monsters are not solid to each other or to
; the player (they stop short of the player instead); they do not open
; doors, and are traced only against the movers near the PLAYER, which are
; the ones the frame's CLIP_MOVE selected; and they never lose the player
; once awake, unless the player gets further than 64 units away.
; ======================================================================

AI_SLEEP  = 0			; acAi
AI_CHASE  = 1
AI_SHOOT  = 2
AI_BITE   = 3
AI_LEAP   = 4

AI_ANIM_MASK = 1		; awake: an animation frame every 2nd frame
AI_LOOK_MASK = 3		; asleep: looks every 4th frame
AI_MAXTR  = 5			; traces a frame (with CB_NCAND and AI_MAXMV,
AI_MAXMV  = 6			; moves a frame   at most 15 answers a block)
	.cerror CB_NCAND + AI_MAXTR + AI_MAXMV > 15, "WORLD_TICK's trailer offset is a byte"
AI_MAXMON = 32
AI_EYE    = 200			; fine: Quake's monster view_ofs, 25 QU
FLY_LO    = 64			; fine: a scrag's origin over the eye
FLY_HI    = 144
FLY_STEP  = 64			; fine a move, up or down
FALL_STEP = 256			; fine a move: its body, falling
FALL_N    = 30			; moves before a body is left where it is
AI_SIGHT  = $20			; fine high byte: 32 wu = 1024 QU
AI_NOISE  = 12			; wu: a gunshot wakes sleepers inside this box
AI_TURN   = 32			; yaw per think: 45 degrees
AI_DETOUR = 4			; thinks a detour lasts
AI_BLOCK  = 160			; a step covering less than 160/255 is blocked
AI_WAKE_COOL = 4		; thinks before a new monster's first attack
AI_MAXAWAKE = 8			; sight and noise wake no more than this many;
				; a hurt monster wakes regardless

; Per DT_REF frame; an awake monster moves every other frame, and
; aiMoveReq doubles the step for it.
GR_STEP   = 40			; fine per frame: ~110 QU/s, Quake's run
DOG_STEP  = 72			; ~200 QU/s
LEAP_STEP = 128			; ~350 QU/s, Quake's leap is 300 forward
LEAP_FRAMES = 4			; acting frames, i.e. every other frame

GR_STOP_F   = 512		; fine: a grunt closes to 64 QU ...
DOG_STOP_F  = 320		; ... a dog to 40 QU
GR_NEAR_F   = 960		; 120 QU: SoldierCheckAttack's melee range ...
GR_MID_F    = 4000		; 500 QU: near ...
GR_FAR_F    = 6144		; 768 QU: and a shot beyond this is not taken
GR_COOL     = 10		; thinks after a volley: ~1 s
GR_PEL      = 4
GR_DMG      = 4
GR_SPREAD_X = 10240		; r * dist/32 under this: inside 16 QU laterally
GR_SPREAD_Y = 17920		; ... and inside 28 QU vertically
DOG_MELEE_F = 640		; 80 QU: bite
DOG_BITE_F  = 800		; 100 QU: the bite still lands
DOG_LEAP_HI = 1200		; 150 QU: leap from DOG_MELEE_F up to this
DOG_TOUCH_F = 400		; 50 QU: a leap that gets this close hits
DOG_COOL    = 6
GR_FIRE_F   = 4			; the shoot animation frame that fires
DOG_BITE_AF = 3			; the attack animation frame that bites

; ======================================================================
; Setup, once, after combatCount: where each monster started, so a
; restart can put it back.
; ======================================================================
aiInit
	lda #0
	sta aiRR
	lda #0
	sta aiLoud
	sta aiMon
	sta aiNL
	sta aiWakes
	sta aiBlocks
	sta aiRefs
	sta aiHits
	ldx #SL_WORLD
aiiLoop
	cpx nAct
	bcs aiiDone
	lda #0
	sta acAi,x
	jsr aiIsMon
	bcc aiiNext
	ldy aiNL			; every monster, saved or not, is on the list
	txa
	sta aiList,y
	inc aiNL
	lda aiMon
	cmp #AI_MAXMON
	bcs aiiNext			; not saved: respawns where it fell
	sta acSpIx,x
	tay
	lda acYaw,x
	sta aiSpYaw,y
	.for k = 0, k < 12, k += 1
	lda acPos+k*MAX_ACT,x
	sta aiSpPos+k*AI_MAXMON,y
	.next
	inc aiMon
aiiNext
	inx
	bne aiiLoop
aiiDone
	rts

; X = slot. C set if it is a monster. Preserves X.
aiIsMon
	jsr mtType
	bcs +
	sec
	rts
+	clc
	rts

; monSpawn's half. X = monster slot: asleep, where it started. Preserves X.
aiRespawn
	lda #AI_SLEEP
	sta acAi,x
	sta acSee,x
	sta acDet,x
	sta acBlk,x
	sta acLeapT,x
	lda acSpIx,x
	cmp aiMon
	bcs arRts
	tay
	lda aiSpYaw,y
	sta acYaw,x
	.for k = 0, k < 12, k += 1
	lda aiSpPos+k*AI_MAXMON,y
	sta acPos+k*MAX_ACT,x
	.next
arRts
	rts

; C set when nothing may notice the player: runsim's --notarget, or a dead
; player -- Quake's monsters go on standing over a corpse.
aiOff
	lda SIM_NOTARGET
	cmp #SIM_NOTARGET_ON
	beq aoOff
	lda plDead
	bne aoOff
	clc
	rts
aoOff
	sec
	rts

; monHurt's half, before the damage. X = monster slot: awake, whatever it
; was doing -- the pain animation that follows interrupts an attack.
; Preserves X.
aiHurt
	jsr aiOff
	bcs ahRts
	lda acAi,x
	bne ahAwake
	inc aiWakes
	bne ahAwake
	dec aiWakes
ahAwake
	lda #AI_CHASE
	sta acAi,x
	lda #0
	sta acLeapT,x
ahRts
	rts

; X = monster slot: awake and running at the player. Preserves X.
aiWake
	lda #SFX_SIGHT
	jsr sfxPlay
	inc aiWakes
	bne +
	dec aiWakes
+	lda aiAwN			; counted: one more
	bmi +
	inc aiAwN
+	lda #1
	sta acSee,x
	lda #AI_WAKE_COOL
	sta acAtkT,x
	lda #0
	sta acDet,x
	sta acBlk,x
	; fall through

; X = slot: chasing, the run animation. Preserves X.
aiRun
	lda #AI_CHASE
	sta acAi,x
	lda #AS_IDLE
	sta acState,x
	lda #AM_LOOP
	sta cbMode
	lda #MA_RUN
	jmp mtAnim

; actFrame's end of an AM_ONCE animation (pain, an attack). X = slot.
; Preserves X.
aiAnimEnd
	lda acAi,x
	bne aeAwake
	jmp acStand
aeAwake
	cmp #AI_CHASE
	beq aiRun			; pain over: back to the chase
	jsr mtType			; an attack over: a rest before the next
	lda mbCool,y
	sta acAtkT,x
	jmp aiRun

; ======================================================================
; A frame, after combatFrame and before actFrame: every monster, from a
; rotating start so no one of them always gets the last of the budget.
; ======================================================================
aiFrame
	lda #0
	sta aiTrN
	sta aiMvN
	jsr pjFrame			; the missiles first: their traces before
					; the monsters' can spend the budget
	lda aiLoud			; fire's: a gun went off this frame
	sta aiNoisy
	lda #0
	sta aiLoud
	lda #$ff			; the awake count: unknown until a wake
	sta aiAwN			; asks (aiCanWake)
	jsr aiOff
	bcc +
	rts
+	lda aiNL
	beq afiRts
	sta aiCnt
	ldy aiRR
	iny
	cpy aiNL
	bcc +
	ldy #0
+	sty aiRR
afiLoop
	sty aiI
	ldx aiList,y
	stx aiX
	jsr aiOne
	ldy aiI
	iny
	cpy aiNL
	bcc +
	ldy #0
+	dec aiCnt
	bne afiLoop
afiRts
	rts

; X = slot, a monster's (aiList).
aiOne
	lda acState,x
	cmp #AS_IDLE			; in pain or dead: nothing to decide
	beq +
	cmp #AS_DEAD
	bne aoNo
	lda acLeapT,x			; ... but a scrag's body falls
	beq aoNo
	txa
	eor wtFrame
	and #1
	bne aoNo
	dec acLeapT,x
	jmp aiFallReq
aoNo
	rts
+	lda acAi,x
	beq aoAsleep
	txa				; awake: acts on every other frame,
	eor wtFrame			; the one actFrame does not animate it on
	and #1
	beq aoGeom
	rts
aoAsleep
	lda aiNoisy			; asleep: nothing to hear, and not its
	bne aoSlNear			; frame to look -- no geometry at all
	txa
	eor wtFrame
	and #AI_LOOK_MASK
	beq aoSlNear
	rts
aoSlNear
	.cerror ANIM_NEAR < AI_SIGHT, "the prefilter must reach AI_SIGHT"
	jsr acNearWu			; 32 wu out on x or z: past AI_SIGHT and
	bcc aoGeom			; AI_NOISE, so nothing to compute
	rts
aoGeom
	jsr acDelta			; dd = monster - eye, fine
	jsr aiGeom
	ldx aiX
	lda acAi,x
	beq aoSleep
	jmp aoAwake

; --- asleep ------------------------------------------------------------
aoSleep
	lda aiNoisy
	beq aoLook
	lda aiFar
	bne aoLook
	lda aiAdx+1			; a gunshot: inside the AI_NOISE box
	cmp #AI_NOISE
	bcs aoLook
	lda aiAdz+1
	cmp #AI_NOISE
	bcs aoLook
	jsr aiCanWake
	bcs aoLook
	jmp aiWake
aoLook
	txa				; looks every AI_LOOK_MASK+1 frames
	eor wtFrame
	and #AI_LOOK_MASK
	bne aoRts
	lda aiFar
	bne aoRts
	lda aiDist+1
	cmp #AI_SIGHT
	bcs aoRts
	; In front: (player - monster) . (sin yaw, cos yaw) >= 0.
	ldy acYaw,x
	jsr aiFacing
	bmi aoRts
	jsr aiCanWake			; no trace a wake could not use
	bcs aoRts
	jmp aiLosReq
aoRts
	rts

; C clear if one more monster may wake: fewer than AI_MAXAWAKE alive and
; awake. Counted at most once a frame, and only when a sleeper could
; wake. Preserves X.
aiCanWake
	lda aiAwN
	bpl acwCmp
	stx aiT2
	lda #0
	sta aiAwN
	ldy aiNL
	beq acwDone
-	ldx aiList-1,y
	lda acAi,x
	beq +
	lda acState,x
	cmp #AS_DEAD
	beq +
	inc aiAwN
+	dey
	bne -
acwDone
	ldx aiT2
	lda aiAwN
acwCmp
	cmp #AI_MAXAWAKE
	rts

; --- awake -------------------------------------------------------------
aoAwake
	lda aiFar
	beq +
	lda #AI_SLEEP			; lost: over 64 units away
	sta acAi,x
	jmp acStand
+	jsr aiThink
	ldx aiX
	lda acAi,x
	cmp #AI_CHASE
	beq aoChase
	cmp #AI_SHOOT
	beq aoShoot
	cmp #AI_BITE
	bne +
	jmp aoBite
+	jmp aoLeap

aoChase
	; Close in, but not onto the player.
	jsr mtType
	sty aiTy
	lda mbStopH,y
	pha
	lda mbStopL,y
	tay
	pla
	jsr aiDistLt
	bcs aocRts
	ldy aiTy
	lda mbStep,y
	jmp aiMoveReq
aocRts
	rts

; mbShots of them, from the SHOOT frame on, one an animation frame: a
; volley, or a missile (gpu64_game_proj.inc).
aoShoot
	jsr mtType
	lda mtShootF,y
	clc
	adc acFired,x
	cmp acAnF,x
	bne aosRts
	lda acFired,x
	cmp mbShots,y
	bcs aosRts
	inc acFired,x
	lda #SFX_GRUNT
	jsr sfxPlay
	lda mbProj,y
	beq +
	jmp pjLaunch
+	jmp aiVolley
aosRts
	rts

aoBite
	jsr mtType
	sty aiTy
	lda acAnF,x
	cmp mtMeleeF,y
	bne aobRts
	lda acFired,x
	bne aobRts
	lda #1
	sta acFired,x
	lda #SFX_BITE
	jsr sfxPlay
	ldy aiTy
	lda mbReachH,y
	pha
	lda mbReachL,y
	tay
	pla
	jsr aiDistLt
	bcc aobRts			; it backed off in time
	ldy aiTy
	jsr rnd				; preserves X and Y
	and mbMRnd,y
	clc
	adc mbMDmg,y
	jmp aiHit
aobRts
	rts

aoLeap
	lda acLeapT,x
	beq aolRts			; landed: the animation finishes it
	dec acLeapT,x
	lda acFired,x
	bne aolMove
	ldy #<DOG_TOUCH_F
	lda #>DOG_TOUCH_F
	jsr aiDistLt
	bcc aolMove
	lda #1				; Quake's dog_JumpTouch: one hit, then
	sta acFired,x			; it just falls
	lda #0
	sta acLeapT,x
	jsr mtType
	jsr rnd				; preserves X and Y
	and mbLRnd,y
	clc
	adc mbLDmg,y
	jmp aiHit
aolMove
	lda #LEAP_STEP
	jmp aiMoveReq
aolRts
	rts

; A think, 10 Hz: line of sight, an attack, or where to run. X = slot.
; aiGeom's max/min/signs are still this monster's: aiIdeal from them now.
aiThink
	jsr aiAtan			; preserves X
	lda acAtkT,x
	beq +
	dec acAtkT,x
+	jsr aiLosReq
	ldx aiX
	lda acAi,x
	cmp #AI_CHASE
	beq atChase
	cmp #AI_LEAP
	beq atRts			; committed to the jump
	lda aiIdeal			; winding up or biting: face the player
	sta acYaw,x
	lda #1
	sta acDirty,x
atRts
	rts

; Whichever attacks the type's bank has frames for, in Quake's order:
; melee in reach, a leap from a little further out, else a shot.
atChase
	jsr mtType
	sty aiTy
	lda #MA_MELEE
	jsr mtHas
	beq atNoBite
	ldy aiTy
	lda mbMeleeH,y
	pha
	lda mbMeleeL,y
	tay
	pla
	jsr aiDistLt
	bcc atNoBite
	ldy #MA_MELEE
	lda #AI_BITE
	jmp atAttack
atNoBite
	lda #MA_LEAP
	jsr mtHas
	beq atNoLeap
	lda acSee,x
	beq atNoLeap
	lda acAtkT,x
	bne atNoLeap
	ldy aiTy
	lda mbLeapH,y
	pha
	lda mbLeapL,y
	tay
	pla
	jsr aiDistLt
	bcc atSteer
	jsr rnd				; preserves X
	bmi atNoLeap			; half the time
	lda #SFX_LEAP
	jsr sfxPlay
	lda #LEAP_FRAMES
	sta acLeapT,x
	ldy #MA_LEAP
	lda #AI_LEAP
	jmp atAttack
atNoLeap
	lda #MA_SHOOT
	jsr mtHas
	beq atSteer
	lda acSee,x
	beq atSteer
	lda acAtkT,x
	bne atSteer
	; SoldierCheckAttack: a chance by distance, none past GR_FAR_F.
	ldy #<GR_FAR_F
	lda #>GR_FAR_F
	jsr aiDistLt
	bcc atSteer
	lda #13				; mid: 0.05
	sta aiChance
	ldy #<GR_MID_F
	lda #>GR_MID_F
	jsr aiDistLt
	bcc atRoll
	lda #102			; near: 0.4
	sta aiChance
	ldy #<GR_NEAR_F
	lda #>GR_NEAR_F
	jsr aiDistLt
	bcc atRoll
	lda #230			; melee: 0.9
	sta aiChance
atRoll
	jsr rnd
	cmp aiChance
	bcs atSteer
	ldy #MA_SHOOT
	lda #AI_SHOOT
	; fall through

; A = acAi, Y = MA_*: face the player and start it.
atAttack
	sta acAi,x
	lda #0
	sta acFired,x
	lda aiIdeal
	sta acYaw,x
	lda #AM_ONCE
	sta cbMode
	tya
	jmp mtAnim

; Where to run: straight at the player, or a detour after a blocked step.
atSteer
	lda acDet,x
	beq atNoDet
	dec acDet,x
	lda acDetY,x
	jmp atTurn
atNoDet
	lda acBlk,x
	beq atIdeal
	lda #0
	sta acBlk,x
	lda #AI_DETOUR
	sta acDet,x
	inc acDetS,x
	lda acDetS,x
	and #3
	tay
	lda aiIdeal
	clc
	adc aiDetTab,y
	sta acDetY,x
	jmp atTurn
atIdeal
	lda aiIdeal
; A = the yaw wanted: turn toward it, at most AI_TURN.
atTurn
	sec
	sbc acYaw,x			; signed difference, -128..127
	bmi atLeft
	cmp #AI_TURN
	bcc atAdd
	lda #AI_TURN
	bne atAdd
atLeft
	cmp #$100 - AI_TURN
	bcs atAdd
	lda #$100 - AI_TURN
atAdd
	clc
	adc acYaw,x
	cmp acYaw,x
	beq atDone
	sta acYaw,x
	lda #1
	sta acDirty,x
atDone
	rts

; Off the ideal yaw: 45 and 90 degrees, alternating sides.
aiDetTab	.byte 32, $100 - 64, 64, $100 - 32

; A:Y = a distance in fine. C set if aiDist is under it. Preserves X.
aiDistLt
	cmp aiDist+1
	bne +
	cpy aiDist
	beq adlNo
	rts				; C from cpy: Y > aiDist
+	rts				; C from cmp: A > aiDist+1
adlNo
	clc
	rts

; ======================================================================
; Geometry, from dd = monster - eye (acDelta). aiDx/aiDz: the player
; relative to the monster, s16 fine; aiAdx/aiAdz their absolute values;
; aiDist ~ the floor-plane distance (max + 3/8 min, within 7%); aiIdeal the
; yaw that faces the player. aiFar set when either axis is past 64 units,
; and then nothing else is valid.
; ======================================================================
aiGeom
	lda #1
	sta aiFar
	ldy #0
	jsr ddNear
	bcc +
	rts
+	ldy #6
	jsr ddNear
	bcc +
	rts
+	lda #0
	sta aiFar
	sec				; toward the player: -dd
	lda #0
	sbc dd+0
	sta aiDx
	lda #0
	sbc dd+1
	sta aiDx+1
	sec
	lda #0
	sbc dd+6
	sta aiDz
	lda #0
	sbc dd+7
	sta aiDz+1
	ldx #0
	jsr agAbs
	ldx #2
	jsr agAbs
	; max and min
	lda aiAdx+1
	cmp aiAdz+1
	bne +
	lda aiAdx
	cmp aiAdz
+	bcc agZBig
	lda aiAdx
	sta aiMax
	lda aiAdx+1
	sta aiMax+1
	lda aiAdz
	sta aiMin
	lda aiAdz+1
	sta aiMin+1
	jmp agDist
agZBig
	lda aiAdz
	sta aiMax
	lda aiAdz+1
	sta aiMax+1
	lda aiAdx
	sta aiMin
	lda aiAdx+1
	sta aiMin+1
agDist
	; min/4 + min/8
	lda aiMin+1
	lsr a
	sta aiT+1
	lda aiMin
	ror a
	lsr aiT+1
	ror a
	sta aiT
	lda aiT+1
	lsr a
	sta aiT2+1
	lda aiT
	ror a
	sta aiT2
	clc
	lda aiT
	adc aiT2
	sta aiT
	lda aiT+1
	adc aiT2+1
	sta aiT+1
	clc
	lda aiMax
	adc aiT
	sta aiDist
	lda aiMax+1
	adc aiT+1
	sta aiDist+1
	rts

; X = 0 (x) or 2 (z): aiAdx+X = |aiDx+X|.
agAbs
	lda aiDx+1,x
	bpl +
	sec
	lda #0
	sbc aiDx,x
	sta aiAdx,x
	lda #0
	sbc aiDx+1,x
	sta aiAdx+1,x
	rts
+	lda aiDx,x
	sta aiAdx,x
	lda aiDx+1,x
	sta aiAdx+1,x
	rts

; aiIdeal = atan2(aiDx, aiDz) as a yaw: (sin a, cos a) points at the
; player. The octant from the signs and which axis is longer, then
; min/max as 5 bits through a 33-entry arctangent table.
aiAtan
	lda aiMax
	sta aiT
	lda aiMax+1
	sta aiT+1
	lda aiMin
	sta aiT2
	lda aiMin+1
	sta aiT2+1
-	lda aiT+1			; both down to 8 bits
	beq +
	lsr aiT+1
	ror aiT
	lsr aiT2+1
	ror aiT2
	jmp -
+	lda aiT
	bne +
	lda #0				; on top of the player
	sta aiIdeal
	rts
+	; q = min * 32 / max, 0..32, by five steps of long division.
	lda aiT2
	cmp aiT
	bcc +
	lda #32				; min == max: 45 degrees
	sta aiQ
	bne aaQ
+	ldy #0
	sty aiQ
	ldy #5
aaDiv
	asl a				; the remainder, 9 bits in C:A
	bcs aaSub
	cmp aiT
	bcc aaZero
aaSub
	sbc aiT				; C is set on both paths in
	sec
	rol aiQ
	jmp aaNext
aaZero
	asl aiQ
aaNext
	dey
	bne aaDiv
aaQ
	ldy aiQ
	lda aiAtTab,y			; 0..32: the angle off the longer axis
	sta aiT
	; The longer axis: z, then the angle is off z toward x; x, then 64 - it.
	lda aiAdz+1
	cmp aiAdx+1
	bne +
	lda aiAdz
	cmp aiAdx
+	bcs +
	lda #64
	sec
	sbc aiT
	sta aiT
+	; The quadrant, measured from +z toward +x.
	lda aiDz+1
	bmi aaSouth
	lda aiDx+1
	bmi aaNW
	lda aiT				; +x, +z
	sta aiIdeal
	rts
aaNW
	lda #0				; -x, +z: 256 - a
	sec
	sbc aiT
	sta aiIdeal
	rts
aaSouth
	lda aiDx+1
	bmi aaSW
	lda #128			; +x, -z: 128 - a
	sec
	sbc aiT
	sta aiIdeal
	rts
aaSW
	lda #128			; -x, -z: 128 + a
	clc
	adc aiT
	sta aiIdeal
	rts

; atan(i / 32) in 1/256 turns, i = 0..32.
aiAtTab
	.for i = 0, i <= 32, i += 1
	.byte round(atan(i / 32.0) * 128 / pi)
	.next

; Y = a yaw. N set if the player is behind it: aiDx*sin + aiDz*cos < 0.
aiFacing
	lda aiDx
	sta mA
	lda aiDx+1
	sta mA+1
	lda sinLo,y
	sta mS
	lda sinHi,y
	sta mS+1
	sty aiT
	jsr mulTrig
	lda mR
	sta aiT2
	lda mR+1
	sta aiT2+1
	lda aiT
	clc
	adc #64
	tay
	lda aiDz
	sta mA
	lda aiDz+1
	sta mA+1
	lda sinLo,y
	sta mS
	lda sinHi,y
	sta mS+1
	jsr mulTrig
	ldx aiX
	clc
	lda mR
	adc aiT2
	lda mR+1
	adc aiT2+1
	rts

; ======================================================================
; The queries. Each is built into aiTrBuf/aiMvBuf here and copied into the
; block by aiEmit; aiResolve reads the answers back.
; ======================================================================
; X = slot: its eye to the player's, hull 0. Nothing if the frame's budget
; is spent -- the next think asks again.
aiLosReq
	ldy aiTrN
	cpy #AI_MAXTR
	bcc +
	rts
+
	txa
	sta aiTrSl,y
	lda aiTrOfs,y
	tay
	.for k = 0, k < 12, k += 1
	lda acPos+k*MAX_ACT,x
	sta aiTrBuf+k,y
	.next
	clc				; up to the eye: y's fine bytes
	lda aiTrBuf+5,y
	adc #<AI_EYE
	sta aiTrBuf+5,y
	lda aiTrBuf+6,y
	adc #>AI_EYE
	sta aiTrBuf+6,y
	lda aiTrBuf+7,y
	adc #0
	sta aiTrBuf+7,y
	.for k = 0, k < 12, k += 1
	lda cam+k
	sta aiTrBuf+12+k,y
	.next
	lda #0				; hull 0, reserved
	sta aiTrBuf+24,y
	sta aiTrBuf+25,y
	sta aiTrBuf+26,y
	sta aiTrBuf+27,y
	inc aiTrN
	rts

; X = slot, A = step in fine per DT_REF frame: its origin plus twice the
; step (it moves every other frame), scaled by the frame time, along its
; yaw, hull 1, walking. At most 2 x 240 fine, inside aiBound's two units.
; Nothing if the budget is spent.
aiMoveReq
	jsr dtMul			; preserves X
	ldy aiMvN
	cpy #AI_MAXMV
	bcc +
	rts
+
	asl a
	sta aiStep
	lda #0
	rol a
	sta aiStep+1
	txa
	sta aiMvSl,y
	lda aiMvOfs,y
	sta aiT
	tay
	.for k = 0, k < 12, k += 1
	lda acPos+k*MAX_ACT,x
	sta aiMvBuf+k,y
	.next
	lda #0
	sta aiMvBuf+12,y		; x: step * sin, into bytes 13-15
	sta aiMvBuf+16,y		; y: none
	sta aiMvBuf+17,y
	sta aiMvBuf+18,y
	sta aiMvBuf+19,y
	sta aiMvBuf+20,y		; z: step * cos, into bytes 21-23
	lda #1				; hull 1
	sta aiMvBuf+24,y
	lda #AI_CLIP_MODE
	sta aiMvBuf+25,y
	lda #0
	sta aiMvBuf+26,y
	sta aiMvBuf+27,y
	ldy acYaw,x
	lda sinLo,y
	sta mS
	lda sinHi,y
	sta mS+1
	lda aiStep
	sta mA
	lda aiStep+1
	sta mA+1
	jsr mulTrig
	ldy aiT
	ldx #13
	jsr amPut
	ldx aiX
	lda acYaw,x
	clc
	adc #64
	tay
	lda sinLo,y
	sta mS
	lda sinHi,y
	sta mS+1
	lda aiStep
	sta mA
	lda aiStep+1
	sta mA+1
	jsr mulTrig
	ldy aiT
	ldx #21
	jsr amPut
	ldx aiX
	jsr aiFlyY
	inc aiMvN
	rts

; X = slot, aiT = its move's offset, dd = it - the eye: a flyer's move
; does not settle on a floor, and climbs or sinks FLY_STEP to keep its
; origin FLY_LO..FLY_HI over the eye -- Quake's scrag, 30-40 QU over the
; player. Preserves X.
aiFlyY
	jsr mtType
	lda mbFly,y
	beq afyRts
	ldy aiT
	lda #$01			; slide
	sta aiMvBuf+25,y
	lda dd+5
	bmi afyUp
	ora dd+4
	bne afyDown
	lda dd+3
	cmp #FLY_LO
	bcc afyUp
	cmp #FLY_HI+1
	bcc afyRts
afyDown
	lda #<-FLY_STEP
	sta aiMvBuf+17,y
	lda #$ff
	bne afyHi
afyUp
	lda #FLY_STEP
	sta aiMvBuf+17,y
	lda #0
afyHi
	sta aiMvBuf+18,y
	sta aiMvBuf+19,y
afyRts
	rts

; X = aiX, a flyer's body: FALL_STEP straight down, settling on a floor.
aiFallReq
	lda aiMvN
	pha
	lda #0				; a move of nothing, then made a fall
	jsr aiMoveReq
	pla
	cmp aiMvN
	beq afrRts			; the budget was spent
	ldy aiT
	lda #0
	sta aiMvBuf+17,y
	lda #>-FALL_STEP
	sta aiMvBuf+18,y
	lda #$ff
	sta aiMvBuf+19,y
	lda #$05			; slide, settle
	sta aiMvBuf+25,y
afrRts
	rts

; mR, fine, sign-extended into aiMvBuf + Y + X .. +X+2.
amPut
	stx aiT2
	tya
	clc
	adc aiT2
	tay
	lda mR
	sta aiMvBuf,y
	lda mR+1
	sta aiMvBuf+1,y
	and #$80
	beq +
	lda #$ff
+	sta aiMvBuf+2,y
	rts

AI_CLIP_MODE = CLIP_MODE_WALK & ~$08	; slide, step, floor settle; no eye

aiTrOfs		.for i = 0, i < AI_MAXTR, i += 1
		.byte i * 28
		.next
aiMvOfs		.for i = 0, i < AI_MAXMV, i += 1
		.byte i * 28
		.next

; worldTick's: the traces, then the moves, at (DST), which moves past them.
aiEmit
	lda aiTrN
	beq aeMoves
	tax
	lda aiTrOfs-1,x
	clc
	adc #28
	jsr aeCopyTr
aeMoves
	lda aiMvN
	beq aeRts
	tax
	lda aiMvOfs-1,x
	clc
	adc #28
	jmp aeCopyMv
aeRts
	rts

; A = bytes (1..255) from aiTrBuf / aiMvBuf.
aeCopyTr
	sta aiT
	ldy #0
-	lda aiTrBuf,y
	sta (DST),y
	iny
	cpy aiT
	bne -
	jmp aeAdvance
aeCopyMv
	sta aiT
	ldy #0
-	lda aiMvBuf,y
	sta (DST),y
	iny
	cpy aiT
	bne -
aeAdvance
	clc
	lda DST
	adc aiT
	sta DST
	bcc +
	inc DST+1
+	rts

; ======================================================================
; After worldTick (and combatResolve): the answers. Traces are at OUT + 8 +
; 16 * (the shot's + i), moves after every trace.
; ======================================================================
aiResolve
	lda wtTrOk
	bne +
	rts				; the block never verified: no answers
+	sec				; the monsters' traces follow the shot's
	lda wtQTr
	sbc aiTrN
	sta aiTr0
	lda #0
	sta aiI
arTrLoop
	lda aiI
	cmp aiTrN
	bcs arMoves
	clc
	adc aiTr0
	jsr aiAnsOfs
	ldx aiI
	lda aiTrSl,x
	tax
	lda acKind,x
	cmp #K_PROJ
	bne +
	jsr pjAnswer			; a missile's: how far it flies
	jmp arTrNext
+	lda acState,x
	cmp #AS_DEAD
	beq arTrNext
	lda (DST),y			; flags: CLEAR, STARTSOLID, ALLSOLID
	and #$f8
	bne arTrNext			; a flag that does not exist: not an answer
	lda (DST),y
	and #1
	sta aiT
	lda acAi,x
	bne arSee
	lda aiT				; asleep: a clear line wakes it
	beq arTrNext
	lda acState,x
	cmp #AS_IDLE
	bne arTrNext
	jsr aiCanWake
	bcs arTrNext
	jsr aiWake
	jmp arTrNext
arSee
	lda aiT
	sta acSee,x
arTrNext
	inc aiI
	bne arTrLoop

arMoves
	lda #0
	sta aiI
arMvLoop
	lda aiI
	cmp aiMvN
	bcc +
	rts
+	clc
	adc wtQTr
	jsr aiAnsOfs			; Y at +14: the fraction ...
	lda (DST),y
	sta aiFrac
	dey
	dey				; ... +12: the flags
	lda (DST),y
	sta aiFlags
	tya
	sec
	sbc #12
	sta aiAns			; +0: the end
	ldx aiI
	lda aiMvSl,x
	tax
	stx aiX
	lda acState,x
	cmp #AS_DEAD
	bne arAlive
	lda aiFlags			; a scrag's body, falling
	and #CLIP_SOLID
	bne arLand
	jsr aiBound
	bcs arLand
	jsr arTake
	lda aiFlags
	and #CLIP_ONGROUND
	beq arMvNext
arLand
	lda #0
	sta acLeapT,x
	beq arMvNext
arAlive
	lda aiFlags
	and #CLIP_SOLID
	bne arBlocked
	jsr mtType			; a flyer needs no floor
	lda mbFly,y
	bne +
	lda aiFlags
	and #CLIP_ONGROUND
	beq arBlocked			; a ledge: not taken
+	jsr aiBound
	bcs arRefuse
	jsr arTake
	lda aiFrac
	cmp #AI_BLOCK
	bcs arMvNext
arBlocked
	lda #1
	sta acBlk,x
	lda #0				; a leap stops at what it hit
	sta acLeapT,x
	inc aiBlocks
	bne arMvNext
	dec aiBlocks
	jmp arMvNext
arRefuse
	inc aiRefs
	bne arMvNext
	dec aiRefs
arMvNext
	inc aiI
	jmp arMvLoop

; X = slot: the move's end at (DST),aiAns is where it is now. Preserves X.
arTake
	ldy aiAns
	.for k = 0, k < 12, k += 1
	lda (DST),y
	sta acPos+k*MAX_ACT,x
	iny
	.next
	lda #1
	sta acDirty,x
	rts

; A = answer index. Y = its offset + 14 from DST.
aiAnsOfs
	asl a
	asl a
	asl a
	asl a
	clc
	adc #8+14
	tay
	rts

; X = slot, aiAns = the move's end at (DST),Y. C clear if the end is less
; than two units from acPos on every axis (a double step is at most 1.9).
; Preserves X.
aiBound
	.for ax = 0, ax < 3, ax += 1
	ldy aiAns
	sec
	lda (DST),y
	sbc acPos+(ax*4+0)*MAX_ACT,x
	iny
	lda (DST),y
	sbc acPos+(ax*4+1)*MAX_ACT,x
	iny
	lda (DST),y
	sbc acPos+(ax*4+2)*MAX_ACT,x
	sta aiT
	iny
	lda (DST),y
	sbc acPos+(ax*4+3)*MAX_ACT,x
	sta aiT+1
	; top:whole in -2..1
	clc
	lda aiT
	adc #2
	sta aiT
	lda aiT+1
	adc #0
	beq +
	jmp abFar
+	lda aiT
	cmp #4
	bcc +
	jmp abFar
+	lda aiAns
	clc
	adc #4
	sta aiAns
	.next
	lda aiAns			; back where it started
	sec
	sbc #12
	sta aiAns
	clc
	rts
abFar
	sec				; refused: aiAns is not used again
	rts

; ======================================================================
; Damage to the player. The grunt's volley: GR_PEL pellets, each hitting
; when both |crandom| * 0.1 * distance are inside the player's box.
; ======================================================================
aiVolley
	lda acSee,x
	beq avRts			; out of sight: every pellet in a wall
	lda aiDist+1			; D = distance / 32 fine, 8 bits
	cmp #$20
	bcs avRts			; past 32 units: not a volley (GR_FAR_F)
	sta aiT+1
	lda aiDist
	.for i = 0, i < 5, i += 1
	lsr aiT+1
	ror a
	.next
	sta aiD
	lda #0
	sta aiDmg
	jsr mtType
	sty aiTy
	lda mbPel,y
	beq avRts
	sta aiPel
avPel
	jsr rnd
	ldy aiD
	jsr mul8
	lda m8H
	cmp #>GR_SPREAD_X
	bcs avMiss
	jsr rnd
	ldy aiD
	jsr mul8
	lda m8H
	cmp #>GR_SPREAD_Y
	bcs avMiss
	ldy aiTy
	lda aiDmg
	clc
	adc mbPelD,y
	sta aiDmg
avMiss
	dec aiPel
	bne avPel
	ldx aiX
	lda aiDmg
	beq avRts
	; fall through

; A = damage to the player, from monster aiX. Preserves nothing; aiX is
; the caller's slot.
aiHit
	jsr plHurt
	inc aiHits
	bne +
	dec aiHits
+	ldx aiX
avRts
	rts

; ----------------------------------------------------------------------
; Row 23: AWK monsters awake now, WK wakes ever, SEE awake ones with a
; clear line, BLK blocked steps, RF moves refused by the bound, HT times
; a monster hurt the player.
; ----------------------------------------------------------------------
showAi
	lda #0
	sta aiT
	sta aiT+1
	ldx #SL_WORLD
-	cpx nAct
	bcs +
	lda acAi,x
	beq sa0
	lda acState,x
	cmp #AS_DEAD
	beq sa0
	inc aiT
	lda acSee,x
	beq sa0
	inc aiT+1
sa0
	inx
	bne -
+	lda #23
	jsr dmRow
	jsr dmText
	.text "AWK"
	.byte 0
	lda aiT
	jsr dmDecA
	jsr dmText
	.text " WK"
	.byte 0
	lda aiWakes
	jsr dmDecA
	jsr dmText
	.text " SEE"
	.byte 0
	lda aiT+1
	jsr dmDecA
	jsr dmText
	.text " BLK"
	.byte 0
	lda aiBlocks
	jsr dmDecA
	jsr dmText
	.text " RF"
	.byte 0
	lda aiRefs
	jsr dmDecA
	jsr dmText
	.text " HT"
	.byte 0
	lda aiHits
	jmp dmDecA

; ----------------------------------------------------------------------
; State.
; ----------------------------------------------------------------------
aiTrN		.byte 0
aiMvN		.byte 0
aiLoud		.byte 0
aiNoisy		.byte 0
aiAwN		.byte $ff	; awake and alive, $ff unknown
aiRR		.byte 0
aiCnt		.byte 0
aiX		.byte 0
aiI		.byte 0
aiMon		.byte 0
aiNL		.byte 0
aiList		.fill MAX_ACT, 0
aiFar		.byte 0
aiIdeal		.byte 0
aiChance	.byte 0
aiStep		.word 0
aiAnN		.byte 0
aiTy		.byte 0
aiTr0		.byte 0
aiFrac		.byte 0
aiFlags		.byte 0
aiAns		.byte 0
aiQ		.byte 0
aiD		.byte 0
aiDmg		.byte 0
aiPel		.byte 0
aiWakes		.byte 0
aiBlocks	.byte 0
aiRefs		.byte 0
aiHits		.byte 0
aiDx		.word 0			; aiDx, aiDz adjacent: agAbs indexes them
aiDz		.word 0
aiAdx		.word 0
aiAdz		.word 0
aiMax		.word 0
aiMin		.word 0
aiDist		.word 0
aiT		.word 0
aiT2		.word 0
aiTrSl		.fill AI_MAXTR, 0
aiMvSl		.fill AI_MAXMV, 0
aiTrBuf		.fill AI_MAXTR * 28, 0
aiMvBuf		.fill AI_MAXMV * 28, 0
aiSpYaw		.fill AI_MAXMON, 0
aiSpPos		.fill 12 * AI_MAXMON, 0
acAi		.fill MAX_ACT, 0
acSee		.fill MAX_ACT, 0
acAtkT		.fill MAX_ACT, 0
acDet		.fill MAX_ACT, 0
acDetY		.fill MAX_ACT, 0
acDetS		.fill MAX_ACT, 0
acBlk		.fill MAX_ACT, 0
acFired		.fill MAX_ACT, 0
acLeapT		.fill MAX_ACT, 0
acSpIx		.fill MAX_ACT, $ff

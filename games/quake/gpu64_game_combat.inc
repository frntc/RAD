; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_combat.inc -- milestone 20 stage D: weapons, hitscan, pickups,
; player damage, death and restart, for gpu64_demo_game.a.
;
; The Pi answers one question here and the C64 does the rest. A shot looks
; for up to CB_NCAND live monsters in a cone ahead of the eye, nearest first,
; and asks WORLD_TICK for a point trace from the eye to each one's origin --
; in the same block, and the same dispatch, as the frame's poses. A monster
; whose trace comes back TR_CLEAR can be hit; which pellets do hit is then
; plain 6502 arithmetic against the forward/lateral distances the search
; already computed. That is Quake's own split: aim() tests line of sight,
; and FireBullets' pellets are rays with a random spread. Vertical aim is
; automatic, as sv_aim makes it in Quake; the vertical SPREAD still counts,
; so a shotgun at the far end of its range scatters over and under.
;
; Units: "fine" is 1/256 world unit = 1/8 Quake unit, the middle two bytes
; of a 16.16 coordinate. All of this is signed 16-bit fine arithmetic,
; which is why a candidate has to be inside CB_RANGE on both floor axes.
;
; The nailgun is hitscan too -- a nail is a projectile in Quake, flying
; 1000 units a second, and at E1M1's ranges it lands a frame or two after a
; hitscan would. Drawing it would cost a node per nail in flight.
; ======================================================================

K_HEALTH  = 31
K_ARM1    = 32
K_ARM3    = 34
K_SSG     = 35
K_NAILGUN = 36
K_QUAD    = 40
K_SUIT    = 42
K_SHELLS  = 45
K_NAILS   = 46
K_GRUNT   = 51			; monsters are K_MON0..K_MON_LAST
K_ZOMBIE  = 55
ZB_KILL   = 48			; damage at once that kills a zombie
ZB_DOWN   = 25			; ... that knocks it flat
K_DOG     = 52			; (gpu64_game_monsters.inc)
K_PACK    = 70			; a dead grunt's backpack; not a map kind

W_AXE     = 0
W_SHOT    = 1
W_SSG     = 2
W_NAIL    = 3

AS_IDLE   = 0			; acState: monsters only
AS_PAIN   = 1
AS_DEAD   = 2

AM_LOOP   = 0			; acAnMode
AM_HOLD   = 1			; stop on the last frame (death)
AM_ONCE   = 2			; then back to standing (pain)

CB_NCAND  = 3			; traces a shot may ask for
CB_RANGE  = $40			; fine high byte: 64 world units = 2048 QU
AXE_REACH = 3			; fine high byte: < 96 QU (64 + a monster's 16 + slack)
AXE_HALF  = 160			; fine: 16 QU box half-width + 4 QU slack
PEL_HALF  = 128			; fine: a monster box is 32 QU wide ...
PEL_LOW   = 192			;   ... and 24 QU below its origin ...
PEL_SPAN  = 512			;   ... to 40 QU above it: 64 QU = 512 fine

FIRE_HOLD = 2			; frames fire must read down to start
KEY_HOLD  = 2			; weapon keys and the health cheat
RESPAWN_WAIT = 22		; ~1 s dead before fire restarts
DROP_MAX  = 240			; fine: the eye sinks 30 QU when dead ...
DROP_STEP = 40			; ... over six frames
PAIN_FACE = 11			; ~0.5 s of the pain face
QUAD_FRAMES = 650		; ~30 s at ~46 ms a frame

; Per weapon: axe, shotgun, super shotgun, nailgun.
wpGun		.byte M_V_AXE, M_V_SHOT, M_V_SHOT2, M_V_NAIL
wpGunN		.byte 5, N_V_SHOT, N_V_SHOT2, N_V_NAIL	; axe: its first swing only
wpWait		.byte 11, 11, 15, 2			; 0.5, 0.5, 0.7, 0.1 s
wpPel		.byte 1, 6, 14, 1
wpSfx		.byte SFX_AXE, SFX_SHOT, SFX_SSHOT, SFX_NAIL
wpDmg		.byte 20, 4, 4, 9
wpSx2		.byte 0, 20, 72, 0	; 2 * spread * 256: 0.04, 0.14
wpSy2		.byte 0, 20, 41, 0	;                   0.04, 0.08
wpCost		.byte 0, 1, 2, 1
wpBit		.byte 1, 2, 4, 8

; ======================================================================
; Setup, after seedCamera and actInit: the start is where restart returns.
; ======================================================================
combatInit
	ldx #11
-	lda cam,x
	sta startCam,x
	dex
	bpl -
	lda camYaw
	sta startYaw
	lda camYaw+1
	sta startYaw+1
	lda #$5a
	sta rndLo
	lda #$c3
	sta rndHi
	lda #0
	sta cbShots
	sta cbHits
	sta cbLost
	sta cbPicks
	sta cbHurts
	sta cbDeaths
	sta kills
	sta hurtHold
	sta fireHold
	ldx #3
-	sta wkHold,x
	dex
	bpl -
	; fall through

; The player's side of a restart.
plReset
	lda #100
	sta plHp
	lda #0
	sta plArm
	sta plArmT
	sta plNails
	sta plDead
	sta plDrop
	sta plPain
	sta plQuad
	sta plQuad+1
	sta fireWait
	sta gunF
	sta shotPend
	sta wtTrN
	lda #25
	sta plShells
	lda #1 << W_AXE | 1 << W_SHOT
	sta plWeaps
	lda #W_SHOT
	sta plWeap
	lda #AF_VIS | AF_VIEW
	sta acFlags+SL_GUN
	lda #1
	sta acDirty+SL_GUN
	jsr gunPose
	jsr envReset
	jmp hudStats

; entScan's count of monsters, for the K row. Called once after it.
combatCount
	lda #0
	sta kTotal
	ldx #SL_WORLD
ccLoop
	cpx nAct
	bcs ccDone
	lda acKind,x
	cmp #K_MON0
	bcc +
	cmp #K_MON_LAST+1
	bcs +
	inc kTotal
+	inx
	bne ccLoop
ccDone
	rts

; ======================================================================
; A frame, before actFrame: dying, weapon keys, the fire key, the gun.
; ======================================================================
combatFrame
	lda #0
	sta wtTrN
	sta shotPend

	; Fire: readKeys's fireDown (C=, B, joystick 2's button, the mouse's),
	; held FIRE_HOLD frames to start (column 7 is read once a frame, and
	; one phantom sample is not worth a shell), then repeating at the
	; weapon's rate while held.
	lda #0
	sta fireEdge
	lda fireDown
	bne cfDown
	lda #0
	sta fireHold
	beq cfKeyed
cfDown
	lda fireHold
	cmp #FIRE_HOLD
	bcs cfKeyed
	inc fireHold
	lda fireHold
	cmp #FIRE_HOLD
	bne cfKeyed
	inc fireEdge
cfKeyed

	lda plPain
	beq +
	dec plPain
+	lda plQuad
	ora plQuad+1
	beq cfNoQuad
	lda plQuad
	bne +
	dec plQuad+1
+	dec plQuad
cfNoQuad
	; runsim.py --hurt: damage no real C64 can ask for. Taken off the
	; byte even while dead, so it is dropped there rather than kept for
	; after the restart.
	ldx SIM_HURT
	lda #0
	sta SIM_HURT
	lda plDead
	beq cfAlive
	jmp cfDeadFrame
cfAlive
	cpx #SIM_HURT_ON
	bne cfNoSimHurt
	lda #SIM_HURT_DMG
	jsr plHurt
	lda plDead
	beq cfNoSimHurt
	jmp cfDeadFrame
cfNoSimHurt

	; The health cheat (H), for testing levels. <- used to be a test HURT
	; key from before the monsters; it is the pause menu now.
	lda keyF
	and #KF_H
	beq +
	lda #0
	sta hurtHold
	beq cfNoCheat
+	lda hurtHold
	cmp #KEY_HOLD
	bcs cfNoCheat
	inc hurtHold
	lda hurtHold
	cmp #KEY_HOLD
	bne cfNoCheat
	jsr plCheat
cfNoCheat
	jsr giveCheat			; I, under BASIC

	jsr weapKeys

	lda fireWait
	beq +
	dec fireWait
	jmp cfGun
+	lda fireHold
	cmp #FIRE_HOLD
	bcc cfGun
	cmp #$ff			; still the press that restarted
	beq cfGun
	jsr fire
cfGun
	jsr gunStep
	jmp hudStats

; Dead: the eye sinks, nothing is controlled, and after RESPAWN_WAIT the
; fire key starts the level again.
cfDeadFrame
	lda plDead
	cmp #$ff
	beq +
	inc plDead
+	lda plDrop
	cmp #DROP_MAX
	bcs +
	adc #DROP_STEP
	sta plDrop
+	lda plDead
	cmp #RESPAWN_WAIT
	bcc +
	lda fireEdge
	beq +
	jsr restart
+	jmp hudStats

; moveCamera, straight after readKeys: a dead player's movement keys are
; released. Fire (it restarts) and the view keys still work; mouseLook
; checks plDead itself.
deadKeys
	lda plDead
	beq +
	lda #$ff
	sta keyB
	sta keyC
	lda keyD
	ora #KD_Q | KD_SPC
	sta keyD
+	rts

; sendCamera, after it copies cam: the dead eye's drop, in fine units.
camDrop
	lda plDrop
	beq +
	sec
	lda stBuf+CAM_Y+1
	sbc plDrop
	sta stBuf+CAM_Y+1
	lda stBuf+CAM_Y+2
	sbc #0
	sta stBuf+CAM_Y+2
	lda stBuf+CAM_Y+3
	sbc #0
	sta stBuf+CAM_Y+3
+	rts

; 1-4 pick axe, shotgun, super shotgun, nailgun -- if owned and loaded.
weapKeys
	ldy #0
wkLoop
	lda wkCol,y
	tax
	lda keyB,x			; keyB, keyC, keyD sit together
	and wkBit,y
	beq +
	lda #0
	sta wkHold,y
	beq wkNext
+	lda wkHold,y
	cmp #KEY_HOLD
	bcs wkNext
	clc
	adc #1
	sta wkHold,y
	cmp #KEY_HOLD
	bne wkNext
	sty cbT
	tya
	jsr weapOk
	bcc +
	lda cbT
	jsr setWeap
+	ldy cbT
wkNext
	iny
	cpy #4
	bne wkLoop
	rts

wkCol		.byte keyD - keyB, keyD - keyB, 0, 0
wkBit		.byte KD_1, KD_2, KB_3, KB_4

; A = weapon. C set if the player has it and the ammo for one shot.
weapOk
	tax
	lda wpBit,x
	and plWeaps
	beq woNo
	txa
	jsr ammoOf
	cmp wpCost,x
	rts
woNo
	clc
	rts

; A = weapon (preserved in X). A = the ammo it uses.
ammoOf
	tax
	beq aoNone
	cpx #W_NAIL
	beq +
	lda plShells
	rts
+	lda plNails
	rts
aoNone
	lda #0
	rts

; Quake's W_BestWeapon, over the four this level has.
bestWeap
	lda #W_NAIL
	sta cbT
-	lda cbT
	jsr weapOk
	bcs +
	dec cbT
	bne -
+	lda cbT
	; fall through

; A = weapon: in hand, at its idle frame.
setWeap
	sta plWeap
	lda #0
	sta gunF
	jmp gunPose

; ======================================================================
; Firing.
; ======================================================================
fire
	lda plWeap
	jsr weapOk
	bcs +
	jmp bestWeap			; out of ammo: switch, and do not fire
+	ldx plWeap
	lda wpWait,x
	sta fireWait
	lda plShells
	cpx #W_NAIL
	bne +
	lda plNails
+	sec
	sbc wpCost,x
	cpx #W_NAIL
	bne +
	sta plNails
	jmp ++
+	sta plShells
+
	; The view model: the nailgun keeps cycling while it fires, the
	; others start their animation over.
	cpx #W_NAIL
	bne +
	lda gunF
	bne ++
+	lda #1
	sta gunF
+
	inc cbShots
	bne +
	dec cbShots
+
	lda wpSfx,x
	jsr sfxPlay
	jsr mfShot			; the muzzle flash (not the axe)
	lda wpPel,x
	sta cbPel
	lda wpDmg,x
	sta cbDmg
	lda wpSx2,x
	sta cbSx2
	lda wpSy2,x
	sta cbSy2

	cpx #W_AXE
	bne fiGun
	lda #AXE_REACH
	sta cbMaxF
	lda #1
	sta cbAxe
	jsr cbScan
	lda cN
	beq +
	ldx cSlot
	lda cbDmg
	jsr quadDmg
	jsr monHurt
	inc cbHits
	bne +
	dec cbHits
+	rts

fiGun
	lda #1				; a gunshot: aiFrame wakes whoever heard it
	sta aiLoud
	lda #CB_RANGE
	sta cbMaxF
	lda #0
	sta cbAxe
	jsr cbScan
	lda cN
	beq +				; nothing to hit: no traces to ask
	sta wtTrN
	lda #1
	sta shotPend
+	rts

; A = damage; x4 under the quad, saturating.
quadDmg
	ldy plQuad
	bne +
	ldy plQuad+1
	beq qdRts
+	asl a
	bcs qdMax
	asl a
	bcc qdRts
qdMax
	lda #$ff
qdRts
	rts

; ======================================================================
; The search. Every live monster inside cbMaxF ahead, and inside the cone
; (the axe: its reach), nearest first, up to CB_NCAND.
;   fwd = d . (sin y, cos y)      lat = d . (cos y, -sin y)
; with d the floor-plane distance, eye to origin, in fine units.
; ======================================================================
cbScan
	lda #0
	sta cN
	ldy camYaw+1
	lda sinLo,y
	sta cbSin
	lda sinHi,y
	sta cbSin+1
	tya
	clc
	adc #64
	tay
	lda sinLo,y
	sta cbCos
	lda sinHi,y
	sta cbCos+1

	ldx #SL_WORLD
csLoop
	cpx nAct
	bcc +
	rts
+	lda acKind,x
	cmp #K_MON0
	bcc csNext
	cmp #K_MON_LAST+1
	bcs csNext
	lda acState,x
	cmp #AS_DEAD
	beq csNext
	stx cbX
	jsr acDelta
	ldy #0
	jsr ddNear
	bcs csSkip
	ldy #6
	jsr ddNear
	bcs csSkip
	jsr csFwdLat
	bcc csSkip
	jsr csInsert
csSkip
	ldx cbX
csNext
	inx
	bne csLoop
	rts

; cbF = fwd, cbL = lat, cbA = |lat|. C set if in range and in the cone.
csFwdLat
	lda dd+0
	sta mA
	lda dd+1
	sta mA+1
	lda cbSin
	sta mS
	lda cbSin+1
	sta mS+1
	jsr mulTrig
	lda mR
	sta cbF
	lda mR+1
	sta cbF+1
	lda dd+6
	sta mA
	lda dd+7
	sta mA+1
	lda cbCos
	sta mS
	lda cbCos+1
	sta mS+1
	jsr mulTrig
	clc
	lda cbF
	adc mR
	sta cbF
	lda cbF+1
	adc mR+1
	sta cbF+1
	bpl lb01			; (long branch)
	jmp cflNo	; behind
lb01
	cmp cbMaxF
	bcc lb02			; (long branch)
	jmp cflNo	; too far
lb02
	ora cbF
	bne lb03			; (long branch)
	jmp cflNo
lb03

	lda dd+0
	sta mA
	lda dd+1
	sta mA+1
	lda cbCos
	sta mS
	lda cbCos+1
	sta mS+1
	jsr mulTrig
	lda mR
	sta cbL
	lda mR+1
	sta cbL+1
	lda dd+6
	sta mA
	lda dd+7
	sta mA+1
	lda cbSin
	sta mS
	lda cbSin+1
	sta mS+1
	jsr mulTrig
	sec
	lda cbL
	sbc mR
	sta cbL
	sta cbA
	lda cbL+1
	sbc mR+1
	sta cbL+1
	sta cbA+1
	bpl +
	sec
	lda #0
	sbc cbA
	sta cbA
	lda #0
	sbc cbA+1
	sta cbA+1
+
	; The limit: the axe's box, or the cone (fwd / 4) plus a box.
	lda cbAxe
	beq +
	lda #<AXE_HALF
	sta cbT
	lda #>AXE_HALF
	sta cbT+1
	jmp ++
+	lda cbF+1
	lsr a
	sta cbT+1
	lda cbF
	ror a
	lsr cbT+1
	ror a
	clc
	adc #PEL_HALF
	sta cbT
	bcc +
	inc cbT+1
+	jsr cbBigLim			; a large monster: wider
	lda cbA+1			; |lat| < limit?
	cmp cbT+1
	bcc cflYes
	bne cflNo
	lda cbA
	cmp cbT
	bcc cflYes
cflNo
	clc
	rts
cflYes
	sec
	rts

; Into the candidate list, sorted by fwd; the farthest falls off.
csInsert
	ldy #0
ciFind
	cpy cN
	beq ciAt
	lda cbF+1
	cmp cFwdH,y
	bcc ciAt
	bne +
	lda cbF
	cmp cFwdL,y
	bcc ciAt
+	iny
	bne ciFind
ciAt
	cpy #CB_NCAND
	bcs ciDone
	sty cbT
	ldx cN
	cpx #CB_NCAND
	bcc +
	dex
	bcs ciShift
+	inc cN
ciShift
	cpx cbT
	beq ciPut
	lda cSlot-1,x
	sta cSlot,x
	lda cFwdL-1,x
	sta cFwdL,x
	lda cFwdH-1,x
	sta cFwdH,x
	lda cLatL-1,x
	sta cLatL,x
	lda cLatH-1,x
	sta cLatH,x
	dex
	bne ciShift
ciPut
	lda cbX
	sta cSlot,x
	lda cbF
	sta cFwdL,x
	lda cbF+1
	sta cFwdH,x
	lda cbL
	sta cLatL,x
	lda cbL+1
	sta cLatH,x
ciDone
	rts

; ======================================================================
; worldTick's half: the trace records, eye to each candidate's origin, and
; the movers they are traced against (the ones clipMove selected this
; frame, so a shut door stops a shot exactly where it stops the player).
; Called with DST at the first free input byte; leaves it past them.
; ======================================================================
wtTraces
	ldx #0
wttLoop
	cpx wtTrN
	beq wttAi
	stx cbT
	ldy #0
-	lda cam,y
	sta (DST),y
	iny
	cpy #12
	bne -
	lda cSlot,x
	tax
	.for k = 0, k < 12, k += 1
	lda acPos+k*MAX_ACT,x
	sta (DST),y
	iny
	.next
	lda #0				; hull 0: a point
	sta (DST),y
	iny
	sta (DST),y
	iny
	sta (DST),y
	iny
	sta (DST),y
	jsr dstAdd28
	ldx cbT
	inx
	bne wttLoop
wttAi
	jsr aiEmit			; the monsters' traces, then their moves
wttMovers
	lda selCnt
	sta wtMvN
	beq wttDone
	asl a
	asl a
	asl a
	asl a
	sta cbT				; selCnt * 16 <= 256 ...
	ldy #0
-	lda clipBlk+CLIP_MVOFF,y
	sta (DST),y
	iny
	cpy cbT
	bne -				; ... and 16 * 16 = 256 wraps to 0: all of it
	clc
	lda DST
	adc cbT
	sta DST
	bcc +
	inc DST+1
+	lda cbT
	bne wttDone
	inc DST+1
wttDone
	rts

dstAdd28
	clc
	lda DST
	adc #28
	sta DST
	bcc +
	inc DST+1
+	rts

; ======================================================================
; After actFrame: the pellets, against the candidates the Pi can see.
; ======================================================================
combatResolve
	lda shotPend
	bne +
	rts
+	lda #0
	sta shotPend
	lda wtTrOk
	bne +
	inc cbLost			; the block never verified: the shot is lost
	bne crRts
	dec cbLost
crRts
	rts
+
	; Visibility from the answers: TR_CLEAR at OUT + 8 + 16i + 14.
	ldx #0
	ldy #8+14
-	lda (DST),y
	and #1
	sta cVis,x
	lda #0
	sta cDmg,x
	tya
	clc
	adc #16
	tay
	inx
	cpx cN
	bne -

	lda cbPel
	sta cbPi
crPellet
	; Uniform spread, as crandom() * spread: (rnd - 128) * 2s / 256.
	jsr rnd
	sec
	sbc #128
	jsr crSpread
	lda cbSx2
	sta mS
	lda #0
	sta mS+1
	jsr mulTrig
	lda mR
	sta cbSx
	lda mR+1
	sta cbSx+1
	jsr rnd
	sec
	sbc #128
	jsr crSpread
	lda cbSy2
	sta mS
	lda #0
	sta mS+1
	jsr mulTrig
	lda mR
	sta cbSy
	lda mR+1
	sta cbSy+1

	ldy #0
crCand
	cpy cN
	bne lb04			; (long branch)
	jmp crNextPel	; flew past everything
lb04
	sty cbCi
	lda cVis,y
	bne lb20			; (long branch)
	jmp crNextCand
lb20
	; Lateral: lat - fwd * sx within the box's half-width.
	lda cFwdL,y
	sta mA
	lda cFwdH,y
	sta mA+1
	lda cbSx
	sta mS
	lda cbSx+1
	sta mS+1
	jsr mulTrig
	ldy cbCi
	sec
	lda cLatL,y
	sbc mR
	sta cbT
	lda cLatH,y
	sbc mR+1
	sta cbT+1
	jsr crBig			; a large monster: half the miss
	clc
	lda cbT
	adc #PEL_HALF
	lda cbT+1
	adc #0
	bne crNextCand			; |d| > PEL_HALF
	; Vertical: fwd * sy between -PEL_LOW and PEL_SPAN - PEL_LOW.
	lda cFwdL,y
	sta mA
	lda cFwdH,y
	sta mA+1
	lda cbSy
	sta mS
	lda cbSy+1
	sta mS+1
	jsr mulTrig
	clc
	lda mR
	adc #<PEL_LOW
	lda mR+1
	adc #>PEL_LOW
	cmp cbSpanH			; under PEL_SPAN (+256 large): inside
	bcs crNextCand
	ldy cbCi
	lda cDmg,y
	clc
	adc cbDmg
	bcc +
	lda #$ff
+	sta cDmg,y
	jmp crNextPel
crNextCand
	ldy cbCi
	iny
	beq lb05			; (long branch)
	jmp crCand
lb05
crNextPel
	dec cbPi
	beq lb06			; (long branch)
	jmp crPellet
lb06

	; Quake's multidamage: each monster takes its pellets as one hit.
	ldy #0
crApply
	cpy cN
	beq crDone
	sty cbCi
	lda cDmg,y
	beq +
	ldx cSlot,y
	jsr quadDmg
	jsr monHurt
	inc cbHits
	bne +
	dec cbHits
+	ldy cbCi
	iny
	bne crApply
crDone
	rts

; A = a signed byte, into mA sign-extended.
crSpread
	sta mA
	and #$80
	beq +
	lda #$ff
+	sta mA+1
	rts

; ======================================================================
; Monsters taking damage. X = slot, A = damage.
; ======================================================================
monHurt
	sta cbHd
	lda acState,x
	cmp #AS_DEAD
	beq mhRts			; a corpse
	jsr aiHurt			; awake, and after the player
	lda acKind,x
	cmp #K_ZOMBIE
	beq mhZombie
	lda acHp,x
	sec
	sbc cbHd
	sta cbT
	lda acHpH,x
	sbc #0
	bcc mhDie
	sta acHpH,x
	ora cbT
	beq mhDie
	lda cbT
	sta acHp,x
	lda acState,x
	cmp #AS_PAIN
	beq mhRts			; already flinching
	jsr mtType
	bcs mhRts
	jsr rnd				; preserves X and Y
	cmp mbPain,y
	bcs mhRts			; shrugged off
	lda #AS_PAIN
	sta acState,x
	lda #SFX_MPAIN
	jsr sfxPlay
	lda #AM_ONCE
	sta cbMode
	lda #MA_PAIN
	jmp mtAnim
mhRts
	rts

; Quake's zombie keeps its health: only ZB_KILL at once puts it down for
; good (Quake's 60, a rocket; here the super shotgun's 56 point blank, as
; there are no explosives). ZB_DOWN or more knocks it flat, and it gets
; up again; less, a flinch.
mhZombie
	lda cbHd
	cmp #ZB_KILL
	bcs mhDie
	lda acState,x
	cmp #AS_PAIN
	beq mhRts			; down already, or flinching
	lda #AS_PAIN
	sta acState,x
	lda #SFX_MPAIN
	jsr sfxPlay
	lda #AM_ONCE
	sta cbMode
	lda cbHd
	cmp #ZB_DOWN
	lda #MA_PAIN
	bcc +
	lda #MA_DEATHB			; its DEATHB is the whole fall and rise
+	jmp mtAnim

mhDie
	lda #0
	sta acHp,x
	sta acHpH,x
	lda #AS_DEAD
	sta acState,x
	inc kills
	lda #SFX_MDIE
	jsr sfxPlay
	jsr mtType			; a scrag's body falls (aiFallReq)
	lda mbFly,y
	beq +
	lda #FALL_N
+	sta acLeapT,x
	lda #AM_HOLD
	sta cbMode
	lda acKind,x
	cmp #K_ZOMBIE			; its DEATHB gets up again
	beq +
	jsr rnd
	and #1
	.byte $2c			; bit abs: over the lda
+	lda #0
	.cerror MA_DEATHB != MA_DEATH + 1
	ora #MA_DEATH
	jsr mtAnim
	lda acKind,x
	cmp #K_GRUNT
	bne +
	jmp dropPack			; grunts carry shells
+	rts

; X = slot, A = first mesh (level index), Y = frames, cbMode = acAnMode.
; Preserves X.
acAnim
	sta acAnB,x
	tya
	sta acAnN,x
	lda cbMode
	sta acAnMode,x
	lda #0
	sta acAnF,x
	lda acAnB,x
	clc
	adc #<MESH_BASE
	sta acResL,x
	lda #>MESH_BASE
	adc #0
	sta acResH,x
	lda #1
	sta acDirty,x
	rts

; X = slot: back to the kind's standing loop. Preserves X.
acStand
	lda #AS_IDLE
	sta acState,x
	lda #AM_LOOP
	sta cbMode
	lda #MA_STAND
	jmp mtAnim

; X = the dead grunt. A backpack where it fell: a freed pack slot if there
; is one (its node still exists), else a new slot, whose node the frame
; loop's one-a-frame repair creates -- exactly as it re-creates a refusal.
dropPack
	stx cbX
	ldy #SL_WORLD
-	cpy nAct
	bcs dpNew
	lda acKind,y
	beq dpHave
	iny
	bne -
dpNew
	cpy #MAX_ACT
	bcc lb07			; (long branch)
	jmp dpNone
lb07
	inc nAct
	lda #1
	sta acFix,y
dpHave
	tya
	tax				; X = the pack, cbX = the grunt
	ldy cbX
	lda #K_PACK
	sta acKind,x
	lda #M_BACKPACK
	sta acAnB,x
	clc
	adc #<MESH_BASE
	sta acResL,x
	lda #>MESH_BASE
	adc #0
	sta acResH,x
	lda #0
	sta acType,x
	sta acAnN,x
	sta acAnF,x
	sta acAnMode,x
	sta acRot,x
	lda #AF_VIS
	sta acFlags,x
	lda #1
	sta acDirty,x
	lda acYaw,y
	sta acYaw,x
	.for k = 0, k < 12, k += 1
	lda acPos+k*MAX_ACT,y
	sta acPos+k*MAX_ACT,x
	.next
	sec				; Quake drops it at origin - 24: 0.75 wu
	lda acPos+5*MAX_ACT,x
	sbc #$c0
	sta acPos+5*MAX_ACT,x
	lda acPos+6*MAX_ACT,x
	sbc #0
	sta acPos+6*MAX_ACT,x
	lda acPos+7*MAX_ACT,x
	sbc #0
	sta acPos+7*MAX_ACT,x
dpNone
	ldx cbX
	rts

; ======================================================================
; Pickups: every visible item the player's box overlaps, after clipMove.
; Every other frame, opposite gameTouch: the touch box is 32 QU across and
; two frames of running are 21 QU.
; ======================================================================
combatTouch
	lda wtFrame
	and #1
	bne +
	lda plDead
	beq ++
+	rts
+	ldx #SL_WORLD
ctLoop
	stx cbX
	cpx nAct
	bcc +
	rts
+	lda acFlags,x
	and #AF_VIS
	bne lb08			; (long branch)
	jmp ctNext
lb08
	lda acKind,x
	cmp #K_PACK
	beq ctItem
	cmp #K_HEALTH
	bcs lb09			; (long branch)
	jmp ctNext
lb09
	cmp #K_NAILS+1
	bcc lb10			; (long branch)
	jmp ctNext
lb10
ctItem
	; A cheap necessary test first: a touch needs |dx| and |dz| under
	; 1.5 units (the box plus a corner item's offset), so the whole-unit
	; bytes differ by at most 2. The full 24-bit test below cost 10 ms a
	; frame over E1M1's 48 actors; this rejects nearly all of them.
	lda acPos+2*MAX_ACT,x
	sec
	sbc cam+2
	clc
	adc #2
	cmp #5
	bcs ctFar
	lda acPos+10*MAX_ACT,x
	sec
	sbc cam+10
	clc
	adc #2
	cmp #5
	bcc ctNear
ctFar
	jmp ctNext
ctNear
	jsr acDelta
	; Health and ammo boxes have their origin at a corner: box 0..32 QU,
	; so their middle is 16 QU along Quake's x and y, i.e. gpu64 +z and -x.
	ldx cbX
	lda acKind,x
	cmp #K_HEALTH
	beq ctCorner
	cmp #K_SHELLS
	beq ctCorner
	cmp #K_NAILS
	bne ctBox
ctCorner
	sec
	lda dd+0
	sbc #128
	sta dd+0
	lda dd+1
	sbc #0
	sta dd+1
	lda dd+2
	sbc #0
	sta dd+2
	clc
	lda dd+6
	adc #128
	sta dd+6
	lda dd+7
	adc #0
	sta dd+7
	lda dd+8
	adc #0
	sta dd+8
ctBox
	; |dx|, |dz| < 32 QU: the two 16 QU half-widths.
	ldy #0
	jsr ddWithin
	bcs ctNext
	ldy #6
	jsr ddWithin
	bcs ctNext
	; The item's 56 QU above its origin against the player's 46 below
	; and 10 above the eye: origin - eye in (-102, 10) QU.
	clc
	lda dd+3
	adc #<816
	sta cbT
	lda dd+4
	adc #>816
	sta cbT+1
	lda dd+5
	adc #0
	bne ctNext
	lda cbT+1
	cmp #>896
	bcc +
	bne ctNext
	lda cbT
	cmp #<896
	bcs ctNext
+	ldx cbX
	jsr pickup
	bcc ctNext
	ldx cbX
	jsr pickSfx
	lda #0
	sta acFlags,x
	sta acRot,x
	lda #1
	sta acDirty,x
	inc cbPicks
	bne ctNext
	dec cbPicks
ctNext
	ldx cbX
	inx
	beq lb11			; (long branch)
	jmp ctLoop
lb11
	rts

; Y = axis offset in dd. C clear if the s24 there is in (-256, 256).
ddWithin
	lda dd+2,y
	beq +
	cmp #$ff
	bne dwNo
	lda dd+1,y
	cmp #$ff
	bne dwNo
	clc
	rts
+	lda dd+1,y
	bne dwNo
	clc
	rts
dwNo
	sec
	rts

; X = item slot. Quake's touch rules, simplified. C set if taken.
pickup
	lda acKind,x
	cmp #K_HEALTH
	bne puNotHealth
	lda acAnB,x
	cmp #M_BH100
	beq puMega
	ldy #25
	cmp #M_BH25
	beq +
	ldy #15
+	lda plHp
	cmp #100
	bcs puNo			; full: left for later
	sty cbT
	adc cbT
	cmp #100
	bcc +
	lda #100
+	sta plHp
	sec
	rts
puMega
	lda plHp
	clc
	adc #100
	bcs +
	cmp #250
	bcc ++
+	lda #250
+	sta plHp
	sec
	rts
puNo
	clc
	rts

puNotHealth
	cmp #K_ARM1
	bcc puNo
	cmp #K_ARM3+1
	bcs puNotArmour
	; Taken only if it is worth more: type * value against the worn one's.
	sec
	sbc #K_ARM1
	tay
	iny				; plArmT: 1 green, 2 yellow, 3 red
	sty cbT
	ldx plArmT
	lda armK,x
	ldy plArm
	jsr mul8			; worn: k * value, 16 bits
	ldx cbT
	lda armWorthH,x
	cmp m8H
	bcc puNo
	bne +
	lda armWorthL,x
	cmp m8L
	bcc puNo
	beq puNo
+	stx plArmT
	lda armValue,x
	sta plArm
	sec
	rts

puNotArmour
	cmp #K_SSG
	bne +
	lda #1 << W_SSG
	ldy #5
	jmp puGunShells
+	cmp #K_NAILGUN
	bne +
	lda #1 << W_NAIL
	ora plWeaps
	sta plWeaps
	lda #30
	jsr addNails
	lda #W_NAIL
	jsr setWeap
	sec
	rts
+	cmp #K_QUAD
	bne +
	lda #<QUAD_FRAMES
	sta plQuad
	lda #>QUAD_FRAMES
	sta plQuad+1
	sec
	rts
+	cmp #K_SUIT
	bne +
	lda #<SUIT_MS			; slime, lava and drowning: see
	sta plSuit			; gpu64_game_env.inc
	lda #>SUIT_MS
	sta plSuit+1
	sec
	rts
+	cmp #K_SHELLS
	bne +
	lda plShells
	cmp #100
	bcc lb12			; (long branch)
	jmp puNo
lb12
	ldy #20
	lda acAnB,x
	cmp #M_SHELL1
	bne puShells
	ldy #40
	bne puShells
+	cmp #K_NAILS
	bne +
	lda plNails
	cmp #200
	bcc lb13			; (long branch)
	jmp puNo
lb13
	lda #25
	ldy acAnB,x
	cpy #M_NAIL1
	bne ++
	lda #50
	bne ++
+	cmp #K_PACK
	bne puNo2
	ldy #5
	jmp puShells
+	jsr addNails
	sec
	rts
puNo2
	clc
	rts

puGunShells
	ora plWeaps
	sta plWeaps
	tya
	jsr addShells
	lda #W_SSG
	jsr setWeap
	sec
	rts
puShells
	tya
	jsr addShells
	sec
	rts

addShells
	clc
	adc plShells
	cmp #100
	bcc +
	lda #100
+	sta plShells
	rts

addNails
	clc
	adc plNails
	bcs +
	cmp #200
	bcc ++
+	lda #200
+	sta plNails
	rts

; Armour, by plArmT (0 none, 1 green, 2 yellow, 3 red): the fraction it
; absorbs in 1/256, its value when picked up, and type * value, the worth
; Quake compares.
armK		.byte 0, 77, 154, 205
armValue	.byte 0, 100, 150, 200
armWorthL	.byte 0, <(77*100), <(154*150), <(205*200)
armWorthH	.byte 0, >(77*100), >(154*150), >(205*200)

; ======================================================================
; The health cheat: a megahealth's +100, at most 250, with its sound and a
; line on the C64's message row.
; ======================================================================
plCheat
	lda plHp
	clc
	adc #100
	bcs +
	cmp #250
	bcc ++
+	lda #250
+	sta plHp
	jsr msgRow
	jsr dmText
	.text "          CHEAT: HEALTH"
	.byte 0
	lda #SFX_HEALTH
	jmp sfxPlay

; ======================================================================
; The player taking damage. A = damage. Quake's T_Damage for a client:
; the armour saves ceil(type * damage), at most what it has left.
; ======================================================================
plHurt
	ldx plDead
	bne phRts
	sta cbHd
	inc cbHurts
	bne +
	dec cbHurts
+	ldx plArmT
	beq phTake
	lda armK,x
	ldy cbHd
	jsr mul8
	lda m8L				; ceil(k * damage / 256)
	beq +
	inc m8H
+	lda m8H
	cmp plArm
	bcc +
	lda plArm			; the last of it
	ldx #0
	stx plArmT
+	sta cbT
	lda plArm
	sec
	sbc cbT
	sta plArm
	lda cbHd
	sec
	sbc cbT
	sta cbHd
phTake
	lda #PAIN_FACE
	sta plPain
	lda plHp
	sec
	sbc cbHd
	bcc phDie
	beq phDie
	sta plHp
	lda #SFX_PAIN
	jmp sfxPlay
phRts
	rts
phDie
	lda #SFX_DIE
	jsr sfxPlay
	lda #0
	sta plHp
	lda #1
	sta plDead
	inc cbDeaths
	lda #AF_VIEW			; the gun goes
	sta acFlags+SL_GUN
	lda #1
	sta acDirty+SL_GUN
	rts

; The pickup's sound, X = its slot. X and Y survive.
pickSfx
	lda acKind,x
	cmp #K_HEALTH
	beq psHealth
	cmp #K_SSG
	beq psWeapon
	cmp #K_NAILGUN
	beq psWeapon
	cmp #K_QUAD
	beq psPower
	cmp #K_SUIT
	beq psPower
	lda #SFX_ITEM
	jmp sfxPlay
psHealth
	lda #SFX_HEALTH
	jmp sfxPlay
psWeapon
	lda #SFX_WEAPON
	jmp sfxPlay
psPower
	lda #SFX_POWER
	jmp sfxPlay

; runsim.py --warp: a new start, then a restart onto it. Never on a C64.
simWarp
	lda SIM_WARP
	cmp #SIM_HOP_ON
	beq swHop
	cmp #SIM_WARP_ON
	bne +
	lda #0
	sta SIM_WARP
	ldx #11
-	lda SIM_WARP_POS,x
	sta startCam,x
	dex
	bpl -
	lda SIM_WARP_POS+12
	sta startYaw
	lda SIM_WARP_POS+13
	sta startYaw+1
	jmp restart
+	rts
; --hop: the eye alone moves, the level and its counters keep their state,
; so one run can press buttons that are rooms apart (tools/check_exit.py).
swHop
	lda #0
	sta SIM_WARP
	ldx #11
-	lda SIM_WARP_POS,x
	sta cam,x
	sta lastGood,x
	dex
	bpl -
	lda SIM_WARP_POS+12
	sta camYaw
	lda SIM_WARP_POS+13
	sta camYaw+1
	rts

; ======================================================================
; Restart: the level as it was loaded. Nodes are not re-created -- every
; slot is re-posed from its own spawn state, which for a monster that never
; moved (stage D) is where it already stands.
; ======================================================================
restart
	lda #$ff			; the key that restarts does not also
	sta fireHold			; fire: it has to be let go first
	ldx #11
-	lda startCam,x
	sta cam,x
	sta lastGood,x
	dex
	bpl -
	lda startYaw
	sta camYaw
	lda startYaw+1
	sta camYaw+1
	lda #0
	sta camPitch
	sta onGround
	sta jumping
	sta airFrames
	sta stuckRun
	sta fireN
	lda #<VEL_MIN
	sta velY
	lda #>VEL_MIN
	sta velY+1

	ldx #0
-	cpx nTrg
	bcs +
	lda #0
	sta trFired,x
	inx
	bne -
+
	lda #0
	sta cbX
rsMov
	ldx cbX
	cpx nMov
	bcs rsMovDone
	lda #MV_CLOSED
	sta mvState,x
	lda #0
	sta mvFrac,x
	sta mvWait,x
	jsr mulOfs
	ldx cbX
	lda #1
	sta mvDirty,x
	inc cbX
	bne rsMov
rsMovDone

	ldx #SL_WORLD
rsAct
	cpx nAct
	bcs rsActDone
	lda acKind,x
	cmp #K_PACK
	bne +
	lda #0				; freed for the next grunt to drop
	sta acKind,x
	sta acFlags,x
	beq rsDirty
+	cmp #K_MON0
	bcc rsItem
	cmp #K_MON_LAST+1
	bcs rsItem
	jsr monSpawn
	jmp rsDirty
rsItem
	cmp #K_HEALTH			; kind 0: a freed pack slot, stays hidden
	bcc rsDirty
	lda #AF_VIS
	sta acFlags,x
	lda acKind,x
	cmp #K_ARM1			; weapons, armour and powerups turn
	bcc rsDirty
	cmp #K_SHELLS
	bcs rsDirty
	lda #1
	sta acRot,x
rsDirty
	lda #1
	sta acDirty,x
	inx
	bne rsAct
rsActDone
	lda #0
	sta kills
	jsr pjClear			; nothing left in the air
	jsr exReset			; secrets, time, counters, the exit
	jmp plReset

; X = monster slot: alive, standing, at full health. Preserves X.
monSpawn
	jsr mtHealth
	lda #AF_VIS
	sta acFlags,x
	jsr aiRespawn			; asleep, where the level put it
	jmp acStand

; ======================================================================
; The view model and the status bar.
; ======================================================================
; The gun's mesh: the weapon's first frame plus gunF.
gunPose
	ldx plWeap
	lda wpGun,x
	clc
	adc gunF
	clc
	adc #<MESH_BASE
	sta cbT
	lda #>MESH_BASE
	adc #0
	sta cbT+1
	lda cbT
	cmp acResL+SL_GUN
	bne +
	lda cbT+1
	cmp acResH+SL_GUN
	beq ++
+	lda cbT
	sta acResL+SL_GUN
	lda cbT+1
	sta acResH+SL_GUN
	lda #1
	sta acDirty+SL_GUN
+	rts

; A frame of the fire animation, then back to the idle frame 0.
gunStep
	lda gunF
	beq +
	inc gunF
	ldx plWeap
	lda gunF
	cmp wpGunN,x
	bcc +
	lda #0
	sta gunF
+	jmp gunPose

hudStats
	; Health, red at 25 and under -- Quake's rule, and armour's too.
	lda plHp
	ldy #T_NUM_0
	cmp #26
	bcs +
	ldy #T_ANUM_0
+	ldx #SL_HPD
	jsr hudNum
	lda plArm
	ldy #T_NUM_0
	cmp #26
	bcs +
	ldy #T_ANUM_0
+	ldx #SL_ARMD
	jsr hudNum
	ldx plArmT
	lda armIcon,x
	ldx #SL_ARMI
	jsr hudIcon

	; Ammo for the weapon in hand, red at 10 and under; the axe has none.
	lda plWeap
	jsr ammoOf
	ldy #T_NUM_0
	cmp #11
	bcs +
	ldy #T_ANUM_0
+	ldx #SL_AMMD
	jsr hudNum
	ldx plWeap
	lda ammoIcon,x
	ldx #SL_AMMI
	jsr hudIcon

	; The face: health in fifths, flinching after a hit, the quad's.
	lda plHp
	ldx #0
-	cmp #20
	bcc +
	sbc #20
	inx
	cpx #4
	bne -
+	stx cbT				; 0 (dying) .. 4 (healthy)
	lda plQuad
	ora plQuad+1
	beq +
	lda #T_FACE_QUAD
	bne hsFace
+	lda plPain
	beq +
	lda #T_FACE_P5
	bne ++
+	lda #T_FACE5
+	sec
	sbc cbT
hsFace
	ldx #SL_FACE
	; fall through

; X = slot, A = texture, 0 = hidden. Dirties the slot only on a change.
hudIcon
	cmp #0
	beq hiHide
	cmp acResL,x
	beq +
	sta acResL,x
	lda #1
	sta acDirty,x
+	lda #AF_VIS | AF_VIEW
	bne +
hiHide
	lda #AF_VIEW
+	cmp acFlags,x
	beq +
	sta acFlags,x
	lda #1
	sta acDirty,x
+	rts

armIcon		.byte 0, T_SB_ARMOR1, T_SB_ARMOR2, T_SB_ARMOR2	; no red icon in the set
ammoIcon	.byte 0, T_SB_SHELLS, T_SB_SHELLS, T_SB_NAILS

; ======================================================================
; Arithmetic.
; ======================================================================
; X = slot. dd = acPos - cam per axis, bytes 1-3 of the 16.16 difference:
; fine units, signed 24-bit, x at dd+0, y at dd+3, z at dd+6. Preserves X.
acDelta
	.for ax = 0, ax < 3, ax += 1
	sec
	lda acPos+(ax*4+0)*MAX_ACT,x
	sbc cam+ax*4+0
	lda acPos+(ax*4+1)*MAX_ACT,x
	sbc cam+ax*4+1
	sta dd+ax*3+0
	lda acPos+(ax*4+2)*MAX_ACT,x
	sbc cam+ax*4+2
	sta dd+ax*3+1
	lda acPos+(ax*4+3)*MAX_ACT,x
	sbc cam+ax*4+3
	sta dd+ax*3+2
	.next
	rts

; Y = axis offset in dd. C clear if |value| < CB_RANGE * 256, which is
; then an s16 in dd+Y, dd+Y+1.
ddNear
	lda dd+2,y
	beq dnPos
	cmp #$ff
	bne dnFar
	lda dd+1,y
	cmp #$100 - CB_RANGE
	bcc dnFar
	clc
	rts
dnPos
	lda dd+1,y
	cmp #CB_RANGE
	bcs dnFar
	clc
	rts
dnFar
	sec
	rts

; mR = mA * mS / 256, all signed 16-bit, |mS| <= 256 (an 8.8 sine, or a
; spread). Destroys mA and mS.
mulTrig
	lda mA+1
	eor mS+1
	sta mSgn
	lda mA+1
	bpl +
	sec
	lda #0
	sbc mA
	sta mA
	lda #0
	sbc mA+1
	sta mA+1
+	lda mS+1
	bpl +
	sec
	lda #0
	sbc mS
	sta mS
	lda #0
	sbc mS+1
	sta mS+1
+	lda mS+1
	beq mtByte
	lda mA				; |mS| = 256: mR = |mA|
	sta mR
	lda mA+1
	sta mR+1
	jmp mtSign
mtByte
	lda #0
	sta mP0
	sta mR
	sta mR+1
	ldx #8
mtLoop
	asl mP0
	rol mR
	rol mR+1
	asl mS
	bcc +
	clc
	lda mP0
	adc mA
	sta mP0
	lda mR
	adc mA+1
	sta mR
	bcc +
	inc mR+1
+	dex
	bne mtLoop
mtSign
	lda mSgn
	bpl +
	sec
	lda #0
	sbc mR
	sta mR
	lda #0
	sbc mR+1
	sta mR+1
+	rts

; m8H:m8L = A * Y, unsigned.
mul8
	sta m8A
	sty m8L
	lda #0
	ldx #8
	lsr m8L
-	bcc +
	clc
	adc m8A
+	ror a
	ror m8L
	dex
	bne -
	sta m8H
	rts

; A = a pseudo-random byte (John Metcalf's 16-bit xorshift).
rnd
	lda rndHi
	lsr a
	lda rndLo
	ror a
	eor rndHi
	sta rndHi
	ror a
	eor rndLo
	sta rndLo
	eor rndHi
	sta rndHi
	rts

; ----------------------------------------------------------------------
; Rows 21 and 22: the player, then what the fighting did. SHOT and HIT
; count trigger pulls and monsters damaged; LOST is shots whose block never
; verified, so the traces were never answered.
; ----------------------------------------------------------------------
showCombat
	lda #21
	jsr dmRow
	jsr dmText
	.text "HP"
	.byte 0
	lda plHp
	jsr dmDecA
	jsr dmText
	.text " AR"
	.byte 0
	lda plArm
	jsr dmDecA
	jsr dmText
	.text " SH"
	.byte 0
	lda plShells
	jsr dmDecA
	jsr dmText
	.text " NL"
	.byte 0
	lda plNails
	jsr dmDecA
	jsr dmText
	.text " W"
	.byte 0
	lda plWeap
	clc
	adc #1
	jsr dmDigit
	jsr dmText
	.text " K"
	.byte 0
	lda kills
	jsr dmDecA
	jsr dmText
	.text "/"
	.byte 0
	lda kTotal
	jsr dmDecA

	lda #22
	jsr dmRow
	jsr dmText
	.text "SHOT"
	.byte 0
	lda cbShots
	jsr dmDecA
	jsr dmText
	.text " HIT"
	.byte 0
	lda cbHits
	jsr dmDecA
	jsr dmText
	.text " LOST"
	.byte 0
	lda cbLost
	jsr dmDecA
	jsr dmText
	.text " PICK"
	.byte 0
	lda cbPicks
	jsr dmDecA
	jsr dmText
	.text " DIE"
	.byte 0
	lda cbDeaths
	jmp dmDecA

dmDigit
	jmp dmDecDigit

; ----------------------------------------------------------------------
; State.
; ----------------------------------------------------------------------
plHp		.byte 0
plArm		.byte 0
plArmT		.byte 0
plShells	.byte 0
plNails		.byte 0
plWeaps		.byte 0
plWeap		.byte 0
plDead		.byte 0
plDrop		.byte 0
plPain		.byte 0
plQuad		.word 0
startCam	.fill 12, 0
startYaw	.word 0
fireHold	.byte 0
fireEdge	.byte 0
fireWait	.byte 0
hurtHold	.byte 0
wkHold		.fill 4, 0
gunF		.byte 0
joy2		.byte $ff
kills		.byte 0
kTotal		.byte 0
cbShots		.byte 0
cbHits		.byte 0
cbLost		.byte 0
cbPicks		.byte 0
cbHurts		.byte 0
cbDeaths	.byte 0
shotPend	.byte 0
cbPel		.byte 0
cbPi		.byte 0
cbDmg		.byte 0
cbHd		.byte 0
cbSx2		.byte 0
cbSy2		.byte 0
cbSx		.word 0
cbSy		.word 0
cbMaxF		.byte 0
cbAxe		.byte 0
cbMode		.byte 0
cbX		.byte 0
cbCi		.byte 0
cbT		.word 0
cbSin		.word 0
cbCos		.word 0
cbF		.word 0
cbL		.word 0
cbA		.word 0
cN		.byte 0
cSlot		.fill CB_NCAND, 0
cFwdL		.fill CB_NCAND, 0
cFwdH		.fill CB_NCAND, 0
cLatL		.fill CB_NCAND, 0
cLatH		.fill CB_NCAND, 0
cVis		.fill CB_NCAND, 0
cDmg		.fill CB_NCAND, 0
dd		.fill 9, 0
mA		.word 0
mS		.word 0
mR		.word 0
mP0		.byte 0
mSgn		.byte 0
m8A		.byte 0
m8L		.byte 0
m8H		.byte 0
rndLo		.byte 0
rndHi		.byte 0
acHp		.fill MAX_ACT, 0
acState		.fill MAX_ACT, 0
acAnMode	.fill MAX_ACT, 0

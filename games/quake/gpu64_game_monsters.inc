; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_monsters.inc -- the monster types, for gpu64_demo_game.a.
; Under BASIC with the trigger tables: only the level scan and the frame
; loop use it, both inside the BASIC-out span.
;
; A monster's TYPE is its kind - K_MON0: grunt, dog, ogre, knight, zombie,
; scrag, fiend, shambler. What each type DOES is compiled in below (mb*,
; per type); what it LOOKS like comes from the level. A level carries only
; the monsters its map places (tools/quake_assets.py, build_bank), so their
; frames sit at mesh indices that vary by level, and each arrives as one
; gpu64_bank entity, scanned before any monster:
;
;   p0         the monster kind
;   origin     payload 0..11, travel 12..23:
;   0..15      per animation slot (MA_*): first mesh index, frame count
;   16, 17     the MELEE and SHOOT frames that deal the damage
;   18         the frame step: 2 = every other frame kept, played at half
;              rate so an animation lasts as long as Quake's
;
; A type with no bank has no STAND frames, and actTake does not place it --
; any type whose bank record the bus lost three times over.
;
; Which attacks a type has is which slots its bank fills: MELEE (the dog's
; bite, the ogre's chainsaw, the knight's sword, the fiend's claws, the
; shambler's smash), LEAP (dog, fiend) and SHOOT (the grunt's volley and
; the shambler's lightning, a volley too; the ogre's grenade, the zombie's
; gib and the scrag's spikes, missiles).
; ======================================================================

K_MON0    = 51			; monster kinds K_MON0..K_MON_LAST
MT_N      = 8
K_MON_LAST = K_MON0 + MT_N - 1
K_BANK    = 18			; quake_assets.BANK_KIND

MA_STAND  = 0			; animation slots, as quake_assets.BANK_SLOTS
MA_RUN    = 1
MA_MELEE  = 2
MA_LEAP   = 3
MA_SHOOT  = 4
MA_PAIN   = 5
MA_DEATH  = 6
MA_DEATHB = 7

; ----------------------------------------------------------------------
; Per type: grunt, dog, ogre, knight, zombie, scrag, fiend, shambler.
; Distances are fine (8 per QU), steps fine per DT_REF frame (aiMoveReq
; doubles them), as gpu64_game_ai.inc's GR_* and DOG_*.
; ----------------------------------------------------------------------
mbHpL		.byte <30, <25, <200, <75, <60, <80, <300, <600
mbHpH		.byte >30, >25, >200, >75, >60, >80, >300, >600
; Quake's run speeds: soldier ~110 QU/s, dog ~200, ogre ~135, knight ~140,
; fiend ~240, shambler ~210.
mbStep		.byte GR_STEP, DOG_STEP, 48, 52, 32, 40, 86, 76
mbStopL		.byte <GR_STOP_F, <DOG_STOP_F, <480, <400, <400, <512, <400, <560
mbStopH		.byte >GR_STOP_F, >DOG_STOP_F, >480, >400, >400, >512, >400, >560
; MELEE starts inside mbMelee and still lands inside mbReach.
mbMeleeL	.byte 0, <DOG_MELEE_F, <800, <640, 0, 0, <640, <800
mbMeleeH	.byte 0, >DOG_MELEE_F, >800, >640, 0, 0, >640, >800
mbReachL	.byte 0, <DOG_BITE_F, <960, <800, 0, 0, <800, <1024
mbReachH	.byte 0, >DOG_BITE_F, >960, >800, 0, 0, >800, >1024
mbMDmg		.byte 0, 8, 16, 10, 0, 0, 12, 40	; + rnd & mbMRnd
mbMRnd		.byte 0, 7, 15, 7, 0, 0, 7, 31
; LEAP from mbMelee out to mbLeapHi; touching the player does mbLDmg.
mbLeapL		.byte 0, <DOG_LEAP_HI, 0, 0, 0, 0, <1600, 0
mbLeapH		.byte 0, >DOG_LEAP_HI, 0, 0, 0, 0, >1600, 0
mbLDmg		.byte 0, 10, 0, 0, 0, 0, 40, 0
mbLRnd		.byte 0, 7, 0, 0, 0, 0, 15, 0
; SHOOT: mbPel pellets of mbPelD each, the grunt's spread.
mbPel		.byte GR_PEL, 0, 0, 0, 0, 0, 0, 3
mbPelD		.byte GR_DMG, 0, 0, 0, 0, 0, 0, 10
; ... or a missile (gpu64_game_proj.inc's PK_*): the ogre's grenade, the
; zombie's gib, the scrag's spikes. mbShots a SHOOT, one a frame.
mbProj		.byte 0, 0, PK_GREN, 0, PK_GIB, PK_SPIKE, 0, 0
mbShots		.byte 1, 0, 1, 0, 1, 2, 0, 1
; Flies: no floor under its moves, and a height over the player.
mbFly		.byte 0, 0, 0, 0, 0, 1, 0, 0
mbCool		.byte GR_COOL, DOG_COOL, 12, 6, 10, 10, 6, 16
; A hit plays the pain animation when rnd < mbPain: Quake's big monsters
; mostly shrug a hit off.
mbPain		.byte 255, 255, 160, 200, 255, 200, 128, 40
; Quake's large hull (64 QU wide, 88 tall) instead of the 32 x 64 one.
mbBig		.byte 0, 0, 1, 0, 0, 0, 1, 1

; ----------------------------------------------------------------------
; entScan's start: no type has frames until its bank arrives.
; ----------------------------------------------------------------------
mtClear
	lda #0
	ldx #MT_LEN
-	sta mtFirst-1,x
	dex
	bne -
	rts

; entTake's: entBlk is a gpu64_bank record. Into the tables.
mtBank
	lda entBlk+ENT_P0+1
	bne mbkRts
	lda entBlk+ENT_P0
	sec
	sbc #K_MON0
	cmp #MT_N
	bcs mbkRts
	sta mtT
	ldx #11
-	lda entBlk+ENT_ORIGIN,x
	sta mtRec,x
	lda entBlk+ENT_TRAVEL,x
	sta mtRec+12,x
	dex
	bpl -
	ldx mtT				; slot * MT_N + type
	ldy #0
-	lda mtRec,y
	sta mtFirst,x
	lda mtRec+1,y
	sta mtCount,x
	txa
	clc
	adc #MT_N
	tax
	iny
	iny
	cpy #2 * 8
	bne -
	ldx mtT
	lda mtRec+16
	sta mtMeleeF,x
	lda mtRec+17
	sta mtShootF,x
	lda mtRec+18
	lsr a				; step 2: half rate
	sta mtSlow,x
mbkRts
	rts

; X = slot. Y = its type and C clear if a monster. Preserves X.
mtType
	lda acKind,x
	sec
	sbc #K_MON0
	tay
	cpy #MT_N
	rts

; X = slot, A = MA_*, cbMode = acAnMode: that animation of its type.
; Preserves X.
mtAnim
	asl a
	asl a
	asl a
	.cerror MT_N != 8
	sta mtT
	jsr mtType
	tya
	clc
	adc mtT
	tay
	lda mtCount,y
	sta mtT
	lda mtFirst,y
	ldy mtT
	jmp acAnim

; X = slot, A = MA_*. A = its frame count, Z set if the type has none.
; Preserves X.
mtHas
	asl a
	asl a
	asl a
	sta mtT
	jsr mtType
	bcs +
	tya
	clc
	adc mtT
	tay
	lda mtCount,y
	rts
+	lda #0
	rts

; X = slot. A = 1 for a large-hull monster, else 0, and Z from it.
; Preserves X.
mtBig
	jsr mtType
	bcs +
	lda mbBig,y
	rts
+	lda #0
	rts

; X = slot. actFrame's: acMask one notch slower for a half-rate type.
; Preserves X.
mtSlowMask
	jsr mtType
	bcs +
	lda mtSlow,y
	beq +
	sec
	rol acMask
+	rts

; X = slot: full health for its type. Preserves X.
mtHealth
	jsr mtType
	bcs +
	lda mbHpL,y
	sta acHp,x
	lda mbHpH,y
	sta acHpH,x
+	rts

; csFwdLat's: the cone limit cbT widened by 128 fine (16 QU) for a large
; monster, slot cbX.
cbBigLim
	ldx cbX
	jsr mtBig
	beq +
	lda cbT
	clc
	adc #128
	sta cbT
	bcc +
	inc cbT+1
+	rts

; crCand's: candidate cbCi. A large monster halves the lateral miss cbT, so
; the PEL_HALF test reads twice as wide, and takes a taller span. Returns
; Y = cbCi.
crBig
	ldy cbCi
	ldx cSlot,y
	jsr mtBig
	tay
	clc
	adc #>PEL_SPAN
	sta cbSpanH
	tya
	beq +
	lda cbT+1
	cmp #$80
	ror cbT+1
	ror cbT
+	ldy cbCi
	rts

; ----------------------------------------------------------------------
; State.
; ----------------------------------------------------------------------
mtFirst		.fill 8 * MT_N, 0	; [MA_* * MT_N + type]
mtCount		.fill 8 * MT_N, 0
mtMeleeF	.fill MT_N, 0
mtShootF	.fill MT_N, 0
mtSlow		.fill MT_N, 0
MT_LEN = * - mtFirst
mtRec		.fill 24, 0
mtT		.byte 0
cbSpanH		.byte 0
acHpH		.fill MAX_ACT, 0

; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_proj.inc -- the monsters' missiles, for gpu64_demo_game.a.
; Under BASIC with the monster types: only the level setup and the frame
; loop call it, both inside the BASIC-out span.
;
; The ogre's grenade, the scrag's spike and the zombie's gib. Each is one
; of PJ_N actor slots reserved after the level's own, created at setup like
; any other, hidden while unused; a throw takes a free one, or is not made.
;
; A missile flies on the C64, which knows where it is every frame, and asks
; the Pi ONE question: a hull-0 trace from where it is along its velocity,
; 2^PJ_RANGE_SH frames' worth, riding the AI's trace budget and answered in
; the same block. The fraction is how long it flies before something stops
; it. A spike and a gib fly that long; a grenade and a gib also stop where
; the player was standing when they were thrown -- their arc, a lob drawn
; over the straight line, comes back down onto it there. No answer yet
; (budget spent, a block that did not verify) and it asks again next frame;
; meanwhile it flies to that spot.
;
; Every frame it is tested against the player's box: a spike does 9, a gib
; 10, and a grenade explodes, as it does at the end of its flight: Quake's
; T_RadiusDamage, 40 less half the distance in QU, out to 80 QU, lit by the
; muzzle flash's point light at the blast. Simplifications: the arc is not
; traced, so a lob can graze a low ceiling; a grenade does not bounce; the
; splash hurts only the player, through walls too.
; ======================================================================

PJ_N      = 4			; missiles in flight at once
K_PROJ    = 29			; their slots' acKind: under every item kind, so
				; no pickup, respawn or backpack drop takes one
PK_GREN   = 1			; mbProj
PK_SPIKE  = 2
PK_GIB    = 3
PJ_SPEED  = 154			; fine per DT_REF frame: Quake's 600 QU/s
PJ_G      = 4			; the lob's fall, fine per frame per frame
PJ_AIM    = 176			; fine below the eye: the player's origin
PJ_RANGE_SH = 6			; the trace: 64 frames of flight, 2048 ms
PJ_HITX   = 160			; fine: the player's box plus 4 QU, half-width
PJ_HITUP  = 112			; ... above the eye
PJ_HITDN  = 400			; ... and below it
PJ_CENTRE = 144			; fine below the eye: the box's middle
PJ_SPLASH = 640			; fine: 80 QU, where 40 - d/2 reaches 0
PJ_GDMG   = 40

; Per PK_* (index 0 unused).
pkMesh		.byte 0, M_GRENADE, M_W_SPIKE, M_ZOM_GIB
pkDmg		.byte 0, 0, 9, 10	; a direct hit; the grenade's is its blast
pkArc		.byte 0, 1, 0, 1	; lobbed, and stops where the player stood

; ----------------------------------------------------------------------
; Setup, after entScan: the slots, hidden, kind K_PROJ. actTake leaves
; PJ_N of MAX_ACT for them.
; ----------------------------------------------------------------------
pjInit
	lda nAct
	sta pjSl0
	lda #0
	sta pjHits
	ldy #PJ_N-1
-	ldx nAct
	lda #K_PROJ
	sta acKind,x
	lda #0
	sta pjKind,y
	sta acType,x
	sta acFlags,x
	sta acAnN,x
	sta acRot,x
	sta acFix,x
	sta acState,x
	lda #<( MESH_BASE + M_GRENADE )
	sta acResL,x
	lda #>( MESH_BASE + M_GRENADE )
	sta acResH,x
	lda #1
	sta acDirty,x
	inc nAct
	dey
	bpl -
	rts

; restart's: nothing left in the air.
pjClear
	ldy #PJ_N-1
-	jsr pjKill
	dey
	bpl -
	rts

; Y = missile: gone, its slot hidden. Preserves Y.
pjKill
	lda #0
	sta pjKind,y
	tya
	clc
	adc pjSl0
	tax
	lda #0
	sta acFlags,x
	lda #1
	sta acDirty,x
	rts

; X = slot: pjP = its position. Preserves X.
pjGet
	.for k = 0, k < 12, k += 1
	lda acPos+k*MAX_ACT,x
	sta pjP+k
	.next
	rts

; X = slot: its position = pjP, to be sent. Preserves X.
pjPut
	.for k = 0, k < 12, k += 1
	lda pjP+k
	sta acPos+k*MAX_ACT,x
	.next
	lda #1
	sta acDirty,x
	rts

; Y = missile. pjVi = its first velocity index, missile * 3 + axis.
pjVIdx
	tya
	asl a
	sty pjVi
	adc pjVi
	sta pjVi
	rts

; ----------------------------------------------------------------------
; aoShoot's. X = aiX, the monster; A = PK_*. From its eye at the player's
; origin, PJ_SPEED along the straight line. Returns X = aiX.
; ----------------------------------------------------------------------
pjLaunch
	sta pjK
	ldy #PJ_N-1
-	lda pjKind,y
	beq +
	dey
	bpl -
plNo
	rts				; all in the air: not thrown
+	sty pjJ
	jsr acDelta			; aiThink came between: fresh geometry
	jsr aiGeom
	ldx aiX
	lda aiFar
	bne plNo
	ldy #3				; and the height inside an s16
	jsr ddNear
	bcs plNo
	; pjDel: the player's origin - the monster's eye, x y z, s16.
	lda aiDx
	sta pjDel
	lda aiDx+1
	sta pjDel+1
	lda aiDz
	sta pjDel+4
	lda aiDz+1
	sta pjDel+5
	sec
	lda #<( -PJ_AIM - AI_EYE )
	sbc dd+3
	sta pjDel+2
	sta pjN
	lda #>( -PJ_AIM - AI_EYE )
	sbc dd+4
	sta pjDel+3
	sta pjN+1
	; T = (aiDist + |dy| / 2) / PJ_SPEED frames, 1..255: the distance
	; within 7% when level, short by up to half straight up or down.
	bpl +
	jsr pjNeg
+	lsr pjN+1
	ror pjN
	clc
	lda pjN
	adc aiDist
	sta pjN
	lda pjN+1
	adc aiDist+1
	sta pjN+1
	lda #PJ_SPEED
	sta pjD
	jsr pjDiv
	lda pjN+1
	beq +
	lda #$ff
	sta pjN
+	lda pjN
	bne +
	lda #1
+	sta pjT
	; Its velocity: pjDel / T.
	ldy pjJ
	jsr pjVIdx
	ldx #0
plVel
	stx pjAx
	lda pjDel,x
	sta pjN
	lda pjDel+1,x
	sta pjN+1
	jsr pjSDiv
	ldy pjVi
	lda pjN
	sta pjVL,y
	lda pjN+1
	sta pjVH,y
	inc pjVi
	ldx pjAx
	inx
	inx
	cpx #6
	bne plVel
	; Its life, ms: T * 32, until the trace says otherwise. The lob's
	; upward speed: PJ_G * (T - 1) / 2, so it is back on the line at T.
	ldx pjJ
	lda #0
	sta pjAvH,x
	sta pjE
	lda pjT
	.cerror DT_REF != 32
	.rept 5
	asl a
	rol pjE
	.endrept
	sta pjLeftL,x
	lda pjE
	sta pjLeftH,x
	ldy pjK
	lda pkArc,y
	beq +
	lda pjT
	sec
	sbc #1
	.cerror PJ_G != 4
	asl a
	rol pjAvH,x
+	sta pjAvL,x
	lda pjK
	sta pjKind,x
	lda #1
	sta pjAsk,x
	; At the monster's eye, facing as it does.
	ldx aiX
	jsr pjGet
	clc
	lda pjP+5
	adc #<AI_EYE
	sta pjP+5
	lda pjP+6
	adc #>AI_EYE
	sta pjP+6
	bcc +
	inc pjP+7
+	ldy acYaw,x
	lda pjJ
	clc
	adc pjSl0
	tax
	tya
	sta acYaw,x
	jsr pjPut
	ldy pjK
	lda pkMesh,y
	clc
	adc #<MESH_BASE
	sta acResL,x
	lda #>MESH_BASE
	adc #0
	sta acResH,x
	lda #AF_VIS
	sta acFlags,x
	ldx aiX
	rts

; pjN = pjN / pjT, signed 16 by unsigned 8. Destroys X.
pjSDiv
	lda pjN+1
	sta pjSg
	bpl +
	jsr pjNeg
+	lda pjT
	sta pjD
	jsr pjDiv
	lda pjSg
	bpl pnRts
pjNeg
	sec
	lda #0
	sbc pjN
	sta pjN
	lda #0
	sbc pjN+1
	sta pjN+1
pnRts
	rts

; pjN = pjN / pjD, unsigned 16 by 8. Destroys X.
pjDiv
	lda #0
	ldx #16
-	asl pjN
	rol pjN+1
	rol a
	bcs +
	cmp pjD
	bcc ++
+	sbc pjD
	inc pjN
+	dex
	bne -
	rts

; ----------------------------------------------------------------------
; A frame, from aiFrame once the budgets are reset: every missile moves,
; ends, hits, or asks for its trace.
; ----------------------------------------------------------------------
pjFrame
	ldy #PJ_N-1
pfLoop
	sty pjJ
	lda pjKind,y
	bne +
	jmp pfNext
+	tya
	clc
	adc pjSl0
	tax
	stx pjX
	jsr pjGet
	jsr pjVIdx
	ldx #0
pfAx
	ldy pjVi			; the line's speed; on y, plus the lob's
	lda pjVL,y
	sta mA
	lda pjVH,y
	sta mA+1
	cpx #4
	bne +
	ldy pjJ
	clc
	lda mA
	adc pjAvL,y
	sta mA
	lda mA+1
	adc pjAvH,y
	sta mA+1
+	jsr dtMulS			; preserves X
	clc
	lda pjP+1,x
	adc dtR
	sta pjP+1,x
	lda pjP+2,x
	adc dtR+1
	sta pjP+2,x
	lda pjP+3,x
	adc dtR+2
	sta pjP+3,x
	inc pjVi
	inx
	inx
	inx
	inx
	cpx #12
	bne pfAx
	ldx pjX
	jsr pjPut
	ldy pjJ				; the lob falls: PJ_G per DT_REF frame
	ldx pjKind,y
	lda pkArc,x
	beq +
	.cerror PJ_G * 8 != DT_REF
	lda dtMs
	lsr a
	lsr a
	lsr a
	sta pjT2
	sec
	lda pjAvL,y
	sbc pjT2
	sta pjAvL,y
	lda pjAvH,y
	sbc #0
	sta pjAvH,y
+	sec				; its life
	lda pjLeftL,y
	sbc dtMs
	sta pjLeftL,y
	lda pjLeftH,y
	sbc #0
	sta pjLeftH,y
	bcc pfEnd
	; The player's box.
	ldx pjX
	jsr acDelta
	lda #<PJ_HITX
	sta pjLo
	sta pjHi
	lda #>PJ_HITX
	sta pjLo+1
	sta pjHi+1
	ldy #0
	jsr pjRange
	bcs pfMiss
	ldy #6
	jsr pjRange
	bcs pfMiss
	lda #<PJ_HITDN
	sta pjLo
	lda #>PJ_HITDN
	sta pjLo+1
	lda #<PJ_HITUP
	sta pjHi
	lda #>PJ_HITUP
	sta pjHi+1
	ldy #3
	jsr pjRange
	bcs pfMiss
	ldy pjJ				; touched
	ldx pjKind,y
	cpx #PK_GREN
	beq pfEnd
	lda pkDmg,x
	jsr pjHurt
	ldy pjJ
	jsr pjKill
	jmp pfNext
pfMiss
	ldy pjJ
	lda pjAsk,y
	beq pfNext
	ldx pjX
	jsr pjAskTr
pfNext
	ldy pjJ
	dey
	bmi +
	jmp pfLoop
+	rts
pfEnd
	ldy pjJ
	lda pjKind,y
	cmp #PK_GREN
	bne +
	ldx pjX
	jsr pjBoom
	ldy pjJ
+	jsr pjKill
	jmp pfNext

; A = damage to the player from a missile. Preserves nothing.
pjHurt
	beq +
	jsr plHurt
	inc pjHits
	bne +
	dec pjHits
+	rts

; Y = dd's axis (0, 3, 6). C clear if -pjLo <= it <= pjHi, both u16.
pjRange
	lda dd+2,y
	beq prPos
	cmp #$ff
	bne prOut
	lda dd+1,y
	bpl prOut			; under -32768
	clc
	lda dd,y
	adc pjLo
	lda dd+1,y
	adc pjLo+1
	bcc prOut			; no wrap: still below -pjLo
	clc
	rts
prPos
	lda dd+1,y
	bmi prOut
	sec
	lda pjHi
	sbc dd,y
	lda pjHi+1
	sbc dd+1,y
	bcc prOut
	clc
	rts
prOut
	sec
	rts

; X = missile slot, pjP its position, pjJ the missile: its trace into the
; AI's buffer, from here along its line, 2^PJ_RANGE_SH frames. Nothing if
; the frame's budget is spent: it asks again next frame.
pjAskTr
	ldy aiTrN
	cpy #AI_MAXTR
	bcc +
	rts
+
	txa
	sta aiTrSl,y
	lda aiTrOfs,y
	sta pjT2
	tay
	ldx #0
-	lda pjP,x
	sta aiTrBuf,y
	sta aiTrBuf+12,y
	iny
	inx
	cpx #12
	bne -
	lda #0				; hull 0, reserved
	sta aiTrBuf+12,y
	sta aiTrBuf+13,y
	sta aiTrBuf+14,y
	sta aiTrBuf+15,y
	ldy pjJ
	jsr pjVIdx
	ldx pjT2
	lda #3
	sta pjAx
patAx
	ldy pjVi			; v << PJ_RANGE_SH: (v << 8) >> 2
	lda #0
	sta pjE
	lda pjVL,y
	sta pjE+1
	lda pjVH,y
	sta pjE+2
	.rept 8 - PJ_RANGE_SH
	lda pjE+2
	cmp #$80
	ror pjE+2
	ror pjE+1
	ror pjE
	.endrept
	clc
	lda aiTrBuf+13,x
	adc pjE
	sta aiTrBuf+13,x
	lda aiTrBuf+14,x
	adc pjE+1
	sta aiTrBuf+14,x
	lda aiTrBuf+15,x
	adc pjE+2
	sta aiTrBuf+15,x
	inc pjVi
	inx
	inx
	inx
	inx
	dec pjAx
	bne patAx
	inc aiTrN
	rts

; aiResolve's, for a trace whose slot X is a missile's. Y = the answer's
; flags at (DST),Y, the fraction just before them.
pjAnswer
	txa
	sec
	sbc pjSl0
	tax
	lda pjKind,x
	beq paRts			; gone meanwhile
	lda (DST),y
	and #$f8
	bne paRts			; not an answer: it asks again
	lda (DST),y
	and #$06
	beq +
	txa				; thrown from inside a wall
	tay
	jmp pjKill
+	lda #0
	sta pjAsk,x
	sta pjT2+1
	dey
	lda (DST),y			; the fraction's high byte: 2048 ms * f
	.rept 11 - 8
	asl a
	rol pjT2+1
	.endrept
	sta pjT2
	ldy pjKind,x
	lda pkArc,y			; a lob keeps the shorter of the two
	beq +
	lda pjT2
	cmp pjLeftL,x
	lda pjT2+1
	sbc pjLeftH,x
	bcs paRts
+	lda pjT2
	sta pjLeftL,x
	lda pjT2+1
	sta pjLeftH,x
paRts
	rts

; X = the grenade's slot, pjP where it is: the blast. 40 less half the QU
; to the middle of the player's box, Chebyshev, and the light at it.
pjBoom
	lda #SFX_BOOM
	jsr sfxPlay
	jsr mfBoom
	jsr acDelta
	clc				; y from the box's middle
	lda dd+3
	adc #<PJ_CENTRE
	sta dd+3
	lda dd+4
	adc #>PJ_CENTRE
	sta dd+4
	lda dd+5
	adc #0
	sta dd+5
	lda #0
	sta pjN
	sta pjN+1
	ldy #6
pbAxis
	lda dd+2,y			; |axis|, 16-bit or out of reach
	beq pbPos
	cmp #$ff
	bne pbRts
	lda dd+1,y
	bpl pbRts
	sec
	lda #0
	sbc dd,y
	sta pjE
	lda #0
	sbc dd+1,y
	jmp pbMax
pbPos
	lda dd,y
	sta pjE
	lda dd+1,y
pbMax
	sta pjE+1
	lda pjN
	cmp pjE
	lda pjN+1
	sbc pjE+1
	bcs +
	lda pjE
	sta pjN
	lda pjE+1
	sta pjN+1
+	dey
	dey
	dey
	bpl pbAxis
	lda pjN				; past PJ_SPLASH: nothing
	cmp #<PJ_SPLASH
	lda pjN+1
	sbc #>PJ_SPLASH
	bcs pbRts
	.rept 4				; fine / 16 = half the QU
	lsr pjN+1
	ror pjN
	.endrept
	sec
	lda #PJ_GDMG
	sbc pjN
	jmp pjHurt
pbRts
	rts

; ----------------------------------------------------------------------
; State: set by pjInit at every level's setup, so it is not in the PRG --
; it shares the logo's screen with the trigger tables (gpu64_demo_game.a).
; ----------------------------------------------------------------------
	.virtual PJ_VARS
pjSl0		.byte 0
pjKind		.fill PJ_N, 0
pjAsk		.fill PJ_N, 0
pjVL		.fill 3 * PJ_N, 0	; [missile * 3 + axis], fine per frame
pjVH		.fill 3 * PJ_N, 0
pjAvL		.fill PJ_N, 0		; the lob's upward speed
pjAvH		.fill PJ_N, 0
pjLeftL		.fill PJ_N, 0		; ms of flight left
pjLeftH		.fill PJ_N, 0
pjHits		.byte 0
pjJ		.byte 0
pjX		.byte 0
pjK		.byte 0
pjT		.byte 0
pjD		.byte 0
pjSg		.byte 0
pjAx		.byte 0
pjVi		.byte 0
pjN		.word 0
pjT2		.word 0
pjLo		.word 0
pjHi		.word 0
pjE		.fill 3, 0
pjDel		.fill 6, 0
pjP		.fill 12, 0
	.cerror * > LOGO_SCREEN + 1000, "the logo screen's tenants outgrew it"
	.endv

; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_world.inc -- milestone 20 stage C: the actor table and the
; WORLD_TICK plumbing for gpu64_demo_game.a.
;
; Every monster, item, the gun and the status bar is one slot here and one
; node on the Pi, created once at setup and from then on only POSED -- by a
; single WORLD_TICK block a frame (docs/class1-3d-mesh-reference.md, "A world
; in one block"). What goes in the block is every slot marked dirty, up to
; WT_DIRTY_MAX, plus WT_REFRESH clean slots taken round-robin, so a pose the
; bus lost is put back by rotation even when nothing re-dirties it
; (dev/.. gpu64-state-refresh-ring). A slot is only marked clean on a
; verified answer: the magic at both ends, the echoed frame and the output
; sum. A record the Pi REFUSED names a node that is not there or not that
; kind any more -- a phantom DESTROY or CREATE got it -- so the slot is
; re-created, one a frame, and posed again.
;
; Slot layout: the gun, then the status bar in the order it has to draw
; (tiles first, so the digits land on top of them), then the world.
; Node id = ACT_ID0 + slot; ids 2..85 sit between the camera (1) and the
; level's own nodes (NODE_BASE = 100).
; ======================================================================

ACT_ID0   = 2
MAX_ACT   = 84			; the last PJ_N are the missiles' (proj.inc)

SL_GUN    = 0
SL_SBAR   = 1			; five 64-pixel tiles
SL_ARMI   = 6			; armour icon
SL_ARMD   = 7			; three armour digits
SL_FACE   = 10
SL_HPD    = 11			; three health digits
SL_AMMI   = 14			; ammo icon
SL_AMMD   = 15			; three ammo digits
SL_XHAIR  = 18
SL_WORLD  = 19

AF_VIS    = $01			; WORLD_TICK actor flags
AF_VIEW   = $02
SPR_HUD   = $0a			; SET_SPRITE: depth-test-no-write | unlit

; The gun is v_shot at scale 4 and four times the distance, i.e. the same
; picture as scale 1 at 0.35 wu forward, but everything it draws is deeper
; than the status bar's plane at every FOV, so the bar is never hidden by it.
GUN_SCALE = $0400		; 8.8
GUN_Z     = round( 1.4 * 65536 )
; Lifted so the barrels clear the status bar, as Quake's do. 64tass -D
; GUN_LIFT=... overrides it for a render check.
GUN_LIFT :?= 0.4
GUN_Y     = round( GUN_LIFT * 65536 )

; Idle animation and the items' turn: each slot moves every ANIM_MASK+1
; frames, and the slots take turns, so the block carries about a quarter of
; them a frame. Every record is ~25 cycles a byte of 6502 time (build, sum).
ANIM_MASK :?= 3
ROT_STEP  = 2 * ( ANIM_MASK + 1 )
ANIM_NEAR = 32			; wu: idle loops and spinning pickups inside this

WT_MAGIC  = $d8
WT_OMAGIC = $8d
WT_HDR    = 16
WT_REC    = 20
WT_MAXREC = 32
WT_DIRTY_MAX = 30
WT_REFRESH = 2
WT_TRIES  = 4
; The block at its largest, so stage D/E's traces and moves fit without
; moving the DMA window: header, 32 actors, 24 traces, 24 moves, 16 movers,
; then 8 + 24*16 + 24*16 + 4 bytes of answers.
WT_LEN    = WT_HDR + 32*20 + 24*28 + 24*28 + 16*16 + 8 + 24*16 + 24*16 + 4

; ----------------------------------------------------------------------
; The status bar's screen mapping. Sprites are anchored at the feet (the
; bottom centre), and at z = 0.15625/tan(fov/2) one world unit is exactly
; 1024 screen pixels, whatever the FOV -- so a pixel is 64 in 16.16, a
; 64-pixel texture is 16/256 wu wide in SET_SPRITE's 8.8, and only z moves
; when + or - changes the FOV. The pictures are padded to powers of two with
; the art centred: 24-pixel digits in 32x32, 64x24 tiles in 64x32, so their
; feet sit 4 pixels below the screen's last row.
; ----------------------------------------------------------------------
HUD_FEET = 204
hudCol	.macro	c
	.word ( ( \c - 160 ) * 64 ) & $ffff
	.endm

; Indexed by slot, 0..SL_WORLD-1 (slot 0, the gun, is not a sprite).
hudX
	.word 0
	.for i = 0, i < 5, i += 1
	#hudCol 64 * i + 32
	.next
	#hudCol 12			; armour icon
	#hudCol 36
	#hudCol 60
	#hudCol 84
	#hudCol 124			; face
	#hudCol 148
	#hudCol 172
	#hudCol 196
	#hudCol 236			; ammo icon
	#hudCol 260
	#hudCol 284
	#hudCol 308
	#hudCol 160			; crosshair
hudY
	.word 0
	.for i = 0, i < 17, i += 1
	.word ( ( 100 - HUD_FEET ) * 64 ) & $ffff
	.next
	.word ( ( 100 - 104 ) * 64 ) & $ffff	; crosshair: 8x8, centred on row 100
hudW
	.byte 0, 16, 16, 16, 16, 16
	.byte 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
	.byte 2
hudH
	.byte 0, 8, 8, 8, 8, 8
	.byte 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8
	.byte 2
hudTex
	.byte 0
	.byte T_SBAR, T_SBAR+1, T_SBAR+2, T_SBAR+3, T_SBAR+4
	.byte T_SB_ARMOR1, T_NUM_0, T_NUM_0, T_NUM_0
	.byte T_FACE1, T_NUM_0, T_NUM_0, T_NUM_0
	.byte T_SB_SHELLS, T_NUM_0, T_NUM_0, T_NUM_0
	.byte T_CROSSHAIR

; z of the status bar's plane per fovIx, 16.16 (the top two bytes are 0).
hudZLo
	.for i = 0, i < FOV_N, i += 1
	.byte <round( 0.15625 / tan( ( FOV_DEG0 + i * FOV_STEP ) * pi / 360.0 ) * 65536 )
	.next
hudZHi
	.for i = 0, i < FOV_N, i += 1
	.byte >round( 0.15625 / tan( ( FOV_DEG0 + i * FOV_STEP ) * pi / 360.0 ) * 65536 )
	.next

; The item kinds (gen_quakelevel.py) -> the first mesh of what stands
; there, level mesh index; 0 = not placed. Kinds 30..49. A monster's comes
; from its level's bank (gpu64_game_monsters.inc).
KM_FIRST = 30
kindMesh
	.byte 0, 0, M_ARMOR, M_ARMOR_S1, M_ARMOR_S2	; 30-34 (31: see actTake)
	.byte M_G_SHOT, M_G_NAIL, 0, 0, 0		; 35-39
	.byte M_QUAD, 0, M_SUIT, 0, 0			; 40-44
	.byte 0, 0, 0, 0, 0				; 45-49 (45, 46: see actTake)
KM_LAST = KM_FIRST + * - kindMesh - 1

; ======================================================================
; Setup: the gun and the status bar. Called before entScan, which appends
; the world after them.
; ======================================================================
actInit
	lda #0
	sta actDrop
	sta wtOk
	sta wtOk+1
	sta wtBad
	sta wtBad+1
	sta wtRefd
	sta wtFixCt
	sta wtStart
	sta wtRefIx
	sta wtFrame
	sta wtLastN
	ldx #MAX_ACT-1
-	sta acDirty,x
	sta acFix,x
	sta acAnN,x
	sta acAnF,x
	sta acRot,x
	sta acKind,x
	sta acYaw,x
	.for k = 0, k < 12, k += 1
	sta acPos+k*MAX_ACT,x
	.next
	dex
	bpl -

	; The gun: an object, in view space, straight ahead.
	lda #0
	sta acType+SL_GUN
	lda #<( MESH_BASE + M_V_SHOT )
	sta acResL+SL_GUN
	lda #>( MESH_BASE + M_V_SHOT )
	sta acResH+SL_GUN
	lda #AF_VIS | AF_VIEW
	sta acFlags+SL_GUN
	lda #<GUN_Y
	sta acPos+4*MAX_ACT+SL_GUN
	lda #>GUN_Y
	sta acPos+5*MAX_ACT+SL_GUN
	lda #`GUN_Y
	sta acPos+6*MAX_ACT+SL_GUN
	lda #<GUN_Z
	sta acPos+8*MAX_ACT+SL_GUN
	lda #>GUN_Z
	sta acPos+9*MAX_ACT+SL_GUN
	lda #`GUN_Z
	sta acPos+10*MAX_ACT+SL_GUN
	lda #1
	sta acDirty+SL_GUN

	; The status bar.
	ldx #SL_GUN+1
aiHud
	lda #1
	sta acType,x
	sta acDirty,x
	lda hudTex,x
	sta acResL,x
	lda #0
	sta acResH,x
	lda #AF_VIS | AF_VIEW
	sta acFlags,x
	txa
	asl a
	tay
	lda hudX,y
	sta acPos+0*MAX_ACT,x
	lda hudX+1,y
	sta acPos+1*MAX_ACT,x
	jsr aiSign
	sta acPos+2*MAX_ACT,x
	sta acPos+3*MAX_ACT,x
	lda hudY,y
	sta acPos+4*MAX_ACT,x
	lda hudY+1,y
	sta acPos+5*MAX_ACT,x
	jsr aiSign
	sta acPos+6*MAX_ACT,x
	sta acPos+7*MAX_ACT,x
	inx
	cpx #SL_WORLD
	bne aiHud
	lda #$ff			; forces hudFovCheck to place the plane
	sta hudFov
	jsr hudFovCheck

	; Quake's opening numbers. No armour: the icon is hidden, the 0 is not.
	lda #AF_VIEW
	sta acFlags+SL_ARMI
	lda #100
	ldx #SL_HPD
	ldy #T_NUM_0
	jsr hudNum
	lda #0
	ldx #SL_ARMD
	ldy #T_NUM_0
	jsr hudNum
	lda #25
	ldx #SL_AMMD
	ldy #T_NUM_0
	jsr hudNum

	lda #SL_WORLD
	sta nAct
	rts

; A = the high byte of a 16-bit value; returns its sign extension.
aiSign
	and #$80
	beq +
	lda #$ff
+	rts

; ======================================================================
; entTake's branch for kinds 30 and up: an item or a monster. The skill the
; title menu picked decides which of spawnflags bits 8-10 (NOT_IN_EASY,
; NOT_IN_MEDIUM, NOT_IN_HARD) leaves the entity out: skillMask is that bit,
; high byte, and nightmare filters as hard does, as it does in Quake.
; ======================================================================
actTake
	lda entBlk+ENT_SPAWN+1
	and skillMask
	bne atNo
	lda entBlk+ENT_KIND
	cmp #31
	beq atHealth
	cmp #45
	beq atShells
	cmp #46
	beq atNails
	cmp #K_MON0
	bcc +
	cmp #K_MON_LAST+1
	bcs atNo
	sbc #K_MON0 - 1			; C clear: the type
	tax
	lda mtFirst+MA_STAND*MT_N,x	; 0: no bank, not placed
	bne atPlace
	rts
+	cmp #KM_LAST+1
	bcs atNo
	sec
	sbc #KM_FIRST
	bcc atNo
	tax
	lda kindMesh,x
	bne atPlace
atNo
	rts
atHealth
	lda entBlk+ENT_SPAWN
	lsr a
	bcc +
	lda #M_BH10			; rotten
	bne atPlace
+	lsr a
	bcc +
	lda #M_BH100			; megahealth
	bne atPlace
+	lda #M_BH25
	bne atPlace
atShells
	lda entBlk+ENT_SPAWN
	lsr a
	lda #M_SHELL0
	adc #0				; big box: M_SHELL1
	bne atPlace
atNails
	lda entBlk+ENT_SPAWN
	lsr a
	lda #M_NAIL0
	adc #0
atPlace
	ldx nAct
	cpx #MAX_ACT - PJ_N
	bcc +
	inc actDrop
	rts
+	sta acAnB,x
	clc
	adc #<MESH_BASE
	sta acResL,x
	lda #>MESH_BASE
	adc #0
	sta acResH,x
	lda #0
	sta acType,x
	sta acAnF,x
	sta acAnN,x
	sta acRot,x
	sta acFix,x
	lda #AF_VIS
	sta acFlags,x
	lda #1
	sta acDirty,x
	lda entBlk+ENT_YAW+1
	sta acYaw,x
	.for k = 0, k < 12, k += 1
	lda entBlk+ENT_ORIGIN+k
	sta acPos+k*MAX_ACT,x
	.next
	lda #AM_LOOP
	sta acAnMode,x
	lda #AS_IDLE
	sta acState,x
	lda entBlk+ENT_KIND
	sta acKind,x
	jsr mtType
	bcs +
	lda mtCount+MA_STAND*MT_N,y
	sta acAnN,x
	jsr mtHealth
+	lda acKind,x
	cmp #32				; weapons, armour and powerups turn
	bcc +
	cmp #45
	bcs +
	lda #1
	sta acRot,x
+	inc nAct
	rts

; ======================================================================
; Setup: create every slot's node. A failure is not fatal -- the slot is
; marked for the frame loop's one-a-frame repair, exactly like a refusal.
; ======================================================================
actCreate
	ldx #0
acLoop
	stx acJ
	jsr actCreateOne
	beq +
	ldx acJ
	lda #1
	sta acFix,x
+	ldx acJ
	inx
	cpx nAct
	bne acLoop
	rts

; X = slot. CREATE_*, then SET_SPRITE or the gun's SET_SCALE -- both are
; lost with the node, so they are part of creating it. Z set on success.
actCreateOne
	stx acI
	txa
	clc
	adc #ACT_ID0
	sta stId
	lda #0
	sta stId+1
	lda acResL,x
	sta stBuf+0
	lda acResH,x
	sta stBuf+1
	lda acType,x
	bne acoSprite
	lda #OP_CREATE_OBJECT
	ldx #1
	jsr stage2
	bne acoDone
	lda acI
	cmp #SL_GUN
	bne acoOk
	lda #<GUN_SCALE
	sta stBuf+0
	lda #>GUN_SCALE
	sta stBuf+1
	lda #OP_SET_SCALE
	ldx #1
	jsr stage2
	jmp acoDone
acoSprite
	lda #OP_CREATE_SPRITE
	ldx #1
	jsr stage2
	bne acoDone
	ldx acI
	lda hudW,x
	sta stBuf+0
	lda hudH,x
	sta stBuf+2
	lda #0
	sta stBuf+1
	sta stBuf+3
	lda #SPR_HUD
	sta stBuf+4
	lda #OP_SET_SPRITE
	ldx #4
	jsr stage2
	jmp acoDone
acoOk
	lda #0
acoDone
	php
	ldx acI
	plp
	rts

; ======================================================================
; A frame: idle animation and turning items, one repair, then the block.
; ======================================================================
actFrame
	inc wtFrame
	jsr hudFovCheck

	; Only the slots of this frame's parity: every test below needs
	; (slot ^ wtFrame) & 1 == 0 (both masks are odd), so the other half
	; cannot do anything this frame and is not visited.
	.cerror ( ANIM_MASK & AI_ANIM_MASK & 1 ) == 0, "afLoop steps by two"
	lda wtFrame
	eor #SL_WORLD
	and #1
	clc
	adc #SL_WORLD
	tax
afLoop
	cpx nAct
	bcc +
	jmp afFix
+
	lda acAnN,x
	beq afRot
	ldy #ANIM_MASK			; an awake monster at Quake's 10 Hz
	lda acAi,x
	beq +
	ldy #AI_ANIM_MASK
+	sty acMask
	jsr mtSlowMask			; a half-rate type: every 2nd of those
	txa				; each slot every ANIM_MASK+1 frames, staggered
	eor wtFrame
	and acMask
	bne afRot
	lda acAi,x			; idling asleep: only near the player
	ora acAnMode,x
	bne +
	jsr acNearWu
	bcs afRot
+	lda acAnF,x
	clc
	adc #1
	cmp acAnN,x
	bcc afStep
	lda acAnMode,x			; the last frame: loop, hold or stand
	beq afWrap
	cmp #AM_HOLD
	beq afRot
	jsr aiAnimEnd			; pain or an attack over
	jmp afRot
afWrap
	lda #0
afStep
	sta acAnF,x
	clc
	adc acAnB,x
	clc
	adc #<MESH_BASE
	sta acResL,x
	lda #>MESH_BASE
	adc #0
	sta acResH,x
	lda #1
	sta acDirty,x
afRot
	lda acRot,x
	beq afNext
	txa
	eor wtFrame
	and #ANIM_MASK
	bne afNext
	jsr acNearWu			; a pickup spins only near the player
	bcs afNext
	lda acYaw,x
	clc
	adc #ROT_STEP
	sta acYaw,x
	lda #1
	sta acDirty,x
afNext
	inx
	inx
	jmp afLoop

afFix
	lda wtFrame			; repairs are rare: look every 4th frame
	and #3
	beq +
	jmp worldTick
+	ldx #0
-	lda acFix,x
	bne afDoFix
	inx
	cpx nAct
	bcc -
	jmp worldTick
afDoFix
	jsr actCreateOne
	bne +
	lda #0
	sta acFix,x
	lda #1
	sta acDirty,x
	inc wtFixCt
	bne +
	dec wtFixCt
+	jmp worldTick

; X = slot. C clear if it is within ANIM_NEAR whole units of the eye on
; both x and z. Preserves X. What only decorates -- an idle loop, a pickup
; spinning -- is not worth a record a frame across the map: every one is
; 20 bytes in the WORLD_TICK block, its checksum, and the Pi's time.
acNearWu
	sec
	lda acPos+2*MAX_ACT,x
	sbc cam+2
	tay
	lda acPos+3*MAX_ACT,x
	sbc cam+3
	jsr anwAxis
	bcs anwRts
	sec
	lda acPos+10*MAX_ACT,x
	sbc cam+10
	tay
	lda acPos+11*MAX_ACT,x
	sbc cam+11
anwAxis				; A:Y a signed whole-unit delta
	beq anwPos
	cmp #$ff
	bne anwFar
	tya
	eor #$ff
	cmp #ANIM_NEAR
	rts
anwPos
	cpy #ANIM_NEAR
	rts
anwFar
	sec
anwRts
	rts

; The status bar's plane follows the FOV (see hudZLo).
hudFovCheck
	lda fovIx
	cmp hudFov
	beq hfDone
	sta hudFov
	tay
	ldx #SL_GUN+1
-	lda hudZLo,y
	sta acPos+8*MAX_ACT,x
	lda hudZHi,y
	sta acPos+9*MAX_ACT,x
	lda #0
	sta acPos+10*MAX_ACT,x
	sta acPos+11*MAX_ACT,x
	lda #1
	sta acDirty,x
	inx
	cpx #SL_WORLD
	bne -
hfDone
	rts

; A = value 0-255, X = the first of three digit slots, Y = the texture of
; digit 0 (T_NUM_0, or T_ANUM_0 for Quake's red numbers). Leading zeros are
; hidden, not drawn; a slot is dirtied only when it changes.
hudNum
	sty hnTex
	stx hnSlot
	ldy #0
-	cmp #100
	bcc +
	sbc #100
	iny
	bne -
+	sty hnD+0
	ldy #0
-	cmp #10
	bcc +
	sbc #10
	iny
	bne -
+	sty hnD+1
	sta hnD+2
	lda #0
	sta hnVis
	ldy #0
hnLoop
	lda hnD,y
	ora hnVis
	cpy #2
	bne +
	ora #1				; the units always show
+	sta hnVis
	ldx hnSlot
	lda hnD,y
	clc
	adc hnTex
	cmp acResL,x
	beq +
	sta acResL,x
	lda #1
	sta acDirty,x
+	lda #AF_VIEW
	ldx hnVis
	beq +
	lda #AF_VIS | AF_VIEW
+	ldx hnSlot
	cmp acFlags,x
	beq +
	sta acFlags,x
	lda #1
	sta acDirty,x
+	inc hnSlot
	iny
	cpy #3
	bne hnLoop
	rts

; ======================================================================
; WORLD_TICK. Builds the block in wtBlk, sends it through stage2 (checked
; like every other command), and believes the answer only when it verifies.
; ======================================================================
worldTick
	lda #<( wtBlk + WT_HDR )
	sta DST
	lda #>( wtBlk + WT_HDR )
	sta DST+1
	lda #0
	sta wtN

	; Every dirty slot, downward from where the last full block stopped
	; (normally the top) and round. Two tight loops rather than one with
	; a wrap test in it: this scan visits every actor every frame.
	ldx wtStart
	cpx nAct
	bcc +
	ldx nAct
	dex
+	stx wdFirst
wdSeg1					; wdFirst down to 0
	lda acDirty,x
	bne wdHit1
wdCont1
	dex
	bpl wdSeg1
	ldx nAct			; then the top down to wdFirst + 1
	dex
	cpx wdFirst
	beq wdAll
wdSeg2
	lda acDirty,x
	bne wdHit2
wdCont2
	dex
	cpx wdFirst
	bne wdSeg2
wdAll
	lda #$ff			; the whole ring fitted: next from the top
	sta wtStart
	jmp wdRefresh
wdHit1
	jsr wdTake
	bcc wdCont1
	bcs wdFull
wdHit2
	jsr wdTake
	bcc wdCont2
wdFull
	stx wtStart			; full: this one leads the next block
	jmp wdRefresh

; X = a dirty slot: its record, unless it waits for a repair. C set when
; the block is full and it was not taken. Preserves X.
wdTake
	lda acFix,x
	bne wdtOk
	lda wtN
	cmp #WT_DIRTY_MAX
	bcs wdtRts
	jsr wtEmit
wdtOk
	clc
wdtRts
	rts

	; And two clean ones, round-robin: the refresh ring.
wdRefresh
	lda #WT_REFRESH
	sta wtCnt2
	lda nAct
	sta wtCnt
	ldx wtRefIx
wtrLoop
	inx
	cpx nAct
	bcc +
	ldx #0
+	lda acDirty,x
	ora acFix,x
	bne wtrNext
	jsr wtEmit
	dec wtCnt2
	beq wtrDone
wtrNext
	dec wtCnt
	bne wtrLoop
wtrDone
	stx wtRefIx

	; A shot's traces, the monsters' traces and moves, and the movers
	; they are all traced against. Every query answers in 16 bytes and
	; the trailer's offset is a byte: wtQN <= 15 (CB_NCAND + AI_MAXTR +
	; AI_MAXMV).
	lda #0
	sta wtTrOk
	sta wtMvN
	clc
	lda wtTrN
	adc aiTrN
	sta wtQTr
	adc aiMvN
	sta wtQN
	beq +
	jsr wtTraces
+
	lda wtN
	sta wtLastN
	ora wtQN
	bne +
	rts
+
	lda wtQN			; the answer: 8 + 16 per query + 4
	asl a
	asl a
	asl a
	asl a
	clc
	adc #8
	sta wtTrail
	adc #4
	sta wtOutLen
	; The header, then its sum over everything before OUT (bytes 6-7 are
	; zero while it is taken, which is the same as leaving them out).
	lda #WT_MAGIC
	sta wtBlk+0
	lda wtN
	sta wtBlk+1
	lda wtQTr
	sta wtBlk+2
	lda aiMvN
	sta wtBlk+3
	lda wtMvN
	sta wtBlk+4
	lda #0
	sta wtBlk+6
	sta wtBlk+7
	lda wtFrame
	sta wtBlk+5

	sec				; OUT = DST - wtBlk
	lda DST
	sbc #<wtBlk
	sta wtOut
	lda DST+1
	sbc #>wtBlk
	sta wtOut+1
	lda #<wtBlk
	sta SRC
	lda #>wtBlk
	sta SRC+1
	ldx wtOut+1
	lda wtOut
	jsr sum16
	lda wsLo
	sta wtBlk+6
	lda wsHi
	sta wtBlk+7

	; The descriptor: C64 space, wtBlk, input plus output.
	clc
	lda wtOut
	adc wtOutLen
	sta wtDesc+4
	lda wtOut+1
	adc #0
	sta wtDesc+5

	lda #WT_TRIES
	sta wtTry
wtSend
	ldy wtOutLen
	dey
	lda #$a5
-	sta (DST),y
	dey
	bpl -
	lda #SPACE_C64
	sta stBuf+0
	lda #<wtBlk
	sta stBuf+1
	lda #>wtBlk
	sta stBuf+2
	lda #0
	sta stBuf+3
	sta stId
	sta stId+1
	lda wtDesc+4
	sta stBuf+4
	lda wtDesc+5
	sta stBuf+5
	lda #OP_WORLD_TICK
	ldx #5
	jsr stage2
	bne wtRetry
	jsr wtVerify
	beq wtGood
wtRetry
	dec wtTry
	bne wtSend
	inc wtBad
	bne +
	inc wtBad+1
+	rts

wtGood
	inc wtOk
	bne +
	inc wtOk+1
+	lda #1
	sta wtTrOk
	lda wtN
	bne +
	rts
+
	; Refused bit n -> recSlot[n] is re-created; everything else is clean.
	ldy #4
	lda (DST),y
	sta wtRef+0
	iny
	lda (DST),y
	sta wtRef+1
	iny
	lda (DST),y
	sta wtRef+2
	iny
	lda (DST),y
	sta wtRef+3
	ldy #0
wgLoop
	ldx recSlot,y
	lsr wtRef+3
	ror wtRef+2
	ror wtRef+1
	ror wtRef+0
	bcs wgRefused
	lda #0
	sta acDirty,x
	beq wgNext
wgRefused
	lda #1
	sta acFix,x
	inc wtRefd
	bne wgNext
	dec wtRefd
wgNext
	iny
	cpy wtN
	bne wgLoop
	rts

; The answer at (DST): the magic and the frame at both ends and the sum of
; the eight bytes before the trailer. Z set when it verifies.
wtVerify
	ldy #0
	lda (DST),y
	cmp #WT_OMAGIC
	bne wvBad
	ldy wtTrail
	iny
	iny
	lda (DST),y
	cmp #WT_OMAGIC
	bne wvBad
	ldy #1
	lda (DST),y
	cmp wtFrame
	bne wvBad
	ldy wtTrail
	iny
	iny
	iny
	lda (DST),y
	cmp wtFrame
	bne wvBad
	lda DST
	sta SRC
	lda DST+1
	sta SRC+1
	ldx #0
	lda wtTrail
	jsr sum16
	ldy wtTrail
	lda (DST),y
	cmp wsLo
	bne wvBad
	iny
	lda (DST),y
	cmp wsHi
	bne wvBad
	lda #0
	rts
wvBad
	lda #1
	rts

; X = slot. One actor record at (DST), which then moves on by one record.
wtEmit
	txa
	ldy wtN
	sta recSlot,y
	inc wtN
	clc
	adc #ACT_ID0
	ldy #0
	sta (DST),y
	iny
	lda #0
	sta (DST),y
	iny
	lda acResL,x
	sta (DST),y
	iny
	lda acResH,x
	sta (DST),y
	iny
	lda acFlags,x
	sta (DST),y
	iny
	lda acYaw,x
	sta (DST),y
	iny
	lda #0
	sta (DST),y			; pitch
	iny
	sta (DST),y			; roll
	.for k = 0, k < 12, k += 1
	iny
	lda acPos+k*MAX_ACT,x
	sta (DST),y
	.next
	clc
	lda DST
	adc #WT_REC
	sta DST
	bcc +
	inc DST+1
+	rts

; 16-bit sum of the X*256 + A bytes at (SRC). Result in wsLo/wsHi; SRC is
; destroyed.
sum16
	sta wsRem
	lda #0
	sta wsHi
	tay
	clc
s16Page
	cpx #0
	beq s16Rem
	clc				; cpx just set it
-	adc (SRC),y
	bcc +
	inc wsHi
	clc
+	iny
	bne -
	inc SRC+1
	dex
	jmp s16Page
s16Rem
	; The last partial page, with SRC moved back 256 - wsRem bytes so that
	; Y runs from 256 - wsRem up to the wrap: the INY ends the loop and
	; there is no compare in it.
	ldy wsRem
	beq s16Done
	pha
	tya
	clc
	adc SRC
	sta SRC
	bcs +
	dec SRC+1			; SRC + wsRem - 256
+	lda #0
	sec
	sbc wsRem
	tay
	pla
	clc
-	adc (SRC),y
	bcc +
	inc wsHi
	clc
+	iny
	bne -
s16Done
	sta wsLo
	rts

; ----------------------------------------------------------------------
; Row 20: the world block. N records in the last block, OK and BAD blocks
; (BAD = all WT_TRIES failed to verify), RF records the Pi refused and FX
; slots re-created since, DR entities with no slot left. RF and FX climbing
; together is the repair working; RF alone climbing is a slot that cannot
; be re-created.
; ----------------------------------------------------------------------
showWorld
	lda #20
	jsr dmRow
	jsr dmText
	.text "ACT N"
	.byte 0
	lda wtLastN
	jsr dmDecA
	jsr dmText
	.text " OK "
	.byte 0
	lda wtOk+1
	jsr dmHexA
	lda wtOk
	jsr dmHexA
	jsr dmText
	.text " BD "
	.byte 0
	lda wtBad+1
	jsr dmHexA
	lda wtBad
	jsr dmHexA
	jsr dmText
	.text " RF"
	.byte 0
	lda wtRefd
	jsr dmDecA
	jsr dmText
	.text " FX"
	.byte 0
	lda wtFixCt
	jsr dmDecA
	jsr dmText
	.text " "
	.byte 0
	lda nAct
	jsr dmDecA
	rts

; ----------------------------------------------------------------------
; State.
; ----------------------------------------------------------------------
nAct		.byte 0
actDrop		.byte 0
acI		.byte 0
acMask		.byte 0
acJ		.byte 0
hudFov		.byte 0
hnTex		.byte 0
hnSlot		.byte 0
hnVis		.byte 0
hnD		.fill 3, 0
wtN		.byte 0
wtTrN		.byte 0			; traces this block (combatFrame's)
wtMvN		.byte 0
wtQTr		.byte 0			; traces: the shot's, then the monsters'
wtQN		.byte 0			; and every query, moves included
wtTrOk		.byte 0			; the block verified: its answers are real
wtTrail		.byte 0			; the trailer's offset in the answer
wtOutLen	.byte 0
wtLastN		.byte 0
wtCnt		.byte 0
wtCnt2		.byte 0
wtStart		.byte 0
wdFirst		.byte 0
wtRefIx		.byte 0
wtFrame		.byte 0
wtTry		.byte 0
wtOut		.word 0
wtDesc		.fill 6, 0
wtRef		.fill 4, 0
wtOk		.word 0
wtBad		.word 0
wtRefd		.byte 0
wtFixCt		.byte 0
wsLo		.byte 0
wsHi		.byte 0
wsRem		.byte 0
recSlot		.fill WT_MAXREC, 0

acType		.fill MAX_ACT, 0	; 0 object, 1 sprite
acResL		.fill MAX_ACT, 0
acResH		.fill MAX_ACT, 0
acFlags		.fill MAX_ACT, 0
acYaw		.fill MAX_ACT, 0
acDirty		.fill MAX_ACT, 0
acFix		.fill MAX_ACT, 0
acKind		.fill MAX_ACT, 0
acAnB		.fill MAX_ACT, 0	; first mesh (level index) of the animation
acAnN		.fill MAX_ACT, 0	; its frames; 0 = not animated
acAnF		.fill MAX_ACT, 0
acRot		.fill MAX_ACT, 0
acPos		.fill MAX_ACT * 12, 0	; byte k of slot x at acPos + k*MAX_ACT + x

; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; stunt_sfx.inc -- the race's sound, on the SID. Led Storm is the menus'
; and is off for a race; this is on for one.
;
; Voice 1 is the engine, a pulse whose pitch follows the player's speed
; and rises when the wheels are off the road. Voice 2 is the boost, a hiss
; while it burns, or else the tyres squealing under hard braking. Voice 3
; is one sound at a time, started by what changed since the last frame: a
; crash, the cars hitting each other, a landing, the countdown and laps.
; All of it runs once a frame from the race loop (sfxTick), never from an
; IRQ, so it costs nothing outside a race and needs no zero page.
; ======================================================================

SID	= $d400
V1	= SID + 0
V2	= SID + 7
V3	= SID + 14
SID_VOL	= SID + $18

ENG_BASE = $0300		; the engine at a standstill
ENG_AIR	= $0180			; and how much it climbs off the ground

SQUEAL_V = $20			; braking faster than this squeals
HIT_GAP	= 16			; frames between two crunches

FX_WRECK = 0			; voice 3's sounds: fxTab rows, loudest
FX_CRASH = 1			;   first
FX_HIT	= 2
FX_THUMP = 3
FX_LAND	= 4
FX_GO	= 5
FX_BEEP	= 6
FX_LAP	= 7

; Silence, the volume up, and nothing to compare with yet.
sfxInit
	lda #0
	ldx #$18
-	sta SID,x
	dex
	bpl -
	lda #$0f
	sta SID_VOL
	lda #$00			; the engine: no attack, sustain 10
	sta V1+5
	lda #$a0
	sta V1+6
	lda #$00
	sta V1+2
	lda #$04			; pulse width 1024 of 4096
	sta V1+3
	lda #$41
	sta V1+4
	lda #$00			; the boost: attack 0 decay 0, sustain 6,
	sta V2+5			;   release 6
	lda #$66
	sta V2+6
	lda #$00			; the squeal's pulse: a thin one
	sta V2+2
	lda #$02
	sta V2+3
	lda #0
	sta fxLeft
	sta sfBoost
	sta sfBm
	sta pThr
	sta pBoost
	lda cDmg
	sta sfDmg
	lda cGnd
	sta sfGnd
	lda cLap
	sta sfLap
	lda raceSt
	sta sfSt
	rts

; Every voice off: the end of a race, before the music comes back.
sfxOff
	lda #0
	ldx #$18
-	sta SID,x
	dex
	bpl -
	rts

; Once a frame, after the physics.
sfxTick
	; --- the engine -------------------------------------------------
	lda cState
	cmp #S_WRECK
	bne +
	lda #$40			; wrecked: the gate off, it dies away
	sta V1+4
	jmp sfBoostV
+	lda cVHi			; speed * 20
	sta sfT
	lda #0
	sta sfT+1
	asl sfT				; *4
	rol sfT+1
	asl sfT
	rol sfT+1
	lda sfT				; *5
	clc
	adc cVHi
	sta sfT
	bcc +
	inc sfT+1
+	clc				; ... and *4 again: *20
	lda sfT
	asl a
	rol sfT+1
	asl a
	rol sfT+1
	adc #<ENG_BASE
	sta sfT
	lda sfT+1
	adc #>ENG_BASE
	sta sfT+1
	lda cGnd			; off the ground the wheels spin free
	bne +
	lda sfT
	clc
	adc #<ENG_AIR
	sta sfT
	lda sfT+1
	adc #>ENG_AIR
	sta sfT+1
+	lda pThr			; and the throttle lifts it a little
	cmp #1
	bne +
	lda sfT
	clc
	adc #$40
	sta sfT
	bcc +
	inc sfT+1
+	lda sfT
	sta V1+0
	lda sfT+1
	sta V1+1
	lda #$41
	sta V1+4

sfBoostV
	; --- voice 2: the boost, or the tyres ----------------------------
	ldx #0
	lda raceSt
	cmp #R_RACE
	bne sfV2
	lda pBoost
	beq +
	lda cFuel
	beq +
	ldx #1				; the boost burning
	bne sfV2
+	lda pThr			; braking hard on the road: the tyres
	cmp #2
	bne sfV2
	lda cGnd
	beq sfV2
	lda cState
	bne sfV2
	lda cVHi
	cmp #SQUEAL_V
	bcc sfV2
	ldx #2
sfV2
	cpx #2				; a squeal wavers, every frame
	bne +
	inc sqPh
	lda sqPh
	and #3
	tay
	lda sqTab,y
	sta V2+1
	lda #0
	sta V2+0
+	cpx sfBoost
	beq sfFx
	stx sfBoost
	cpx #1
	bne +
	lda #$00
	sta V2+0
	lda #$38
	sta V2+1
	lda #$81
	sta V2+4
	bne sfFx
+	cpx #2
	bne +
	lda #$41
	sta V2+4
	bne sfFx
+	lda #$40			; off: gate down (the release is 6, so
	sta V2+4			;   it fades either way)

sfFx
	; --- voice 3: what happened since the last frame -----------------
	lda sfBm			; the cars touched: a crunch, scratch
	beq +				;   or not, and while they rub one
	dec sfBm			;   every HIT_GAP frames
+	lda bmEvt
	beq +
	lda sfBm
	bne +
	lda #HIT_GAP
	sta sfBm
	ldx #FX_HIT
	jsr fxStart
+	lda cDmg
	sec
	sbc sfDmg
	beq sfNoDmg
	ldx #FX_THUMP
	cmp #8
	bcc +
	ldx #FX_CRASH
+	lda cDmg
	cmp #$ff
	bne +
	ldx #FX_WRECK
+	jsr fxStart
	jmp sfLand
sfNoDmg
	lda cGnd			; down without a scratch
	beq sfLand
	lda sfGnd
	bne sfLand
	ldx #FX_LAND
	jsr fxStart
sfLand
	lda raceSt
	cmp #R_COUNT
	bne sfNotCount
	lda raceT			; the count: three pips, then GO
	cmp #COUNT_FRAMES - 1
	beq +
	cmp #120
	beq +
	cmp #60
	bne sfLapV
+	ldx #FX_BEEP
	jsr fxStart
	jmp sfLapV
sfNotCount
	cmp #R_RACE
	bne sfLapV
	lda sfSt
	cmp #R_COUNT
	bne sfLapV
	ldx #FX_GO
	jsr fxStart
sfLapV
	lda cLap
	cmp sfLap
	beq sfEnd
	cmp #0				; $ff -> 0 is the start, not a lap
	beq sfEnd
	ldx #FX_LAP
	jsr fxStart
sfEnd
	lda cDmg
	sta sfDmg
	lda cGnd
	sta sfGnd
	lda cLap
	sta sfLap
	lda raceSt
	sta sfSt

	; a falling sound falls, a frame at a time
	ldx fxCur
	lda fxSwp,x
	beq +
	sta sfT
	lda fxHi
	sec
	sbc sfT
	bcc +
	cmp #2
	bcc +
	sta fxHi
	sta V3+1
+
	; voice 3's gate comes off after its time
	lda fxLeft
	beq +
	dec fxLeft
	bne +
	lda fxCtl
	and #$fe
	sta V3+4
+	rts

; X = FX_*: voice 3 starts it. A louder sound is not cut by a softer one
; still sounding: a crash outranks a landing in the same moment.
fxStart
	lda fxLeft
	beq +
	cpx fxCur
	bcc +
	beq +
	rts
+	stx fxCur
	txa
	asl a
	asl a
	asl a
	tax
	lda #0				; the gate off first, so the envelope
	sta V3+4			;   starts again
	lda fxTab+0,x
	sta V3+0
	lda fxTab+1,x
	sta V3+1
	sta fxHi
	lda fxTab+2,x
	sta V3+2
	lda fxTab+3,x
	sta V3+3
	lda fxTab+4,x
	sta V3+5
	lda fxTab+5,x
	sta V3+6
	lda fxTab+7,x
	sta fxLeft
	lda fxTab+6,x
	sta fxCtl
	sta V3+4
	rts

; freq lo/hi, pulse lo/hi, AD, SR, control, frames gated. In rank order:
; a lower row is never cut by a higher one.
fxTab	.byte $00, $08, $00, $00, $0c, $00, $81, 4	; wrecked: the long one
	.byte $00, $0c, $00, $00, $0a, $00, $81, 2	; crash: noise, long fall
	.byte $00, $28, $00, $06, $09, $00, $81, 3	; hit: a crunch, falling
	.byte $00, $06, $00, $00, $06, $00, $81, 1	; thump: a scrape
	.byte $00, $03, $00, $00, $05, $00, $81, 1	; landing: low thud
	.byte $00, $38, $00, $08, $00, $a0, $41, 20	; GO
	.byte $00, $1c, $00, $08, $00, $a0, $41, 8	; count pip
	.byte $00, $2a, $00, $08, $00, $a0, $11, 12	; lap

; voice 3's fall a frame, per sound (fxTab's order)
fxSwp	.byte 0, 0, 4, 0, 0, 0, 0, 0
; the squeal's four pitches
sqTab	.byte $2a, $2e, $2b, $30

sfT	.word 0
sfBm	.byte 0
sqPh	.byte 0
fxHi	.byte 0
sfDmg	.byte 0
sfGnd	.byte 0
sfLap	.byte 0
sfSt	.byte 0
sfBoost	.byte 0
fxLeft	.byte 0
fxCur	.byte 0
fxCtl	.byte 0

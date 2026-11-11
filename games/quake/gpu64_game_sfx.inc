; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; gpu64_game_sfx.inc - sound effects on the C64's SID.
;
; Three voices, played from the KERNAL's own 60 Hz IRQ: sfxIrq runs ahead
; of $EA31 on the vector at $0314, so the game pays one short handler a
; tick and nothing on the frame. The title's music has its own IRQ and is
; gone before this one is installed (tiMusicOff), so the two never share
; the chip.
;
; While installed, timer A runs MOUSE_SUBS times faster so the mouse is
; sampled that often; only every MOUSE_SUBS-th IRQ -- the jiffy -- plays
; the effects. None goes on to $EA31: every IRQ is acked and leaves by
; $EA81, because the KERNAL's keyboard scan would disconnect the mouse's
; port right before a sample, and the game reads neither its keys nor its
; clock. sfxOff puts the KERNAL's jiffy and IRQ back.
;
; An effect is a priority, an envelope, a pulse width and a run of
; segments. A segment is a duration in ticks, a control byte (waveform and
; gate), a start frequency and a signed per-tick slide. A zero duration
; ends the run, and the voice's gate is dropped so the release plays out.
; The segment tables are column-major and indexed by one byte, so the IRQ
; needs no zero page (the game's pointers live there) and no self-
; modifying code.
;
; sfxPlay, A = SFX_*: the voice already playing that effect restarts it
; (a nailgun does not eat all three voices); otherwise a free voice; else
; the lowest-priority voice, if the new effect is at least as important.
; Otherwise the effect is dropped. X and Y survive, as do the flags.
;
; Frequencies are SID register values: Hz x 17.03 on PAL. A noise
; waveform's frequency is its clock, i.e. how bright the hiss is.
; ======================================================================

SFX_SHOT     = 0
SFX_SSHOT    = 1
SFX_NAIL     = 2
SFX_AXE      = 3
SFX_HEALTH   = 4
SFX_ITEM     = 5
SFX_WEAPON   = 6
SFX_POWER    = 7
SFX_PAIN     = 8
SFX_DIE      = 9
SFX_MPAIN    = 10
SFX_MDIE     = 11
SFX_GRUNT    = 12
SFX_BITE     = 13
SFX_LEAP     = 14
SFX_SIGHT    = 15
SFX_DOOR     = 16
SFX_BUTTON   = 17
SFX_SECRET   = 18
SFX_JUMP     = 19
SFX_SPLASH   = 20
SFX_BOOM     = 21
SFX_COUNT    = 22

; (ticks, control, frequency, slide per tick)
SG_SHOT   = [(3, $81, $3000, -$0600), (12, $80, $1200, -$0080)]
SG_SSHOT  = [(4, $81, $2800, -$0400), (18, $80, $1000, -$0050)]
SG_NAIL   = [(1, $81, $5800, 0), (2, $41, $1400, -$0300), (1, $40, $0e00, 0)]
SG_AXE    = [(5, $81, $0600, $0200), (6, $80, $1000, -$0180)]
SG_HEALTH = [(3, $11, $1a00, 0), (3, $11, $2200, 0), (6, $11, $2b00, 0)]
SG_ITEM   = [(2, $41, $2600, 0), (4, $41, $3300, 0)]
SG_WEAPON = [(3, $41, $1100, 0), (3, $41, $1600, 0), (3, $41, $1a00, 0), (8, $41, $2200, -$0010)]
SG_POWER  = [(4, $21, $0800, $0180), (4, $21, $0e00, $0180), (4, $21, $1400, $0180), (16, $21, $1a00, -$0040)]
SG_PAIN   = [(8, $21, $0c00, -$0050), (2, $20, $0900, 0)]
SG_DIE    = [(24, $21, $0c00, -$0020), (40, $20, $0900, -$000c)]
SG_MPAIN  = [(6, $21, $1000, -$0090), (2, $20, $0c00, 0)]
SG_MDIE   = [(3, $81, $1800, -$0200), (18, $21, $0900, -$0030), (2, $20, $0500, 0)]
SG_GRUNT  = [(3, $81, $2400, -$0500), (10, $80, $1000, -$0060)]
SG_BITE   = [(2, $81, $6000, -$2000), (3, $80, $2000, 0)]
SG_LEAP   = [(8, $21, $0500, $0020), (8, $21, $0600, -$0020), (2, $20, $0500, 0)]
SG_SIGHT  = [(5, $21, $0e00, $0080), (6, $21, $1100, -$0100), (2, $20, $0b00, 0)]
SG_DOOR   = [(40, $81, $0280, 0), (4, $80, $0280, 0)]
SG_BUTTON = [(2, $41, $3000, 0), (1, $40, $3000, 0)]
SG_SECRET = [(6, $11, $22cb, 0), (6, $11, $2bd8, 0), (6, $11, $3426, 0), (20, $11, $4597, 0)]
SG_JUMP   = [(5, $11, $0a00, $0180), (1, $10, $1100, 0)]
SG_SPLASH = [(10, $81, $1800, -$0100), (6, $80, $0e00, 0)]
SG_BOOM   = [(4, $81, $0a00, -$0100), (30, $80, $0600, -$0010)]

; (priority, AD, SR, pulse width high, segments), in SFX_* order. Priority
; 0 would mean "free", so every effect has at least 1.
SOUNDS = [(6, $09, $08, 0, SG_SHOT), (7, $0a, $09, 0, SG_SSHOT), (3, $05, $00, 8, SG_NAIL), (2, $44, $00, 0, SG_AXE), (4, $06, $a6, 0, SG_HEALTH), (4, $04, $84, 8, SG_ITEM), (5, $06, $a6, 8, SG_WEAPON), (6, $08, $a8, 0, SG_POWER), (6, $08, $00, 0, SG_PAIN), (9, $0b, $0b, 0, SG_DIE), (4, $07, $00, 0, SG_MPAIN), (5, $0a, $09, 0, SG_MDIE), (5, $09, $08, 0, SG_GRUNT), (4, $06, $00, 0, SG_BITE), (4, $2a, $a6, 0, SG_LEAP), (3, $28, $a4, 0, SG_SIGHT), (2, $4a, $a8, 0, SG_DOOR), (3, $03, $00, 8, SG_BUTTON), (8, $09, $a9, 0, SG_SECRET), (2, $06, $00, 0, SG_JUMP), (3, $0a, $08, 0, SG_SPLASH), (7, $0a, $0a, 0, SG_BOOM)]

.cerror len(SOUNDS) != SFX_COUNT, "SOUNDS and SFX_* disagree"

; The runs end to end, each closed by a zero-duration row, and where each
; one starts.
SFX_ROWS := []
SFX_FIRST := []
.for si := 0, si < len(SOUNDS), si += 1
SFX_FIRST := SFX_FIRST .. [len(SFX_ROWS)]
SFX_ROWS := SFX_ROWS .. SOUNDS[si][4] .. [(0, 0, 0, 0)]
.next
.cerror len(SFX_ROWS) > 256, "the segment tables outgrew one index byte"

SID_VOL = $d418

; ----------------------------------------------------------------------
; Silence, full volume, and the IRQ hooked. Idempotent: a second call does
; not save its own handler as the one to chain to.
; ----------------------------------------------------------------------
sfxInit
	php
	sei
	jsr mouseReset			; nothing was sampled while it was out
	ldx #$18
	lda #0
-	sta $d400,x
	dex
	bpl -
	sta sfxPri
	sta sfxPri+1
	sta sfxPri+2
	lda #$0f
	sta SID_VOL
	lda $0314
	cmp #<sfxIrq
	bne +
	lda $0315
	cmp #>sfxIrq
	beq siDone
+	lda $0314
	sta sfxOld
	lda $0315
	sta sfxOld+1
	lda #<sfxIrq
	sta $0314
	lda #>sfxIrq
	sta $0315
siDone
	lda #MOUSE_SUBS - 1		; the next IRQ is not a jiffy ...
	sta sfxSub
	ldx #<( TIMER_PAL / MOUSE_SUBS )	; ... and they come this much faster
	ldy #>( TIMER_PAL / MOUSE_SUBS )
	lda $02a6			; 1 = PAL
	bne +
	ldx #<( TIMER_NTSC / MOUSE_SUBS )
	ldy #>( TIMER_NTSC / MOUSE_SUBS )
+	stx $dc04
	sty $dc05
	plp
	rts

; Unhooked and silent: finish's half.
sfxOff
	php
	sei
	lda $0314
	cmp #<sfxIrq
	bne +
	lda $0315
	cmp #>sfxIrq
	bne +
	lda sfxOld
	sta $0314
	lda sfxOld+1
	sta $0315
+	ldx #<TIMER_PAL			; the KERNAL's own jiffy again
	ldy #>TIMER_PAL
	lda $02a6
	bne +
	ldx #<TIMER_NTSC
	ldy #>TIMER_NTSC
+	stx $dc04
	sty $dc05
	ldx #$18
	lda #0
-	sta $d400,x
	dex
	bpl -
	plp
	rts

; ----------------------------------------------------------------------
; A = SFX_*. Preserves X, Y and the flags; A is spent.
; ----------------------------------------------------------------------
sfxPlay
	php
	sei
	stx sfxSx
	sty sfxSy
	sta sfxId
	tay
	; The same effect, already playing: start it over there.
	ldx #2
-	lda sfxPri,x
	beq +
	lda sfxCur,x
	cmp sfxId
	beq sfpTake
+	dex
	bpl -
	; A free voice.
	ldx #2
-	lda sfxPri,x
	beq sfpTake
	dex
	bpl -
	; The least important one, if this is at least as important.
	ldx #2
	lda sfxPri+2
	cmp sfxPri+1
	bcc +
	ldx #1
	lda sfxPri+1
+	cmp sfxPri
	bcc +
	ldx #0
	lda sfxPri
+	cmp sfxPriT,y
	beq sfpTake
	bcs sfpDone
sfpTake
	lda sfxPriT,y
	sta sfxPri,x
	lda sfxId
	sta sfxCur,x
	lda sfxFirst,y
	sta sfxSeg,x
	lda #0
	sta sfxTick,x			; the next tick loads the first segment
	lda sidOff,x
	tax
	lda #$08			; test bit: gate off, oscillator reset
	sta $d404,x
	lda sfxAdT,y
	sta $d405,x
	lda sfxSrT,y
	sta $d406,x
	lda #0
	sta $d402,x
	lda sfxPwT,y
	sta $d403,x
sfpDone
	ldx sfxSx
	ldy sfxSy
	plp
	rts

; ----------------------------------------------------------------------
; The tick. Entered from the KERNAL's IRQ entry, A/X/Y already saved; ends
; at the KERNAL's own exit, $EA81, after the CIA's ack.
; ----------------------------------------------------------------------
sfxIrq
	cld
	dec sfxSub
	jsr mouseIrq			; first: a jiffy past the last keyboard scan
	lda sfxSub
	bmi +
	jmp sqOut			; a mouse-only IRQ
+	lda #MOUSE_SUBS - 1
	sta sfxSub
	ldx #2
sqVoice
	lda sfxPri,x
	beq sqNext
	lda sfxTick,x
	bne sqSlide
	ldy sfxSeg,x
	lda sfxDurT,y
	beq sqEnd
	sta sfxTick,x
	lda sfxFloT,y
	sta sfxFlo,x
	lda sfxFhiT,y
	sta sfxFhi,x
	lda sfxSloT,y
	sta sfxSlo,x
	lda sfxShiT,y
	sta sfxShi,x
	inc sfxSeg,x
	lda sfxCtlT,y
	sta sfxCtl,x
	ldy sidOff,x
	sta $d404,y
	jmp sqWrite

sqSlide
	ldy sidOff,x
	clc
	lda sfxFlo,x
	adc sfxSlo,x
	sta sfxFlo,x
	lda sfxFhi,x
	adc sfxShi,x
	sta sfxFhi,x
	; A slide that runs off either end stops there rather than wrapping
	; into a squeal: down and no carry is below 0, up and a carry is past
	; $ffff.
	lda sfxShi,x
	bmi +
	bcc sqWrite
	lda #$ff
	bne sqClamp
+	bcs sqWrite
	lda #0
sqClamp
	sta sfxFlo,x
	sta sfxFhi,x

sqWrite
	dec sfxTick,x
	lda sfxFlo,x
	sta $d400,y
	lda sfxFhi,x
	sta $d401,y
sqNext
	dex
	bpl sqVoice
sqOut
	lda $dc0d			; the ack, as $EA31 would have
	jmp $ea81

sqEnd
	lda #0
	sta sfxPri,x
	ldy sidOff,x
	lda sfxCtl,x
	and #$fe			; the gate: the release plays out
	sta $d404,y
	jmp sqNext

; ----------------------------------------------------------------------
; Tables.
; ----------------------------------------------------------------------
sidOff		.byte 0, 7, 14

sfxPriT
.for si := 0, si < len(SOUNDS), si += 1
	.byte SOUNDS[si][0]
.next
sfxAdT
.for si := 0, si < len(SOUNDS), si += 1
	.byte SOUNDS[si][1]
.next
sfxSrT
.for si := 0, si < len(SOUNDS), si += 1
	.byte SOUNDS[si][2]
.next
sfxPwT
.for si := 0, si < len(SOUNDS), si += 1
	.byte SOUNDS[si][3]
.next
sfxFirst
.for si := 0, si < len(SOUNDS), si += 1
	.byte SFX_FIRST[si]
.next

sfxDurT
.for si := 0, si < len(SFX_ROWS), si += 1
	.byte SFX_ROWS[si][0]
.next
sfxCtlT
.for si := 0, si < len(SFX_ROWS), si += 1
	.byte SFX_ROWS[si][1]
.next
sfxFloT
.for si := 0, si < len(SFX_ROWS), si += 1
	.byte <SFX_ROWS[si][2]
.next
sfxFhiT
.for si := 0, si < len(SFX_ROWS), si += 1
	.byte >SFX_ROWS[si][2]
.next
sfxSloT
.for si := 0, si < len(SFX_ROWS), si += 1
	.byte <(SFX_ROWS[si][3] & $ffff)
.next
sfxShiT
.for si := 0, si < len(SFX_ROWS), si += 1
	.byte >(SFX_ROWS[si][3] & $ffff)
.next

; ----------------------------------------------------------------------
; State.
; ----------------------------------------------------------------------
sfxOld		.word $ea31
sfxSub		.byte 0			; IRQs to the next jiffy, less one
sfxId		.byte 0
sfxSx		.byte 0
sfxSy		.byte 0
sfxPri		.fill 3, 0		; 0 = free
sfxCur		.fill 3, 0		; which effect
sfxSeg		.fill 3, 0		; the next segment row
sfxTick		.fill 3, 0		; ticks left in this one
sfxCtl		.fill 3, 0
sfxFlo		.fill 3, 0
sfxFhi		.fill 3, 0
sfxSlo		.fill 3, 0
sfxShi		.fill 3, 0

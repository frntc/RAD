;
;       {_______            {_          {______
;             {__          {_ __               {__
;             {__         {_  {__               {__
;          {__           {__   {__               {__
;      {______          {__     {__              {__
;            {__       {__       {__            {__
;              {_________         {______________		Expansion Unit
;
;      gpu64 -- small runtime shared by the demonstration programs.
;
;      Included once, at the END of a demo (it contains code and data, not
;      macros). It holds the housekeeping every demo needs and none of the
;      gpu64 API: text on the C64's own screen, a hex printer, the STOP key,
;      and the two vblank helpers whose only job is to keep working on a
;      display gpu64 could not measure a frame period from.
;
;      THE ONE THING WORTH READING HERE
;
;      dmVbWait / dmFlip are written so that the same demo runs on a display
;      with a frame clock and on one without. gpu64 reports the measured
;      frame period in the info block, and 0 means every vblank feature will
;      answer UNSUPPORTED. dmInit reads that once into dmVbOk; the two
;      helpers then either sync to the display or free-run. A real program
;      wants the same fallback -- an animation that refuses to run at all
;      because it cannot be tear-free is worse than one that tears.
;
;      Copyright (c) 2026 Honza Slesinger
;
;      This program is free software: you can redistribute it and/or modify
;      it under the terms of the GNU General Public License as published by
;      the Free Software Foundation, either version 3 of the License, or
;      (at your option) any later version.
;

; ----------------------------------------------------------------------
; Start-up: clear the C64 screen, put gpu64 in a known state, and find out
; whether this display has a usable frame clock.
; ----------------------------------------------------------------------
dmInit
	ldx #0
	lda #$20
dmiCls
	sta SCREEN,x
	sta SCREEN+$100,x
	sta SCREEN+$200,x
	sta SCREEN+$300,x
	inx
	bne dmiCls

	; CMD_HI is sticky and survives RUN/STOP, so a previous program can
	; have left it pointing at another class. Class 0 is the reset value
	; and everything in these demos lives there.
	lda #0
	sta CMD_HI

	; EVERYTHING gpu64 holds is sticky across a program launch. The Pi is
	; not reset when the C64 loads a new .prg -- only a C64 /RESET
	; re-baselines it -- so a demo started from the RAD menu inherits the
	; previous demo's palette, its draw/visible page pairing, its border,
	; its display mode and the pixels it left in every page. Two of those
	; were seen at the bench on 2026-09-09 and both are reproducible on a
	; PC with runsim.py's --chain:
	;
	;   bounce after palette   -- wrong colours. bounce assumes the C64
	;                             palette; palette had loaded a 256-entry
	;                             ramp over it.
	;   palette after bounce   -- palette's own rings never appear and
	;                             bounce's rectangles cycle instead.
	;                             bounce ends with draw page != visible
	;                             page, so palette's CLEAR and its rings
	;                             land on a page nobody is looking at,
	;                             while its PAL_LOAD recolours the page
	;                             that IS visible -- which still holds
	;                             bounce's last frame.
	;
	; FULL_RESET is exactly the "as if the Pi had just booted" this needs:
	; graphics mode, the C64 palette, border 0, page 0 both drawn and
	; visible, every page black. It is a setup-time command -- it can halt
	; the C64 for up to ~20 ms when it has to leave text mode -- which is
	; why it belongs here, once, and never in a frame loop. It also
	; subsumes the TEXT_MODE 0 this used to issue.
	;
	; A firmware too old to have $0B answers BAD_OPCODE and changes
	; nothing, so the TEXT_MODE 0 below stays as the fallback rather than
	; being deleted.
	#cmd OP_FULL_RESET

	; Text mode is sticky in the same way, and it is a DISPLAY mode: a
	; program that left the 80x50 character screen up would otherwise have
	; every drawing command here land in a framebuffer that is not on the
	; display, and every class 1 or class 2 command answer UNSUPPORTED.
	; Asking for graphics mode while already in it costs one dispatch and
	; changes nothing, so this is unconditional.
	#argb 0, 0
	#cmd OP_TEXT_MODE

	; GET_INFO does not answer through a register: it DMA-writes the
	; 16-byte info block into memory you nominate. Bytes 14-15 are the
	; measured frame period in microseconds, and 0 means "no frame clock".
	;
	; This is a writeback, so it needs the same two guards every other
	; readback has, and until 2026-09-20 it had neither -- it was the
	; first thing every demo did, and it ran completely unprotected.
	;
	;   The fence.  The destination travels in ARG1-ARG3, which SEQACK
	;   does not cover. One dropped store and the Pi writes sixteen bytes
	;   over whatever address the previous command left behind, with this
	;   program halted while it happens. quake3d's own SET_DMA_WINDOW was
	;   added for exactly this, but it ran AFTER dmInit -- its comment
	;   said "before anything that can write back into C64 memory" and
	;   one thing already had.
	;
	;   The check.  Nothing verified the block arrived. Run 29 came back
	;   with CLOCK 0000 while the firmware was demonstrably clocked (4078
	;   SCENE_COMMITs succeeded, and a commit hard-requires a calibrated
	;   frame clock), so that readback did not land in dmInfo -- and the
	;   run ended in a hang. An unfenced writeback that missed its
	;   destination is the one failure that explains both.
	;
	; The check is a real magic, not a poison: bytes 0-2 are 'G','6','4'
	; unconditionally, so "did the block arrive" needs no assumption about
	; what plausible data looks like.
	jsr dmSetWin
	lda #8
	sta dmInfoTry
dmiAgain
	lda #$5a
	ldx #15
-	sta dmInfo,x
	dex
	bpl -

	#argblob 0, SPACE_C64, dmInfo, 16
	#cmd OP_GET_INFO

	lda dmInfo+0
	cmp #71				; 'G'
	bne dmiRetry
	lda dmInfo+1
	cmp #54				; '6'
	bne dmiRetry
	lda dmInfo+2
	cmp #52				; '4'
	beq dmiGot
dmiRetry
	dec dmInfoTry
	bne dmiAgain
	; Eight tries and no info block. Leave dmInfo as poison -- the frame
	; period test below then reads $5a5a, which is a *plausible* period,
	; so zero it rather than letting garbage claim a frame clock.
	lda #0
	ldx #15
-	sta dmInfo,x
	dex
	bpl -
dmiGot

	; And hand the window back. A program that wants its own fence sets
	; one (quake3d does); one that does not is left exactly as permissive
	; as it was before this routine existed, rather than silently confined
	; to dmInfo for the rest of its life.
	lda #0
	sta CMD_HI
	#argw 0, 0
	#argw 2, 0
	lda #OP_SET_DMA_WINDOW
	sta CMD_LO

	lda #0
	sta dmVbOk
	lda dmInfo+14
	ora dmInfo+15
	beq dmiNoVb
	lda #1
	sta dmVbOk
dmiNoVb
	rts

; ----------------------------------------------------------------------
; Fence the info-block readback.
;
; RESULT is checked rather than trusted: ARG registers read back as $FF, so
; gpu64's one-byte XOR echo is the only evidence the window landed. Four
; tries, then carry on regardless -- refusing to start because a fence
; could not be confirmed is the run-27 mistake, and an unfenced GET_INFO is
; exactly as safe as it was for the whole project before today.
; ----------------------------------------------------------------------
DMINFO_CK = ( <dmInfo ) ^ ( >dmInfo ) ^ 16 ^ 0 ^ $a5

dmSetWin
	lda #4
	sta dmWinTry
dmwAgain
	lda #0
	sta CMD_HI
	#argw 0, dmInfo
	#argw 2, 16
	lda #OP_SET_DMA_WINDOW
	sta CMD_LO
	lda RESULT
	cmp #DMINFO_CK
	beq dmwOk
	dec dmWinTry
	bne dmwAgain
dmwOk	rts

; ----------------------------------------------------------------------
; Frame pacing.
;
; The polling form of the vblank signal: STATUS bit2 sets once per display
; frame and stays set until VBLANK_ACK clears it. No IRQ handler needed --
; this is usable from BASIC.
; ----------------------------------------------------------------------
dmVbWait
	lda dmVbOk
	beq dmvFree			; no frame clock: free-run instead
dmvPoll
	lda STATUS
	and #ST_VB_PEND
	beq dmvPoll
	#cmd0 OP_VBLANK_ACK
	rts
dmvFree
	; Nothing to sync to. Burn a little time so the animation is not
	; running at the CPU's own speed, and carry on.
	ldx #0
dmvDelay
	dex
	bne dmvDelay
	rts

; Make the drawn page visible. With a frame clock this is a deferred flip,
; which lands on a real frame boundary and cannot tear; without one it is
; an immediate flip, which can. Either way the wait on STATUS bit0 is a
; register spin, not a halt: the C64 is free to do something else instead.
;
; The opcode is written with #cmd0, not #cmd: CMD_HI is sticky, and a
; caller that was last in class 2 would otherwise fire $05 as a class 2
; opcode -- which is SET_CAMERA, and which flips nothing at all.
dmFlip
	lda dmVbOk
	beq dmfNow
	#argb 0, 1			; 1 = at next vblank
	#cmd0 OP_PAGE_FLIP
dmfWait
	lda STATUS
	and #ST_BUSY
	bne dmfWait
	rts
dmfNow
	#argb 0, 0			; 0 = immediately
	#cmd0 OP_PAGE_FLIP
	rts

; ----------------------------------------------------------------------
; The C64's own screen. Demos report on it, so a bench tester can read the
; result without also reading the HDMI log.
; ----------------------------------------------------------------------

; A = row (0-24). Sets the cursor (DST) to the start of it.
;
; A table, not a multiply: 25 rows is small, and the bounds check below is
; here because a row number past the end of a table like this one does not
; fail loudly -- it writes the text into the program instead, and the demo
; then looks like it never ran.
dmRow
	cmp #25
	bcc +
	lda #24
+
	tax
	lda dmRowLo,x
	sta DST
	lda dmRowHi,x
	sta DST+1
	rts

dmRowLo
	.for r = 0, r < 25, r = r + 1
	.byte <(SCREEN + r * 40)
	.next
dmRowHi
	.for r = 0, r < 25, r = r + 1
	.byte >(SCREEN + r * 40)
	.next

; Print the string that follows the JSR, at the cursor. The text sits where
; it is used, so a demo needs no string table.
dmText
	pla
	sta SRC
	pla
	sta SRC+1
	inc SRC
	bne +
	inc SRC+1
+
	ldy #0
dmtLoop
	lda (SRC),y
	beq dmtEnd
	jsr dmScreenCode
	sta (DST),y
	iny
	bne dmtLoop
dmtEnd
	tya
	clc
	adc DST
	sta DST
	bcc +
	inc DST+1
+
	tya
	sec				; +1 for the terminator
	adc SRC
	sta SRC
	bcc +
	inc SRC+1
+
	jmp (SRC)

; A = byte, printed as two hex digits at the cursor. Clobbers Y -- read
; every byte you are about to print BEFORE you print the first one.
dmHexA
	pha
	lsr a
	lsr a
	lsr a
	lsr a
	jsr dmHexDigit
	ldy #0
	sta (DST),y
	pla
	and #$0f
	jsr dmHexDigit
	ldy #1
	sta (DST),y
	clc
	lda DST
	adc #2
	sta DST
	bcc +
	inc DST+1
+
	rts

dmHexDigit
	cmp #10
	bcc dmhDec
	sec
	sbc #9				; 'A'..'F' are screen codes $01..$06
	rts
dmhDec
	clc
	adc #$30			; '0'..'9' are $30..$39
	rts

; A = byte, printed as three DECIMAL digits at the cursor, leading zeros
; kept so the column does not move. Hex is the right format for a register
; and the wrong one for a measurement: these numbers are read off a
; photograph of a CRT and compared against each other, and a reader who has
; to convert $3C to 60 before knowing whether the frame was slow will not.
dmDecA
	ldx #$ff
	sec
ddaHun
	inx
	sbc #100
	bcs ddaHun
	adc #100
	pha
	txa
	jsr dmDecDigit
	pla
	ldx #$ff
	sec
ddaTen
	inx
	sbc #10
	bcs ddaTen
	adc #10
	pha
	txa
	jsr dmDecDigit
	pla
	; falls into the units digit

dmDecDigit
	clc
	adc #$30			; '0'..'9' are screen codes $30..$39
	ldy #0
	sta (DST),y
	inc DST
	bne +
	inc DST+1
+
	rts

; PETSCII upper case to screen code; digits, space and punctuation pass
; through unchanged, which covers everything these demos print.
dmScreenCode
	cmp #$41
	bcc dmsDone
	cmp #$5b
	bcs dmsDone
	sec
	sbc #$40
dmsDone
	rts

; ERRCODE at the cursor, as "OK" or as the code itself. Demos print this
; once at the end: every dispatch writes ERRCODE, so if the last one says
; OK and something looked wrong on the HDMI screen, the argument block was
; wrong rather than the command.
dmErr
	lda ERRCODE
	beq dmeOk
	pha
	ldy #0
	lda #$05			; 'E'
	sta (DST),y
	iny
	lda #$12			; 'R'
	sta (DST),y
	clc
	lda DST
	adc #2
	sta DST
	bcc +
	inc DST+1
+
	pla
	jmp dmHexA
dmeOk
	ldy #0
	lda #$0f			; 'O'
	sta (DST),y
	iny
	lda #$0b			; 'K'
	sta (DST),y
	rts

; ----------------------------------------------------------------------
; Ending.
; ----------------------------------------------------------------------

; Z set when the STOP key is down. Animation loops end on this.
dmStop
	jmp $ffe1

; Hold the C64 screen until STOP, then clear it -- returning straight to
; BASIC would print READY. over the last line.
dmHold
	jsr $ffe1
	bne dmHold
	jsr $e544			; KERNAL clear screen
	lda #0
	sta CMD_HI
	rts

; A cheap 8-bit LFSR. Demos that want scattered values use it so that two
; runs look the same, which matters when a bench tester is comparing a
; photograph against what the host renderer produced.
dmRand
	lda dmSeed
	beq dmrZero
	asl a
	beq dmrNoEor
	bcc dmrNoEor
dmrEor
	eor #$1d
dmrNoEor
	sta dmSeed
	rts
dmrZero
	lda #$1d
	sta dmSeed
	rts

; ----------------------------------------------------------------------
; Runtime data.
; ----------------------------------------------------------------------
dmVbOk	.byte 0				; 1 = this display has a frame clock
dmSeed	.byte $a5
dmInfo	.fill 16, 0			; the info block GET_INFO wrote
dmInfoTry .byte 0			; GET_INFO attempts left when it landed
dmWinTry .byte 0			; SET_DMA_WINDOW attempts left

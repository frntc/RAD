;
;      gpu64 -- API conformance suite: the harness runtime.
;
;      Included ONCE, at the end of each gpu64_test_*.a, after the tests.
;      Equates and macros live in gpu64_testkit.inc; this is the code they
;      call. Split in two so a test program reads as tests, with the
;      plumbing out of the way below it.
;
;      OUTPUT LAYOUT
;
;        row 0        title
;        rows 2-22    results, two columns of 20 -- 42 slots
;        row 23       how to leave
;        row 24       counts and the verdict
;
;      A result line is a name, dots, and either OK or F<hex>. The dots are
;      not decoration: they make a column of results scannable without
;      reading a single name, which is the only way a 42-line screen is
;      readable at bench speed.
;
;      Copyright (c) 2026 Honza Slesinger
;
;      This program is free software: you can redistribute it and/or modify
;      it under the terms of the GNU General Public License as published by
;      the Free Software Foundation, either version 3 of the License, or
;      (at your option) any later version.
;

; ----------------------------------------------------------------------
; Dispatch.
;
; A command write halts the C64 until the command finishes -- gpu64 holds
; the bus exactly the way an REU transfer does -- so there is no polling
; and no settling delay here, and none is needed. Staging ARG bytes and
; firing CMD_LO back to back always works.
; ----------------------------------------------------------------------

; A = opcode. Returns A = ERRCODE, flags set from it.
cmd0
	ldx #0
	stx CMD_HI
	sta CMD_LO
	lda ERRCODE
	rts

cmd1
	ldx #1
	stx CMD_HI
	sta CMD_LO
	lda ERRCODE
	rts

; Class 2, the raster layer. Same three writes -- CMD_HI is sticky, so a
; program that stays in one class only pays for it once; this sets it every
; time so a test can interleave classes without bookkeeping.
cmd2
	ldx #2
	stx CMD_HI
	sta CMD_LO
	lda ERRCODE
	rts

; A = ERRCODE, Y = the code the test demands. X = 0 on a match. On a
; mismatch X is the code that actually came back -- except when that is OK,
; which becomes $FE, because a 0 fail code is how this harness spells
; "passed" and an error path that quietly succeeded must never read as one.
kitExpect
	sty kitTmp
	cmp kitTmp
	beq keOk
	tax
	bne keDone
	ldx #F_NOERR
keDone
	rts
keOk
	ldx #0
	rts

; A = a value already reduced to "the thing under test". kitWantSet passes
; when it is nonzero, kitWantClear when it is zero. Both exist as routines
; rather than as inline branches because a fail code has to end up in X and
; doing that inline needs two forward labels, which reads worse than a call.
kitWantSet
	bne kwSetOk
	ldx #F_RANGE
	rts
kwSetOk
	ldx #0
	rts

kitWantClear
	bne kwClrBad
	ldx #0
	rts
kwClrBad
	ldx #F_RANGE
	rts

; A = a byte that came back wrong. Records it as the fail code unless one
; is already recorded -- the first failure in a test is the informative one.
kitBad
	ldx accFail
	bne kbDone
	tax
	bne kbStore
	ldx #F_PIXEL
kbStore
	stx accFail
kbDone
	rts

; ----------------------------------------------------------------------
; Reporting.
;
; The name follows the JSR inline, so the return address points at it. Each
; of these three walks the string, prints it, and resumes at the byte after
; the terminator -- which is exactly where SRC ends up, so the resume is a
; plain indirect jump and costs nothing.
; ----------------------------------------------------------------------

kitCheck
	stx failCode
	pla
	sta SRC
	pla
	sta SRC+1
	inc SRC
	bne +
	inc SRC+1
+
	jsr kitSlotAddr
	bcs kcNoRoom
	jsr kitPrintStr
	jsr kitPad
	jsr kitResult
	jmp kcCount
kcNoRoom
	jsr kitSkipStr
kcCount
	jsr kitAdvSlot
	lda failCode
	beq kcPass
	inc failCnt
	jmp (SRC)
kcPass
	inc passCnt
	jmp (SRC)

; A line with no verdict, for a measurement that is reported rather than
; judged. It consumes a slot so it stays in reading order with the tests
; around it.
kitNote
	pla
	sta SRC
	pla
	sta SRC+1
	inc SRC
	bne +
	inc SRC+1
+
	jsr kitSlotAddr
	bcs knNoRoom
	jsr kitPrintStr
	jsr kitAdvSlot
	jmp (SRC)
knNoRoom
	jsr kitSkipStr
	jsr kitAdvSlot
	jmp (SRC)

kitTitle
	pla
	sta SRC
	pla
	sta SRC+1
	inc SRC
	bne +
	inc SRC+1
+
	lda #0
	jsr kitRowAddr
	jsr kitPrintLong
	jmp (SRC)

; A slot that carries a measured 16-bit value instead of a verdict. Some
; things a bench run needs are numbers, not judgements -- the frame period
; is the standing example: "nonzero" is the pass condition, but the actual
; microseconds are what tell you whether the display came up PAL or NTSC.
kitShow
	pla
	sta SRC
	pla
	sta SRC+1
	inc SRC
	bne +
	inc SRC+1
+
	jsr kitSlotAddr
	bcs ksNoRoom
	jsr kitPrintStr
	jsr kitPad
	lda kitVal16+1
	jsr kitPutHex
	lda kitVal16
	jsr kitPutHex
	jmp ksCount
ksNoRoom
	jsr kitSkipStr
ksCount
	jsr kitAdvSlot
	jmp (SRC)

; ----------------------------------------------------------------------
; Slots.
; ----------------------------------------------------------------------

kitSlotAddr
	lda slotN
	cmp #42
	bcs ksaFull
	lda slotPtr
	sta DST
	lda slotPtr+1
	sta DST+1
	clc
	rts
ksaFull
	sec
	rts

kitAdvSlot
	inc slotN
	lda slotN
	cmp #21
	bne asAdd
	; Column one is full; start column two at the same top row.
	lda #<(SCREEN+2*40+20)
	sta slotPtr
	lda #>(SCREEN+2*40+20)
	sta slotPtr+1
	rts
asAdd
	clc
	lda slotPtr
	adc #40
	sta slotPtr
	lda slotPtr+1
	adc #0
	sta slotPtr+1
	rts

; ----------------------------------------------------------------------
; Text.
; ----------------------------------------------------------------------

; (SRC) -> (DST), stopping at the terminator or 14 characters, whichever
; comes first. Leaves Y = characters written and SRC past the terminator.
kitPrintStr
	ldy #0
kpsLoop
	lda (SRC),y
	beq kpsEnd
	jsr kitScreenCode
	sta (DST),y
	iny
	cpy #14
	bcc kpsLoop
kpsSkip
	lda (SRC),y
	beq kpsEnd
	iny
	bne kpsSkip
kpsEnd
	jmp kitSrcPast

; Same, without the 14-character field limit -- the title row is 40 wide.
kitPrintLong
	ldy #0
kplLoop
	lda (SRC),y
	beq kitSrcPast
	jsr kitScreenCode
	sta (DST),y
	iny
	cpy #40
	bcc kplLoop
	jmp kpsSkip

kitSkipStr
	ldy #0
kssLoop
	lda (SRC),y
	beq kitSrcPast
	iny
	bne kssLoop

; SRC += Y + 1, i.e. past the terminator, which is the resume address.
kitSrcPast
	tya
	sec
	adc SRC
	sta SRC
	bcc +
	inc SRC+1
+
	rts

kitPad
	lda #$2e			; '.'
kpLoop
	cpy #15
	bcs kpDone
	sta (DST),y
	iny
	bne kpLoop
kpDone
	rts

kitResult
	lda failCode
	bne krFail
	lda #$0f			; screen code 'O'
	sta (DST),y
	iny
	lda #$0b			; 'K'
	sta (DST),y
	rts
krFail
	lda #$06			; 'F'
	sta (DST),y
	iny
	lda failCode
	jmp kitPutHex

; PETSCII upper case to screen code; everything else passes through, which
; covers the digits, the space and the punctuation these names use.
kitScreenCode
	cmp #$41
	bcc kscDone
	cmp #$5b
	bcs kscDone
	sec
	sbc #$40
kscDone
	rts

; A = row. DST = the start of it.
kitRowAddr
	sta kitTmp
	lda #<SCREEN
	sta DST
	lda #>SCREEN
	sta DST+1
	ldx kitTmp
	beq kraDone
kraAdd
	clc
	lda DST
	adc #40
	sta DST
	lda DST+1
	adc #0
	sta DST+1
	dex
	bne kraAdd
kraDone
	rts

; A = byte, printed as two hex digits at (DST),y. Advances Y past them.
kitPutHex
	pha
	lsr
	lsr
	lsr
	lsr
	jsr kitHexDigit
	sta (DST),y
	iny
	pla
	and #$0f
	jsr kitHexDigit
	sta (DST),y
	iny
	rts

kitHexDigit
	cmp #10
	bcs khdAlpha
	clc
	adc #$30
	rts
khdAlpha
	sec
	sbc #9
	rts

; A/Y = string, X = row. Printed at column 0.
kitMsg
	sta SRC
	sty SRC+1
	txa
	jsr kitRowAddr
	jmp kitPrintLong

; ----------------------------------------------------------------------
; Memory helpers. All three take the length in kitLen (16-bit).
; ----------------------------------------------------------------------

; Fills (P1) with kitVal.
kitFill
	ldy #0
	ldx kitLen+1
	beq kfTail
kfPage
	lda kitVal
	sta (P1),y
	iny
	bne kfPage
	inc P1+1
	dex
	bne kfPage
kfTail
	ldy #0
	cpy kitLen
	beq kfDone
kfByte
	lda kitVal
	sta (P1),y
	iny
	cpy kitLen
	bne kfByte
kfDone
	rts

; Fills (P1) with a non-uniform, deterministic pattern. Uniform test data
; is worthless for a copy path: a blit that dropped every second byte, or
; got the stride wrong, would still compare equal.
;
; A Galois LFSR (x^8 + x^6 + x^5 + x^4 + 1), NOT the obvious add-and-xor
; step this used to be. That one was a bijection on the byte, so its period
; was exactly 256 -- the same as a page. Every byte value therefore sat at
; one fixed offset within every page, which made "the byte value is $06" and
; "the page offset is $E1" the same statement about the data, and the first
; five bulk-transfer failures on hardware could not be attributed to either.
; The LFSR's period is 255, coprime with 256, so value and page offset drift
; apart across the buffer and a run can tell them apart. The cost is that 0
; never appears; that is worth it.
kitSeq
	lda #$01
	sta kitVal
	ldy #0
	ldx kitLen+1
	beq ksTail
ksPage
	lda kitVal
	sta (P1),y
	jsr ksNext
	iny
	bne ksPage
	inc P1+1
	dex
	bne ksPage
ksTail
	ldy #0
	cpy kitLen
	beq ksDone
ksByte
	lda kitVal
	sta (P1),y
	jsr ksNext
	iny
	cpy kitLen
	bne ksByte
ksDone
	rts

ksNext
	lda kitVal
	asl a
	bcc +
	eor #$1d
+
	sta kitVal
	rts

; Compares (P1) against (P2). X = 0 if identical, else F_CMP.
; Compares kitLen bytes at P1 against P2. On a mismatch it records WHERE
; the first one is (kitOff), both bytes (kitDiffA from P1, kitDiffB from
; P2), and -- the part that turns a symptom into a diagnosis -- HOW MANY
; bytes differ in total (kitDiffN).
;
; That count is what separates the two things a bulk transfer can do
; wrong. One byte wrong in eight thousand is a dropped byte, and this
; project already knows it has a drop floor. Everything wrong from some
; offset onward is a shifted stream, which is a different bug entirely and
; would mean the transfer lost or gained a byte rather than corrupting one.
; The two report identical $E1 without this.
kitCmp
	lda #0
	sta kitOff
	sta kitOff+1
	sta kitDiffA
	sta kitDiffB
	sta kitDiffN
	sta kitDiffN+1
	sta kitPage
	sta kitOff2
	sta kitOff2+1
	sta kitOff3
	sta kitOff3+1
	sta kitGot2
	sta kitGot3
	ldy #0
	ldx kitLen+1
	beq kcTail
kcPage
	lda (P1),y
	cmp (P2),y
	beq +
	jsr kcNote
+
	iny
	bne kcPage
	inc P1+1
	inc P2+1
	inc kitPage
	dex
	bne kcPage
kcTail
	ldy #0
	cpy kitLen
	beq kcDone
kcByte
	lda (P1),y
	cmp (P2),y
	beq +
	jsr kcNote
+
	iny
	cpy kitLen
	bne kcByte
kcDone
	lda kitDiffN
	ora kitDiffN+1
	bne kcDiff
	ldx #0
	rts
kcDiff
	ldx #F_CMP
	rts

; Y = offset within the page kitPage. Records the first THREE mismatches,
; counts every one.
;
; Three, not one, because a handful of scattered bad bytes and a single bad
; byte are different bugs and the first offset alone cannot tell them apart.
; With three offsets and three actual values in hand, a run says whether the
; damage is dispersed, and -- the question this was actually built for --
; whether the wrong byte is the same wrong byte every time. It is: both
; hardware runs so far read back $5C, which in kitSeq's pattern is the value
; that lives at offset $FF of every page. That is not corruption, that is a
; read from the right page at the wrong low address.
kcNote
	lda kitDiffN+1
	bne kcnCount
	lda kitDiffN
	beq kcn1
	cmp #1
	beq kcn2
	cmp #2
	beq kcn3
	bne kcnCount
kcn1
	sty kitOff
	lda kitPage
	sta kitOff+1
	lda (P1),y
	sta kitDiffA
	lda (P2),y
	sta kitDiffB
	jmp kcnCount
kcn2
	sty kitOff2
	lda kitPage
	sta kitOff2+1
	lda (P2),y
	sta kitGot2
	jmp kcnCount
kcn3
	sty kitOff3
	lda kitPage
	sta kitOff3+1
	lda (P2),y
	sta kitGot3
kcnCount
	inc kitDiffN
	bne +
	inc kitDiffN+1
+
	rts

; Checks that every byte of (P1) equals kitVal. X = 0 or F_FILL.
kitChkFill
	ldy #0
	ldx kitLen+1
	beq kcfTail
kcfPage
	lda (P1),y
	cmp kitVal
	bne kcfBad
	iny
	bne kcfPage
	inc P1+1
	dex
	bne kcfPage
kcfTail
	ldy #0
	cpy kitLen
	beq kcfGood
kcfByte
	lda (P1),y
	cmp kitVal
	bne kcfBad
	iny
	cpy kitLen
	bne kcfByte
kcfGood
	ldx #0
	rts
kcfBad
	ldx #F_FILL
	rts

; ----------------------------------------------------------------------
; Session.
; ----------------------------------------------------------------------

kitInit
	ldx #0
	lda #$20
kiCls
	sta SCREEN,x
	sta SCREEN+$100,x
	sta SCREEN+$200,x
	sta SCREEN+$300,x
	inx
	bne kiCls

	lda #<(SCREEN+2*40)
	sta slotPtr
	lda #>(SCREEN+2*40)
	sta slotPtr+1
	lda #0
	sta slotN
	sta passCnt
	sta failCnt
	sta accFail
	sta failCode
	; Class 0 is the reset default, but a previous run may have left CMD_HI
	; elsewhere -- it is sticky and survives a RUN/STOP.
	sta CMD_HI
	rts

kitFinish
	lda #24
	jsr kitRowAddr

	lda #<txtPass_
	ldy #>txtPass_
	jsr kitAppend
	lda passCnt
	jsr kitDecD
	lda #<txtFail_
	ldy #>txtFail_
	jsr kitAppend
	lda failCnt
	jsr kitDecD

	lda failCnt
	bne kfBad
	lda #<txtVerdOk
	ldy #>txtVerdOk
	jmp kfVerd
kfBad
	lda #<txtVerdNo
	ldy #>txtVerdNo
kfVerd
	jsr kitAppend

	lda #<txtExit
	ldy #>txtExit
	ldx #23
	jsr kitMsg

	; Hold the results on screen. Returning straight to BASIC would print
	; READY. over a result line, so the screen is cleared on the way out
	; instead and the tester decides when that happens.
kfWait
	jsr $ffe1			; STOP key
	bne kfWait
	jsr $e544			; clear screen
	lda #0
	sta CMD_HI
	rts

; A/Y = string, appended at (DST). DST advances past what it wrote, so a
; row can be assembled left to right out of several pieces.
kitAppend
	sta SRC
	sty SRC+1
	ldy #0
kaLoop
	lda (SRC),y
	beq kaDone
	jsr kitScreenCode
	sta (DST),y
	iny
	bne kaLoop
kaDone
	tya
	clc
	adc DST
	sta DST
	bcc +
	inc DST+1
+
	rts

; A = byte, decimal, at (DST). Counts are printed in decimal and everything
; else in hex on purpose: a fail code is a byte off the bus and reads
; naturally as hex, but "PASS 28" for forty tests is a trap.
kitDecD
	ldx #0
kdHund
	cmp #100
	bcc kdH2
	sbc #100
	inx
	bne kdHund
kdH2
	pha
	txa
	beq kdNoH
	clc
	adc #$30
	ldy #0
	sta (DST),y
	jsr kitBumpDst
kdNoH
	pla
	ldx #0
kdTens
	cmp #10
	bcc kdOnes
	sbc #10
	inx
	bne kdTens
kdOnes
	pha
	txa
	clc
	adc #$30
	ldy #0
	sta (DST),y
	jsr kitBumpDst
	pla
	clc
	adc #$30
	ldy #0
	sta (DST),y
	jsr kitBumpDst
	rts

kitBumpDst
	inc DST
	bne +
	inc DST+1
+
	rts

; A = byte, two hex digits at (DST), DST advancing like kitAppend.
kitHexD
	ldy #0
	jsr kitPutHex
	tya
	clc
	adc DST
	sta DST
	bcc +
	inc DST+1
+
	rts

txtExit		.text "PRESS RUN/STOP"
		.byte 0
txtPass_	.text "PASS "
		.byte 0
txtFail_	.text "  FAIL "
		.byte 0
txtVerdOk	.text "  VERDICT PASS"
		.byte 0
txtVerdNo	.text "  VERDICT FAIL"
		.byte 0

; ----------------------------------------------------------------------

kitTmp		.byte 0
failCode	.byte 0
accFail		.byte 0
passCnt		.byte 0
failCnt		.byte 0
slotN		.byte 0
slotPtr		.word 0
kitLen		.word 0
kitVal		.byte 0
kitVal16	.word 0
kitOff		.word 0
kitDiffN	.word 0
kitPage		.byte 0
kitDiffA	.byte 0
kitDiffB	.byte 0
kitOff2		.word 0
kitOff3		.word 0
kitGot2		.byte 0
kitGot3		.byte 0

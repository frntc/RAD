# Examples

Ten complete, commented programs are in
[`examples/`](../../examples/) and [`quake/`](../../games/quake/). See
[getting-started.md](getting-started.md) for the full table of what each
one shows and which opcodes it uses. The three below are short enough to
read inline; they cover the three most common shapes a gpu64 program
takes: drawing directly, blitting with vsync, and using the matrix
coprocessor.

## 1. Clear and fill

```asm
	.include "gpu64.inc"
	#basicStub

start
	#argb 0, CLASS_0
	#cmd0 CLASS_0			; select class 0 (redundant here, shown for clarity)

	#argb 0, BLUE
	#cmd OP_CLEAR			; blue background

	#argw 0, 40			; x
	#argw 2, 30			; y
	#argw 4, 240			; w
	#argw 6, 140			; h
	#argb 8, WHITE
	#cmd OP_RECT_FILL		; white filled box

	jmp dmHold
	.include "gpu64_rt.inc"
```

## 2. Blit a sprite, then flip on vblank

```asm
	.include "gpu64.inc"
	#basicStub

start
	jsr dmInit

	#argw 0, 100			; dest x
	#argw 2, 80			; dest y
	#argb 4, 0			; source space: 0 = C64 RAM
	#argw 5, <SPRITE_DATA		; source addr lo/mid
	#argb 7, >SPRITE_DATA
	#argb 8, 0			; addr hi byte (24-bit addr)
	#argw 9, 32*32			; source len
	#argw 11, 32			; w
	#argw 13, 32			; h
	#cmd OP_BLIT

	#cmd OP_VBLANK_ARM
waitVb
	lda STATUS
	and #%00000100			; bit2: vblank pending
	beq waitVb
	#cmd OP_VBLANK_ACK
	#cmd OP_PAGE_FLIP

	jmp dmHold
	.include "gpu64_rt.inc"

SPRITE_DATA
	.byte $c0,$00			; ... 32x32 sprite bytes at $C000
```

## 3. Matrix multiply

Multiplies a 4x4 matrix at `$2000` by a 4x1 vector at `$2040`, both 8.8
fixed point, both plain C64 RAM, result written back to `$2040`:

```asm
	.include "gpu64.inc"
	#basicStub

start
	#argb 0, 4			; rows(a)
	#argb 1, 4			; cols(a)
	#argb 2, 4			; rows(b)
	#argb 3, 1			; cols(b)
	#argcd 4, 0, $2000		; operand a: space 0, addr $2000
	#argcd 8, 0, $2040		; operand b: space 0, addr $2040
	#argcd 12, 0, $2040		; dest: overwrite b in place
	#cmd0 CLASS_0
	#cmd OP_MAT_MUL_FIXED		; $80 + $00

	jmp dmHold
	.include "gpu64_rt.inc"
```

8 bytes of result (4 elements x 2 bytes, 8.8 fixed) land at `$2040` before
the `#cmd` macro's `CMD_LO` write returns. The float32 variant is
identical except for `#cmd OP_MAT_MUL_FLOAT` ($90 + $00) and 16 bytes of
result instead of 8 — every element doubles from 2 bytes to 4.

See also: [getting-started.md](getting-started.md) for the full demo table,
and [known-gaps.md](../api/known-gaps.md) for which harder scenarios (multi-batch
frames, texture eviction, dynamic-light budgets, ...) don't yet have a
worked example here.

; Copyright (c) 2026 Honza Slesinger. GPL-3.0-or-later (see LICENSE).
; ======================================================================
; The textures of gpu64_demo_quake, in class 1's texel order.
;
; Same four images, same expressions, one difference that matters: class 2
; addresses a texel as u * height + v and class 1 as v * width + u, so the
; two loops are nested the other way round here. Get that backwards and
; every wall in the room is its own texture transposed -- which looks like
; a texcoord bug and is not one.
;
; TEXW/TEXH must be defined before this file is included.
; ======================================================================

brickTex
	.for v = 0, v < TEXH, v += 1
	  .for u = 0, u < TEXW, u += 1
	  .byte ((v % 8) == 0) || (((u + ((v / 8) % 2) * 8) % 16) < 2) ? 0*32+20 : 1*32+18+((u*5+v*3) % 6)
	  .next
	.next

stoneTex
	.for v = 0, v < TEXH, v += 1
	  .for u = 0, u < TEXW, u += 1
	  .byte ((v % 16) < 1) || ((u % 16) < 1) ? 2*32+12 : 2*32+20+((u*3+v*7) % 5)
	  .next
	.next

gunTex
	.for v = 0, v < TEXH, v += 1
	  .for u = 0, u < TEXW, u += 1
	  .byte (u >= 12 && u < 20 && v >= 8) ? (u < 14 || u >= 18 ? 7*32+14 : 7*32+24) :  ((u >= 10 && u < 22 && v >= 20) ? 7*32+8 : 0)
	  .next
	.next

; The monster, eight times over: a squat two-legged silhouette seen from
; eight directions round the compass, index 0 clear. View 0 is its front,
; view 4 its back. Three things change as it turns: the silhouette narrows
; toward the side views, the eyes slide across the face and then go out of
; sight altogether, and the back views get a pale plate across the
; shoulders that the front ones do not.
MHW	.function d
	.endf (d == 0 || d == 4) ? 10 : ((d == 2 || d == 6) ? 6 : 8)

MBODY	.function d, u, v
	.endf (v >= 4) && (u >= 16 - MHW(d)) && (u < 16 + MHW(d)) && !((v >= 26) && ((u % 12) >= 6))

MEYE	.function d, u, v
	.endf (v >= 8) && (v < 12) && ((d == 0) ? ((u >= 11 && u < 14) || (u >= 18 && u < 21)) : (d == 1) ? ((u >= 13 && u < 16) || (u >= 20 && u < 23)) : (d == 7) ? ((u >= 9 && u < 12) || (u >= 16 && u < 19)) : (d == 2) ? (u >= 18 && u < 21) : (d == 6) ? (u >= 11 && u < 14) : 0)

MBACK	.function d, u, v
	.endf (d >= 3) && (d <= 5) && (v >= 7) && (v < 12)

monTex
	.for d = 0, d < 8, d += 1
	  .for v = 0, v < TEXH, v += 1
	    .for u = 0, u < TEXW, u += 1
	    .byte !MBODY(d, u, v) ? 0 : MEYE(d, u, v) ? 7*32+30 : MBACK(d, u, v) ? 4*32+28 : 6*32+12+((u*3+v*5) % 8)
	    .next
	  .next
	.next

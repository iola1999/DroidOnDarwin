/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
// Small and disabled calls tail-call libc without a stack frame or PAC pair.
.macro entry symbol, original, bulk, fallback
.balign 16
.global \symbol
.type \symbol, %function
\symbol:
    hint #34 // BTI c
    cmp x2, #1024
    b.lo 1f
    adrp x16, husk_bulk_enabled
    ldr w16, [x16, :lo12:husk_bulk_enabled]
    cbnz w16, 2f
1:  adrp x16, \original
    ldr x16, [x16, :lo12:\original]
    cbz x16, 3f
    br x16
2:  b \bulk
3:  b \fallback
.size \symbol, .-\symbol
.endm

entry memcpy, husk_original_copy, husk_bulk_copy, husk_fallback_move
entry memmove, husk_original_move, husk_bulk_move, husk_fallback_move
entry memset, husk_original_set, husk_bulk_set, husk_fallback_set

.section .note.GNU-stack,"",%progbits
.section .note.gnu.property,"a",%note
.p2align 3
.long 4, 16, 5
.asciz "GNU"
.long 0xc0000000, 4, 3, 0 // GNU_PROPERTY_AARCH64_FEATURE_1_AND: BTI and PAC

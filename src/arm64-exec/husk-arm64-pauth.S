/* SPDX-License-Identifier: BSD-2-Clause
 * Hash rounds derived from QEMU include/qemu/xxhash.h.
 * Copyright (C) 2012-2016, Yann Collet
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "husk-arm64-layout.h"
.text
.p2align 2
.globl _husk_direct_pauth
/* Private Direct ABI: x16=value, x17=modifier, w26=instruction, x27=Frame.
 * Return x16=result and x26=1 after computation, or x26=2 after a cache hit.
 * Refusal returns the original value and x26=0. Resident guest
 * registers, vectors, NZCV and FP state are preserved. No guest access occurs.
 * The caller has checked the algorithm, PAuth2 semantics and trap controls. */
_husk_direct_pauth:
    stp x0, x1, [sp, #-96]!
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x30, x17, [sp, #64]
    mrs x7, nzcv
    str x7, [sp, #80]
    str x26, [sp, #88]
    mov x0, x16
    lsr x1, x0, #2
    eor x1, x1, x0, lsr #32
    eor x1, x1, x17, lsr #4
    ubfx x2, x26, #5, #8
    eor x1, x1, x2
    and x1, x1, #255
    ldr x2, [x27, #HUSK_PAUTH_CACHE]
    add x1, x2, x1, lsl #5
    ldp x2, x3, [x1]
    cmp x2, x0
    b.ne 9f
    cmp x3, x17
    b.ne 9f
    ldr w2, [x1, #24]
    cmp w2, w26
    b.ne 9f
    ldr x16, [x1, #16]
    mov x26, #2
    b 8f
9:
    ldr x26, [x27, #HUSK_NATIVE_PAUTH]
    cbz x26, 6f
    ldr w7, [sp, #88]
    movz w2, #0x20ff
    movk w2, #0xd503, lsl #16
    cmp w7, w2
    ubfx w7, w7, #6, #2
    b.ne 10f
    mov w7, #4
10:
    ubfx x1, x0, #55, #1
    add x26, x26, x1, lsl #6
    and x1, x7, #1
    add x26, x26, x1, lsl #5
    ldr x2, [x26, #16]
    bic x1, x0, x2
    tbz x0, #55, 1f
    orr x1, x0, x2
1:
    cmp x7, #4
    b.eq 4f
    ldr w2, [x26, #24]
    cbz w2, 5f
    tbnz x7, #1, 2f
    // Noncanonical signing uses the owner's complete extension-bit rules.
    cmp x0, x1
    b.ne 6f
2:
    movz x5, #0xca87
    movk x5, #0x85eb, lsl #16
    movk x5, #0x79b1, lsl #32
    movk x5, #0x9e37, lsl #48
    movz x6, #0xeb4f
    movk x6, #0x27d4, lsl #16
    movk x6, #0xae3d, lsl #32
    movk x6, #0xc2b2, lsl #48

    add x3, x5, x6
    add x3, x3, #1
    madd x3, x1, x6, x3
    ror x3, x3, #33
    mul x3, x3, x5
    add x4, x6, #1
    madd x4, x17, x6, x4
    ror x4, x4, #33
    mul x4, x4, x5
    ldp x16, x17, [x26]
    mov x2, #1
    madd x16, x16, x6, x2
    ror x16, x16, #33
    mul x16, x16, x5
    sub x2, x2, x5
    madd x17, x17, x6, x2
    ror x17, x17, #33
    mul x17, x17, x5

    ror x2, x3, #63
    ror x30, x4, #57
    add x2, x2, x30
    ror x30, x16, #52
    add x2, x2, x30
    ror x30, x17, #46
    add x2, x2, x30
    movz x7, #0xae63
    movk x7, #0xc2b2, lsl #16
    movk x7, #0xca77, lsl #32
    movk x7, #0x85eb, lsl #48
.macro merge value
    mul x30, \value, x6
    ror x30, x30, #33
    mul x30, x30, x5
    eor x2, x2, x30
    madd x2, x2, x5, x7
.endm
    merge x3
    merge x4
    merge x16
    merge x17
    eor x2, x2, x2, lsr #33
    mul x2, x2, x6
    eor x2, x2, x2, lsr #29
    movz x7, #0x79f9
    movk x7, #0x9e37, lsl #16
    movk x7, #0x67b1, lsl #32
    movk x7, #0x1656, lsl #48
    mul x2, x2, x7
    eor x2, x2, x2, lsr #32

    ldr x3, [x26, #16]
    bic x3, x3, #0x80000000000000
    and x2, x2, x3
    eor x16, x0, x2
    ldr x7, [sp, #88]
    tbz x7, #7, 7f
    // Authentication failure keeps the faulting guest state for QEMU.
    cmp x16, x1
    b.ne 6f
    b 7f
4:
    mov x16, x1
    b 7f
5:
    mov x16, x0
    b 7f
6:
    mov x16, x0
    mov x26, #0
    b 8f
7:
    ldr x17, [sp, #72]
    ldr w7, [sp, #88]
    lsr x1, x0, #2
    eor x1, x1, x0, lsr #32
    eor x1, x1, x17, lsr #4
    ubfx x2, x7, #5, #8
    eor x1, x1, x2
    and x1, x1, #255
    ldr x2, [x27, #HUSK_PAUTH_CACHE]
    add x1, x2, x1, lsl #5
    stp x0, x17, [x1]
    str x16, [x1, #16]
    str w7, [x1, #24]
    mov x26, #1
8:
    ldr x7, [sp, #80]
    msr nzcv, x7
    ldp x30, x17, [sp, #64]
    ldp x6, x7, [sp, #48]
    ldp x4, x5, [sp, #32]
    ldp x2, x3, [sp, #16]
    ldp x0, x1, [sp], #96
    ret

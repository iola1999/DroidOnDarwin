/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.p2align 2
.globl _husk_direct_enter
_husk_direct_enter:
    stp x19, x20, [sp, #-96]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    // x18 belongs to Darwin. Guest x16-x18 and x26-x30 stay in the frame.
    mov x27, x0
    mov x16, x1
    mov x28, x2
    ldr x29, [x27, #272]
    ldr w17, [x27, #264]
    msr nzcv, x17
    ldp x0, x1, [x27]
    ldp x2, x3, [x27, #16]
    ldp x4, x5, [x27, #32]
    ldp x6, x7, [x27, #48]
    ldp x8, x9, [x27, #64]
    ldp x10, x11, [x27, #80]
    ldp x12, x13, [x27, #96]
    ldp x14, x15, [x27, #112]
    ldp x19, x20, [x27, #152]
    ldp x21, x22, [x27, #168]
    ldp x23, x24, [x27, #184]
    ldr x25, [x27, #200]
    br x16

.p2align 2
.globl _husk_direct_leave
_husk_direct_leave:
    stp x0, x1, [x27]
    stp x2, x3, [x27, #16]
    stp x4, x5, [x27, #32]
    stp x6, x7, [x27, #48]
    stp x8, x9, [x27, #64]
    stp x10, x11, [x27, #80]
    stp x12, x13, [x27, #96]
    stp x14, x15, [x27, #112]
    stp x19, x20, [x27, #152]
    stp x21, x22, [x27, #168]
    stp x23, x24, [x27, #184]
    str x25, [x27, #200]
    mrs x16, nzcv
    str w16, [x27, #264]
    str x29, [x27, #272]
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret

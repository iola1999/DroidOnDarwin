/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.global signal_register_test
.type signal_register_test, %function
signal_register_test:
    mov x1, #10
    mov x8, #129
    mov x9, #0x9876
    movi v0.16b, #0x5a
    movi v31.16b, #0xa5
    mov x10, #0x400000
    msr fpcr, x10
    mov x10, #0x8000000
    msr fpsr, x10
    cmp xzr, xzr
    svc #0
    // Read condition flags before the checks overwrite them.
    mrs x10, nzcv
    mov x11, #0x60000000
    cmp x10, x11
    b.ne 1f
    mov x10, #0x9876
    cmp x9, x10
    b.ne 1f
    movi v1.16b, #0x5a
    eor v1.16b, v1.16b, v0.16b
    umaxv b1, v1.16b
    umov w10, v1.b[0]
    cbnz w10, 1f
    movi v1.16b, #0xa5
    eor v1.16b, v1.16b, v31.16b
    umaxv b1, v1.16b
    umov w10, v1.b[0]
    cbnz w10, 1f
    mrs x10, fpcr
    mov x11, #0x400000
    cmp x10, x11
    b.ne 1f
    mrs x10, fpsr
    mov x11, #0x8000000
    cmp x10, x11
    b.ne 1f
    mov x0, #0
    b 2f
1:  mov x0, #1
2:  msr fpcr, xzr
    msr fpsr, xzr
    ret
.size signal_register_test, .-signal_register_test
.section .note.GNU-stack,"",%progbits

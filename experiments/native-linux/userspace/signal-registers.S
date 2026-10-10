/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.macro init_registers
    mov x9, #0x9876
    mov x18, x9
    movi v0.16b, #0x5a
    movi v31.16b, #0xa5
    mov x10, #0x400000
    msr fpcr, x10
    mov x10, #0x8000000
    msr fpsr, x10
    cmp xzr, xzr
.endm

.macro check_registers
    // Read condition flags before the checks overwrite them.
    mrs x10, nzcv
    mov x11, #0x60000000
    cmp x10, x11
    b.ne 1f
    mov x10, #0x9876
    cmp x9, x10
    b.ne 1f
    mov x11, x18
    cmp x11, x10
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
.endm

.global signal_register_test
.type signal_register_test, %function
signal_register_test:
    mov x1, #10
    mov x8, #129
    init_registers
    svc #0
    check_registers
.size signal_register_test, .-signal_register_test

.global fault_register_test
.type fault_register_test, %function
fault_register_test:
    init_registers
    // A context switch invalidates this private page's host view.
    ldrb w2, [x0]
    eor w2, w2, w1
    cbnz w2, 1f
    check_registers
.size fault_register_test, .-fault_register_test
.section .note.GNU-stack,"",%progbits

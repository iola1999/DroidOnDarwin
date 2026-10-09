/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.p2align 2
.globl _nce_enter
_nce_enter:
    stp x19, x20, [sp, #-176]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    stp d8, d9, [sp, #96]
    stp d10, d11, [sp, #112]
    stp d12, d13, [sp, #128]
    stp d14, d15, [sp, #144]
    mrs x1, fpcr
    mrs x2, fpsr
    stp x1, x2, [sp, #160]
    mov x1, sp
    str x1, [x0, #800]
    str x18, [x0, #808]
    mov x16, x0
    b Lrestore

.globl _nce_gate
_nce_gate:
    stp x0, x1, [x16]
    stp x2, x3, [x16, #16]
    stp x4, x5, [x16, #32]
    stp x6, x7, [x16, #48]
    stp x8, x9, [x16, #64]
    stp x10, x11, [x16, #80]
    stp x12, x13, [x16, #96]
    stp x14, x15, [x16, #112]
    ldp x0, x1, [sp]
    stp x0, x1, [x16, #128]
    // Guest x18 is emulated; the hardware register always belongs to Darwin.
    str x19, [x16, #152]
    stp x20, x21, [x16, #160]
    stp x22, x23, [x16, #176]
    stp x24, x25, [x16, #192]
    stp x26, x27, [x16, #208]
    stp x28, x29, [x16, #224]
    add x0, sp, #16
    str x0, [x16, #248]
    mrs x0, nzcv
    mrs x1, fpcr
    mrs x2, fpsr
    stp x0, x1, [x16, #264]
    str x2, [x16, #280]
    add x0, x16, #288
    stp q0, q1, [x0]
    stp q2, q3, [x0, #32]
    stp q4, q5, [x0, #64]
    stp q6, q7, [x0, #96]
    stp q8, q9, [x0, #128]
    stp q10, q11, [x0, #160]
    stp q12, q13, [x0, #192]
    stp q14, q15, [x0, #224]
    stp q16, q17, [x0, #256]
    stp q18, q19, [x0, #288]
    stp q20, q21, [x0, #320]
    stp q22, q23, [x0, #352]
    stp q24, q25, [x0, #384]
    stp q26, q27, [x0, #416]
    stp q28, q29, [x0, #448]
    stp q30, q31, [x0, #480]
    mov x0, x16
    ldr x1, [x16, #800]
    mov sp, x1
    // Restore the host FP environment before calling compiler-generated code.
    ldp x1, x2, [sp, #160]
    msr fpcr, x1
    msr fpsr, x2
    bl _nce_dispatch
    ldr x1, [x0, #848]
    cbnz x1, Lfinish
    mov x16, x0
    b Lrestore

// Signal return enters here on the saved host stack. Kernel work then runs
// outside the Darwin signal handler, with the host floating-point state.
.globl _nce_event_gate
_nce_event_gate:
    ldp x1, x2, [sp, #160]
    msr fpcr, x1
    msr fpsr, x2
    bl _nce_event_dispatch
    mov x16, x0
Lrestore:
    // Another host thread may have replaced code before waking this task.
    isb
    ldr x17, [x16, #248]
    sub x17, x17, #16
    ldp x0, x1, [x16, #128]
.globl _nce_restore_stack
_nce_restore_stack:
    stp x0, x1, [x17]
    mov sp, x17
    add x0, x16, #288
    ldp q0, q1, [x0]
    ldp q2, q3, [x0, #32]
    ldp q4, q5, [x0, #64]
    ldp q6, q7, [x0, #96]
    ldp q8, q9, [x0, #128]
    ldp q10, q11, [x0, #160]
    ldp q12, q13, [x0, #192]
    ldp q14, q15, [x0, #224]
    ldp q16, q17, [x0, #256]
    ldp q18, q19, [x0, #288]
    ldp q20, q21, [x0, #320]
    ldp q22, q23, [x0, #352]
    ldp q24, q25, [x0, #384]
    ldp q26, q27, [x0, #416]
    ldp q28, q29, [x0, #448]
    ldp q30, q31, [x0, #480]
    ldp x0, x1, [x16, #264]
    msr nzcv, x0
    msr fpcr, x1
    ldr x0, [x16, #280]
    msr fpsr, x0
    ldr x18, [x16, #808]
    ldp x19, x20, [x16, #152]
    ldp x21, x22, [x16, #168]
    ldp x23, x24, [x16, #184]
    ldp x25, x26, [x16, #200]
    ldp x27, x28, [x16, #216]
    ldp x29, x30, [x16, #232]
    ldp x0, x1, [x16]
    ldp x2, x3, [x16, #16]
    ldp x4, x5, [x16, #32]
    ldp x6, x7, [x16, #48]
    ldp x8, x9, [x16, #64]
    ldp x10, x11, [x16, #80]
    ldp x12, x13, [x16, #96]
    ldp x14, x15, [x16, #112]
    ldr x17, [x16, #816]
    br x17
Lfinish:
    ldp x1, x2, [sp, #160]
    msr fpcr, x1
    msr fpsr, x2
    ldp d8, d9, [sp, #96]
    ldp d10, d11, [sp, #112]
    ldp d12, d13, [sp, #128]
    ldp d14, d15, [sp, #144]
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #176
    ret

// Copied into the JIT pool; literals and relative branches are filled there.
.p2align 3
.globl _nce_stub, _nce_stub_end
_nce_stub:
    stp x16, x17, [sp, #-16]!
    ldr x16, 1f
    ldr x16, [x16]
    str x30, [x16, #240]
    ldr x17, 2f
    str x17, [x16, #816]
    ldr w17, 3f
    str w17, [x16, #824]
    ldr x17, 4f
    br x17
    ldp x16, x17, [sp], #16
    b .
1:  .quad 0
2:  .quad 0
3:  .word 0
    nop
4:  .quad 0
_nce_stub_end:

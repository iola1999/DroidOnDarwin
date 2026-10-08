/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "husk-arm64-layout.h"
.text
.p2align 2
.globl _husk_direct_enter
_husk_direct_enter:
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
    mrs x3, fpcr
    mrs x4, fpsr
    stp x3, x4, [sp, #160]
    // x18 belongs to Darwin. Guest x16-x18 and x26-x30 stay in the frame.
    mov x27, x0
    mov x16, x1
    mov x28, x2
    ldr x29, [x27, #HUSK_BUDGET]
    ldr w17, [x27, #HUSK_FPCR]
    msr fpcr, x17
    ldr w17, [x27, #HUSK_FPSR]
    msr fpsr, x17
    // All guest vectors are resident; the Darwin callee-save D registers
    // and host floating-point environment are restored on every exit.
    add x17, x27, #HUSK_VECTORS
    ldp q0, q1, [x17]
    ldp q2, q3, [x17, #32]
    ldp q4, q5, [x17, #64]
    ldp q6, q7, [x17, #96]
    ldp q8, q9, [x17, #128]
    ldp q10, q11, [x17, #160]
    ldp q12, q13, [x17, #192]
    ldp q14, q15, [x17, #224]
    ldp q16, q17, [x17, #256]
    ldp q18, q19, [x17, #288]
    ldp q20, q21, [x17, #320]
    ldp q22, q23, [x17, #352]
    ldp q24, q25, [x17, #384]
    ldp q26, q27, [x17, #416]
    ldp q28, q29, [x17, #448]
    ldp q30, q31, [x17, #480]
    ldr w17, [x27, #HUSK_NZCV]
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
    str w16, [x27, #HUSK_NZCV]
    str x29, [x27, #HUSK_BUDGET]
    add x16, x27, #HUSK_VECTORS
    stp q0, q1, [x16]
    stp q2, q3, [x16, #32]
    stp q4, q5, [x16, #64]
    stp q6, q7, [x16, #96]
    stp q8, q9, [x16, #128]
    stp q10, q11, [x16, #160]
    stp q12, q13, [x16, #192]
    stp q14, q15, [x16, #224]
    stp q16, q17, [x16, #256]
    stp q18, q19, [x16, #288]
    stp q20, q21, [x16, #320]
    stp q22, q23, [x16, #352]
    stp q24, q25, [x16, #384]
    stp q26, q27, [x16, #416]
    stp q28, q29, [x16, #448]
    stp q30, q31, [x16, #480]
    mrs x16, fpsr
    str w16, [x27, #HUSK_FPSR]
    ldp x3, x4, [sp, #160]
    msr fpcr, x3
    msr fpsr, x4
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

// Private JIT ABI: x27 is Frame; x16, x17, x26 and x30 are scratch.
// Neither stub may change NZCV or guest resident GPR/SIMD state.
.p2align 2
.globl _husk_direct_exit_stub
_husk_direct_exit_stub:
    and x16, x17, #15
    str x16, [x27, #HUSK_REASON]
    lsr x17, x17, #4
    ldr x16, [x27, #HUSK_CODE_PAGE]
    add x17, x16, x17
    str x17, [x27, #HUSK_PC]
    b _husk_direct_leave

.p2align 2
.globl _husk_direct_branch_stub
_husk_direct_branch_stub:
    and x17, x16, #3
    cbnz x17, Lbranch_miss
    ldr x26, [x27, #HUSK_CODE_ROUTES]
    ubfx x17, x16, #12, #8
    add x17, x26, x17, lsl #5
    ldr x26, [x17]
    eor x26, x26, x16, lsr #12
    cbnz x26, Lbranch_miss
    ldr x26, [x17, #8]
    and x30, x16, #4095
    add x26, x26, x30, lsl #1
    ldr x26, [x26]
    cbz x26, Lbranch_miss
    ldr x17, [x17, #16]
    str x17, [x27, #HUSK_CODE]
    and x17, x16, #0xfffffffffffff000
    str x17, [x27, #HUSK_CODE_PAGE]
    br x26
Lbranch_miss:
    str x16, [x27, #HUSK_PC]
    mov x17, #5 // CodeBranch
    str x17, [x27, #HUSK_REASON]
    b _husk_direct_leave

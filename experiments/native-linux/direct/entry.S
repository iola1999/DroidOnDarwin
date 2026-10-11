/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "layout.h"
#include "page-table-layout.h"
.text
.p2align 2
.globl _nce_direct_enter
_nce_direct_enter:
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
    ldr x29, [x27, #DIRECT_BUDGET]
    ldr w17, [x27, #DIRECT_FPCR]
    msr fpcr, x17
    ldr w17, [x27, #DIRECT_FPSR]
    msr fpsr, x17
    // All guest vectors are resident; the Darwin callee-save D registers
    // and host floating-point environment are restored on every exit.
    add x17, x27, #DIRECT_VECTORS
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
    ldr w17, [x27, #DIRECT_NZCV]
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
.globl _nce_direct_leave
_nce_direct_leave:
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
    str w16, [x27, #DIRECT_NZCV]
    str x29, [x27, #DIRECT_BUDGET]
    add x16, x27, #DIRECT_VECTORS
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
    str w16, [x27, #DIRECT_FPSR]
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
.globl _nce_direct_exit_stub
_nce_direct_exit_stub:
    and x16, x17, #15
    str x16, [x27, #DIRECT_REASON]
    lsr x17, x17, #4
    ldr x16, [x27, #DIRECT_CODE_PAGE]
    add x17, x16, x17
    str x17, [x27, #DIRECT_PC]
    b _nce_direct_leave

.macro direct_branch_router epochs
    and x17, x16, #3
    cbnz x17, Lbranch_miss
    ldr x26, [x27, #DIRECT_CODE_ROUTES]
    lsr x17, x16, #12
    ldr x30, [x27, #DIRECT_ROUTE_MASK]
    and x17, x17, x30
    add x17, x26, x17, lsl #5
    ldr x26, [x17]
    eor x26, x26, x16, lsr #12
    cbnz x26, Lbranch_miss
    .if \epochs
    ldr x26, [x17, #24]
    ldr x30, [x27, #DIRECT_ROUTE_EPOCH]
    eor x26, x26, x30
    cbnz x26, Lbranch_miss
    .endif
    ldr x26, [x17, #8]
    and x30, x16, #4095
    add x26, x26, x30, lsl #1
    ldr x26, [x26]
    cbz x26, Lbranch_miss
    ldr x17, [x17, #16]
    str x17, [x27, #DIRECT_CODE]
    and x17, x16, #0xfffffffffffff000
    str x17, [x27, #DIRECT_CODE_PAGE]
    br x26
.endm
.p2align 2
.globl _nce_direct_branch_stub
_nce_direct_branch_stub:
    direct_branch_router 0
.p2align 2
.globl _nce_direct_branch_epoch_stub
_nce_direct_branch_epoch_stub:
    direct_branch_router 1
Lbranch_miss:
    str x16, [x27, #DIRECT_PC]
    mov x17, #5 // CodeBranch
    str x17, [x27, #DIRECT_REASON]
    b _nce_direct_leave

// Private ABI: preserve guest registers, NZCV and x16 (guest address).
// x17 returns the host address or zero; x26/x30 are scratch. No C call or
// host stack access occurs while the guest register set remains resident.
.p2align 2
.globl _nce_direct_read_pte
_nce_direct_read_pte:
    mov x17, #DIRECT_PTE_READ_REQUIRED
    b Lpte_walk
.globl _nce_direct_write_pte
_nce_direct_write_pte:
    mov x17, #DIRECT_PTE_WRITE_REQUIRED
Lpte_walk:
    str x17, [x27, #DIRECT_PTE_REQUIRED]
    str x30, [x27, #DIRECT_PTE_SAVED_LINK]
    mrs x17, nzcv
    str x17, [x27, #DIRECT_PTE_SAVED_NZCV]
    ldr x17, [x27, #DIRECT_PTE_USER_BASE]
    sub x26, x16, x17
    ldr x17, [x27, #DIRECT_PTE_USER_SIZE]
    cmp x26, x17
    b.hs Lpte_miss
    ldr x17, [x27, #DIRECT_PTE_ROOT]
    ubfx x26, x16, #DIRECT_PTE_PGD_SHIFT, #DIRECT_PTE_INDEX_BITS
    ldr x17, [x17, x26, lsl #3]
    and x17, x17, #0xffffffffffffc000
    cbz x17, Lpte_miss
    ubfx x26, x16, #DIRECT_PTE_PMD_SHIFT, #DIRECT_PTE_INDEX_BITS
    ldr x17, [x17, x26, lsl #3]
    and x17, x17, #0xffffffffffffc000
    cbz x17, Lpte_miss
    ubfx x26, x16, #DIRECT_PTE_PAGE_SHIFT, #DIRECT_PTE_INDEX_BITS
    ldr x17, [x17, x26, lsl #3]
    tbnz x17, #4, Lpte_miss
    ldr x26, [x27, #DIRECT_PTE_REQUIRED]
    and x30, x17, x26
    cmp x30, x26
    b.ne Lpte_miss
    and x17, x17, #0xffffffffffffc000
    ldr x26, [x27, #DIRECT_PTE_PHYSICAL_BASE]
    sub x17, x17, x26
    ldr x26, [x27, #DIRECT_PTE_RAM_SIZE]
    cmp x17, x26
    b.hs Lpte_miss
    ldr x26, [x27, #DIRECT_PTE_REQUIRED]
    tbz x26, #5, Lpte_address
    lsr x26, x17, #DIRECT_PTE_PAGE_SHIFT
    ldr x30, [x27, #DIRECT_PTE_CODE_RUNS]
    ldr x26, [x30, x26, lsl #3]
    ldr x30, [x27, #DIRECT_RUN_ID]
    cmp x26, x30
    b.eq Lpte_miss
Lpte_address:
    ldr x26, [x27, #DIRECT_PTE_RAM]
    add x17, x17, x26
    and x26, x16, #16383
    add x17, x17, x26
    b Lpte_return
Lpte_miss:
    mov x17, #0
Lpte_return:
    cbz x17, Lpte_restore
    ldr x26, [x27, #DIRECT_PTE_REFILL_MASK]
    cbz x26, Lpte_restore
    // Refill read fields only; the host owns write aliases and their index.
    lsr x30, x16, #DIRECT_PTE_PAGE_SHIFT
    eor x30, x30, x30, lsr #8
    and x26, x26, x30
    add x26, x28, x26, lsl #5
    lsr x30, x16, #DIRECT_PTE_PAGE_SHIFT
    str x30, [x26]
    sub x30, x17, x16
    str x30, [x26, #8]
    ldr x26, [x27, #DIRECT_PTE_READ_REFILLS]
    add x26, x26, #1
    str x26, [x27, #DIRECT_PTE_READ_REFILLS]
Lpte_restore:
    ldr x26, [x27, #DIRECT_PTE_SAVED_NZCV]
    msr nzcv, x26
    ldr x30, [x27, #DIRECT_PTE_SAVED_LINK]
    ret

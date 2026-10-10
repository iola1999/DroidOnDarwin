/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.p2align 2
.globl guest_thread
guest_thread:
    // x0/x1 are this thread's and its peer's progress counters.
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mov x22, x3
    mov x23, xzr
    mov x24, #1
    stlr x24, [x19]
1:
    ldar x25, [x20]
    cbz x25, 1b
2:
    mrs x0, tpidr_el0
    cmp x0, x21
    b.ne 9f
    mrs x16, tpidr_el0
    cmp x16, x21
    b.ne 9f
    mrs x17, tpidr_el0
    cmp x17, x21
    b.ne 9f
    add v0.2d, v0.2d, v31.2d
    add x23, x23, #1
    add x24, x23, #1
    stlr x24, [x19]
    // Exercise full state save/restore between batches of native instructions.
    tst x23, #255
    b.ne 3f
    mov x8, #172
    mov x9, #0xa0000000
    msr nzcv, x9
    svc #0
    cmp x0, x21
    b.ne 9f
3:
    cmp x23, x22
    b.ne 2b
    mrs x18, tpidr_el0
    mov x8, #93
    mov x0, #0
    svc #0
9:
    mov x8, #93
    mov x0, #1
    svc #0

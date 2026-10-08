/* SPDX-License-Identifier: GPL-2.0-or-later */
.section .text
.global _start
_start:
    msr spsel, #1
    adr x0, stack_end
    mov sp, x0
    adr x0, vectors
    msr vbar_el1, x0
    isb
    mov x0, #10000
    adr x9, 1f
1:  subs x0, x0, #1
    b.eq 2f
    br x9
2:  mov x19, #0
    mov x21, #0
    mov x1, #0x5678
    msr tpidr_el1, x1
    mrs x2, tpidr_el1
    cmp x1, x2
    b.ne fail
    msr sp_el0, x1
    mrs x2, sp_el0
    cmp x1, x2
    b.ne fail
    msr daifset, #15
    mrs x2, daif
    cmp x2, #0x3c0
    b.ne fail
    msr daifclr, #2
    mrs x2, daif
    cmp x2, #0x340
    b.ne fail
    msr daifset, #2

    mrs x23, cntvct_el0
    mov x24, #0x4240
    movk x24, #0xf, lsl #16
    adr x9, branch_first
    adr x10, branch_second
branch_first:
    br x10
branch_second:
    subs x24, x24, #1
    b.eq branch_done
    br x9
branch_done:
    mrs x24, cntvct_el0
    sub x23, x24, x23
    adr x0, branch_elapsed
    bl print
    mov x0, x23
    bl hex
    adr x0, frequency
    bl print
    mrs x0, cntfrq_el0
    bl hex

    /* Exercise TCR-dependent PAC stripping before enabling the MMU. Repeat
     * lower/upper instruction/data queries after every control-register write. */
    adr x9, va_controls
    mov x10, #0x1234
    mov x11, #8192
    mov x17, #0
va_loop:
    and x12, x11, #7
    ldr x13, [x9, x12, lsl #3]
    msr tcr_el1, x13
    isb
    eor x10, x10, x10, lsl #13
    eor x10, x10, x10, lsr #7
    eor x10, x10, x10, lsl #17
    mov x0, x10
    mov x1, x10
    .inst 0xdac143e0 // xpaci x0
    .inst 0xdac143e1 // xpaci x1
    cmp x0, x1
    b.ne fail
    mov x2, x10
    .inst 0xdac147e2 // xpacd x2
    eor x3, x10, #0x80000000000000
    mov x4, x3
    .inst 0xdac143e3 // xpaci x3
    .inst 0xdac147e4 // xpacd x4
    ror x17, x17, #7
    eor x17, x17, x0
    add x17, x17, x2
    ror x17, x17, #11
    eor x17, x17, x3
    add x17, x17, x4
    subs x11, x11, #1
    b.ne va_loop
    adr x0, va_hash
    bl print
    mov x0, x17
    bl hex

    /* All executable code remains identity-mapped while TTBR0 changes the
     * test VA between two physical RAM blocks. Tables use 4 KiB granules. */
    adr x0, root_a
    adr x1, root_b
    mov x2, #0x40000000
    orr x2, x2, #0x700
    orr x2, x2, #1
    str x2, [x0, #8]
    str x2, [x1, #8]
    mov x2, #0x405
    str x2, [x0]
    str x2, [x1]
    adr x2, level2_a
    orr x2, x2, #3
    str x2, [x0, #16]
    adr x2, level2_b
    orr x2, x2, #3
    str x2, [x1, #16]
    mov x2, #0x40200000
    mov x3, #41
    str x3, [x2]
    orr x3, x2, #0x700
    orr x3, x3, #1
    adr x4, level2_a
    str x3, [x4]
    mov x2, #0x40400000
    mov x3, #73
    str x3, [x2]
    orr x3, x2, #0x700
    orr x3, x3, #1
    adr x4, level2_b
    str x3, [x4]
    /* The same executable VA must follow the new physical mapping after TLBI. */
    mov x2, #0x40200000
    mov w3, #0x0527
    movk w3, #0xd280, lsl #16
    str w3, [x2, #256] // mov x7, #41
    mov w3, #0x03c0
    movk w3, #0xd65f, lsl #16
    str w3, [x2, #260] // ret
    mov x2, #0x40400000
    str w3, [x2, #260]
    mov w3, #0x0927
    movk w3, #0xd280, lsl #16
    str w3, [x2, #256] // mov x7, #73
    msr ttbr0_el1, x0
    mov x2, #0x3519
    msr tcr_el1, x2
    mov x2, #0xff
    msr mair_el1, x2
    dsb sy
    isb
    mrs x22, sctlr_el1
    orr x2, x22, #1
    msr sctlr_el1, x2
    isb
    mov x5, #0x80000000
    mov x6, #100
3:  adr x0, root_a
    msr ttbr0_el1, x0
    tlbi vmalle1
    dsb ish
    isb
    ldr x7, [x5]
    cmp x7, #41
    b.ne fail
    add x12, x5, #256
    blr x12
    cmp x7, #41
    b.ne fail
    adr x0, root_b
    msr ttbr0_el1, x0
    tlbi vmalle1
    dsb ish
    isb
    ldr x7, [x5]
    cmp x7, #73
    b.ne fail
    add x12, x5, #256
    blr x12
    cmp x7, #73
    b.ne fail
    subs x6, x6, #1
    b.ne 3b

    /* Revoke writes after the VA has already been accessed. The faulting
     * store must preserve RAM; the handler advances ELR through QEMU. */
    adr x0, level2_b
    ldr x1, [x0]
    orr x1, x1, #0x80
    str x1, [x0]
    dsb ishst
    tlbi vmalle1
    dsb ish
    isb
    mov x19, #1
    mov x7, #99
    str x7, [x5]
    cmp x21, #1
    b.ne fail
    ldr x7, [x5]
    cmp x7, #73
    b.ne fail
    mov x19, #0
    msr sctlr_el1, x22
    isb

    mov x6, #10000
    mrs x23, cntvct_el0
4:  msr tpidr_el1, x6
    mrs x7, tpidr_el1
    cmp x6, x7
    b.ne fail
    msr sp_el0, x6
    mrs x7, sp_el0
    cmp x6, x7
    b.ne fail
    msr daifset, #2
    msr daifclr, #2
    subs x6, x6, #1
    b.ne 4b
    mrs x24, cntvct_el0
    sub x24, x24, x23
    mrs x25, cntfrq_el0
    adr x0, elapsed
    bl print
    mov x0, x24
    bl hex
    adr x0, frequency
    bl print
    mov x0, x25
    bl hex
    adr x0, passed
    bl print
    mov x0, #0
    b quit
fail:
    adr x0, failed
    bl print
    mov x0, #1
quit:
    adr x1, exit_block
    str x0, [x1, #8]
    mov x0, #0x20
    hlt #0xf000
    b .

print:
    mov x2, #0x09000000
5:  ldrb w1, [x0], #1
    cbz w1, 6f
    str w1, [x2]
    b 5b
6:  ret
hex:
    mov x2, #0x09000000
    mov x3, #60
7:  lsr x1, x0, x3
    and w1, w1, #15
    add w4, w1, #48
    add w1, w1, #87
    cmp w4, #58
    csel w1, w4, w1, lo
    str w1, [x2]
    subs x3, x3, #4
    b.ge 7b
    ret

.balign 2048
vectors:
.rept 16
    b exception
    .space 124
.endr
exception:
    cmp x19, #1
    b.ne fail
    mrs x16, esr_el1
    lsr x16, x16, #26
    cmp x16, #0x25
    b.ne fail
    mrs x16, elr_el1
    add x16, x16, #4
    msr elr_el1, x16
    add x21, x21, #1
    eret

.section .data
branch_elapsed: .asciz "BENCH kernel_branch_ticks="
va_hash: .asciz "VA_HASH="
.balign 8
va_controls:
    .quad 0, 0x3519, 0x0000002000353519, 0x0000016000353519
    .quad 0x00000060b535b519, 0x0000016040357510, 0x080000200035350c, 0x00000060b535b53f
elapsed: .asciz "BENCH kernel_ticks="
frequency: .asciz " frequency="
passed: .asciz "\nHUSK-KERNEL-TEST PASS\n"
failed: .asciz "HUSK-KERNEL-TEST FAIL\n"
.balign 16
exit_block: .quad 0x20026, 0
.balign 4096
root_a: .space 4096
root_b: .space 4096
level2_a: .space 4096
level2_b: .space 4096
stack: .space 4096
stack_end:

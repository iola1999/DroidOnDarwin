/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.p2align 2
#ifdef NATIVE
.globl _native_work
_native_work:
    stp x19, x20, [sp, #-48]!
    stp x21, x22, [sp, #16]
    stp x23, x30, [sp, #32]
#else
.globl guest_work
guest_work:
#endif
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mov x22, x2
    mov x23, #0
1:  ldr x9, [x20]
    add x9, x9, #17
    eor x9, x9, x9, lsl #7
    ror x9, x9, #11
    str x9, [x20]
    cbz x21, 3f
    subs x22, x22, #1
    b.ne 3f
    mov x8, #172
#ifdef NATIVE
    mov x0, #11
#else
    svc #0
#endif
    add x23, x23, x0
    mov x22, x21
3:  subs x19, x19, #1
    b.ne 1b
    eor x0, x9, x23
#ifdef NATIVE
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    ldp x23, x30, [sp, #32]
    add sp, sp, #48
    ret
#else
    mov x8, #93
    svc #0
    brk #0

.globl guest_registers
guest_registers:
    svc #1
    svc #2
    brk #0

.globl guest_tls
guest_tls:
    mrs x5, tpidr_el0
    mov x7, #0x678
    msr tpidr_el0, x7
    mrs x6, tpidr_el0
    mov x8, #93
    svc #0
    brk #0

.globl guest_x18
guest_x18:
    mov x9, x18
    mov w10, w18
    mov x18, x2
    mov x8, #172
    svc #0
    mov x11, x18
    mov w18, w3
    mov x12, x18
    mov x18, xzr
    mov x13, x18
    mov x18, x18
    mov xzr, x18
    mov x8, #93
    svc #0
    brk #0

.globl guest_fault_read, guest_fault_write
.globl guest_x18_memory
guest_x18_memory:
    stp x18, x19, [x0, #144]
    mov x18, xzr
    mov x19, xzr
    ldp x18, x19, [x0, #144]
    and x3, x18, #0x3fff
    mov x5, x3
    and x18, x18, #0xffffffffffffc000
    orr x18, x3, x18
    mov x4, x18
    mov x8, #93
    svc #0
    brk #0

guest_fault_read:
    ldr x0, [x0]
    mov x8, #93
    svc #0
    brk #0
guest_fault_write:
    str x1, [x0]
    mov x8, #93
    svc #0
    brk #0

.globl guest_update
guest_update:
    mov x0, #41
    mov x8, #93
    svc #0
    brk #0

.globl guest_process
guest_process:
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mov x22, #0
1:  ldr x5, [x20]
    add x5, x5, #1
    str x5, [x20]
    ldr x6, [x21]
    add x6, x6, #1
    str x6, [x21]
    mrs x4, tpidr_el0
    add x22, x22, #1
    // Each address space keeps its own stack contents across a switch.
    stp x5, x22, [sp, #-16]!
    svc #3
    ldp x9, x10, [sp], #16
    cmp x9, x5
    b.ne 2f
    cmp x10, x22
    b.ne 2f
    subs x19, x19, #1
    b.ne 1b
    mov x8, #93
    mov x0, #0
    svc #0
2:  mov x8, #93
    mov x0, #1
    svc #0
    brk #0

.globl guest_linux_file
guest_linux_file:
    mov x0, #-100
    adr x1, 3f
    mov x2, #66
    mov x3, #384
    mov x8, #56
    svc #0
    tbnz x0, #63, 9f
    mov x19, x0
    adr x1, 4f
    mov x2, #8
    mov x8, #64
    svc #0
    cmp x0, #8
    b.ne 9f
    mov x0, x19
    mov x1, #0
    mov x2, #0
    mov x8, #62
    svc #0
    cbnz x0, 9f
    sub sp, sp, #32
    mov x0, x19
    mov x1, sp
    mov x2, #8
    mov x8, #63
    svc #0
    cmp x0, #8
    b.ne 9f
    ldr x1, [sp]
    adr x2, 4f
    ldr x2, [x2]
    cmp x1, x2
    b.ne 9f
    mov x0, x19
    mov x8, #57
    svc #0
    cbnz x0, 9f
    // Linux clockevent and host timer must wake a blocking syscall.
    str xzr, [sp]
    mov x1, #0x4240
    movk x1, #0xf, lsl #16
    str x1, [sp, #8]
    mov x0, sp
    mov x1, #0
    mov x8, #101
    svc #0
    cbnz x0, 9f
    add sp, sp, #32
    mov x0, #0
    mov x8, #93
    svc #0
9:  mov x0, #1
    mov x8, #93
    svc #0
    brk #0
.balign 8
3:  .asciz "/nce-probe"
.balign 8
4:  .quad 0x123456789abcdef0
.balign 4
#endif

// Linux owns the mappings, child PID, COW, and wait status.
.p2align 2
.global guest_linux_process
guest_linux_process:
    add x0, x21, #0x100, lsl #12
    mov x1, #65536
    mov x2, #3
    mov x3, #0x32
    mov x4, #-1
    mov x5, #0
    mov x8, #222
    svc #0
    tbnz x0, #63, process_fail
    mov x19, x0
    mov x9, #7
    str x9, [x19]
    add x0, x21, #0x200, lsl #12
    mov x1, #16384
    mov x2, #3
    mov x3, #0x31
    mov x4, #-1
    mov x5, #0
    mov x8, #222
    svc #0
    tbnz x0, #63, process_fail
    mov x22, x0
    mov x9, #77
    str x9, [x22]
    add x0, x19, #16
    mov x1, #0
    mov x8, #59
    svc #0
    cbnz x0, process_fail
    mov x9, #123
    msr tpidr_el0, x9
    mov x0, #17
    movk x0, #8, lsl #16
    mov x1, #0
    mov x2, #0
    mov x3, #456
    mov x4, #0
    mov x8, #220
    svc #0
    tbnz x0, #63, process_fail
    cbz x0, process_child
    mov x20, x0
    mov x0, x22
    mov x1, #0
    mov x2, #77
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x8, #98
    svc #0
    cbnz x0, process_fail
    ldr w0, [x19, #16]
    add x1, x19, #24
    mov x2, #8
    mov x8, #63
    svc #0
    cmp x0, #8
    b.ne process_fail
    ldr x9, [x19, #24]
    cmp x9, #90
    b.ne process_fail
    ldr x9, [x22]
    cmp x9, #88
    b.ne process_fail
    mov x0, x20
    add x1, x19, #8
    mov x2, #0
    mov x3, #0
    mov x8, #260
    svc #0
    tbnz x0, #63, process_fail
    ldr x9, [x19]
    cmp x9, #7
    b.ne process_fail
    ldr w9, [x19, #8]
    mov w10, #10752
    cmp w9, w10
    b.ne process_fail
    mrs x9, tpidr_el0
    cmp x9, #123
    b.ne process_fail
    mov x0, #0
    b process_exit
process_child:
    mrs x9, tpidr_el0
    cmp x9, #456
    b.ne process_fail
    ldr x9, [x19]
    cmp x9, #7
    b.ne process_fail
    mov x9, #11
    str x9, [x19]
    mov x9, #77
    str x9, [x22]
process_wake:
    mov x0, x22
    mov x1, #1
    mov x2, #1
    mov x3, #0
    mov x4, #0
    mov x5, #0
    mov x8, #98
    svc #0
    cmp x0, #1
    b.eq process_awakened
    cbnz x0, process_fail
    mov x8, #124
    svc #0
    b process_wake
process_awakened:
    mov x9, #88
    str x9, [x22]
    mov x9, #90
    str x9, [x19, #32]
    ldr w0, [x19, #20]
    add x1, x19, #32
    mov x2, #8
    mov x8, #64
    svc #0
    cmp x0, #8
    b.ne process_fail
    mov x9, #789
    msr tpidr_el0, x9
    // Linux string helpers require the pathname inside the guest address range.
    adr x9, process_exec_path
    ldr x10, [x9]
    ldrh w11, [x9, #8]
    sub sp, sp, #64
    str x10, [sp, #32]
    strh w11, [sp, #40]
    add x0, sp, #32
    stp x0, xzr, [sp]
    mov x1, sp
    mov x2, #0
    mov x8, #221
    svc #0
    b process_fail
process_fail:
    mov x0, #99
process_exit:
    mov x8, #93
    svc #0
    brk #0

.p2align 2
process_exec_path:
    .asciz "/nce-exec"
.p2align 2

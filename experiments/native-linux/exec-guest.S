/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.p2align 2
.global _start, exec_text_end
_start:
    // Linux supplies argc, argv and the auxiliary vector on the new stack.
    ldr x9, [sp]
    cmp x9, #1
    b.ne fail
    ldr x9, [sp, #8]
    ldrb w9, [x9]
    cmp w9, #47
    b.ne fail
    mrs x9, tpidr_el0
    cbnz x9, fail
    mov x8, #172
    svc #0
    cmp x0, #1
    b.le fail
    mov x0, #42
    b finish
fail:
    mov x0, #98
finish:
    mov x8, #93
    svc #0
    brk #0
exec_text_end:
    .space 65536 - (. - _start)
.section .note.GNU-stack,"",%progbits

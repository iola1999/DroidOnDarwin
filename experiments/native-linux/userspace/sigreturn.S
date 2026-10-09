/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.p2align 2
.global _start
.type _start, %function
_start:
    mov x8, #139
    svc #0
    .inst 0
.size _start, . - _start
.section .note.GNU-stack,"",%progbits

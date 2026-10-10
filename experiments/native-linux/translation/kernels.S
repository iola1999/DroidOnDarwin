/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.p2align 2
.globl _translation_integer, _translation_integer_end
_translation_integer:
    mov x0, #17
    mov x2, #28657
    mov x3, #83
1:
    madd x0, x0, x2, x3
    eor x0, x0, x0, lsr #29
    subs x1, x1, #1
    b.ne 1b
    ret
_translation_integer_end:

.p2align 2
.globl _translation_memory, _translation_memory_end
_translation_memory:
    mov x4, x0
    mov x0, #17
    mov x2, #0
1:
    and x3, x2, #8191
    ldr x5, [x4, x3, lsl #3]
    add x0, x0, x5
    eor x0, x0, x0, lsr #29
    str x0, [x4, x3, lsl #3]
    add x2, x2, #1
    subs x1, x1, #1
    b.ne 1b
    ret
_translation_memory_end:

.p2align 2
.globl _translation_atomic, _translation_atomic_end
_translation_atomic:
    mov x4, x0
1:
    ldaxr x0, [x4]
    add x0, x0, #1
    stlxr w2, x0, [x4]
    cbnz w2, 1b
    subs x1, x1, #1
    b.ne 1b
    ret
_translation_atomic_end:

/* This guest-only function may use x18 and a guest TLS register. */
.p2align 2
.globl _translation_state, _translation_state_end
_translation_state:
    mrs x0, tpidr_el0
    add x18, x18, #1
    eor x0, x0, x18
    add v0.2d, v0.2d, v1.2d
    ret
_translation_state_end:

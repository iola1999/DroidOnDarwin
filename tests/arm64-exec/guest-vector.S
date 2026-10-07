/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.balign 16
.global husk_vector_loop
husk_vector_loop:
    movi v0.2d, #0
    mov x1, #1
    dup v1.2d, x1
1:  add v0.2d, v0.2d, v1.2d
    subs x0, x0, #1
    b.ne 1b
    umov x0, v0.d[0]
    ret

.global husk_float_loop
husk_float_loop:
    fmov d0, #1.0
    fmov d1, #1.0
1:  fadd d0, d0, d1
    subs x0, x0, #1
    b.ne 1b
    fcvtzu x0, d0
    ret

.global husk_vector_memory
husk_vector_memory:
    mov x2, #1
    dup v1.2d, x2
1:  ldr q0, [x0]
    add v0.2d, v0.2d, v1.2d
    str q0, [x0]
    subs x1, x1, #1
    b.ne 1b
    ret

.global husk_vector_checks
husk_vector_checks:
    msr fpcr, xzr
    msr fpsr, xzr
    fmov v1.2s, #1.0
    fmov v2.2s, #2.0
    mov x1, #-1
    dup v4.2d, x1
    movi v5.2d, #0
    mov x1, #1
    ins v4.d[1], x1
    ins v5.d[1], x1
    // Long loops exercise QEMU/Direct transitions before checking architectural
    // flags. Inactive FDIV lanes must not raise an invalid-operation exception.
1:  fdiv v0.2s, v1.2s, v2.2s
    cmhi v3.2d, v4.2d, v5.2d
    cmhs v6.2d, v4.2d, v5.2d
    subs x0, x0, #1
    b.ne 1b
    mov x0, #1
    umov w1, v0.s[0]
    mov w2, #0x3f000000
    cmp w1, w2
    b.ne 2f
    mrs x1, fpsr
    cbnz x1, 2f
    umov x1, v3.d[0]
    cmn x1, #1
    b.ne 2f
    umov x1, v3.d[1]
    cbnz x1, 2f
    umov x1, v6.d[0]
    cmn x1, #1
    b.ne 2f
    umov x1, v6.d[1]
    cmn x1, #1
    b.ne 2f
    mov x0, #0
2:  ret

.balign 4096
.global husk_call_loop
husk_call_loop:
    stp x19, x30, [sp, #-16]!
    mov x19, x0
    mov x0, #0
1:  bl husk_call_target
    subs x19, x19, #1
    b.ne 1b
    ldp x19, x30, [sp], #16
    ret
.balign 4096
husk_call_target:
    add x0, x0, #1
    ret

.balign 4096
.global husk_indirect_call_loop
husk_indirect_call_loop:
    stp x19, x30, [sp, #-32]!
    str x20, [sp, #16]
    mov x19, x0
    mov x0, #0
    adr x20, husk_indirect_call_target
1:  blr x20
    subs x19, x19, #1
    b.ne 1b
    ldr x20, [sp, #16]
    ldp x19, x30, [sp], #32
    ret
.balign 4096
husk_indirect_call_target:
    hint #34 // BTI c, also a valid hint on earlier CPUs.
    add x0, x0, #1
    ret

.balign 16
.global husk_pauth_loop
husk_pauth_loop:
    mov x2, x30
    mov x16, #0x1234
1:  paciasp
    autiasp
    cmp x30, x2
    b.ne 2f
    pacibsp
    autibsp
    cmp x30, x2
    b.ne 2f
    paciaz
    autiaz
    cmp x30, x2
    b.ne 2f
    pacibz
    autibz
    cmp x30, x2
    b.ne 2f
    mov x17, x2
    pacia1716
    autia1716
    cmp x17, x2
    b.ne 2f
    pacib1716
    autib1716
    cmp x17, x2
    b.ne 2f
    subs x0, x0, #1
    b.ne 1b
    ret
2:  mov x30, x2
    mov x0, #1
    ret

.global husk_pauth_invalid
husk_pauth_invalid:
    stp x19, x30, [sp, #-16]!
    bl husk_pauth_loop
    ldp x19, x30, [sp], #16
    paciasp
    eor x30, x30, #0x1000000000000
    autiasp
    ret

.arch_extension lse
.global husk_atomic_loop
husk_atomic_loop:
    mov x2, #1
1:  ldaddal x2, x3, [x0]
    add x3, x3, #1
    add x4, x3, #1
    mov x5, x3
    casal x3, x4, [x0]
    cmp x3, x5
    b.ne 2f
    subs x1, x1, #1
    b.ne 1b
    mov x0, #0
    ret
2:  mov x0, #1
    ret

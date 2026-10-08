/* SPDX-License-Identifier: GPL-2.0-or-later */
.text
.balign 16
.global husk_neon_copy
husk_neon_copy:
1:  ldp q0, q1, [x1], #32
    ldp q2, q3, [x1], #32
    stp q0, q1, [x0], #32
    stp q2, q3, [x0], #32
    subs x2, x2, #64
    b.ne 1b
    ret

.global husk_neon_set
husk_neon_set:
    dup v0.16b, w1
1:  stp q0, q0, [x0], #32
    stp q0, q0, [x0], #32
    subs x2, x2, #64
    b.ne 1b
    ret

.global husk_bulk_register_check
husk_bulk_register_check:
    stp x19, x20, [sp, #-48]!
    stp x21, x22, [sp, #16]
    str x30, [sp, #32]
    mov x19, x0
    mov x20, x1
    mov x21, x2
    mov x22, x3
    mov x4, #0x1234
    mov x17, #0x2345
    movi v0.16b, #0x55
    movi v31.16b, #0xaa
    mov x9, #0xa0000000
    msr nzcv, x9
    mov x10, #0x1b
    msr fpsr, x10
    hlt #0x484d
    mrs x11, nzcv
    cmp x11, x9
    b.ne 2f
    mrs x11, fpsr
    cmp x11, x10
    b.ne 2f
    cmp x0, x19
    b.ne 2f
    cmp x1, x20
    b.ne 2f
    cmp x2, x21
    b.ne 2f
    cmp x3, x22
    b.ne 2f
    mov x11, #0x1234
    cmp x4, x11
    b.ne 2f
    mov x11, #0x2345
    cmp x17, x11
    b.ne 2f
    umov x11, v0.d[1]
    mov x12, #0x5555555555555555
    cmp x11, x12
    b.ne 2f
    umov x11, v31.d[1]
    mov x12, #0xaaaaaaaaaaaaaaaa
    cmp x11, x12
    b.ne 2f
    mov x0, #0
    b 3f
2:  mov x0, #1
3:  msr fpsr, xzr
    ldp x21, x22, [sp, #16]
    ldr x30, [sp, #32]
    ldp x19, x20, [sp], #48
    ret

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

.global husk_vector_structures
husk_vector_structures:
1:  mov x2, x0
    ld1 {v0.16b, v1.16b}, [x2], #32
    cnt v0.16b, v0.16b
    cnt v1.16b, v1.16b
    uminp v2.16b, v0.16b, v1.16b
    cmeq v3.16b, v2.16b, #0
    st1 {v2.16b, v3.16b}, [x2]
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
.global husk_plain_call_loop
husk_plain_call_loop:
    stp x19, x30, [sp, #-32]!
    str x20, [sp, #16]
    mov x19, x0
    mov x0, #0
    adr x20, husk_plain_call_target
1:  blr x20
    subs x19, x19, #1
    b.ne 1b
    ldr x20, [sp, #16]
    ldp x19, x30, [sp], #32
    ret
.balign 4096
husk_plain_call_target:
    stp x29, x30, [sp, #-16]!
    add x0, x0, #1
    ldp x29, x30, [sp], #16
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
.global husk_exclusive_loop
husk_exclusive_loop:
1:  ldaxr x2, [x0]
    add x2, x2, #1
    stlxr w3, x2, [x0]
    cbnz w3, 1b
    subs x1, x1, #1
    b.ne 1b
    mov x0, #0
    ret

.global husk_exclusive_bridge
husk_exclusive_bridge:
1:  ldaxr x2, [x0]
    isb
    add x2, x2, #1
    stlxr w3, x2, [x0]
    cbnz w3, 1b
    subs x1, x1, #1
    b.ne 1b
    mov x0, #0
    ret

.global husk_tail_call_loop
husk_tail_call_loop:
    stp x19, x30, [sp, #-16]!
    mov x19, x0
    mov x0, #0
    adr x16, 3f
1:  bl 2f
    subs x19, x19, #1
    b.ne 1b
    ldp x19, x30, [sp], #16
    ret
2:  br x16
3:  add x0, x0, #1
    ret

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

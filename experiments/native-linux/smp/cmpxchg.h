/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_CMPXCHG_H
#define _ASM_LKL_CMPXCHG_H

/* The standalone SMP build targets AArch64 with inline atomics. */
#define arch_xchg(ptr, value) __atomic_exchange_n((ptr), (value), __ATOMIC_SEQ_CST)
#define arch_cmpxchg(ptr, old, new) \
    ({ \
        __typeof__(*(ptr)) expected = (old); \
        __atomic_compare_exchange_n((ptr), &expected, (new), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); \
        expected; \
    })
#define arch_cmpxchg_local arch_cmpxchg
#define arch_cmpxchg64 arch_cmpxchg
#define arch_cmpxchg64_local arch_cmpxchg

#endif

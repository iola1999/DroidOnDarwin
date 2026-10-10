/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_SMP_FUTEX_H
#define _ASM_LKL_SMP_FUTEX_H

#include <linux/mutex.h>
#include <linux/futex.h>
#include <linux/uaccess.h>
#include <asm/errno.h>

#ifdef CONFIG_MMU
#include <asm/nce-smp-uaccess.h>
#endif

/* Host tasks supply live host allocations. Guest tasks use the MMU's locked
 * RAM alias. Both paths reject NULL capability probes and unaligned words. */
static inline int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
                                                u32 oldval, u32 newval)
{
    if (!uaddr || ((unsigned long)uaddr & (sizeof(u32) - 1)))
        return -EFAULT;
#ifdef CONFIG_MMU
    if (current->thread.nce_state)
        return nce_futex_cmpxchg(uval, uaddr, oldval, newval);
#endif
    __atomic_compare_exchange_n(uaddr, &oldval, newval, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *uval = oldval;
    return 0;
}

static inline int arch_futex_atomic_op_inuser(int op, u32 oparg, int *oval,
                                              u32 __user *uaddr)
{
    if (!uaddr || ((unsigned long)uaddr & (sizeof(u32) - 1)))
        return -EFAULT;
#ifdef CONFIG_MMU
    if (current->thread.nce_state)
        return nce_futex_operation(op, oparg, oval, uaddr);
#endif
    switch (op) {
        case FUTEX_OP_SET:
            *oval = __atomic_exchange_n(uaddr, oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ADD:
            *oval = __atomic_fetch_add(uaddr, oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_OR:
            *oval = __atomic_fetch_or(uaddr, oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_ANDN:
            *oval = __atomic_fetch_and(uaddr, ~oparg, __ATOMIC_SEQ_CST);
            break;
        case FUTEX_OP_XOR:
            *oval = __atomic_fetch_xor(uaddr, oparg, __ATOMIC_SEQ_CST);
            break;
        default:
            return -ENOSYS;
    }
    return 0;
}

#endif

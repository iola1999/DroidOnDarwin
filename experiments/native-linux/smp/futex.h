/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_SMP_FUTEX_H
#define _ASM_LKL_SMP_FUTEX_H

#include <linux/mutex.h>
#include <linux/futex.h>
#include <linux/uaccess.h>
#include <asm/errno.h>

#ifdef CONFIG_MMU
#error "SMP futex prototype requires trusted NOMMU host pointers"
#endif

/* The NOMMU harness supplies live host allocations. Reject the NULL capability
 * probe and unaligned words; arbitrary guest pointers require MMU uaccess. */
static inline int futex_atomic_cmpxchg_inatomic(u32 *uval, u32 __user *uaddr,
                                                u32 oldval, u32 newval)
{
    if (!uaddr || ((unsigned long)uaddr & (sizeof(u32) - 1)))
        return -EFAULT;
    __atomic_compare_exchange_n(uaddr, &oldval, newval, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    *uval = oldval;
    return 0;
}

static inline int arch_futex_atomic_op_inuser(int op, u32 oparg, int *oval,
                                              u32 __user *uaddr)
{
    if (!uaddr || ((unsigned long)uaddr & (sizeof(u32) - 1)))
        return -EFAULT;
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

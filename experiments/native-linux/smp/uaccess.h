/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LKL_NCE_SMP_UACCESS_H
#define _LKL_NCE_SMP_UACCESS_H
#include <linux/types.h>

unsigned long nce_copy_user_smp(void *kernel, unsigned long user,
                                unsigned long size, int operation);
int nce_futex_cmpxchg(u32 *value, u32 __user *address, u32 old, u32 next);
int nce_futex_operation(int operation, u32 argument, int *old, u32 __user *address);
#endif

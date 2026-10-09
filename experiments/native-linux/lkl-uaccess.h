/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LKL_NCE_UACCESS_H
#define _LKL_NCE_UACCESS_H
#ifdef CONFIG_MMU
#include <linux/string.h>
int nce_access_ok(const void *ptr, unsigned long size);
unsigned long nce_copy_user(void *kernel, unsigned long user, unsigned long size, int operation);
#define __access_ok(addr, size) nce_access_ok((const void *)(addr), size)
static inline unsigned long raw_copy_from_user(void *to, const void __user *from, unsigned long size)
{
    return nce_copy_user(to, (unsigned long)from, size, 0);
}
static inline unsigned long raw_copy_to_user(void __user *to, const void *from, unsigned long size)
{
    return nce_copy_user((void *)from, (unsigned long)to, size, 1);
}
static inline unsigned long nce_clear_user(void __user *to, unsigned long size)
{
    return nce_copy_user(NULL, (unsigned long)to, size, 2);
}
#define __clear_user nce_clear_user
/* Kernel nofault accesses retain LKL's trusted-pointer contract. */
#define __get_kernel_nofault(dst, src, type, err_label) \
    do { \
        memcpy(dst, src, sizeof(type)); \
        if (0) \
            goto err_label; \
    } while (0)
#define __put_kernel_nofault(dst, src, type, err_label) \
    do { \
        memcpy(dst, src, sizeof(type)); \
        if (0) \
            goto err_label; \
    } while (0)
#endif
#include <asm-generic/uaccess.h>
#endif

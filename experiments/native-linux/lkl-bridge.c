/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compiled with the Linux AArch64 ABI. Variadic calls stay on this side. */
#include "lkl-host-ops.h"
#include "layout.h"
extern struct lkl_host_operations *lkl_ops;
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);
extern unsigned int sysctl_sched_base_slice;

static int print(const char *format, __builtin_va_list args)
{
    char buffer[1024];
    int length = vsnprintf(buffer, sizeof(buffer), format, args);
    if (length > 0 && lkl_ops->print)
        lkl_ops->print(buffer, length < (int)sizeof(buffer) ? length : (int)sizeof(buffer) - 1);
    return length;
}
int lkl_printf(const char *format, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int result = print(format, args);
    __builtin_va_end(args);
    return result;
}
void lkl_bug(const char *format, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, format);
    print(format, args);
    __builtin_va_end(args);
    lkl_ops->panic();
}
int nce_lkl_start(unsigned long slice_ns, unsigned long *applied_slice_ns)
{
    if (!applied_slice_ns || (slice_ns && (slice_ns < 100000 || slice_ns > 100000000)))
        return -22;
    int result = lkl_start_kernel("mem=%luM loglevel=4", NCE_RAM_SIZE / (1024 * 1024));
    if (result)
        return result;
    /* Apply after boot-time CPU scaling. The runtime keeps its CPU set fixed;
     * later CPU hotplug would require updating the normalized scheduler value. */
    if (slice_ns)
        __atomic_store_n(&sysctl_sched_base_slice, slice_ns, __ATOMIC_RELAXED);
    *applied_slice_ns = __atomic_load_n(&sysctl_sched_base_slice, __ATOMIC_RELAXED);
    return 0;
}

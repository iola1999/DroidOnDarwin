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

static int cmdline_length(const char *format, ...)
{
    __builtin_va_list args;
    __builtin_va_start(args, format);
    int length = vsnprintf(0, 0, format, args);
    __builtin_va_end(args);
    return length;
}

int nce_lkl_start_cpus_cmdline(unsigned long slice_ns, unsigned long *applied_slice_ns, unsigned int cpus, const char *extra_cmdline)
{
    if (!applied_slice_ns || cpus > 2 || (slice_ns && (slice_ns < 100000 || slice_ns > 100000000)))
        return -22;
    int result;
    /* Zero preserves the kernel's configured CPU set for existing callers. */
    if (extra_cmdline && *extra_cmdline) {
        /* arch/lkl/kernel/setup.c uses COMMAND_LINE_SIZE=4096 and first adds
         * the builtin cmdline plus a space. Current builds have an empty
         * builtin cmdline and no host virtio_devices suffix. Reserve that
         * space and the NUL; bound measurement by the actual buffer size. */
        const int capacity = 4096;
        int length = cpus ? cmdline_length("mem=%luM loglevel=4 maxcpus=%u %.*s", NCE_RAM_SIZE / (1024 * 1024), cpus, capacity, extra_cmdline)
                          : cmdline_length("mem=%luM loglevel=4 %.*s", NCE_RAM_SIZE / (1024 * 1024), capacity, extra_cmdline);
        if (length < 0 || length > capacity - 2)
            return -7;
        result = cpus ? lkl_start_kernel("mem=%luM loglevel=4 maxcpus=%u %s", NCE_RAM_SIZE / (1024 * 1024), cpus, extra_cmdline)
                      : lkl_start_kernel("mem=%luM loglevel=4 %s", NCE_RAM_SIZE / (1024 * 1024), extra_cmdline);
    } else {
        result = cpus ? lkl_start_kernel("mem=%luM loglevel=4 maxcpus=%u", NCE_RAM_SIZE / (1024 * 1024), cpus)
                      : lkl_start_kernel("mem=%luM loglevel=4", NCE_RAM_SIZE / (1024 * 1024));
    }
    if (result)
        return result;
    /* Apply after boot-time CPU scaling. The runtime keeps its CPU set fixed;
     * later CPU hotplug would require updating the normalized scheduler value. */
    if (slice_ns)
        __atomic_store_n(&sysctl_sched_base_slice, slice_ns, __ATOMIC_RELAXED);
    *applied_slice_ns = __atomic_load_n(&sysctl_sched_base_slice, __ATOMIC_RELAXED);
    return 0;
}

int nce_lkl_start_cpus(unsigned long slice_ns, unsigned long *applied_slice_ns, unsigned int cpus)
{
    return nce_lkl_start_cpus_cmdline(slice_ns, applied_slice_ns, cpus, 0);
}

int nce_lkl_start(unsigned long slice_ns, unsigned long *applied_slice_ns)
{
    return nce_lkl_start_cpus(slice_ns, applied_slice_ns, 0);
}

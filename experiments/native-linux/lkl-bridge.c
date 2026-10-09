/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compiled with the Linux AArch64 ABI. Variadic calls stay on this side. */
#include "lkl-host-ops.h"
#include "layout.h"
extern struct lkl_host_operations *lkl_ops;
extern int vsnprintf(char *, unsigned long, const char *, __builtin_va_list);

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
int nce_lkl_start(void) { return lkl_start_kernel("mem=%luM loglevel=4", NCE_RAM_SIZE / (1024 * 1024)); }

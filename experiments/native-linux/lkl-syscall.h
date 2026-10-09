/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NCE_LKL_SYSCALL_H
#define NCE_LKL_SYSCALL_H
#include <linux/audit.h>
#include <linux/err.h>
#include <asm/guest-registers.h>

struct task_struct;
static inline int syscall_get_arch(struct task_struct *task)
{
    return AUDIT_ARCH_AARCH64;
}
static inline int syscall_get_nr(struct task_struct *task, struct pt_regs *regs)
{
    return regs ? (int)regs->x[8] : -1;
}
static inline void syscall_get_arguments(struct task_struct *task, struct pt_regs *regs,
                                         unsigned long *args)
{
    for (unsigned int i = 0; i < 6; ++i)
        args[i] = regs->x[i];
}
static inline long syscall_get_return_value(struct task_struct *task, struct pt_regs *regs)
{
    return regs->x[0];
}
static inline long syscall_get_error(struct task_struct *task, struct pt_regs *regs)
{
    return IS_ERR_VALUE(regs->x[0]) ? regs->x[0] : 0;
}
static inline void syscall_set_return_value(struct task_struct *task, struct pt_regs *regs,
                                            int error, long value)
{
    regs->x[0] = error ? error : value;
}
/* Filtering runs before nce_run_syscall changes the saved argument registers. */
static inline void syscall_rollback(struct task_struct *task, struct pt_regs *regs) {}
#endif

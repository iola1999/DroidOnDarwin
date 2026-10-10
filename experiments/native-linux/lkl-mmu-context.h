/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_MMU_CONTEXT_H
#define _ASM_LKL_MMU_CONTEXT_H
#ifdef CONFIG_MMU
void nce_prepare_user(void);
int nce_init_mm(struct task_struct *task, struct mm_struct *mm);
void nce_destroy_mm(struct mm_struct *mm);
#define init_new_context nce_init_mm
#define destroy_context nce_destroy_mm
static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
                             struct task_struct *tsk)
{
    /* Kernel user copies use RAM aliases. Restore the host window only when
     * this task is about to execute or directly access guest user memory. */
}
#include <asm-generic/mm_hooks.h>
#include <asm-generic/mmu_context.h>
#else
#include <asm-generic/nommu_context.h>
#endif
#endif

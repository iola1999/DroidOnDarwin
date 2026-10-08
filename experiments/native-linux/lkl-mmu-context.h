/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ASM_LKL_MMU_CONTEXT_H
#define _ASM_LKL_MMU_CONTEXT_H
#ifdef CONFIG_MMU
void nce_switch_mm(struct mm_struct *next);
static inline void switch_mm(struct mm_struct *prev, struct mm_struct *next,
                             struct task_struct *tsk)
{
    nce_switch_mm(next);
}
#include <asm-generic/mm_hooks.h>
#include <asm-generic/mmu_context.h>
#else
#include <asm-generic/nommu_context.h>
#endif
#endif

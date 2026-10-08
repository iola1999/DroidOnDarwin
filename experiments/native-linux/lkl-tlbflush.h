/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LKL_NCE_TLBFLUSH_H
#define _LKL_NCE_TLBFLUSH_H
#include <linux/mm.h>

#ifdef CONFIG_MMU
void nce_flush_range(struct mm_struct *mm, unsigned long start, unsigned long end);
static inline void flush_tlb_mm(struct mm_struct *mm)
{
    nce_flush_range(mm, nce_user_base, TASK_SIZE);
}
static inline void flush_tlb_range(struct vm_area_struct *vma,
                                  unsigned long start, unsigned long end)
{
    nce_flush_range(vma->vm_mm, start, end);
}
static inline void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
    nce_flush_range(vma->vm_mm, addr, addr + PAGE_SIZE);
}
#else
static inline void flush_tlb_mm(struct mm_struct *mm) {}
static inline void flush_tlb_range(struct vm_area_struct *vma, unsigned long start, unsigned long end) {}
static inline void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr) {}
#endif
static inline void flush_tlb_kernel_range(unsigned long start, unsigned long end) {}
#endif

/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <asm/host_ops.h>
#include <asm/nce-layout.h>

unsigned long nce_user_base, nce_task_size, nce_memory_start;
void nce_set_layout(unsigned long base)
{
    nce_user_base = base;
    nce_task_size = base + NCE_USER_SIZE;
    nce_memory_start = nce_task_size;
}
#define USER_BASE nce_user_base
static struct mm_struct *active_mm;

static pte_t lookup(struct mm_struct *mm, unsigned long addr)
{
    pgd_t *pgd = pgd_offset(mm, addr);
    p4d_t *p4d = p4d_offset(pgd, addr);
    pud_t *pud = pud_offset(p4d, addr);
    pmd_t *pmd;
    if (pud_none(*pud)) return __pte(0);
    pmd = pmd_offset(pud, addr);
    if (pmd_none(*pmd)) return __pte(0);
    return *pte_offset_kernel(pmd, addr);
}

void mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va, unsigned int nr, pte_t pte)
{
    enum lkl_prot prot = 0;
    unsigned long pa, offset;
    if (mm != &init_mm && mm != active_mm) return;
    if (pte_val(pte) & _PAGE_PROTNONE) {
        BUG_ON(lkl_ops->munmap((void *)va, PAGE_SIZE * nr));
        return;
    }
    prot = LKL_PROT_READ;
    if (pte_write(pte)) prot |= LKL_PROT_WRITE;
    if (pte_exec(pte)) prot |= LKL_PROT_EXEC;
    pa = pte_val(pte) & PAGE_MASK;
    offset = pa - memory_start;
    BUG_ON(lkl_ops->shmem_mmap((void *)va, offset, PAGE_SIZE * nr, prot) != (void *)va);
}

void munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp)
{
    if (mm == &init_mm || mm == active_mm)
        BUG_ON(lkl_ops->munmap((void *)addr, PAGE_SIZE));
}

void nce_flush_range(struct mm_struct *mm, unsigned long start, unsigned long end)
{
    struct vm_area_struct *vma;
    unsigned long addr;
    VMA_ITERATOR(iter, mm, start);
    if (mm != active_mm) return;
    start = max(start & PAGE_MASK, USER_BASE);
    end = min(PAGE_ALIGN(end), TASK_SIZE);
    if (end <= start) return;
    BUG_ON(lkl_ops->munmap((void *)start, end - start));
    for_each_vma_range(iter, vma, end) {
        for (addr = max(start, vma->vm_start); addr < min(end, vma->vm_end); addr += PAGE_SIZE) {
            pte_t pte = lookup(mm, addr);
            if (pte_present(pte)) mmap_pages_for_ptes(mm, addr, 1, pte);
        }
    }
}

void nce_switch_mm(struct mm_struct *next)
{
    if (next == active_mm) return;
    active_mm = next;
    nce_flush_range(next, USER_BASE, TASK_SIZE);
}

/* Called only for a fault in the controlled guest window while the Linux CPU
 * is owned by the faulting pthread. Invalid mappings return an error. */
int nce_page_fault(unsigned long address, int write)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    vm_fault_t result;
    if (!mm || address < USER_BASE || address >= TASK_SIZE) return -EFAULT;
    mmap_read_lock(mm);
    vma = find_vma(mm, address);
    if (!vma || address < vma->vm_start || !(vma->vm_flags & (write ? VM_WRITE : VM_READ))) {
        mmap_read_unlock(mm);
        return -EFAULT;
    }
    result = handle_mm_fault(vma, address, write ? FAULT_FLAG_WRITE : 0, NULL);
    mmap_read_unlock(mm);
    return result & VM_FAULT_ERROR ? -EFAULT : 0;
}

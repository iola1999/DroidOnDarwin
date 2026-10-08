/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/pgtable.h>
#include <linux/bitops.h>
#include <linux/uaccess.h>
#include <asm/host_ops.h>
#include <asm/nce-layout.h>

unsigned long nce_user_base, nce_task_size, nce_memory_start;
static unsigned long mapping_mode;

void nce_set_layout(unsigned long base, unsigned long mode)
{
    mapping_mode = mode;
    nce_user_base = base;
    nce_task_size = base + NCE_USER_SIZE;
    nce_memory_start = nce_task_size;
}
#define USER_BASE nce_user_base
static struct mm_struct *active_mm;

int nce_init_mm(struct task_struct *task, struct mm_struct *mm)
{
    memset(mm->context.accessed, 0, sizeof(mm->context.accessed));
    return 0;
}

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

/* One Linux CPU owns the host window. Entries describe installed host views,
 * not another mm's page tables; clearing a Linux PTE invalidates its view. */
static unsigned long installed[NCE_USER_SIZE / PAGE_SIZE];
static unsigned long desired[NCE_USER_SIZE / PAGE_SIZE];

static unsigned long view(pte_t pte)
{
    if (!pte_present(pte) || (pte_val(pte) & _PAGE_PROTNONE)) return 0;
    return pte_val(pte) & (PAGE_MASK | _PAGE_PRESENT | _PAGE_RW | _PAGE_EXEC);
}

static void map_view(unsigned long addr, unsigned long size, unsigned long value)
{
    enum lkl_prot prot = LKL_PROT_READ;
    if (!value) {
        BUG_ON(lkl_ops->munmap((void *)addr, size));
        return;
    }
    if (value & _PAGE_RW) prot |= LKL_PROT_WRITE;
    if (value & _PAGE_EXEC) prot |= LKL_PROT_EXEC;
    BUG_ON(lkl_ops->shmem_mmap((void *)addr, (value & PAGE_MASK) - memory_start,
                              size, prot) != (void *)addr);
}

static void apply_views(unsigned long first, unsigned long count)
{
    unsigned long end = first + count;
    for (unsigned long i = first; i < end;) {
        unsigned long value = desired[i], stop = i + 1;
        if (installed[i] == value) { ++i; continue; }
        while (stop < end && installed[stop] != desired[stop] &&
               desired[stop] == (value ? value + (stop - i) * PAGE_SIZE : 0)) ++stop;
        map_view(USER_BASE + i * PAGE_SIZE, (stop - i) * PAGE_SIZE, value);
        memcpy(installed + i, desired + i, (stop - i) * sizeof(*installed));
        i = stop;
    }
}

void mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va, unsigned int nr, pte_t pte)
{
    unsigned long value = view(pte);
    if (mm != &init_mm && mm != active_mm) return;
    if (mapping_mode == NCE_MM_FULL || mm == &init_mm) {
        map_view(va, nr * PAGE_SIZE, value);
        return;
    }
    BUG_ON(va < USER_BASE || va + nr * PAGE_SIZE > TASK_SIZE);
    unsigned long first = (va - USER_BASE) / PAGE_SIZE;
    for (unsigned long i = 0; i < nr; ++i)
        desired[first + i] = value ? value + i * PAGE_SIZE : 0;
    apply_views(first, nr);
}

void munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp)
{
    if (mm != &init_mm && addr >= USER_BASE && addr < TASK_SIZE)
        __clear_bit((addr - USER_BASE) / PAGE_SIZE, mm->context.accessed);
    if (mm != &init_mm && mm != active_mm) return;
    if (mapping_mode == NCE_MM_FULL || mm == &init_mm) {
        map_view(addr, PAGE_SIZE, 0);
        return;
    }
    unsigned long first = (addr - USER_BASE) / PAGE_SIZE;
    BUG_ON(addr < USER_BASE || addr >= TASK_SIZE);
    desired[first] = 0;
    apply_views(first, 1);
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
    unsigned long first = (start - USER_BASE) / PAGE_SIZE;
    unsigned long count = (end - start) / PAGE_SIZE;
    if (mapping_mode == NCE_MM_FULL) map_view(start, end - start, 0);
    else memset(desired + first, 0, count * sizeof(*desired));
    for_each_vma_range(iter, vma, end) {
        for (addr = max(start, vma->vm_start); addr < min(end, vma->vm_end); addr += PAGE_SIZE) {
            pte_t pte = lookup(mm, addr);
            if (mapping_mode == NCE_MM_FULL) {
                if (pte_present(pte)) mmap_pages_for_ptes(mm, addr, 1, pte);
            } else {
                unsigned long index = (addr - USER_BASE) / PAGE_SIZE;
                unsigned long value = view(pte);
                /* Prepared code must contain the next process's instructions
                 * before entry. Changed data can wait until its first access. */
                if (mapping_mode == NCE_MM_LAZY && !(value & _PAGE_EXEC) &&
                    value != installed[index] && !test_bit(index, mm->context.accessed)) value = 0;
                desired[index] = value;
            }
        }
    }
    if (mapping_mode != NCE_MM_FULL) apply_views(first, count);
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
    unsigned long prior = view(lookup(mm, address));
    bool reused = prior && (!write || (prior & _PAGE_RW));
    result = handle_mm_fault(vma, address, write ? FAULT_FLAG_WRITE : 0, NULL);
    if (!(result & VM_FAULT_ERROR) && mapping_mode == NCE_MM_LAZY) {
        unsigned long first = address & PAGE_MASK;
        unsigned long value = view(lookup(mm, first));
        unsigned long stop = first + PAGE_SIZE;
        /* Restore a contiguous physical run, bounded to 512 KiB so a sparse
         * access does not eagerly reinstall an entire large private VMA. */
        while (value && stop < vma->vm_end && stop - first < 32 * PAGE_SIZE &&
               view(lookup(mm, stop)) == value + stop - first) stop += PAGE_SIZE;
        if (!value) result = VM_FAULT_SIGBUS;
        else {
            mmap_pages_for_ptes(mm, first, (stop - first) / PAGE_SIZE, __pte(value));
            /* Learn demand after a switch, excluding initial allocation and
             * COW setup. Dense readers then avoid repeated signal delivery. */
            if (reused)
                for (unsigned long addr = first; addr < stop; addr += PAGE_SIZE)
                    __set_bit((addr - USER_BASE) / PAGE_SIZE, mm->context.accessed);
        }
    }
    mmap_read_unlock(mm);
    return result & VM_FAULT_ERROR ? -EFAULT : 0;
}

int nce_access_ok(const void *ptr, unsigned long size)
{
    unsigned long address = (unsigned long)ptr;
    /* The explicit host syscall API passes host buffers. Guest syscalls use
     * only their Linux user window; an empty iovec touches no memory. */
    if (!current->thread.nce_state) return 1;
    return !size || (address >= USER_BASE && address <= TASK_SIZE && size <= TASK_SIZE - address);
}

unsigned long nce_copy_user(void *kernel, unsigned long user, unsigned long size, int operation)
{
    unsigned long remaining = size;
    if (!current->thread.nce_state) {
        if (operation == 2) memset((void *)user, 0, size);
        else if (operation) memcpy((void *)user, kernel, size);
        else memcpy(kernel, (void *)user, size);
        return 0;
    }
    if (!nce_access_ok((void *)user, size)) return size;
    while (remaining) {
        pte_t pte = lookup(current->mm, user);
        if (!pte_present(pte) || !pte_read(pte) || (operation && !pte_write(pte))) {
            if (pagefault_disabled() || in_atomic() || nce_page_fault(user, operation != 0)) break;
            pte = lookup(current->mm, user);
            if (!pte_present(pte) || !pte_read(pte) || (operation && !pte_write(pte))) break;
        }
        unsigned long part = min(remaining, PAGE_SIZE - (user & ~PAGE_MASK));
        void *physical = (void *)((pte_val(pte) & PAGE_MASK) + (user & ~PAGE_MASK));
        /* The single Linux CPU keeps this PTE stable during the copy. Using
         * its RAM alias avoids Darwin faults inside kernel critical sections. */
        if (operation == 2) memset(physical, 0, part);
        else if (operation) memcpy(physical, kernel, part);
        else memcpy(kernel, physical, part);
        user += part;
        if (kernel) kernel += part;
        remaining -= part;
    }
    return remaining;
}

#!/usr/bin/env python3
"""在独立的单核内核中启用 PTE 软件地址转换。"""
from pathlib import Path


def apply(source: Path, experiment: Path):
    path = source / 'arch/lkl/mm/nce_mmu.c'
    text = (experiment / 'lkl-nce-mmu.c').read_text()

    def edit(before, after):
        nonlocal text
        if text.count(before) != 1:
            raise RuntimeError('Translation MMU source mismatch: ' + before[:100])
        text = text.replace(before, after)

    edit('static unsigned long mapping_mode;', '''#include <asm/nce-translation.h>
static struct nce_translation_state *translation;
static unsigned long mapping_mode;''')
    edit('    installed = views;\n    desired = views + NCE_USER_SIZE / PAGE_SIZE;', '''    BUG_ON(mode != NCE_MM_TRANSLATED);
    translation = (void *)views;
    installed = desired = NULL;''')
    edit('    size_t bytes = NCE_USER_SIZE / PAGE_SIZE / 8;', '''    if (mapping_mode == NCE_MM_TRANSLATED) {
        mm->context.accessed = NULL;
        return 0;
    }
    size_t bytes = NCE_USER_SIZE / PAGE_SIZE / 8;''')
    edit('/* One Linux CPU owns the host window.', '''static void changed_mapping(struct mm_struct *mm, unsigned long start, unsigned long end)
{
    ++translation->mappings;
    if (mm != active_mm) {
        ++translation->inactive_mapping_changes;
        return;
    }
    start = max(start & PAGE_MASK, USER_BASE);
    end = min(PAGE_ALIGN(end), TASK_SIZE);
    if (end <= start)
        return;
    if (!translation->tlb_end) {
        translation->tlb_start = start;
        translation->tlb_end = end;
    } else {
        translation->tlb_start = min(translation->tlb_start, start);
        translation->tlb_end = max(translation->tlb_end, end);
    }
}

static void changed_code(pte_t pte, unsigned long pages)
{
    unsigned long index = ((pte_val(pte) & PAGE_MASK) - memory_start) >> PAGE_SHIFT;
    BUILD_BUG_ON(NCE_TRANSLATION_CODE_WORDS * 64 != NCE_RAM_SIZE / PAGE_SIZE);
    BUG_ON(index >= NCE_RAM_SIZE / PAGE_SIZE || pages > NCE_RAM_SIZE / PAGE_SIZE - index);
    bitmap_set(translation->code_pages, index, pages);
    ++translation->code;
}

/* The caller owns the sole Linux CPU. Fault handling and scheduling happen
 * only after leaving the translation engine. Low bits use LKL_PROT flags. */
unsigned long nce_translate_user(unsigned long address, unsigned long permission)
{
    struct mm_struct *mm = current->mm;
    if (!mm || address < USER_BASE || address >= TASK_SIZE)
        return 0;
    pte_t pte = lookup(mm, address);
    if (!pte_present(pte) || !pte_read(pte) || !pte_young(pte))
        return 0;
    unsigned long prot = LKL_PROT_READ;
    if (pte_write(pte) && pte_dirty(pte))
        prot |= LKL_PROT_WRITE;
    if (pte_exec(pte))
        prot |= LKL_PROT_EXEC;
    if (!(prot & permission))
        return 0;
    return ((pte_val(pte) & PAGE_MASK) - memory_start) | prot;
}

/* One Linux CPU owns the host window.''')
    edit('''    for (unsigned long addr = start; addr < end; addr += PAGE_SIZE) {
        unsigned long value = view(lookup(mm, addr));''', '''    if (mapping_mode == NCE_MM_TRANSLATED) {
        for (unsigned long addr = start; addr < end; addr += PAGE_SIZE)
            changed_code(lookup(mm, addr), 1);
        mmap_read_unlock(mm);
        return 0;
    }
    for (unsigned long addr = start; addr < end; addr += PAGE_SIZE) {
        unsigned long value = view(lookup(mm, addr));''')
    edit('''    unsigned long value = view(pte);
    if (mm != &init_mm && mm != active_mm)''', '''    unsigned long value = view(pte);
    if (mapping_mode == NCE_MM_TRANSLATED && mm != &init_mm) {
        changed_mapping(mm, va, va + nr * PAGE_SIZE);
        if (pte_exec(pte))
            changed_code(pte, nr);
        return;
    }
    if (mm != &init_mm && mm != active_mm)''')
    edit('''    if (mm != &init_mm && addr >= USER_BASE && addr < TASK_SIZE) {''', '''    if (mapping_mode == NCE_MM_TRANSLATED && mm != &init_mm) {
        changed_mapping(mm, addr, addr + PAGE_SIZE);
        if (pte_exec(*xp))
            changed_code(*xp, 1);
        return;
    }
    if (mm != &init_mm && addr >= USER_BASE && addr < TASK_SIZE) {''')
    edit('''    struct vm_area_struct *vma;
    unsigned long addr;
    VMA_ITERATOR(iter, mm, start);''', '''    if (mapping_mode == NCE_MM_TRANSLATED && mm != &init_mm) {
        changed_mapping(mm, start, end);
        return;
    }
    struct vm_area_struct *vma;
    unsigned long addr;
    VMA_ITERATOR(iter, mm, start);''')
    edit('    active_mm = next;\n    nce_flush_range(next, USER_BASE, TASK_SIZE);', '''    active_mm = next;
    if (mapping_mode == NCE_MM_TRANSLATED)
        ++translation->mm_switches;
    nce_flush_range(next, USER_BASE, TASK_SIZE);''')
    edit('''    if (access & NCE_FAULT_TRY_PRESENT)
        return restore_present_data(address, access & 1);''', '''    if (access & NCE_FAULT_TRY_PRESENT)
        return mapping_mode == NCE_MM_TRANSLATED ? 0 : restore_present_data(address, access & 1);''')
    edit('execute && mapping_mode != NCE_MM_FULL ? installed[index] : 0',
         'execute && mapping_mode != NCE_MM_FULL && mapping_mode != NCE_MM_TRANSLATED ? installed[index] : 0')
    edit('if (execute && prior && (mapping_mode == NCE_MM_FULL || trapped_view == prior))',
         'if (mapping_mode != NCE_MM_TRANSLATED && execute && prior && (mapping_mode == NCE_MM_FULL || trapped_view == prior))')
    edit('if (!(result & VM_FAULT_ERROR) && mapping_mode >= NCE_MM_LAZY)',
         'if (!(result & VM_FAULT_ERROR) && mapping_mode >= NCE_MM_LAZY && mapping_mode != NCE_MM_TRANSLATED)')
    # Reapply from the tracked MMU source so previous overlay output is never input.
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    header = source / 'arch/lkl/include/asm/nce-translation.h'
    content = (experiment / 'translation/kernel-api.h').read_bytes()
    if not header.exists() or header.read_bytes() != content:
        header.write_bytes(content)

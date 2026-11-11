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
#include <asm/nce-direct-page-table.h>
#include <linux/smp.h>
static struct nce_translation_state *translation;
static LIST_HEAD(translation_mms);
static struct nce_translation_cpu *cpu_translation(void)
{
    return &translation->cpus[raw_smp_processor_id()];
}
static unsigned long mapping_mode;''')
    edit('''    nce_task_size = base + NCE_USER_SIZE;
    nce_memory_start = nce_task_size;
    installed = views;
    desired = views + NCE_USER_SIZE / PAGE_SIZE;''', '''    BUG_ON(mode != NCE_MM_TRANSLATED || !views);
    translation = (void *)views;
    BUG_ON(!translation->cpu_count || translation->cpu_count > NCE_TRANSLATION_MAX_CPUS);
    BUG_ON(!base || offset_in_page(base) || base > ~0UL - NCE_USER_SIZE);
    BUG_ON(translation->guest_end <= base || translation->guest_end - base > NCE_USER_SIZE ||
           offset_in_page(translation->guest_end));
    BUG_ON(translation->physical_base < translation->guest_end || offset_in_page(translation->physical_base) ||
           translation->physical_base >= NCE_HOST_ADDRESS_LIMIT ||
           NCE_RAM_SIZE + NCE_VMALLOC_SIZE > NCE_HOST_ADDRESS_LIMIT - translation->physical_base);
    nce_task_size = translation->guest_end;
    nce_memory_start = translation->physical_base;
    installed = desired = NULL;''')
    edit('    size_t bytes = NCE_USER_SIZE / PAGE_SIZE / 8;', '''    if (mapping_mode == NCE_MM_TRANSLATED) {
        INIT_LIST_HEAD(&mm->context.translation_node);
        mm->context.accessed = NULL;
        mm->context.flat = NULL;
        mm->context.read_view = NULL;
        if (translation->flat_enabled) {
            mm->context.flat = lkl_ops->page_alloc(DIRECT_FLAT_BYTES);
            if (!mm->context.flat)
                return -ENOMEM;
            translation->flat_live_bytes += DIRECT_FLAT_BYTES;
            translation->flat_peak_bytes = max(translation->flat_peak_bytes, translation->flat_live_bytes);
        }
        if (translation->read_view_create) {
            mm->context.read_view = translation->read_view_create(NCE_USER_SIZE);
            if (!mm->context.read_view) {
                lkl_ops->page_free(mm->context.flat, DIRECT_FLAT_BYTES);
                translation->flat_live_bytes -= DIRECT_FLAT_BYTES;
                mm->context.flat = NULL;
                return -ENOMEM;
            }
        }
        list_add_tail(&mm->context.translation_node, &translation_mms);
        return 0;
    }
    size_t bytes = NCE_USER_SIZE / PAGE_SIZE / 8;''')
    edit('    lkl_ops->mem_free(mm->context.accessed);', '''    if (mapping_mode == NCE_MM_TRANSLATED) {
        list_del_init(&mm->context.translation_node);
        for (unsigned int i = 0; i < translation->cpu_count; ++i) {
            struct nce_translation_cpu *cpu = &translation->cpus[i];
            if (cpu->pgd == (unsigned long)mm->pgd) {
                cpu->pgd = cpu->flat = 0;
                cpu->read_view = NULL;
            }
        }
    }
    if (mapping_mode == NCE_MM_TRANSLATED && mm->context.read_view) {
        translation->read_view_destroy(mm->context.read_view);
        mm->context.read_view = NULL;
    }
    if (mapping_mode == NCE_MM_TRANSLATED && mm->context.flat) {
        lkl_ops->page_free(mm->context.flat, DIRECT_FLAT_BYTES);
        translation->flat_live_bytes -= DIRECT_FLAT_BYTES;
        mm->context.flat = NULL;
    }
    lkl_ops->mem_free(mm->context.accessed);''')
    edit('/* One Linux CPU owns the host window.', '''static void changed_mapping(struct mm_struct *mm, unsigned long start, unsigned long end)
{
    start = max(start & PAGE_MASK, USER_BASE);
    end = min(PAGE_ALIGN(end), TASK_SIZE);
    if (end <= start)
        return;
    for (unsigned int i = 0; i < translation->cpu_count; ++i) {
        struct nce_translation_cpu *cpu = &translation->cpus[i];
        ++cpu->mappings;
        if (cpu->pgd != (unsigned long)mm->pgd) {
            ++cpu->inactive_mapping_changes;
            continue;
        }
        if (!cpu->tlb_end) {
            cpu->tlb_start = start;
            cpu->tlb_end = end;
        } else {
            cpu->tlb_start = min(cpu->tlb_start, start);
            cpu->tlb_end = max(cpu->tlb_end, end);
        }
    }
}

static void flat_update(struct mm_struct *mm, unsigned long address, pte_t pte)
{
    struct nce_direct_flat_page *table = mm->context.flat;
    if (!table || address < USER_BASE || address >= TASK_SIZE)
        return;
    unsigned long read = 0, write = 0;
    unsigned long offset = (pte_val(pte) & PAGE_MASK) - memory_start;
    if (pte_present(pte) && pte_read(pte) && pte_young(pte) && offset < NCE_RAM_SIZE) {
        read = translation->ram + offset;
        if (pte_write(pte) && pte_dirty(pte))
            write = read;
    }
    struct nce_direct_flat_page *entry = &table[(address - USER_BASE) >> PAGE_SHIFT];
    /* Keep untouched anonymous table pages uncommitted. */
    if (entry->read != read || entry->write != write) {
        if (entry->read != read) {
            ++translation->flat_read_changes;
            if (mm->context.read_view)
                translation->read_view_update(mm->context.read_view, address - USER_BASE, read);
        }
        if (entry->write != write)
            ++translation->flat_write_changes;
        entry->read = read;
        entry->write = write;
        ++translation->flat_updates;
    }
}

static void flat_refresh(struct mm_struct *mm, unsigned long start, unsigned long end)
{
    if (!mm->context.flat)
        return;
    start = max(start & PAGE_MASK, USER_BASE);
    end = min(PAGE_ALIGN(end), TASK_SIZE);
    for (unsigned long address = start; address < end; address += PAGE_SIZE) {
        flat_update(mm, address, lookup(mm, address));
        ++translation->flat_refresh_pages;
    }
}

static void changed_code(pte_t pte, unsigned long pages)
{
    unsigned long index = ((pte_val(pte) & PAGE_MASK) - memory_start) >> PAGE_SHIFT;
    BUILD_BUG_ON(NCE_TRANSLATION_CODE_WORDS * 64 != NCE_RAM_SIZE / PAGE_SIZE);
    BUG_ON(index >= NCE_RAM_SIZE / PAGE_SIZE || pages > NCE_RAM_SIZE / PAGE_SIZE - index);
    if (translation->code_guard) {
        for (unsigned long i = index; i < index + pages; ++i)
            translation->code_guard[i] = 1;
    }
    for (unsigned int i = 0; i < translation->cpu_count; ++i) {
        bitmap_set(translation->cpus[i].code_pages, index, pages);
        ++translation->cpus[i].code;
    }
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
        for (unsigned int i = 0; mm->context.flat && i < nr; ++i)
            flat_update(mm, va + i * PAGE_SIZE, __pte(pte_val(pte) + i * PAGE_SIZE));
        changed_mapping(mm, va, va + nr * PAGE_SIZE);
        if (pte_exec(pte))
            changed_code(pte, nr);
        return;
    }
    if (mm != &init_mm && mm != active_mm)''')
    edit('''    if (mm != &init_mm && addr >= USER_BASE && addr < TASK_SIZE) {''', '''    if (mapping_mode == NCE_MM_TRANSLATED && mm != &init_mm) {
        flat_update(mm, addr, __pte(0));
        changed_mapping(mm, addr, addr + PAGE_SIZE);
        if (pte_exec(*xp))
            changed_code(*xp, 1);
        return;
    }
    if (mm != &init_mm && addr >= USER_BASE && addr < TASK_SIZE) {''')
    edit('''    struct vm_area_struct *vma;
    unsigned long addr;
    VMA_ITERATOR(iter, mm, start);''', '''    if (mapping_mode == NCE_MM_TRANSLATED && mm != &init_mm) {
        flat_refresh(mm, start, end);
        changed_mapping(mm, start, end);
        return;
    }
    struct vm_area_struct *vma;
    unsigned long addr;
    VMA_ITERATOR(iter, mm, start);''')
    edit('''void nce_prepare_user(void)
{
    struct mm_struct *mm = current->mm;''', '''void nce_prepare_user(void)
{
    struct mm_struct *mm = current->mm;
    BUILD_BUG_ON(PAGE_SHIFT != DIRECT_PTE_PAGE_SHIFT || PMD_SHIFT != DIRECT_PTE_PMD_SHIFT || PGDIR_SHIFT != DIRECT_PTE_PGD_SHIFT);
    BUILD_BUG_ON(PTRS_PER_PGD != (1 << DIRECT_PTE_INDEX_BITS) || PTRS_PER_PMD != PTRS_PER_PGD || PTRS_PER_PTE != PTRS_PER_PGD);
    BUILD_BUG_ON(_PAGE_PRESENT != DIRECT_PTE_PRESENT || _PAGE_PROTNONE != DIRECT_PTE_PROTNONE || _PAGE_RW != DIRECT_PTE_WRITE);
    BUILD_BUG_ON(_PAGE_USER != DIRECT_PTE_USER || _PAGE_ACCESSED != DIRECT_PTE_YOUNG || _PAGE_DIRTY != DIRECT_PTE_DIRTY);
    BUILD_BUG_ON(sizeof(pgd_t) != 8 || sizeof(pmd_t) != 8 || sizeof(pte_t) != 8);''')
    edit('''    if (!mm || mm == active_mm)
        return;
    mmap_read_lock(mm);
    prepare_mm_locked(mm);
    mmap_read_unlock(mm);
}''', '''    if (mapping_mode == NCE_MM_TRANSLATED) {
        struct nce_translation_cpu *cpu = cpu_translation();
        unsigned long pgd = mm ? (unsigned long)mm->pgd : 0;
        if (cpu->pgd != pgd) {
            ++cpu->mm_switches;
            ++cpu->mappings;
            cpu->tlb_start = USER_BASE;
            cpu->tlb_end = TASK_SIZE;
        }
        cpu->pgd = pgd;
        cpu->flat = mm ? (unsigned long)mm->context.flat : 0;
        cpu->read_view = mm ? mm->context.read_view : NULL;
        return;
    }
    if (!mm || mm == active_mm)
        return;
    mmap_read_lock(mm);
    prepare_mm_locked(mm);
    mmap_read_unlock(mm);
}''')
    edit('''    if (next == active_mm)
        return;''', '''    if (mapping_mode == NCE_MM_TRANSLATED || next == active_mm)
        return;''')
    edit('''    if (access & NCE_FAULT_TRY_PRESENT)
        return restore_present_data(address, access & 1);''', '''    if (access & NCE_FAULT_TRY_PRESENT)
        return mapping_mode == NCE_MM_TRANSLATED ? 0 : restore_present_data(address, access & 1);''')
    edit('execute && mapping_mode != NCE_MM_FULL ? installed[index] : 0',
         'execute && mapping_mode != NCE_MM_FULL && mapping_mode != NCE_MM_TRANSLATED ? installed[index] : 0')
    edit('if (execute && prior && (mapping_mode == NCE_MM_FULL || trapped_view == prior))',
         'if (mapping_mode != NCE_MM_TRANSLATED && execute && prior && (mapping_mode == NCE_MM_FULL || trapped_view == prior))')
    edit('if (!(result & VM_FAULT_ERROR) && mapping_mode >= NCE_MM_LAZY)',
         'if (!(result & VM_FAULT_ERROR) && mapping_mode >= NCE_MM_LAZY && mapping_mode != NCE_MM_TRANSLATED)')
    text += '''

void nce_translation_cleanup(void)
{
    /* Called after all Linux task threads have joined, before freeing RAM. */
    while (!list_empty(&translation_mms)) {
        struct mm_struct *mm = list_first_entry(&translation_mms, struct mm_struct, context.translation_node);
        nce_destroy_mm(mm);
    }
}
'''
    # Reapply from the tracked MMU source so previous overlay output is never input.
    if not path.exists() or path.read_text() != text:
        path.write_text(text)
    threads = source / 'arch/lkl/kernel/threads.c'
    if threads.exists():
        content = threads.read_text()
        marker = '\tlkl_ops->sem_free(init_thread_union.thread_info.sched_sem);'
        legacy = '''\t/* All task threads have stopped; halt bypasses the surviving mm teardown. */
\tfor_each_process_thread(p, t) {
\t\tif (t->mm && t->mm->context.flat)
\t\t\tnce_destroy_mm(t->mm);
\t}

'''
        content = content.replace(legacy, '')
        replacement = '\textern void nce_translation_cleanup(void);\n\tnce_translation_cleanup();\n\n' + marker
        if replacement not in content:
            if content.count(marker) != 1:
                raise RuntimeError('Translation thread cleanup mismatch')
            content = content.replace('#include <linux/module.h>', '#include <linux/module.h>\n#include <linux/mm.h>\n#include <asm/mmu_context.h>')
            threads.write_text(content.replace(marker, replacement))
    context = source / 'arch/lkl/include/asm/mmu.h'
    if context.exists():
        content = context.read_text()
        marker = '    unsigned long *accessed;'
        addition = '\n    struct nce_direct_flat_page *flat;'
        if addition not in content:
            if content.count(marker) != 1:
                raise RuntimeError('Translation mm context mismatch')
            content = content.replace(marker, marker + addition)
        if '\n    void *read_view;' not in content:
            content = content.replace(addition, addition + '\n    void *read_view;')
        if 'struct list_head translation_node;' not in content:
            content = content.replace(marker, marker + '\n    struct list_head translation_node;')
        if context.read_text() != content:
            context.write_text(content)
    header = source / 'arch/lkl/include/asm/nce-translation.h'
    content = (experiment / 'translation/kernel-api.h').read_bytes()
    if not header.exists() or header.read_bytes() != content:
        header.write_bytes(content)
    header = source / 'arch/lkl/include/asm/nce-direct-page-table.h'
    content = (experiment / 'direct/page-table-layout.h').read_bytes()
    if not header.exists() or header.read_bytes() != content:
        header.write_bytes(content)

    # Linux owns executable mappings in the translated address space. Native
    # ELF registration and private code syscalls remain available to ART/linker.
    elf = source / 'arch/lkl/kernel/nce_elf.c'
    if elf.exists():
        content = (experiment / 'lkl-nce-elf.c').read_text()
        start = content.index('static bool overlaps_dynamic(')
        end = content.index('\n}', start) + 2
        content = content[:start] + content[end:]
        marker = '/* Keep ordinary VM operations from replacing prepared host entries or'
        if content.count(marker) != 1:
            raise RuntimeError('Translation memory syscall source mismatch')
        content = content[:content.index(marker)] + '''/* The host can translate only this guest range. Linux retains ownership of
 * VMA permissions, file checks, security policy and mapping placement. */
static long translated_range(unsigned long start, unsigned long length)
{
    if (!length)
        return 0;
    if (length > ULONG_MAX - (PAGE_SIZE - 1))
        return -EINVAL;
    length = PAGE_ALIGN(length);
    if (start < nce_user_base || start >= nce_task_size || length > nce_task_size - start ||
        lkl_ops->user_reserved(start, length))
        return -EACCES;
    return 0;
}

long nce_check_memory_syscall(long number, long *params)
{
    unsigned long length = params[1];
    if (number == 216 && !length)
        length = PAGE_SIZE;
    long result = translated_range(params[0], length);
    if (!result && number == 216 && (params[3] & MREMAP_FIXED))
        result = translated_range(params[4], params[2]);
    return result;
}

SYSCALL_DEFINE6(nce_mmap, unsigned long, addr, unsigned long, len,
                unsigned long, prot, unsigned long, flags, unsigned long, fd,
                unsigned long, offset)
{
    /* AArch64 mmap takes a byte offset; the generic helper takes pages. */
    if (offset & ~PAGE_MASK)
        return -EINVAL;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        long result = translated_range(addr, len);
        if (result)
            return result;
    } else if (addr < nce_user_base) {
        addr = 0;
    }
    return ksys_mmap_pgoff(addr, len, prot, flags, fd, offset >> PAGE_SHIFT);
}
'''
        elf.write_text(content)
    pgtable = source / 'arch/lkl/include/asm/pgtable-mmu-3level.h'
    if pgtable.exists():
        content = pgtable.read_text()
        marker = '#define FIRST_USER_ADDRESS\t0UL'
        replacement = 'extern unsigned long nce_user_base;\n#define FIRST_USER_ADDRESS\t(nce_user_base)'
        if replacement not in content:
            if content.count(marker) != 1:
                raise RuntimeError('Translation first user address mismatch')
            pgtable.write_text(content.replace(marker, replacement))

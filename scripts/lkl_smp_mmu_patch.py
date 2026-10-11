#!/usr/bin/env python3
"""为独立 SMP/MMU 构建加入页表回收与 RAM 别名访问保护。"""
from pathlib import Path


def apply(source: Path, experiment: Path, *, tests=True):
    def edit(name, before, after):
        path = source / name
        text = path.read_text()
        if after in text:
            return
        if text.count(before) != 1:
            raise RuntimeError(f'SMP MMU source mismatch: {name}: {before[:80]}')
        path.write_text(text.replace(before, after))

    arch = 'arch/lkl/'
    edit(arch + 'include/uapi/asm/host_ops.h',
         '\tvoid (*user_run)(void *state);',
         '\tvoid (*user_run)(void *state);\n\tvoid (*user_interrupt)(unsigned int mask);')
    edit(arch + 'include/uapi/asm/host_ops.h', '\tvoid (*user_interrupt)(unsigned int mask);',
         '\tvoid (*user_interrupt)(unsigned int mask);\n'
         '\t/* Nonblocking query over the immutable prepared-code layout. */\n'
         '\tint (*user_data_range)(unsigned long address, unsigned long length);')
    edit(arch + 'Kconfig', '\tdepends on !MMU\n\tselect ARCH_USE_QUEUED_RWLOCKS',
         '\tselect MMU_GATHER_RCU_TABLE_FREE if MMU\n\tselect ARCH_HAS_PTE_SPECIAL if MMU\n'
         '\tselect ARCH_USE_QUEUED_RWLOCKS')
    files = [('uaccess.c', 'mm/nce_smp_uaccess.c'),
                         ('uaccess.h', 'include/asm/nce-smp-uaccess.h'),
                         ('window.c', 'mm/nce_window.c'),
                         ('window.h', 'include/asm/nce-window.h'),
                         ('present-fault.h', 'mm/nce-present-fault.h')]
    if tests:
        files += [('window-test.c', 'kernel/smp-window-test.c'),
                  ('mmu-test.c', 'kernel/smp-mmu-test.c')]
    for name, target in files:
        path = source / arch / target
        content = (experiment / 'smp' / name).read_bytes()
        if not path.exists() or path.read_bytes() != content:
            path.write_bytes(content)
    if 'nce_smp_uaccess.o' not in (source / arch / 'mm/Makefile').read_text():
        edit(arch + 'mm/Makefile', 'obj-$(CONFIG_MMU) += mmu_mem.o',
             'obj-$(CONFIG_MMU) += nce_smp_uaccess.o\nobj-$(CONFIG_MMU) += mmu_mem.o')
    edit(arch + 'mm/Makefile', 'obj-$(CONFIG_MMU) += nce_smp_uaccess.o',
         'obj-$(CONFIG_MMU) += nce_smp_uaccess.o nce_window.o')
    edit(arch + 'kernel/irq.c', 'void init_IRQ(void)\n',
         '/* A guest signal handler retains CPU ownership and only restores\n'
         ' * the flag here. Pending IRQs run at a normal Linux transition. */\n'
         'void lkl_restore_guest_irq_flags(unsigned long flags)\n'
         '{\n\tirqs_enabled = flags;\n}\n\nvoid init_IRQ(void)\n')
    if tests:
        if 'smp-mmu-test.o' not in (source / arch / 'kernel/Makefile').read_text():
            edit(arch + 'kernel/Makefile', 'obj-$(CONFIG_SMP) += smp.o smp-test.o',
                 'obj-$(CONFIG_SMP) += smp.o smp-test.o\nobj-$(CONFIG_MMU) += smp-mmu-test.o')
        edit(arch + 'kernel/Makefile', 'obj-$(CONFIG_MMU) += smp-mmu-test.o',
             'obj-$(CONFIG_MMU) += smp-mmu-test.o smp-window-test.o')
        edit(arch + 'kernel/syscalls.c', 'extern long nce_launch(long *params);',
             'extern long nce_launch(long *params);\nextern long lkl_smp_mmu_test(void);')
        edit(arch + 'kernel/syscalls.c', '\tif (no == 243)',
             '\tif (no == 242) return lkl_smp_mmu_test();\n\tif (no == 243)')
    else:
        edit(arch + 'kernel/nce_process.c', '    current->thread.nce_state = state;',
             '    if (set_cpus_allowed_ptr(current, cpu_online_mask))\n        return -EINVAL;\n'
             '    current->thread.nce_state = state;')
    # The host registers execution through nce_begin_user after all signal
    # delivery work. Preparing here can make the two CPUs repeatedly replace
    # each other's window before either one reaches that final entry.
    signal = source / arch / 'kernel/nce_signal.c'
    text = signal.read_text()
    calls = text.count('nce_prepare_user();')
    if calls not in (0, 2):
        raise RuntimeError('SMP signal return preparation source mismatch')
    if calls:
        signal.write_text(text.replace('            nce_prepare_user();\n', '')
                         .replace('    nce_prepare_user();\n', ''))
    edit(arch + 'mm/nce_mmu.c', '#include <asm/nce-mmu-profile.h>',
         '#include <asm/nce-mmu-profile.h>\n#include <asm/nce-smp-uaccess.h>')
    mmu = source / arch / 'mm/nce_mmu.c'
    text = mmu.read_text()
    if 'return nce_copy_user_smp(kernel, user, size, operation);' not in text:
        start = text.index('    while (remaining) {\n        pte_t pte = lookup(current->mm, user);')
        end = text.index('    return remaining;\n}', start) + len('    return remaining;')
        text = text[:start] + '    return nce_copy_user_smp(kernel, user, size, operation);' + text[end:]
        text = text.replace('    unsigned long remaining = size;\n', '')
        mmu.write_text(text)
    # Keep page-table pages alive for IRQ-disabled software walks. The gather
    # batches ptdesc pointers; destructors run before enqueueing them.
    pgalloc = arch + 'include/asm/pgalloc.h'
    edit(pgalloc, 'tlb_remove_page_ptdesc((tlb), page_ptdesc(pte));',
         'tlb_remove_ptdesc((tlb), page_ptdesc(pte));')
    edit(pgalloc, 'tlb_remove_page_ptdesc((tlb), virt_to_ptdesc(pmd));',
         'tlb_remove_ptdesc((tlb), virt_to_ptdesc(pmd));')
    edit(arch + 'include/asm/tlb.h', '#include <asm-generic/cacheflush.h>',
         '#include <asm-generic/cacheflush.h>\n'
         'static inline void __tlb_remove_table(void *table)\n'
         '{\n\tfree_page((unsigned long)ptdesc_address((struct ptdesc *)table));\n}\n')
    pgtable = arch + 'include/asm/pgtable-mmu-3level.h'
    edit(pgtable, '#define _PAGE_EXEC\t0x200', '#define _PAGE_EXEC\t0x200\n#define _PAGE_SPECIAL\t0x800')
    edit(pgtable, '#define _PAGE_CHG_MASK\t(PAGE_MASK | _PAGE_ACCESSED | _PAGE_DIRTY)',
         '#define _PAGE_CHG_MASK\t(PAGE_MASK | _PAGE_ACCESSED | _PAGE_DIRTY | _PAGE_SPECIAL)')
    edit(pgtable, 'static inline int pte_young(pte_t pte)',
         'static inline int pte_special(pte_t pte)\n{\n\treturn pte_val(pte) & _PAGE_SPECIAL;\n}\n\n'
         'static inline pte_t pte_mkspecial(pte_t pte)\n{\n\treturn __pte(pte_val(pte) | _PAGE_SPECIAL);\n}\n\n'
         'static inline int pte_young(pte_t pte)')
    apply_window(source)


def apply_window(source: Path):
    """将 PTE 更新和宿主窗口改动放进同一停止区间。"""
    mmu = source / 'arch/lkl/mm/nce_mmu.c'
    text = mmu.read_text()
    include = '#include <asm/nce-window.h>\n'
    if include not in text:
        text = include + text

    def wrap(signature, name, arguments, returned=False):
        nonlocal text
        internal = name + '_window_locked'
        if internal in text:
            return
        start = text.index(signature + '\n{')
        end = text.index('\n}', start) + 2
        body = text[start:end].replace(name + '(', internal + '(', 1)
        declaration = body if signature.startswith('static ') else 'static ' + body
        result = ('    pte_t result = ' if returned else '    ') + internal + '(' + arguments + ');\n'
        wrapper = signature + '\n{\n    nce_window_lock();\n' + result
        wrapper += '    nce_window_unlock();\n' + ('    return result;\n' if returned else '') + '}'
        text = text[:start] + declaration + '\n\n' + wrapper + text[end:]

    wrap('void nce_destroy_mm(struct mm_struct *mm)', 'nce_destroy_mm', 'mm')
    wrap('static pte_t lookup(struct mm_struct *mm, unsigned long addr)', 'lookup', 'mm, addr', True)
    # Parent entries can be cleared without the window lock. IRQ exclusion
    # retains their table pages; snapshots prevent a second parent read from
    # following a newly cleared entry during the same walk.
    start = text.index('static pte_t lookup_window_locked(')
    end = text.index('\n}', start) + 2
    text = text[:start] + '''static pte_t lookup_window_locked(struct mm_struct *mm, unsigned long addr)
{
    pgd_t *pgd = pgd_offset(mm, addr);
    p4d_t *p4d = p4d_offset(pgd, addr);
    pud_t pud = READ_ONCE(*pud_offset(p4d, addr));
    if (!pud_present(pud))
        return __pte(0);
    pmd_t pmd = READ_ONCE(*pmd_offset(&pud, addr));
    if (!pmd_present(pmd))
        return __pte(0);
    return ptep_get(pte_offset_kernel(&pmd, addr));
}''' + text[end:]
    wrap('void mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va, unsigned int nr, pte_t pte)',
         'mmap_pages_for_ptes', 'mm, va, nr, pte')
    wrap('void munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp)',
         'munmap_page_for_pte', 'mm, addr, xp')
    wrap('void nce_flush_range(struct mm_struct *mm, unsigned long start, unsigned long end)',
         'nce_flush_range', 'mm, start, end')
    wrap('static void prepare_mm_locked(struct mm_struct *next)', 'prepare_mm_locked', 'next')
    begin = text.index('static void map_view(')
    end = text.index('\n}', begin) + 2
    body = text[begin:end]
    if 'nce_window_stop();' not in body:
        body = body.replace('    enum lkl_prot prot',
                            '    if (addr >= USER_BASE && addr < TASK_SIZE)\n        nce_window_stop();\n'
                            '    enum lkl_prot prot')
    body = body.replace('    if (addr >= USER_BASE && addr < TASK_SIZE)\n',
                        '    if (addr >= USER_BASE && addr < TASK_SIZE && !new_data_view(addr, size, value))\n')
    text = text[:begin] + body + text[end:]
    helper = '''static bool new_data_view(unsigned long addr, unsigned long size, unsigned long value)
{
    if (mapping_mode < NCE_MM_LAZY || !value || (value & _PAGE_EXEC) ||
        addr < USER_BASE || addr >= TASK_SIZE || size > TASK_SIZE - addr)
        return false;
    unsigned long first = (addr - USER_BASE) / PAGE_SIZE;
    for (unsigned long i = 0; i < size / PAGE_SIZE; ++i)
        if (installed[first + i])
            return false;
    return addr >= USER_BASE + NCE_BRK_OFFSET ||
           (lkl_ops->user_data_range && lkl_ops->user_data_range(addr, size));
}
'''
    if 'static bool new_data_view(' in text:
        begin = text.index('static bool new_data_view(')
        end = text.index('\n}', begin) + 3
        text = text[:begin] + helper + text[end:]
    else:
        comment = ('/* The writer lock excludes PTE updates and mm switches. Installing an\n'
                   ' * absent data view leaves other threads\' existing mappings and code intact. */\n')
        text = text.replace('static void map_view(', comment + helper + '\nstatic void map_view(', 1)
    text = text.replace('    PROFILE_ADD(switches, 1);\n    active_mm = next;',
                        '    PROFILE_ADD(switches, 1);\n    nce_window_stop();\n    active_mm = next;')
    text = text.replace('if (!mm || mm == active_mm)', 'if (!mm || mm == READ_ONCE(active_mm))')
    text = text.replace('/* One Linux CPU owns the host window. Entries describe installed host views,',
                        '/* The window writer lock protects installed host views,')
    # Replace the UP helper with a nonblocking implementation that preserves
    # the guest slot and leaves pending IRQ delivery to normal Linux entry.
    begin = text.find('static int restore_present_data(')
    if begin >= 0:
        end = text.index('\n}', begin) + 2
        comment = text.rfind('/* The host calls this only', 0, begin)
        text = text[:comment if comment >= 0 else begin] + text[end:]
    include_fault = '#include "nce-present-fault.h"\n\n'
    if include_fault not in text:
        before = '/* Called only for a fault in the controlled guest window'
        if text.count(before) != 1:
            raise RuntimeError('SMP present fault include source mismatch')
        text = text.replace(before, include_fault + before)
    text = text.replace('    if (access & NCE_FAULT_TRY_PRESENT)\n        return 0;',
                        '    if (access & NCE_FAULT_TRY_PRESENT)\n        return restore_present_data(address, access & 1);')
    begin = text.index('long nce_publish_code(')
    end = text.index('\n}', begin) + 2
    body = text[begin:end]
    if 'nce_window_lock();' not in body:
        body = body.replace('    mmap_read_lock(mm);', '    mmap_read_lock(mm);\n    nce_window_lock();')
        body = body.replace('mmap_read_unlock(mm);', 'nce_window_unlock();\n        mmap_read_unlock(mm);')
        text = text[:begin] + body + text[end:]
    marker = '    if (!(result & VM_FAULT_ERROR) && direct)\n'
    if '\n    nce_window_lock();\n' + marker not in text:
        text = text.replace(marker, '    nce_window_lock();\n' + marker)
        text = text.replace('    if ((result & VM_FAULT_ERROR) && profile)',
                            '    nce_window_unlock();\n    if ((result & VM_FAULT_ERROR) && profile)')
    if 'void nce_begin_user(void)' not in text:
        text += '''

/* Registration follows all Linux operations, including IRQ restoration that
 * can schedule or migrate the task. A writer changes the token before entry. */
void nce_begin_user(void)
{
    for (;;) {
        nce_prepare_user();
        u64 token = nce_window_token();
        if (READ_ONCE(active_mm) == current->mm && nce_window_try_enter(token))
            return;
        cond_resched();
    }
}
'''
    if 'int nce_guest_fault(' not in text:
        text = text.replace('int nce_page_fault(unsigned long address, int access)\n',
                            'static int nce_page_fault_view(unsigned long address, int access, unsigned long trapped_view)\n')
        before = '''    /* Taking mmap_lock may schedule another thread that restores this view.
     * Classify the original trap using the view present before that wait. */
    unsigned long index = (address - USER_BASE) / PAGE_SIZE;
    unsigned long trapped_view = execute && mapping_mode != NCE_MM_FULL ? installed[index] : 0;
'''
        if 'NCE_MM_TRANSLATED' in text:
            before = before.replace('mapping_mode != NCE_MM_FULL ?',
                                    'mapping_mode != NCE_MM_FULL && mapping_mode != NCE_MM_TRANSLATED ?')
        if before not in text:
            raise RuntimeError('SMP fault view capture source mismatch')
        text = text.replace(before, '')
        text += '''

int nce_page_fault(unsigned long address, int access)
{
    return nce_page_fault_view(address, access, 0);
}

/* The caller still owns a guest slot. Capture its view before a writer can
 * switch address spaces, then leave before taking any Linux locks. */
int nce_guest_fault(unsigned long address, int access)
{
    BUG_ON(!(nce_window_token() & (1ULL << raw_smp_processor_id())));
    unsigned long trapped_view = 0;
    if (installed && access == NCE_FAULT_EXEC && address >= USER_BASE && address < TASK_SIZE)
        trapped_view = installed[(address - USER_BASE) / PAGE_SIZE];
    nce_window_leave();
    return nce_page_fault_view(address, access, trapped_view);
}
'''
    mmu.write_text(text)

    table = source / 'arch/lkl/include/asm/pgtable-mmu-3level.h'
    text = table.read_text()
    if include not in text:
        text = text.replace('#include <linux/mm_types.h>', '#include <linux/mm_types.h>\n' + include)
    for signature in ['static inline void pte_clear(struct mm_struct *mm, unsigned long addr, pte_t *xp)',
                      'static inline void __set_ptes(struct mm_struct *mm, unsigned long addr,\n'
                      '\t\tpte_t *ptep, pte_t pte, unsigned int nr)']:
        start = text.index(signature + '\n{')
        end = text.index('\n}', start) + 2
        body = text[start:end]
        if 'nce_window_lock();' not in body:
            body = body.replace('\n{', '\n{\n\tnce_window_lock();', 1)
            body = body[:-2] + '\n\tnce_window_unlock();\n}'
            text = text[:start] + body + text[end:]
    table.write_text(text)

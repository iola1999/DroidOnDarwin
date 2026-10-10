#!/usr/bin/env python3
"""为独立 SMP/MMU 构建加入页表回收与 RAM 别名访问保护。"""
from pathlib import Path


def apply(source: Path, experiment: Path):
    def edit(name, before, after):
        path = source / name
        text = path.read_text()
        if after in text:
            return
        if text.count(before) != 1:
            raise RuntimeError(f'SMP MMU source mismatch: {name}: {before[:80]}')
        path.write_text(text.replace(before, after))

    arch = 'arch/lkl/'
    edit(arch + 'Kconfig', '\tdepends on !MMU\n\tselect ARCH_USE_QUEUED_RWLOCKS',
         '\tselect MMU_GATHER_RCU_TABLE_FREE if MMU\n\tselect ARCH_HAS_PTE_SPECIAL if MMU\n'
         '\tselect ARCH_USE_QUEUED_RWLOCKS')
    for name, target in [('uaccess.c', 'mm/nce_smp_uaccess.c'),
                         ('uaccess.h', 'include/asm/nce-smp-uaccess.h'),
                         ('window.c', 'mm/nce_window.c'),
                         ('window.h', 'include/asm/nce-window.h'),
                         ('window-test.c', 'kernel/smp-window-test.c'),
                         ('mmu-test.c', 'kernel/smp-mmu-test.c')]:
        path = source / arch / target
        content = (experiment / 'smp' / name).read_bytes()
        if not path.exists() or path.read_bytes() != content:
            path.write_bytes(content)
    if 'nce_smp_uaccess.o' not in (source / arch / 'mm/Makefile').read_text():
        edit(arch + 'mm/Makefile', 'obj-$(CONFIG_MMU) += mmu_mem.o',
             'obj-$(CONFIG_MMU) += nce_smp_uaccess.o\nobj-$(CONFIG_MMU) += mmu_mem.o')
    edit(arch + 'mm/Makefile', 'obj-$(CONFIG_MMU) += nce_smp_uaccess.o',
         'obj-$(CONFIG_MMU) += nce_smp_uaccess.o nce_window.o')
    if 'smp-mmu-test.o' not in (source / arch / 'kernel/Makefile').read_text():
        edit(arch + 'kernel/Makefile', 'obj-$(CONFIG_SMP) += smp.o smp-test.o',
             'obj-$(CONFIG_SMP) += smp.o smp-test.o\nobj-$(CONFIG_MMU) += smp-mmu-test.o')
    edit(arch + 'kernel/Makefile', 'obj-$(CONFIG_MMU) += smp-mmu-test.o',
         'obj-$(CONFIG_MMU) += smp-mmu-test.o smp-window-test.o')
    edit(arch + 'kernel/syscalls.c', 'extern long nce_launch(long *params);',
         'extern long nce_launch(long *params);\nextern long lkl_smp_mmu_test(void);')
    edit(arch + 'kernel/syscalls.c', '\tif (no == 243)',
         '\tif (no == 242) return lkl_smp_mmu_test();\n\tif (no == 243)')
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
    wrap('void mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va, unsigned int nr, pte_t pte)',
         'mmap_pages_for_ptes', 'mm, va, nr, pte')
    wrap('void munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp)',
         'munmap_page_for_pte', 'mm, addr, xp')
    wrap('void nce_flush_range(struct mm_struct *mm, unsigned long start, unsigned long end)',
         'nce_flush_range', 'mm, start, end')
    wrap('static void prepare_mm_locked(struct mm_struct *next)', 'prepare_mm_locked', 'next')
    text = text.replace('if (!mm || mm == active_mm)', 'if (!mm || mm == READ_ONCE(active_mm))')
    text = text.replace('return restore_present_data(address, access & 1);', 'return 0;')
    text = text.replace('/* One Linux CPU owns the host window. Entries describe installed host views,',
                        '/* The window writer lock protects installed host views,')
    # The SMP host handles faults after leaving guest execution. Keep the UP
    # helper out of this independent build so it cannot bypass synchronization.
    begin = text.find('static int restore_present_data(')
    if begin >= 0:
        end = text.index('\n}', begin) + 2
        comment = text.rfind('/* The host calls this only', 0, begin)
        text = text[:comment if comment >= 0 else begin] + text[end:]
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

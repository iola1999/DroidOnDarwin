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
                         ('mmu-test.c', 'kernel/smp-mmu-test.c')]:
        path = source / arch / target
        content = (experiment / 'smp' / name).read_bytes()
        if not path.exists() or path.read_bytes() != content:
            path.write_bytes(content)
    edit(arch + 'mm/Makefile', 'obj-$(CONFIG_MMU) += mmu_mem.o',
         'obj-$(CONFIG_MMU) += nce_smp_uaccess.o\nobj-$(CONFIG_MMU) += mmu_mem.o')
    edit(arch + 'kernel/Makefile', 'obj-$(CONFIG_SMP) += smp.o smp-test.o',
         'obj-$(CONFIG_SMP) += smp.o smp-test.o\nobj-$(CONFIG_MMU) += smp-mmu-test.o')
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

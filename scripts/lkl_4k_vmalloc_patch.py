#!/usr/bin/env python3
"""为单 CPU 4 KiB LKL 保留 16 KiB 宿主共享别名。"""
from pathlib import Path


def apply(source: Path):
    def edit(name, before, after):
        path = source / name
        text = path.read_text()
        if text.count(after) == 1:
            return
        if text.count(before) != 1:
            raise RuntimeError('4 KiB VMALLOC source mismatch: ' + name + ': ' + before[:80])
        path.write_text(text.replace(before, after))

    edit('arch/lkl/include/asm/vmalloc.h', '#define _ASM_LKL_VMALLOC_H',
         '#define _ASM_LKL_VMALLOC_H\n#define ARCH_PAGE_TABLE_SYNC_MASK PGTBL_PTE_MODIFIED')
    edit('arch/lkl/mm/nce_mmu.c', '#include <linux/mm.h>',
         '#include <linux/mm.h>\n#include <linux/vmalloc.h>')
    edit('arch/lkl/mm/nce_mmu.c',
         '    unsigned long value = view(pte);\n    if (mapping_mode == NCE_MM_TRANSLATED',
         '    if (mm == &init_mm)\n        return;\n    unsigned long value = view(pte);\n    if (mapping_mode == NCE_MM_TRANSLATED')
    edit('arch/lkl/mm/nce_mmu.c',
         'void munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp)\n{',
         'void munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp)\n{\n    if (mm == &init_mm)\n        return;')
    path = source / 'arch/lkl/mm/nce_mmu.c'
    mapping_sync = '''
/* A completed init_mm range owns every mapped host group. The guard tail
 * can cover part of an empty group; it never authorizes a wider host revoke. */
void arch_sync_kernel_mappings(unsigned long start, unsigned long end)
{
    const unsigned long group = NCE_HOST_PAGE_SIZE;
    BUG_ON(start >= end || start < VMALLOC_START || end > VMALLOC_END);
    for (unsigned long addr = start & ~(group - 1); addr < end; addr += group) {
        unsigned long first = view(lookup(&init_mm, addr));
        bool empty = first == 0;
        for (unsigned long offset = PAGE_SIZE; offset < group; offset += PAGE_SIZE) {
            unsigned long value = view(lookup(&init_mm, addr + offset));
            BUG_ON(empty ? value != 0 : value != first + offset);
        }
        if (addr < start || group > end - addr) {
            BUG_ON(!empty);
            continue;
        }
        if (!empty) {
            unsigned long physical = first & PAGE_MASK;
            BUG_ON(physical & (group - 1));
            BUG_ON(physical < memory_start || physical - memory_start > NCE_RAM_SIZE - group);
        }
        map_view(addr, group, first);
    }
}
'''
    text = path.read_text()
    if mapping_sync not in text:
        path.write_text(text + mapping_sync)
    edit('mm/memory.c',
         '\tif (mask & ARCH_PAGE_TABLE_SYNC_MASK)\n\t\tarch_sync_kernel_mappings(start, start + size);',
         '\tif (mm == &init_mm && !err && (mask & ARCH_PAGE_TABLE_SYNC_MASK))\n\t\tarch_sync_kernel_mappings(start, start + size);')
    edit('mm/vmalloc.c', '#include <linux/vmalloc.h>',
         '#include <linux/vmalloc.h>\n#include <asm/nce-layout.h>\n#include <asm/host_ops.h>\n'
         '#if defined(CONFIG_HAVE_ARCH_HUGE_VMALLOC) || defined(CONFIG_HAVE_ARCH_HUGE_VMAP)\n'
         '#error "4 KiB LKL VMALLOC requires ordinary PTE mappings"\n#endif')
    edit('mm/vmalloc.c',
         '\tmight_sleep();\n\tBUG_ON(addr >= end);\n\n\tstart = addr;',
         '''\tmight_sleep();
\tif (addr >= end || ((addr | end | phys_addr) & (NCE_HOST_PAGE_SIZE - 1)) ||
\t    phys_addr < nce_memory_start || phys_addr - nce_memory_start >= NCE_RAM_SIZE ||
\t    end - addr > NCE_RAM_SIZE - (phys_addr - nce_memory_start))
\t\treturn -EINVAL;

\tstart = addr;''')
    edit('mm/vmalloc.c',
         '\tif (mask & ARCH_PAGE_TABLE_SYNC_MASK)\n\t\tarch_sync_kernel_mappings(start, end);\n\n\treturn err;',
         '\tif (!err && (mask & ARCH_PAGE_TABLE_SYNC_MASK))\n\t\tarch_sync_kernel_mappings(start, end);\n\n\treturn err;')
    edit('mm/vmalloc.c',
         '\tWARN_ON(page_shift < PAGE_SHIFT);\n\n\tif (!IS_ENABLED(CONFIG_HAVE_ARCH_HUGE_VMALLOC)',
         '''\tWARN_ON(page_shift < PAGE_SHIFT);
\tif (addr >= end || ((addr | end) & (NCE_HOST_PAGE_SIZE - 1)))
\t\treturn -EINVAL;
\tfor (i = 0; i < nr; ++i) {
\t\tunsigned long physical;
\t\tif (!pages[i])
\t\t\treturn -EINVAL;
\t\tphysical = page_to_phys(pages[i]);
\t\tif (physical < nce_memory_start || physical - nce_memory_start >= NCE_RAM_SIZE)
\t\t\treturn -EINVAL;
\t\tif (i % (NCE_HOST_PAGE_SIZE / PAGE_SIZE) == 0) {
\t\t\tif (physical & (NCE_HOST_PAGE_SIZE - 1))
\t\t\t\treturn -EINVAL;
\t\t} else if (physical != page_to_phys(pages[i - 1]) + PAGE_SIZE) {
\t\t\treturn -EINVAL;
\t\t}
\t}

\tif (!IS_ENABLED(CONFIG_HAVE_ARCH_HUGE_VMALLOC)''')
    edit('mm/vmalloc.c',
         '\tBUG_ON(in_interrupt());\n\tsize = ALIGN(size, 1ul << shift);',
         '\tBUG_ON(in_interrupt());\n\talign = max(align, (unsigned long)NCE_HOST_PAGE_SIZE);\n\tsize = ALIGN(size, 1ul << shift);')
    # VMALLOC backing is split into ordinary 4 KiB pages. PTE size and
    # vm_struct.page_order retain their existing small-page meaning.
    edit('mm/vmalloc.c',
         '\tpage_order = vm_area_page_order(area);',
         '\tpage_order = max(vm_area_page_order(area), 2U);')
    edit('mm/vmalloc.c',
         '\tunsigned int shift = PAGE_SHIFT;\n\n\tif (WARN_ON_ONCE(!size))',
         '''\tunsigned int shift = PAGE_SHIFT;

\tif (gfp_mask & __GFP_NOFAIL) {
\t\tlkl_printf("4 KiB VMALLOC rejects NOFAIL: bytes=%lu gfp=%x\\n", size, gfp_mask);
\t\tBUG();
\t}
\tif (size > ULONG_MAX - (NCE_HOST_PAGE_SIZE - 1))
\t\treturn NULL;
\tsize = ALIGN(size, NCE_HOST_PAGE_SIZE);
\talign = max(align, (unsigned long)NCE_HOST_PAGE_SIZE);

\tif (WARN_ON_ONCE(!size))''')
    edit('mm/vmalloc.c',
         '\tarea = __get_vm_area_node(real_size, align, shift, VM_ALLOC |',
         '\tarea = __get_vm_area_node(size, align, shift, VM_ALLOC |')

    edit('fs/erofs/zutil.c', '#include "internal.h"',
         '#include "internal.h"\n#include <asm/nce-layout.h>')
    edit('fs/erofs/zutil.c', '\tint last, i, j;',
         '\tconst unsigned int group = NCE_HOST_PAGE_SIZE / PAGE_SIZE;\n\tint i, j;')
    edit('fs/erofs/zutil.c', '\tmutex_lock(&gbuf_resize_mutex);',
         '\t/* LKL aliases complete host pages with contiguous physical backing. */\n'
         '\tnrpages = ALIGN(nrpages, group);\n\tmutex_lock(&gbuf_resize_mutex);')
    edit('fs/erofs/zutil.c',
         '\t\tdo {\n\t\t\tlast = j;\n\t\t\tj = alloc_pages_bulk_array(GFP_KERNEL, nrpages,\n'
         '\t\t\t\t\t\t   tmp_pages);\n\t\t\tif (last == j)\n\t\t\t\tgoto out;\n\t\t} while (j != nrpages);',
         '''\t\tfor (; j < nrpages; j += group) {
\t\t\tstruct page *page = alloc_pages(GFP_KERNEL, get_order(NCE_HOST_PAGE_SIZE));

\t\t\tif (!page)
\t\t\t\tgoto out;
\t\t\tsplit_page(page, get_order(NCE_HOST_PAGE_SIZE));
\t\t\tfor (unsigned int offset = 0; offset < group; ++offset)
\t\t\t\ttmp_pages[j + offset] = page + offset;
\t\t}''')

    edit('kernel/bpf/ringbuf.c', '#include <linux/vmalloc.h>',
         '#include <linux/vmalloc.h>\n#include <asm/nce-layout.h>')
    # The position pages keep their 4 KiB mmap offsets. Padding before them
    # makes the data and repeated data start at full host-page boundaries.
    edit('kernel/bpf/ringbuf.c',
         '\tunsigned long consumer_pos __aligned(PAGE_SIZE);',
         '\tunsigned long consumer_pos __aligned(NCE_HOST_PAGE_SIZE / 2);')
    edit('kernel/bpf/ringbuf.c', '\tint i;\n\n\t/* Each data page',
         '\tint i, j;\n\n\tBUILD_BUG_ON(RINGBUF_NR_META_PAGES * PAGE_SIZE != NCE_HOST_PAGE_SIZE);\n'
         '\tif (data_sz < NCE_HOST_PAGE_SIZE)\n\t\treturn NULL;\n\n\t/* Each data page')
    edit('kernel/bpf/ringbuf.c',
         '\tfor (i = 0; i < nr_pages; i++) {\n\t\tpage = alloc_pages_node(numa_node, flags, 0);',
         '\tfor (i = 0; i < nr_pages; i += NCE_HOST_PAGE_SIZE / PAGE_SIZE) {\n'
         '\t\tpage = alloc_pages_node(numa_node, flags, get_order(NCE_HOST_PAGE_SIZE));')
    edit('kernel/bpf/ringbuf.c',
         '\t\tpages[i] = page;\n\t\tif (i >= nr_meta_pages)\n\t\t\tpages[nr_data_pages + i] = page;',
         '\t\tsplit_page(page, get_order(NCE_HOST_PAGE_SIZE));\n'
         '\t\tfor (j = 0; j < NCE_HOST_PAGE_SIZE / PAGE_SIZE; j++) {\n'
         '\t\t\tpages[i + j] = page + j;\n'
         '\t\t\tif (i >= nr_meta_pages)\n'
         '\t\t\t\tpages[nr_data_pages + i + j] = page + j;\n\t\t}')

    edit('kernel/bpf/ringbuf.c', '\tint i, nr_pages = rb->nr_pages;\n\n\tvunmap(rb);',
         '\tint i, nr_pages = rb->nr_pages;\n\n'
         '\t/* LKL can defer IRQ work until timer delivery. Keep its storage mapped. */\n'
         '\tirq_work_sync(&rb->work);\n\tvunmap(rb);')

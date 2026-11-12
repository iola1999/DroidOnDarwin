#!/usr/bin/env python3
"""在 translation 生成源码中启用 Linux mmap 随机布局。"""
from pathlib import Path


def replace(path: Path, before: str, after: str):
    text = path.read_text()
    if after in text:
        return
    if text.count(before) != 1:
        raise RuntimeError('Translation ASLR source mismatch: ' + str(path) + ': ' + before[:100])
    path.write_text(text.replace(before, after))


def apply(source: Path, guest_page_bytes: int):
    minimum, maximum = {16384: (16, 24), 4096: (18, 26)}[guest_page_bytes]
    replace(source / 'arch/lkl/Kconfig',
            '\tselect CLONE_BACKWARDS if MMU',
            '\tselect CLONE_BACKWARDS if MMU\n'
            '\tselect ARCH_WANT_DEFAULT_TOPDOWN_MMAP_LAYOUT if MMU\n'
            '\tselect HAVE_ARCH_MMAP_RND_BITS if MMU')
    replace(source / 'arch/lkl/Kconfig', 'if MMU\nconfig PGTABLE_LEVELS',
            f'''if MMU
config ARCH_MMAP_RND_BITS_MIN
	default {minimum}

config ARCH_MMAP_RND_BITS_MAX
	default {maximum}

config ARCH_MMAP_RND_BITS_DEFAULT
	default {maximum}

config PGTABLE_LEVELS''')
    mmu = source / 'arch/lkl/mm/nce_mmu.c'
    replace(mmu, '#include <linux/bitops.h>',
            '#include <linux/bitops.h>\n#include <linux/sizes.h>\n#include <asm/processor.h>')
    replace(mmu, 'void nce_set_layout(unsigned long base,', '''/* Layout is published before lkl_init and the first userspace mapping.
 * mmap_rnd_bits_max retains Linux's __ro_after_init lifetime. */
static void nce_mmap_layout_init(void)
{
    BUG_ON(nce_task_size <= 16 * PAGE_SIZE);
    unsigned long stack_top = STACK_TOP;
    unsigned long lower = TASK_UNMAPPED_BASE;
    BUG_ON(stack_top < lower || stack_top - lower < SZ_128M || stack_top / 6 * 5 < SZ_128M);
    unsigned long pages = ((stack_top - lower - SZ_128M) >> PAGE_SHIFT) + 1;
    int available_bits = (int)fls_long(pages) - 1;
    int bits = min(mmap_rnd_bits_max, available_bits);
    BUG_ON(bits < mmap_rnd_bits_min);
    mmap_rnd_bits_max = bits;
    mmap_rnd_bits = clamp(mmap_rnd_bits, mmap_rnd_bits_min, mmap_rnd_bits_max);
}

void nce_set_layout(unsigned long base,''')
    replace(mmu, '    sparse_scan = sparse;', '    BUG_ON(nce_task_size);\n    sparse_scan = sparse;')
    replace(mmu, '    installed = desired = NULL;',
            '    installed = desired = NULL;\n    nce_mmap_layout_init();')
    replace(source / 'mm/util.c',
            '\tunsigned long gap = rlim_stack->rlim_cur;\n\tunsigned long pad = stack_guard_gap;',
            '''	unsigned long gap = rlim_stack->rlim_cur;
	unsigned long pad = stack_guard_gap;
	unsigned long random_max = (current->flags & PF_RANDOMIZE) ?
		((1UL << mmap_rnd_bits_max) - 1) << PAGE_SHIFT : 0;
	/* Every published random sample must fit the guest window, including
	 * nonzero bases and large finite stack limits. */
	unsigned long max_gap = min(MAX_GAP, STACK_TOP - TASK_UNMAPPED_BASE - random_max);''')
    replace(source / 'mm/util.c',
            '''	if (gap < MIN_GAP && MIN_GAP < MAX_GAP)
		gap = MIN_GAP;
	else if (gap > MAX_GAP)
		gap = MAX_GAP;''',
            '\tgap = clamp(gap, MIN_GAP, max_gap);')
    replace(source / 'mm/mmap.c', '\tinfo.low_limit = PAGE_SIZE;',
            '\tinfo.low_limit = max(PAGE_SIZE, nce_user_base);')

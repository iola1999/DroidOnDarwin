#!/usr/bin/env python3
"""在 translation 生成源码中启用 Linux mmap、ELF 和 brk 随机布局。"""
from pathlib import Path


def replace(path: Path, before: str, after: str):
    text = path.read_text()
    if after in text and (before in after or before not in text):
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
    elf = source / 'arch/lkl/include/asm/elf.h'
    content = elf.read_text()
    for line in (
            '#define NCE_ELF_BRK (nce_user_base + NCE_BRK_OFFSET)\n',
            '#define NCE_ELF_LAYOUT(file, header, phdrs) nce_elf_file_layout(file, 0, header, phdrs)\n',
            '#define NCE_ELF_INTERP_LAYOUT(file, header, phdrs) nce_elf_file_layout(file, 0, header, phdrs)\n'):
        if content.count(line) > 1:
            raise RuntimeError('Translation ELF source mismatch: ' + line)
        content = content.replace(line, '')
    if any(line.startswith('#define NCE_ELF_') for line in content.splitlines()):
        raise RuntimeError('Translation ELF placement override remains')
    if elf.read_text() != content:
        elf.write_text(content)
    replace(elf, '#define ELF_ET_DYN_BASE nce_user_base',
            '/* A nonzero host base must retain the same guest-relative ELF region. */\n'
            '#define ELF_ET_DYN_BASE TASK_UNMAPPED_BASE')
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
    replace(source / 'mm/util.c', '\treturn randomize_page(mm->brk, SZ_1G);',
            '''	if (mm->brk >= TASK_SIZE)
		return mm->brk;
	return randomize_page(mm->brk,
			      min_t(unsigned long, SZ_1G, TASK_SIZE - mm->brk));''')
    mmap = source / 'mm/mmap.c'
    replace(mmap, '\tinfo.low_limit = PAGE_SIZE;',
            '\tinfo.low_limit = max(PAGE_SIZE, nce_user_base);')
    replace(mmap, '\torigbrk = mm->brk;', '''	origbrk = mm->brk;
	if (brk < nce_user_base || brk > TASK_SIZE)
		goto out;''')
    replace(mmap, '''	unsigned long error = arch_mmap_check(addr, len, flags);''',
            '''	/* ELF and brk also call this helper without the syscall adapter. */
	if (!(flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && addr < nce_user_base)
		addr = 0;
	unsigned long error = arch_mmap_check(addr, len, flags);''')
    replace(mmap, '''	/* Careful about overflows.. */
	if (len > TASK_SIZE)
		return -ENOMEM;''', '''	/* Bounds use the translated guest capacity, including nonzero bases. */
	if (len > TASK_SIZE - nce_user_base)
		return -ENOMEM;''')
    replace(mmap, '\tif (addr > TASK_SIZE - len)\n\t\treturn -ENOMEM;',
            '\tif (addr < nce_user_base)\n\t\treturn -EACCES;\n'
            '\tif (addr > TASK_SIZE - len)\n\t\treturn -ENOMEM;')
    binfmt = source / 'fs/binfmt_elf.c'
    replace(binfmt, '''#ifdef NCE_ELF_INTERP_LAYOUT
			if (!load_addr_set) {''', '''			if (!load_addr_set && interp_elf_ex->e_type == ET_DYN) {
				unsigned long alignment = maximum_alignment(interp_elf_phdata,
								     interp_elf_ex->e_phnum);
				if (alignment > ELF_MIN_ALIGN) {
					map_addr = elf_load(interpreter, 0, eppnt, elf_prot,
							    elf_type, total_size);
					if (BAD_ADDR(map_addr)) {
						error = map_addr;
						goto out;
					}
					vm_munmap(map_addr, total_size);
					load_addr = (map_addr & ~(alignment - 1)) - ELF_PAGESTART(vaddr);
					elf_type |= MAP_FIXED_NOREPLACE;
				}
			}
#ifdef NCE_ELF_INTERP_LAYOUT
			if (!load_addr_set) {''')
    replace(binfmt, '#ifdef NCE_ELF_BRK',
            '''	if (mm->brk < nce_user_base || mm->brk > TASK_SIZE) {
		retval = -EINVAL;
		goto out;
	}
#ifdef NCE_ELF_BRK''')

    elf = source / 'arch/lkl/kernel/nce_elf.c'
    replace(elf, 'static unsigned long sigreturn_base, sigreturn_entry;',
            'static unsigned long sigreturn_entry_offset;')
    replace(elf, '''    return vma->vm_start == sigreturn_base && vma->vm_end - vma->vm_start == PAGE_SIZE ? 0 : -EINVAL;''',
            '''    return vma->vm_start == vma->vm_mm->context.sigreturn - sigreturn_entry_offset &&
           vma->vm_end - vma->vm_start == PAGE_SIZE ? 0 : -EINVAL;''')
    replace(elf, '''    const struct elf_phdr *load;
    unsigned long base;''', '    const struct elf_phdr *load;')
    replace(elf, '''    base = nce_elf_file_layout(file, 0, header, load);
    if (!base || offset_in_page(base) || base < nce_user_base || base > TASK_SIZE - PAGE_SIZE)
        goto close;
    error = 0;''', '    error = 0;')
    replace(elf, '''        sigreturn_base = base;
        sigreturn_entry = base + header->e_entry;
        /* Publish after reading the complete file and matching its registered
         * layout. Later execs use the kernel page under their new SELinux domain. */''',
            '''        sigreturn_entry_offset = header->e_entry;
        /* Publish after validating the complete file. Later execs use the
         * kernel page under their new SELinux domain. */''')
    replace(elf, '''/* AArch64 libc may leave sa_restorer unset. Supply the registered stub with
 * a kernel special-mapping lifetime and no permission to acquire write access. */''',
            '''/* AArch64 libc may leave sa_restorer unset. Each mm maps the shared stub
 * at an available address with no permission to acquire write access. */''')
    replace(elf, '''    struct vm_area_struct *vma;
    int error = init_sigreturn_page();''',
            '''    struct vm_area_struct *vma;
    unsigned long base;
    int error = init_sigreturn_page();''')
    replace(elf, '''    if (find_vma_intersection(mm, sigreturn_base, sigreturn_base + PAGE_SIZE))
        error = -EEXIST;
    else {
        vma = _install_special_mapping(mm, sigreturn_base, PAGE_SIZE,''',
            '''    base = get_unmapped_area(NULL, 0, PAGE_SIZE, 0, 0);
    if (IS_ERR_VALUE(base))
        error = base;
    else {
        vma = _install_special_mapping(mm, base, PAGE_SIZE,''')
    replace(elf, '        error = nce_populate_code(sigreturn_base, PAGE_SIZE);',
            '        error = nce_populate_code(base, PAGE_SIZE);')
    replace(elf, '        mm->context.sigreturn = sigreturn_entry;',
            '        mm->context.sigreturn = base + sigreturn_entry_offset;')

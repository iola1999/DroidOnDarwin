/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/elf.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <asm/host_ops.h>

unsigned long nce_elf_file_layout(struct file *file, const struct elfhdr *header,
                                 const struct elf_phdr *phdrs)
{
    char *buffer = kmalloc(PATH_MAX, GFP_KERNEL);
    unsigned long base = 0;
    char *path;
    if (!buffer) return 0;
    path = d_path(&file->f_path, buffer, PATH_MAX);
    if (!IS_ERR(path)) base = lkl_ops->user_elf(path, header, phdrs);
    kfree(buffer);
    return base;
}

static unsigned long file_layout(struct file *file, unsigned long *length)
{
    struct elfhdr header;
    struct elf_phdr *phdrs;
    loff_t pos = 0;
    size_t bytes;
    unsigned long base = 0;
    if (kernel_read(file, &header, sizeof(header), &pos) != sizeof(header) ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_type != ET_DYN ||
        header.e_machine != EM_AARCH64 || header.e_phentsize != sizeof(*phdrs) ||
        !header.e_phnum || header.e_phnum > 128) return 0;
    bytes = header.e_phnum * sizeof(*phdrs);
    phdrs = kmalloc(bytes, GFP_KERNEL);
    if (!phdrs) return 0;
    pos = header.e_phoff;
    if (pos >= 0 && kernel_read(file, phdrs, bytes, &pos) == bytes) {
        base = nce_elf_file_layout(file, &header, phdrs);
        if (base && length) {
            *length = 0;
            for (unsigned i = 0; i < header.e_phnum; ++i)
                if (phdrs[i].p_type == PT_LOAD)
                    *length = max(*length, PAGE_ALIGN(phdrs[i].p_vaddr + phdrs[i].p_memsz));
        }
    }
    kfree(phdrs);
    return base;
}

/* The adapted Android linker asks for its registered ELF reservation before
 * MAP_FIXED segment loads. Metadata reads retain ordinary mmap placement. */
long nce_reserve_elf(long *params)
{
    struct fd file = fdget(params[0]);
    unsigned long length = 0, base;
    if (fd_empty(file)) return -EBADF;
    base = file_layout(fd_file(file), &length);
    fdput(file);
    if (!base) return -ENOEXEC;
    if (!length || params[1] != length) return -EINVAL;
    return vm_mmap(NULL, base, length, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
}

static int populate_code(unsigned long address, unsigned long length)
{
    unsigned long end = PAGE_ALIGN(address + length);
    while (address < end) {
        struct page *page;
        long count = get_user_pages_unlocked(address, 1, &page, 0);
        if (count != 1) return count < 0 ? count : -EFAULT;
        put_page(page);
        address += PAGE_SIZE;
    }
    return 0;
}

SYSCALL_DEFINE6(nce_mmap, unsigned long, addr, unsigned long, len,
               unsigned long, prot, unsigned long, flags, unsigned long, fd,
               unsigned long, offset)
{
    unsigned long base = 0, load_length = 0;
    long result;
    /* AArch64 mmap takes a byte offset; LKL's generic wrapper used pages. */
    if (offset & ~PAGE_MASK) return -EINVAL;
    if (!(flags & MAP_ANONYMOUS)) {
        struct fd file = fdget(fd);
        if (fd_empty(file)) return -EBADF;
        base = file_layout(fd_file(file), &load_length);
        fdput(file);
    }
    if (prot & PROT_EXEC) {
        if (!base || (prot & PROT_WRITE)) return -EACCES;
    }
    if (base && !(flags & MAP_FIXED) && !offset && len == load_length) {
        if (addr && addr != base) return -EINVAL;
        addr = base;
        flags |= MAP_FIXED_NOREPLACE;
    }
    result = ksys_mmap_pgoff(addr, len, prot, flags, fd, offset >> PAGE_SHIFT);
    if (!IS_ERR_VALUE(result) && (prot & PROT_EXEC)) {
        int error = populate_code(result, len);
        if (error) { vm_munmap(result, len); return error; }
    }
    return result;
}

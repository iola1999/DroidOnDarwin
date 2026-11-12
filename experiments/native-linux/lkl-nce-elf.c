/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/elf.h>
#include <linux/file.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/syscalls.h>
#include <linux/security.h>
#include <linux/shmem_fs.h>
#include <linux/userfaultfd_k.h>
#include <asm/host_ops.h>
#include <asm/nce-layout.h>

static bool overlaps_dynamic(unsigned long addr, unsigned long length)
{
    unsigned long base = nce_user_base + NCE_EXEC_OFFSET;
    return length && addr < base + NCE_EXEC_SIZE &&
           (addr >= base || length > base - addr);
}

unsigned long nce_elf_file_layout(struct file *file, unsigned long source_offset, const struct elfhdr *header,
                                  const struct elf_phdr *phdrs)
{
    char *buffer = kmalloc(PATH_MAX, GFP_KERNEL);
    unsigned long base = 0;
    char *path;
    if (!buffer)
        return 0;
    path = d_path(&file->f_path, buffer, PATH_MAX);
    if (!IS_ERR(path))
        base = lkl_ops->user_elf(path, source_offset, header, phdrs);
    kfree(buffer);
    return base;
}

static unsigned long file_layout(struct file *file, unsigned long source_offset, unsigned long *length)
{
    struct elfhdr header;
    struct elf_phdr *phdrs;
    loff_t pos = source_offset;
    size_t bytes;
    unsigned long base = 0;
    if (!file->f_op->read_iter || source_offset > LLONG_MAX - sizeof(header))
        return 0;
    if (kernel_read(file, &header, sizeof(header), &pos) != sizeof(header) ||
        memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_type != ET_DYN ||
        header.e_machine != EM_AARCH64 || header.e_phentsize != sizeof(*phdrs) ||
        !header.e_phnum || header.e_phnum > 128)
        return 0;
    bytes = header.e_phnum * sizeof(*phdrs);
    phdrs = kmalloc(bytes, GFP_KERNEL);
    if (!phdrs)
        return 0;
    if (source_offset > LLONG_MAX - bytes || header.e_phoff > LLONG_MAX - source_offset - bytes) {
        kfree(phdrs);
        return 0;
    }
    pos = source_offset + header.e_phoff;
    if (kernel_read(file, phdrs, bytes, &pos) == bytes) {
        base = nce_elf_file_layout(file, source_offset, &header, phdrs);
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
    if (fd_empty(file))
        return -EBADF;
    if (params[2] < 0 || offset_in_page(params[2])) {
        fdput(file);
        return -EINVAL;
    }
    base = file_layout(fd_file(file), params[2], &length);
    fdput(file);
    if (!base)
        return -ENOEXEC;
    if (!length || params[1] != length)
        return -EINVAL;
    return vm_mmap(NULL, base, length, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0);
}

int nce_populate_code(unsigned long address, unsigned long length)
{
    unsigned long end = PAGE_ALIGN(address + length);
    while (address < end) {
        struct page *page;
        long count = get_user_pages_unlocked(address, 1, &page, 0);
        if (count != 1)
            return count < 0 ? count : -EFAULT;
        put_page(page);
        address += PAGE_SIZE;
    }
    return 0;
}

static DEFINE_MUTEX(sigreturn_mutex);
static struct page *sigreturn_pages[2];
static unsigned long sigreturn_base, sigreturn_entry;

static int sigreturn_mremap(const struct vm_special_mapping *mapping,
                            struct vm_area_struct *vma)
{
    return vma->vm_start == sigreturn_base && vma->vm_end - vma->vm_start == PAGE_SIZE ? 0 : -EINVAL;
}

/* The descriptor, page array and initial page reference live until the kernel
 * RAM is released. Each mm owns only the references acquired by its PTEs. */
static const struct vm_special_mapping sigreturn_mapping = {
    .name = "[nce-sigreturn]",
    .pages = sigreturn_pages,
    .mremap = sigreturn_mremap,
};

static int init_sigreturn_page(void)
{
    struct file *file;
    struct page *page = NULL;
    const struct elfhdr *header;
    const struct elf_phdr *load;
    unsigned long base;
    loff_t size, position = 0;
    int error;

    error = mutex_lock_killable(&sigreturn_mutex);
    if (error)
        return error;
    if (sigreturn_pages[0]) {
        error = 0;
        goto unlock;
    }
    file = filp_open("/lib/nce-sigreturn.so", O_RDONLY, 0);
    if (IS_ERR(file)) {
        error = PTR_ERR(file);
        goto unlock;
    }
    error = -ENOEXEC;
    size = i_size_read(file_inode(file));
    if (size < sizeof(*header) + sizeof(*load) || size > PAGE_SIZE)
        goto close;
    page = alloc_page(GFP_KERNEL | __GFP_ZERO);
    if (!page) {
        error = -ENOMEM;
        goto close;
    }
    if (kernel_read(file, page_address(page), size, &position) != size)
        goto close;
    header = page_address(page);
    if (memcmp(header->e_ident, "\177ELF\2\1\1", 7) || header->e_type != ET_DYN ||
        header->e_machine != EM_AARCH64 || header->e_version != EV_CURRENT ||
        header->e_ehsize != sizeof(*header) || header->e_phentsize != sizeof(*load) ||
        header->e_phnum != 1 || header->e_phoff < sizeof(*header) ||
        header->e_phoff > size - sizeof(*load) || header->e_phoff % __alignof__(*load))
        goto close;
    load = (const void *)header + header->e_phoff;
    if (load->p_type != PT_LOAD || load->p_flags != (PF_R | PF_X) ||
        load->p_offset || load->p_vaddr || load->p_memsz != load->p_filesz ||
        load->p_filesz > size || load->p_filesz < header->e_phoff + sizeof(*load) ||
        header->e_entry < header->e_phoff + sizeof(*load) ||
        header->e_entry >= load->p_filesz || (header->e_entry & 3))
        goto close;
    base = nce_elf_file_layout(file, 0, header, load);
    if (!base || offset_in_page(base) || base < nce_user_base || base > TASK_SIZE - PAGE_SIZE)
        goto close;
    error = 0;
close:
    fput(file);
    if (!error) {
        sigreturn_base = base;
        sigreturn_entry = base + header->e_entry;
        /* Publish after reading the complete file and matching its registered
         * layout. Later execs use the kernel page under their new SELinux domain. */
        sigreturn_pages[0] = page;
        page = NULL;
    }
    if (page)
        __free_page(page);
unlock:
    mutex_unlock(&sigreturn_mutex);
    return error;
}

/* AArch64 libc may leave sa_restorer unset. Supply the registered stub with
 * a kernel special-mapping lifetime and no permission to acquire write access. */
int arch_setup_additional_pages(struct linux_binprm *bprm, int uses_interp)
{
    struct mm_struct *mm = current->mm;
    struct vm_area_struct *vma;
    int error = init_sigreturn_page();
    if (error)
        return error;
    if (mmap_write_lock_killable(mm))
        return -EINTR;
    if (find_vma_intersection(mm, sigreturn_base, sigreturn_base + PAGE_SIZE))
        error = -EEXIST;
    else {
        vma = _install_special_mapping(mm, sigreturn_base, PAGE_SIZE,
                                       VM_READ | VM_EXEC | VM_MAYREAD | VM_MAYEXEC,
                                       &sigreturn_mapping);
        error = PTR_ERR_OR_ZERO(vma);
    }
    mmap_write_unlock(mm);
    if (!error)
        error = nce_populate_code(sigreturn_base, PAGE_SIZE);
    if (!error)
        mm->context.sigreturn = sigreturn_entry;
    return error;
}

/* The fixed host executable window survives debugger detach. Linux still
 * owns allocation and page lifetime; producers write a separate shared alias. */
long nce_map_code(long *params)
{
    unsigned long length = params[1], offset = params[2], result, populate;
    struct fd fd;
    struct file *file;
    struct mm_struct *mm = current->mm;
    LIST_HEAD(uf);
    struct vm_unmapped_area_info info = {
        /* The first page holds the nearby host register-restore branch. */
        .low_limit = nce_user_base + NCE_EXEC_OFFSET + PAGE_SIZE,
        .high_limit = nce_user_base + NCE_EXEC_OFFSET + NCE_EXEC_SIZE,
    };
    if (!length || length > NCE_EXEC_SIZE || offset_in_page(offset) ||
        offset > LONG_MAX - PAGE_ALIGN(length))
        return -EINVAL;
    length = PAGE_ALIGN(length);
    info.length = length;
    fd = fdget(params[0]);
    if (fd_empty(fd))
        return -EBADF;
    file = fd_file(fd);
    if (!shmem_file(file) || offset + length > i_size_read(file_inode(file))) {
        result = -EINVAL;
        goto out;
    }
    result = security_mmap_file(file, PROT_READ | PROT_EXEC, MAP_SHARED);
    if (result)
        goto out;
    if (mmap_write_lock_killable(mm)) {
        result = -EINTR;
        goto out;
    }
    result = vm_unmapped_area(&info);
    if (!IS_ERR_VALUE(result))
        result = do_mmap(file, result, length, PROT_READ | PROT_EXEC,
                         MAP_SHARED | MAP_FIXED_NOREPLACE, 0, offset >> PAGE_SHIFT,
                         &populate, &uf);
    mmap_write_unlock(mm);
    userfaultfd_unmap_complete(mm, &uf);
    /* Publish or the first instruction fault installs the executable pages.
     * Reserving an ART cache must not allocate its unused maximum capacity. */
out:
    fdput(fd);
    return result;
}

/* Keep ordinary VM operations from replacing prepared host entries or
 * installing executable pages outside the registered executable windows. */
long nce_check_memory_syscall(long number, long *params)
{
    unsigned long start = params[0], length = PAGE_ALIGN(params[1]);
    struct vm_area_struct *vma;
    int result = 0;
    if (!length && number == 216)
        length = PAGE_SIZE;
    if (!length)
        return 0;
    if (start + length < start)
        return -EINVAL;
    if (lkl_ops->user_reserved(start, length))
        return -EACCES;
    if (number == 216) {
        if ((params[3] & MREMAP_FIXED) && lkl_ops->user_reserved(params[4], params[2]))
            return -EACCES;
        if ((params[3] & MREMAP_FIXED) && overlaps_dynamic(params[4], params[2]))
            return -EACCES;
        if (overlaps_dynamic(start, length))
            return -EACCES;
    } else {
        if ((params[2] & (PROT_WRITE | PROT_EXEC)) == (PROT_WRITE | PROT_EXEC))
            return -EACCES;
        if (overlaps_dynamic(start, length) && params[2] && params[2] != (PROT_READ | PROT_EXEC))
            return -EACCES;
    }
    mmap_read_lock(current->mm);
    VMA_ITERATOR(iter, current->mm, start);
    for_each_vma_range(iter, vma, start + length)
    {
        if (number == 216 ? (vma->vm_flags & VM_EXEC) : ((params[2] & PROT_EXEC) && !(vma->vm_flags & VM_EXEC) && !overlaps_dynamic(vma->vm_start, vma->vm_end - vma->vm_start))) {
            result = -EACCES;
            break;
        }
    }
    mmap_read_unlock(current->mm);
    return result;
}

SYSCALL_DEFINE6(nce_mmap, unsigned long, addr, unsigned long, len,
                unsigned long, prot, unsigned long, flags, unsigned long, fd,
                unsigned long, offset)
{
    unsigned long base = 0, load_length = 0;
    long result;
    /* AArch64 mmap takes a byte offset; LKL's generic wrapper used pages. */
    if (offset & ~PAGE_MASK)
        return -EINVAL;
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && lkl_ops->user_reserved(addr, len))
        return -EACCES;
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && overlaps_dynamic(addr, len))
        return -EACCES;
    if (!(flags & MAP_ANONYMOUS)) {
        struct fd file = fdget(fd);
        if (fd_empty(file))
            return -EBADF;
        base = file_layout(fd_file(file), lkl_ops->user_elf_offset(addr), &load_length);
        fdput(file);
    }
    if (prot & PROT_EXEC) {
        if (!base || (prot & PROT_WRITE))
            return -EACCES;
    }
    if (base && !(flags & MAP_FIXED) && !offset && len == load_length) {
        if (addr && addr != base)
            return -EINVAL;
        addr = base;
        flags |= MAP_FIXED_NOREPLACE;
    }
    result = ksys_mmap_pgoff(addr, len, prot, flags, fd, offset >> PAGE_SHIFT);
    if (!IS_ERR_VALUE(result) && (prot & PROT_EXEC)) {
        int error = nce_populate_code(result, len);
        if (error) {
            vm_munmap(result, len);
            return error;
        }
    }
    return result;
}

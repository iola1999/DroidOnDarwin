#!/usr/bin/env python3
"""为固定版本的 LKL 应用 N2 原型所需的架构修改。"""
from pathlib import Path


def apply(source: Path, experiment: Path):
    def edit(name, before, after):
        path = source / name
        text = path.read_text()
        if after in text:
            return
        if text.count(before) != 1:
            raise RuntimeError(f"LKL source mismatch: {name}: {before[:80]}")
        path.write_text(text.replace(before, after))

    arch = "arch/lkl/"
    edit(arch + "include/asm/Kbuild", "generic-y += signal.h", "# signal.h uses the AArch64 userspace ABI")
    for name in ("signal.h", "sigcontext.h", "ucontext.h", "sve_context.h"):
        target = source / arch / "include/uapi/asm" / name
        content = (source / "arch/arm64/include/uapi/asm" / name).read_bytes()
        if not target.exists() or target.read_bytes() != content:
            target.write_bytes(content)
    edit(arch + "Kconfig", "\tselect UACCESS_MEMCPY", "\tselect UACCESS_MEMCPY if !MMU")
    edit(arch + "include/asm/page.h", "#include <asm/page-mmu.h>", "#include <asm/page-mmu.h>\n#define VM_DATA_DEFAULT_FLAGS VM_DATA_FLAGS_NON_EXEC")
    edit(arch + "include/asm/page-mmu.h", "#define PAGE_SHIFT\t12", "#define PAGE_SHIFT\t14")
    edit(arch + "include/asm/thread_info.h", "#define THREAD_SIZE\t       (4096)", "#define THREAD_SIZE\t       (16384)")
    edit(arch + "Kconfig", "\tselect ARCH_THREAD_STACK_ALLOCATOR", "\tselect ARCH_THREAD_STACK_ALLOCATOR\n\tselect CLONE_BACKWARDS if MMU")
    # Released PTE pages must lose their page-table type before reuse by drivers.
    edit(arch + "include/asm/pgalloc.h",
         "#define __pte_free_tlb(tlb, pte, address) tlb_remove_page((tlb), (pte))",
         "#define __pte_free_tlb(tlb, pte, address) do { \\\n\tpagetable_pte_dtor(page_ptdesc(pte)); \\\n\ttlb_remove_page_ptdesc((tlb), page_ptdesc(pte)); \\\n} while (0)")
    pg = arch + "include/asm/pgtable-mmu-3level.h"
    edit(pg, "#define PGDIR_SHIFT\t30", "#define PGDIR_SHIFT\t36")
    edit(pg, "#define PMD_SHIFT\t21", "#define PMD_SHIFT\t25")
    for level in ("PTE", "PMD", "PGD"):
        edit(pg, f"#define PTRS_PER_{level} 512", f"#define PTRS_PER_{level} 2048")
    vmalloc_end = "#define VMALLOC_END\t(nce_memory_start + NCE_RAM_SIZE + NCE_VMALLOC_SIZE)"
    if "#define VMALLOC_END\t(nce_memory_start + 0x8000000UL)" in (source / pg).read_text():
        edit(pg, "#define VMALLOC_END\t(nce_memory_start + 0x8000000UL)", vmalloc_end)
    edit(pg, "#define VMALLOC_END\t((TASK_SIZE - 4 * PAGE_SIZE))", vmalloc_end)
    edit(pg, "#define _PAGE_DIRTY\t0x100", "#define _PAGE_DIRTY\t0x100\n#define _PAGE_EXEC\t0x200")
    edit(pg, "return ((pte_get_bits(pte, _PAGE_USER)) &&\n\t       !(pte_get_bits(pte, _PAGE_PROTNONE)));\n}\n\nstatic inline int pte_write", "return ((pte_get_bits(pte, _PAGE_EXEC)) &&\n\t       !(pte_get_bits(pte, _PAGE_PROTNONE)));\n}\n\nstatic inline int pte_write")
    edit(pg, "void mmap_pages_for_ptes(unsigned long va, unsigned int nr, pte_t pte);\nvoid munmap_page_for_pte(unsigned long addr, pte_t *xp);", "void mmap_pages_for_ptes(struct mm_struct *mm, unsigned long va, unsigned int nr, pte_t pte);\nvoid munmap_page_for_pte(struct mm_struct *mm, unsigned long addr, pte_t *xp);")
    edit(pg, "munmap_page_for_pte(addr, xp);", "munmap_page_for_pte(mm, addr, xp);")
    edit(pg, "mmap_pages_for_ptes(addr, nr, pte);", "mmap_pages_for_ptes(mm, addr, nr, pte);")
    edit(arch + "include/asm/processor.h", "struct thread_struct { };", "struct thread_struct { void *nce_state; };")
    edit(arch + "include/asm/processor.h", "#define task_pt_regs(tsk) (struct pt_regs *)(NULL)", "#ifdef CONFIG_MMU\n#define task_pt_regs(tsk) ((struct pt_regs *)(tsk)->thread.nce_state)\n#else\n#define task_pt_regs(tsk) (struct pt_regs *)(NULL)\n#endif")
    ptrace = arch + "include/asm/ptrace.h"
    edit(ptrace, "#include <linux/errno.h>", "#include <linux/errno.h>\n#include <asm/guest-registers.h>")
    edit(ptrace, "#define user_stack_pointer(regs) 0", "#define user_stack_pointer(regs) ((regs) ? (regs)->sp : 0)")
    edit(ptrace, "#define instruction_pointer(regs) 0", "#define instruction_pointer(regs) ((regs) ? (regs)->pc : 0)")
    edit(arch + "include/asm/processor.h", "static inline void start_thread(struct pt_regs *regs, unsigned long entry,\n\t\t\t unsigned long stack) {}", "void start_thread(struct pt_regs *regs, unsigned long entry, unsigned long stack);")
    edit(arch + "include/asm/processor.h", "#define TASK_SIZE ((unsigned long)-1)", "#ifdef CONFIG_MMU\nextern unsigned long nce_user_base, nce_task_size, nce_memory_start;\n#define TASK_SIZE nce_task_size\n#else\n#define TASK_SIZE ((unsigned long)-1)\n#endif")
    edit(arch + "include/asm/processor.h", "(CONFIG_LKL_MEMORY_START - 16 * PAGE_SIZE)", "(TASK_SIZE - 16 * PAGE_SIZE)")
    edit(arch + "mm/bootmem.c", "(void *)CONFIG_LKL_MEMORY_START", "(void *)nce_memory_start")
    processor = source / arch / "include/asm/processor.h"
    original = processor.read_text()
    migrated = original.replace("#define TASK_UNMAPPED_BASE\tnce_user_base\n", "#define TASK_UNMAPPED_BASE\tCONFIG_LKL_TASK_UNMAPPED_BASE\n")
    if migrated != original:
        processor.write_text(migrated)
    edit(arch + "include/asm/processor.h", "#define TASK_UNMAPPED_BASE\tCONFIG_LKL_TASK_UNMAPPED_BASE", "#include <asm/nce-layout.h>\n#define TASK_UNMAPPED_BASE\t(nce_user_base + NCE_MMAP_OFFSET)")
    edit(arch + "include/uapi/asm/host_ops.h", "\tstruct lkl_dev_pci_ops *pci_ops;", "\tstruct lkl_dev_pci_ops *pci_ops;\n\n\t/* N2 controlled userspace. The Linux CPU owns the active context. */\n\tvoid *(*user_clone)(void *parent, unsigned long sp, unsigned long tls, unsigned long flags);\n\tvoid (*user_free)(void *state);\n\tvoid (*user_switch)(void *state);\n\tvoid (*user_run)(void *state);\n\tvoid (*user_exec)(void *state, unsigned long entry, unsigned long stack);\n")
    edit(arch + "include/uapi/asm/host_ops.h", "\tvoid (*user_exec)(void *state, unsigned long entry, unsigned long stack);", "\tvoid (*user_exec)(void *state, unsigned long entry, unsigned long stack);\n\tunsigned long (*user_elf)(const char *path, const void *header, const void *phdrs);")
    edit("fs/binfmt_elf.c", "unsigned long load_bias = 0, phdr_addr = 0;", "unsigned long load_bias = 0, phdr_addr = 0;\n#ifdef NCE_ELF_LAYOUT\n\tunsigned long nce_bias;\n#endif")
    if "\tnce_bias =" not in (source / "fs/binfmt_elf.c").read_text():
        edit("fs/binfmt_elf.c", "\t/* Flush all traces of the currently running executable */", "#ifdef NCE_ELF_LAYOUT\n\tnce_bias = interpreter ? 0 : NCE_ELF_LAYOUT(bprm->filename, elf_ex, elf_phdata);\n\tif (!nce_bias) {\n\t\tretval = -ENOEXEC;\n\t\tgoto out_free_dentry;\n\t}\n#endif\n\t/* Flush all traces of the currently running executable */")
    edit("fs/binfmt_elf.c", "\t\t\tload_bias = ELF_PAGESTART(load_bias - vaddr);", "#ifdef NCE_ELF_LAYOUT\n\t\t\tload_bias = nce_bias;\n\t\t\telf_flags |= MAP_FIXED_NOREPLACE;\n#endif\n\t\t\tload_bias = ELF_PAGESTART(load_bias - vaddr);")
    edit("fs/binfmt_elf.c", "nce_bias = interpreter ? 0 : NCE_ELF_LAYOUT(bprm->filename, elf_ex, elf_phdata);", "nce_bias = NCE_ELF_LAYOUT(bprm->filename, elf_ex, elf_phdata);")
    edit("fs/binfmt_elf.c", "if (!nce_bias) {", "if (!nce_bias || (interpreter && !NCE_ELF_INTERP_LAYOUT(interpreter, interp_elf_ex, interp_elf_phdata))) {")
    edit("fs/binfmt_elf.c", "\t\t\tmap_addr = elf_load(interpreter, load_addr + vaddr,", "#ifdef NCE_ELF_INTERP_LAYOUT\n\t\t\tif (!load_addr_set) {\n\t\t\t\tload_addr = NCE_ELF_INTERP_LAYOUT(interpreter, interp_elf_ex, interp_elf_phdata);\n\t\t\t\tif (!load_addr) return -ENOEXEC;\n\t\t\t\telf_type |= MAP_FIXED_NOREPLACE;\n\t\t\t}\n#endif\n\t\t\tmap_addr = elf_load(interpreter, load_addr + vaddr,")
    edit(arch + "kernel/threads.c", "\tti->flags = org_ti->flags;", "\tp->thread.nce_state = NULL;\n\tti->flags = org_ti->flags;")
    edit(arch + "kernel/threads.c", "\tkill_thread(ti);\n\tkfree(ti);", "\tkill_thread(ti);\n#ifdef CONFIG_MMU\n\tif (tsk->thread.nce_state)\n\t\tlkl_ops->user_free(tsk->thread.nce_state);\n#endif\n\tkfree(ti);")
    edit(arch + "kernel/threads.c", "\tlkl_ops->sem_up(_next->sched_sem);", "#ifdef CONFIG_MMU\n\tif (lkl_ops->user_switch)\n\t\tlkl_ops->user_switch(next->thread.nce_state);\n#endif\n\tlkl_ops->sem_up(_next->sched_sem);")
    edit(arch + "kernel/threads.c", "int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)", "#ifdef CONFIG_MMU\nstatic int user_bootstrap(void *unused)\n{\n\tlkl_ops->user_run(current->thread.nce_state);\n\treturn 0;\n}\n#endif\n\nint copy_thread(struct task_struct *p, const struct kernel_clone_args *args)")
    edit(arch + "kernel/threads.c", "if (!test_ti_thread_flag(ti, TIF_HOST_THREAD)) {", "if (!test_ti_thread_flag(ti, TIF_HOST_THREAD) && ti->tid) {")
    edit(arch + "kernel/threads.c", "\ttba = kmalloc(sizeof(*tba), GFP_KERNEL);", "\tclear_ti_thread_flag(ti, TIF_HOST_THREAD);\n#ifdef CONFIG_MMU\n\tif (!esp) {\n\t\tif (!current->thread.nce_state || !lkl_ops->user_clone)\n\t\t\treturn -EINVAL;\n\t\tp->thread.nce_state = lkl_ops->user_clone(current->thread.nce_state,\n\t\t\targs->stack, args->tls, args->flags);\n\t\tif (!p->thread.nce_state)\n\t\t\treturn -ENOMEM;\n\t\tesp = (unsigned long)user_bootstrap;\n\t}\n#endif\n\ttba = kmalloc(sizeof(*tba), GFP_KERNEL);")
    edit(arch + "include/asm/syscalls.h", "#define sys_clone sys_ni_syscall", "#ifndef CONFIG_MMU\n#define sys_clone sys_ni_syscall\n#endif")
    edit(arch + "include/asm/syscalls.h", "#define sys_rt_sigreturn sys_ni_syscall", "#ifndef CONFIG_MMU\n#define sys_rt_sigreturn sys_ni_syscall\n#endif")
    edit(arch + "include/asm/syscalls.h", "#define sys_mmap sys_mmap_pgoff", "#ifdef CONFIG_MMU\nasmlinkage long sys_nce_mmap(unsigned long, unsigned long, unsigned long, unsigned long, unsigned long, unsigned long);\n#define sys_mmap sys_nce_mmap\n#else\n#define sys_mmap sys_mmap_pgoff\n#endif")
    edit(arch + "include/uapi/asm/unistd.h", "#define __ARCH_WANT_SYSCALL_NO_AT", "#ifdef CONFIG_MMU\n#define __ARCH_WANT_SYS_CLONE\n#endif\n#define __ARCH_WANT_SYSCALL_NO_AT")
    if "nce_reserve_elf(params)" not in (source / arch / "kernel/syscalls.c").read_text():
        edit(arch + "kernel/syscalls.c", "static long run_syscall(long no, long *params)", "#ifdef CONFIG_MMU\nextern long nce_launch(long *params);\n#endif\nlong nce_run_syscall(long no, long *params)")
        edit(arch + "kernel/syscalls.c", "\tsyscall_handler_t handler = (syscall_handler_t)syscall_table[no];\n\n\tif (no < 0 || no >= __NR_syscalls)\n\t\treturn -ENOSYS;", "\tsyscall_handler_t handler;\n\tif (no < 0 || no >= __NR_syscalls)\n\t\treturn -ENOSYS;\n#ifdef CONFIG_MMU\n\tif (no == 246)\n\t\treturn nce_launch(params);\n#endif\n\thandler = (syscall_handler_t)syscall_table[no];")
    edit(arch + "kernel/syscalls.c", "ret = run_syscall(no, params);", "ret = nce_run_syscall(no, params);")
    edit(arch + "kernel/syscalls.c", "extern long nce_launch(long *params);", "extern long nce_launch(long *params);\nextern long nce_reserve_elf(long *params);")
    edit(arch + "kernel/syscalls.c", "\tif (no == 246)", "\tif (no == 245)\n\t\treturn nce_reserve_elf(params);\n\tif (no == 246)")
    edit(arch + "kernel/syscalls.c", "extern long nce_reserve_elf(long *params);", "extern long nce_reserve_elf(long *params);\nextern long nce_map_code(long *params);\nextern long nce_publish_code(long *params);\nextern long nce_check_memory_syscall(long no, long *params);")
    edit(arch + "kernel/syscalls.c", "\tif (no == 245)", "\tif (no == 243) return nce_map_code(params);\n\tif (no == 244) return nce_publish_code(params);\n\tif (no == 226 || no == 216) {\n\t\tlong error = nce_check_memory_syscall(no, params);\n\t\tif (error) return error;\n\t}\n\tif (no == 245)")
    edit(arch + "kernel/syscalls.c", "\tkernel_sigaction(SIGCHLD, SIG_IGN);", "#ifndef CONFIG_MMU\n\tkernel_sigaction(SIGCHLD, SIG_IGN);\n#endif")
    edit(arch + "kernel/setup.c", "if (strcmp(\"/init\", bprm->filename) != 0)\n\t\treturn -EINVAL;", "if (strcmp(\"/init\", bprm->filename) != 0)\n\t\treturn -ENOEXEC;")
    edit(arch + "kernel/setup.c", "\tinit_pid_ns.child_reaper = 0;", "#ifndef CONFIG_MMU\n\tinit_pid_ns.child_reaper = 0;\n#endif")
    edit(arch + "mm/Makefile", "obj-$(CONFIG_MMU) += mmu_mem.o", "obj-$(CONFIG_MMU) += mmu_mem.o nce_mmu.o")
    edit(arch + "kernel/Makefile", "\tsyscalls_32.o cpu.o init.o stacktrace.o", "\tsyscalls_32.o cpu.o init.o stacktrace.o\nobj-$(CONFIG_MMU) += nce_process.o")
    edit(arch + "kernel/Makefile", "obj-$(CONFIG_MMU) += nce_process.o", "obj-$(CONFIG_MMU) += nce_process.o nce_signal.o")
    edit(arch + "kernel/Makefile", "nce_process.o nce_signal.o", "nce_process.o nce_signal.o nce_elf.o")
    for local, destination in (("layout.h", "include/asm/nce-layout.h"),
                               ("guest-registers.h", "include/asm/guest-registers.h"),
                               ("lkl-signal.h", "include/asm/signal.h"),
                               ("lkl-nce-mmu.c", "mm/nce_mmu.c"),
                               ("lkl-nce-process.c", "kernel/nce_process.c"),
                               ("lkl-nce-signal.c", "kernel/nce_signal.c"),
                               ("lkl-nce-elf.c", "kernel/nce_elf.c"),
                               ("lkl-mmu-context.h", "include/asm/mmu_context.h"),
                               ("lkl-mmu.h", "include/asm/mmu.h"),
                               ("lkl-uaccess.h", "include/asm/uaccess.h"),
                               ("lkl-elf.h", "include/asm/elf.h"),
                               ("lkl-tlbflush.h", "include/asm/tlbflush.h")):
        target = source / arch / destination
        content = (experiment / local).read_bytes()
        if not target.exists() or target.read_bytes() != content:
            target.write_bytes(content)
    mmu = source / arch / "mm/mmu_mem.c"
    text = mmu.read_text()
    if "void mmap_pages_for_ptes(" in text:
        text = text[:text.index("void mmap_pages_for_ptes(")]
    # Keep the existing generic page-table allocation and encode execute access.
    text = text.replace("memcpy(pgd, swapper_pg_dir, sizeof(swapper_pg_dir));", "clear_page(pgd);")
    text = text.replace("// There is no user-space & kernel-space virtual memory boundary for LKL.", "/* Each userspace mm owns its page tables; kernel mappings use init_mm. */")
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if "[VM_" in line and "VM_EXEC" in line and "__pgprot" not in line:
            lhs, rhs = line.split("=")
            prot = rhs.strip().rstrip(",")
            lines[i] = lhs + "= __pgprot(pgprot_val(" + prot + ") | _PAGE_EXEC)" + ("," if line.rstrip().endswith(",") else "")
    modified = "\n".join(lines) + "\n"
    if mmu.read_text() != modified:
        mmu.write_text(modified)

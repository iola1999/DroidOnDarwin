#!/usr/bin/env python3
"""在独立 SMP 构建中使用 Linux core scheduling 协调共享 mm。"""
from pathlib import Path


def apply(source: Path, experiment: Path):
    def edit(name, before, after):
        path = source / name
        text = path.read_text()
        if after in text:
            return
        if text.count(before) != 1:
            raise RuntimeError(f'SMP core scheduling source mismatch: {name}: {before[:80]}')
        path.write_text(text.replace(before, after))

    arch = 'arch/lkl/'
    for name, target in [('core.h', 'nce-core.h'), ('topology.h', 'topology.h')]:
        path = source / arch / 'include/asm' / target
        content = (experiment / 'smp' / name).read_bytes()
        if not path.exists() or path.read_bytes() != content:
            path.write_bytes(content)
    edit(arch + 'Kconfig', 'config NR_CPUS\n',
         'config SCHED_SMT\n\tbool\n\tdepends on SMP && MMU\n\tdefault y\n\nconfig NR_CPUS\n')
    edit(arch + 'include/asm/mmu.h', '    unsigned long sigreturn;',
         '    unsigned long sigreturn;\n    unsigned long core_cookie;')
    edit(arch + 'mm/nce_mmu.c', '#include <asm/nce-window.h>',
         '#include <asm/nce-window.h>\n#include <asm/nce-core.h>')
    edit(arch + 'mm/nce_mmu.c', '    memset(mm->context.accessed, 0, bytes);\n    return 0;',
         '    memset(mm->context.accessed, 0, bytes);\n'
         '    int error = nce_core_init_mm(mm);\n'
         '    if (error) {\n        lkl_ops->mem_free(mm->context.accessed);\n'
         '        mm->context.accessed = NULL;\n    }\n    return error;')
    edit(arch + 'mm/nce_mmu.c',
         '    nce_destroy_mm_window_locked(mm);\n    nce_window_unlock();',
         '    nce_destroy_mm_window_locked(mm);\n    nce_window_unlock();\n    nce_core_destroy_mm(mm);')
    edit(arch + 'mm/nce_mmu.c', 'void nce_begin_user(void)\n{\n',
         'void nce_begin_user(void)\n{\n    nce_core_enter();\n')
    # fork has assigned p->mm before this hook and still owns the task. A
    # copied mm gets its own group, while CLONE_VM reuses the existing cookie.
    edit('kernel/sched/core_sched.c',
         '\tp->core_cookie = sched_core_clone_cookie(current);',
         '\tp->core_cookie = sched_core_get_cookie(p->mm ? p->mm->context.core_cookie : 0);')
    core = source / 'kernel/sched/core_sched.c'
    text = core.read_text()
    if 'int nce_core_init_mm(' not in text:
        text += '''

/* Address spaces and tasks hold independent references across fork and exec.
 * Keep Linux's cookie allocation, task-rq updates and release accounting. */
int nce_core_init_mm(struct mm_struct *mm)
{
    mm->context.core_cookie = sched_core_alloc_cookie();
    return mm->context.core_cookie ? 0 : -ENOMEM;
}

void nce_core_destroy_mm(struct mm_struct *mm)
{
    sched_core_put_cookie(mm->context.core_cookie);
    mm->context.core_cookie = 0;
}

void nce_core_enter(void)
{
    struct mm_struct *mm = current->mm;
    BUG_ON(!mm || !mm->context.core_cookie);
    if (current->core_cookie != mm->context.core_cookie)
        __sched_core_set(current, mm->context.core_cookie);
    /* exec or a userspace cookie request may have changed the group. Finish
     * the requested reschedule before preparing the guest-address window. */
    cond_resched();
}
'''
        core.write_text(text)
    # With coordinated groups, these readers can complete without requesting
    # a window stop. Every read must still see its own address space's value.
    test = source / arch / 'kernel/smp-window-test.c'
    if test.exists():
        edit(arch + 'kernel/smp-window-test.c',
             'BUG_ON(!workers[0].reads || !workers[1].reads || !(workers[0].stops + workers[1].stops));',
             'BUG_ON(!workers[0].reads || !workers[1].reads);')

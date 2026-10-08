/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/syscalls.h>
#include <asm/host_ops.h>

static int user_start(void *state)
{
    current->thread.nce_state = state;
    lkl_ops->user_switch(state);
    lkl_ops->user_run(state);
    return 0;
}

long nce_launch(long *params)
{
    if (current->thread.nce_state) return -ENOSYS;
    if (params[1] & ~1L) return -EINVAL;
    struct kernel_clone_args args = {
        .flags = params[1] ? CLONE_NEWPID : 0,
        .exit_signal = SIGCHLD,
        .fn = user_start,
        .fn_arg = (void *)params[0],
    };
    return kernel_clone(&args);
}

void start_thread(struct pt_regs *regs, unsigned long entry, unsigned long stack)
{
    struct vm_area_struct *vma;
    VMA_ITERATOR(iter, current->mm, 0);
    for_each_vma(iter, vma) {
        if (!(vma->vm_flags & VM_EXEC)) continue;
        for (unsigned long address = vma->vm_start; address < vma->vm_end; address += PAGE_SIZE) {
            struct page *page;
            long count = get_user_pages(address, 1, FOLL_WRITE | FOLL_FORCE, &page);
            BUG_ON(count != 1);
            put_page(page);
        }
    }
    /* Private pages retain rewritten instructions across process switches. */
    lkl_ops->user_exec(current->thread.nce_state, entry, stack);
}

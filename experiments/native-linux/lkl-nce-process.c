/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/pid_namespace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/syscalls.h>
#include <linux/smp.h>
#include <asm/host_ops.h>

static int user_start(void *state)
{
    current->thread.nce_state = state;
    lkl_ops->user_switch(state, raw_smp_processor_id(), num_possible_cpus());
    lkl_ops->user_run(state);
    return 0;
}

long nce_launch(long *params)
{
    if (current->thread.nce_state)
        return -ENOSYS;
    if (params[1] & ~1L)
        return -EINVAL;
    struct kernel_clone_args args = {
        .flags = params[1] ? CLONE_NEWPID : 0,
        .exit_signal = SIGCHLD,
        .fn = user_start,
        .fn_arg = (void *)params[0],
    };
    return kernel_clone(&args);
}

long nce_stop_namespace(long *params)
{
    struct task_struct *target;
    struct pid_namespace *namespace;
    pid_t pid;
    int result = -ESRCH;

    if (current->thread.nce_state)
        return -ENOSYS;
    pid = params[0];
    if (pid <= 0 || params[0] != pid)
        return -EINVAL;
    for (unsigned i = 1; i < 6; ++i) {
        if (params[i])
            return -EINVAL;
    }

    rcu_read_lock();
    read_lock(&tasklist_lock);
    target = find_task_by_vpid(pid);
    if (target) {
        namespace = task_active_pid_ns(target);
        if (target->real_parent != current || !READ_ONCE(target->thread.nce_state) ||
            !thread_group_leader(target) || !is_child_reaper(task_pid(target)) ||
            namespace == &init_pid_ns || namespace->parent != task_active_pid_ns(current))
            result = -EPERM;
        else {
            /* Host teardown must remain possible after the guest loads its SELinux policy. */
            result = group_send_sig_info(SIGKILL, SEND_SIG_PRIV, target, PIDTYPE_TGID);
        }
    }
    read_unlock(&tasklist_lock);
    rcu_read_unlock();
    return result;
}

void start_thread(struct pt_regs *regs, unsigned long entry, unsigned long stack)
{
    struct vm_area_struct *vma;
    VMA_ITERATOR(iter, current->mm, 0);
    mmap_read_lock(current->mm);
    for_each_vma(iter, vma)
    {
        if (!(vma->vm_flags & VM_EXEC))
            continue;
        for (unsigned long address = vma->vm_start; address < vma->vm_end; address += PAGE_SIZE) {
            struct page *page;
            long count = get_user_pages(address, 1, 0, &page);
            BUG_ON(count != 1);
            put_page(page);
        }
    }
    mmap_read_unlock(current->mm);
    /* Populate prepared executable ranges before the first user instruction. */
    lkl_ops->user_exec(current->thread.nce_state, entry, stack);
}

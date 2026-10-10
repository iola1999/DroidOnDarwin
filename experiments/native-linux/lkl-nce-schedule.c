/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/ktime.h>
#include <linux/sched/signal.h>
#include <asm/nce-mmu-profile.h>

/* The single Linux CPU calls this before returning to user code, with no
 * mmap lock held. Samples describe runnable peers, not execution overlap. */
void nce_sample_runnable(struct nce_schedule_profile *profile)
{
    static u64 previous;
    u64 now = ktime_get_ns();
    unsigned long same = 0, other = 0, kernel = 0;
    struct task_struct *process, *task;
    if (previous && now - previous < 10000000)
        return;
    rcu_read_lock();
    for_each_process_thread(process, task)
    {
        if (task == current || READ_ONCE(task->__state) != TASK_RUNNING ||
            !READ_ONCE(task->on_rq))
            continue;
        if (!task->mm)
            ++kernel;
        else if (task->mm == current->mm)
            ++same;
        else
            ++other;
    }
    rcu_read_unlock();
    /* Host reports can run between these individual atomic updates. */
#define ADD(field, value) __atomic_fetch_add(&profile->field, value, __ATOMIC_RELAXED)
#define MAXIMUM(field, value) \
    do { \
        if ((value) > __atomic_load_n(&profile->field, __ATOMIC_RELAXED)) \
            __atomic_store_n(&profile->field, value, __ATOMIC_RELAXED); \
    } while (0)
    ADD(same_mm_samples, same != 0);
    ADD(other_mm_samples, other != 0);
    ADD(same_mm_ready, same);
    ADD(other_mm_ready, other);
    ADD(kernel_or_host_ready, kernel);
    MAXIMUM(max_same_mm_ready, same);
    MAXIMUM(max_other_mm_ready, other);
    if (previous) {
        ADD(interval_ns, now - previous);
        MAXIMUM(max_interval_ns, now - previous);
    }
    previous = now;
    ADD(collection_ns, ktime_get_ns() - now);
    ADD(samples, 1);
#undef ADD
#undef MAXIMUM
}

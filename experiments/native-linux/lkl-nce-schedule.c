/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/ktime.h>
#include <linux/sched/signal.h>
#include <asm/nce-mmu-profile.h>

/* Called before returning to user code, with no mmap lock held. The shared
 * timestamp limits sampling across CPUs; counts describe runnable peers. */
void nce_sample_runnable(struct nce_schedule_profile *profile)
{
    static u64 previous;
    u64 now = ktime_get_ns();
    u64 last = __atomic_load_n(&previous, __ATOMIC_RELAXED);
    unsigned long same = 0, other = 0, kernel = 0;
    struct task_struct *process, *task;
    if (now <= last || (last && now - last < 10000000) ||
        !__atomic_compare_exchange_n(&previous, &last, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
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
        unsigned long observed = __atomic_load_n(&profile->field, __ATOMIC_RELAXED); \
        while ((value) > observed && \
               !__atomic_compare_exchange_n(&profile->field, &observed, value, true, \
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED)) { \
        } \
    } while (0)
    ADD(same_mm_samples, same != 0);
    ADD(other_mm_samples, other != 0);
    ADD(same_mm_ready, same);
    ADD(other_mm_ready, other);
    ADD(kernel_or_host_ready, kernel);
    MAXIMUM(max_same_mm_ready, same);
    MAXIMUM(max_other_mm_ready, other);
    if (last) {
        ADD(interval_ns, now - last);
        MAXIMUM(max_interval_ns, now - last);
    }
    ADD(collection_ns, ktime_get_ns() - now);
    ADD(samples, 1);
#undef ADD
#undef MAXIMUM
}

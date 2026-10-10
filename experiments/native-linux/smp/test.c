/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <asm/host_ops.h>
#include <asm/futex.h>

static atomic_t arrived, count;
static atomic64_t count64;
static u32 futex_count;
static struct worker {
    struct completion done;
    u64 start, end;
    unsigned int observed_cpu;
    int error;
} workers[2];

static int compute(void *arg)
{
    struct worker *worker = arg;
    unsigned int i;
    worker->observed_cpu = raw_smp_processor_id();
    atomic_inc(&arrived);
    while (atomic_read(&arrived) != 2)
        cpu_relax();
    worker->start = ktime_get_ns();
    for (i = 0; i < 100000; ++i) {
        int previous;
        atomic_inc(&count);
        atomic64_inc(&count64);
        worker->error |= arch_futex_atomic_op_inuser(FUTEX_OP_ADD, 1, &previous, &futex_count);
        if (!(i & 1023))
            cond_resched();
    }
    worker->end = ktime_get_ns();
    msleep(20);
    complete(&worker->done);
    return 0;
}

static void observe_ipi(void *arg)
{
    unsigned int *observed = arg;
    *observed = raw_smp_processor_id();
}

static int migrate(void *arg)
{
    struct worker *worker = arg;
    worker->observed_cpu = raw_smp_processor_id();
    if (set_cpus_allowed_ptr(current, cpumask_of(1)))
        worker->observed_cpu = 99;
    else if (raw_smp_processor_id() != 1)
        worker->observed_cpu = 98;
    complete(&worker->done);
    return 0;
}

long lkl_smp_test(void)
{
    struct task_struct *tasks[2];
    unsigned int cpu, observed;
    if (num_online_cpus() != 2 || raw_smp_processor_id()) {
        lkl_printf("smp test entry online=%u cpu=%u\n", num_online_cpus(), raw_smp_processor_id());
        return -EINVAL;
    }
    for (cpu = 0; cpu < 2; ++cpu) {
        observed = 99;
        if (smp_call_function_single(cpu, observe_ipi, &observed, 1) || observed != cpu)
            return -EIO;
        init_completion(&workers[cpu].done);
        tasks[cpu] = kthread_create(compute, &workers[cpu], "nce-smp-test%u", cpu);
        if (IS_ERR(tasks[cpu])) {
            if (cpu)
                kthread_stop(tasks[0]);
            return PTR_ERR(tasks[cpu]);
        }
        kthread_bind(tasks[cpu], cpu);
    }
    atomic_set(&arrived, 0);
    atomic_set(&count, 0);
    atomic64_set(&count64, 1ULL << 40);
    futex_count = 0;
    for (cpu = 0; cpu < 2; ++cpu)
        wake_up_process(tasks[cpu]);
    for (cpu = 0; cpu < 2; ++cpu) {
        if (!wait_for_completion_timeout(&workers[cpu].done, 10 * HZ) ||
            workers[cpu].observed_cpu != cpu || workers[cpu].error) {
            lkl_printf("smp compute failed cpu=%u observed=%u error=%d\n",
                       cpu, workers[cpu].observed_cpu, workers[cpu].error);
            return -ETIMEDOUT;
        }
    }
    if (atomic_read(&count) != 200000 || atomic64_read(&count64) != (1ULL << 40) + 200000 ||
        futex_count != 200000 ||
        max(workers[0].start, workers[1].start) >= min(workers[0].end, workers[1].end)) {
        lkl_printf("smp compute counts=%d/%lld/%u times=%llu-%llu/%llu-%llu\n",
                   atomic_read(&count), atomic64_read(&count64), futex_count,
                   workers[0].start, workers[0].end, workers[1].start, workers[1].end);
        return -EUCLEAN;
    }
    lkl_printf("{\"test\":\"linux_smp_parallel_kernel\",\"count\":%d,\"overlap_ns\":%llu,\"passed\":true}\n",
               atomic_read(&count), min(workers[0].end, workers[1].end) - max(workers[0].start, workers[1].start));
    u32 observed_value;
    int old;
    if (futex_atomic_cmpxchg_inatomic(&observed_value, &futex_count, 200000, 9) || observed_value != 200000 ||
        futex_atomic_cmpxchg_inatomic(&observed_value, &futex_count, 200000, 1) || observed_value != 9 ||
        futex_atomic_cmpxchg_inatomic(&observed_value, NULL, 0, 0) != -EFAULT ||
        arch_futex_atomic_op_inuser(FUTEX_OP_OR, 2, &old, &futex_count) || old != 9 ||
        arch_futex_atomic_op_inuser(FUTEX_OP_ANDN, 8, &old, &futex_count) || old != 11 ||
        arch_futex_atomic_op_inuser(FUTEX_OP_XOR, 7, &old, &futex_count) || old != 3 ||
        arch_futex_atomic_op_inuser(FUTEX_OP_SET, 0, &old, &futex_count) || old != 4 ||
        arch_futex_atomic_op_inuser(99, 0, &old, &futex_count) != -ENOSYS || futex_count)
        return -EIO;
    lkl_printf("{\"test\":\"linux_smp_atomic64_futex\",\"passed\":true}\n");
    reinit_completion(&workers[0].done);
    tasks[0] = kthread_create(migrate, &workers[0], "nce-smp-migrate");
    if (IS_ERR(tasks[0]))
        return PTR_ERR(tasks[0]);
    kthread_bind(tasks[0], 0);
    wake_up_process(tasks[0]);
    if (!wait_for_completion_timeout(&workers[0].done, 10 * HZ) || workers[0].observed_cpu != 0)
        return -EIO;
    lkl_printf("{\"test\":\"linux_smp_ipi_timer_migration\",\"passed\":true}\n");
    return 0;
}

/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <asm/futex.h>
#include <asm/host_ops.h>
#include <asm/ptrace.h>
#include <asm/nce-window.h>

static struct mm_struct *test_mm;
static unsigned long counter_address, race_address;
static atomic_t arrived, racing, reader_ready, finished, generation, observed;
static struct mmu_worker {
    struct completion done;
    unsigned index, copies, faults, changes;
    int error;
} workers[2];

static int exercise_memory(void *argument)
{
    struct mmu_worker *worker = argument;
    struct pt_regs registers = {0};
    kthread_use_mm(test_mm);
    current->thread.nce_state = &registers;
    atomic_inc(&arrived);
    while (atomic_read(&arrived) != 2)
        cpu_relax();
    for (unsigned i = 0; i < 20000; ++i) {
        int previous;
        if (arch_futex_atomic_op_inuser(FUTEX_OP_ADD, 1, &previous, (void *)counter_address)) {
            worker->error = 1;
            break;
        }
        if (!(i & 255))
            cond_resched();
    }
    atomic_inc(&racing);
    while (atomic_read(&racing) != 2)
        cond_resched();
    unsigned char buffer[64];
    if (!worker->index) {
        int previous = -1;
        atomic_set(&reader_ready, 1);
        do {
            unsigned long missing = nce_copy_user(buffer, race_address, sizeof(buffer), 0);
            if (missing) {
                ++worker->faults;
            } else {
                ++worker->copies;
                for (unsigned j = 0; j < sizeof(buffer); ++j)
                    if (buffer[j] != buffer[0] || (buffer[j] != 0 && buffer[j] != 0x5a))
                        worker->error = 2;
            }
            int current_generation = atomic_read(&generation);
            if (current_generation != previous) {
                ++worker->changes;
                previous = current_generation;
            }
            atomic_set(&observed, current_generation);
            cond_resched();
        } while (!atomic_read(&finished));
    } else {
        while (!atomic_read(&reader_ready))
            cond_resched();
        for (unsigned i = 0; i < 1000 && !worker->error; ++i) {
            if (vm_munmap(race_address, PAGE_SIZE) ||
                vm_mmap(NULL, race_address, PAGE_SIZE, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0) != race_address) {
                worker->error = 3;
                break;
            }
            memset(buffer, 0x5a, sizeof(buffer));
            if (nce_copy_user(buffer, race_address, sizeof(buffer), 1))
                worker->error = 4;
            int current_generation = atomic_inc_return(&generation);
            /* Periodic handshakes ensure that the reader spans the remapping
             * loop even when the host schedules one CPU much more often. */
            if (!(current_generation % 128))
                while (atomic_read(&observed) < current_generation)
                    cond_resched();
            cond_resched();
        }
        atomic_set(&finished, 1);
    }
    current->thread.nce_state = NULL;
    kthread_unuse_mm(test_mm);
    complete(&worker->done);
    return 0;
}

long lkl_smp_mmu_test(void)
{
    extern long lkl_smp_test(void);
    long error = lkl_smp_test();
    if (error)
        return error;
    if (current->thread.nce_state)
        return -EINVAL;
    bool borrowed = current->mm != NULL;
    test_mm = borrowed ? current->mm : mm_alloc();
    if (!test_mm)
        return -ENOMEM;
    if (borrowed)
        mmget(test_mm);
    else
        kthread_use_mm(test_mm);
    struct pt_regs registers = {0};
    current->thread.nce_state = &registers;
    counter_address = vm_mmap(NULL, 0, 3 * PAGE_SIZE, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, 0);
    BUG_ON(IS_ERR_VALUE(counter_address));
    race_address = counter_address + 2 * PAGE_SIZE;
    u32 count = 0;
    BUG_ON(nce_copy_user(&count, counter_address, sizeof(count), 1));
    u32 previous;
    BUG_ON(futex_atomic_cmpxchg_inatomic(&previous, (void *)counter_address, 0, 17));
    BUG_ON(previous != 0);
    BUG_ON(futex_atomic_cmpxchg_inatomic(&previous, (void *)counter_address, 0, 23));
    BUG_ON(previous != 17);
    BUG_ON(futex_atomic_cmpxchg_inatomic(&previous, (void *)counter_address, 17, 0));
    BUG_ON(previous != 17);
    unsigned char source[64], destination[64];
    memset(source, 0x5a, sizeof(source));
    unsigned long boundary = counter_address + PAGE_SIZE - sizeof(source) / 2;
    BUG_ON(nce_copy_user(source, boundary, sizeof(source), 1));
    BUG_ON(nce_copy_user(destination, boundary, sizeof(destination), 0));
    BUG_ON(memcmp(source, destination, sizeof(source)));
    BUG_ON(sys_mprotect(counter_address + PAGE_SIZE, PAGE_SIZE, PROT_NONE));
    pagefault_disable();
    BUG_ON(nce_copy_user(destination, boundary, sizeof(destination), 0) != sizeof(destination) / 2);
    BUG_ON(arch_futex_atomic_op_inuser(FUTEX_OP_ADD, 1, (int *)&count,
                                       (void *)(counter_address + PAGE_SIZE)) != -EFAULT);
    pagefault_enable();
    BUG_ON(sys_mprotect(counter_address + PAGE_SIZE, PAGE_SIZE, PROT_READ | PROT_WRITE));
    BUG_ON(sys_mprotect(counter_address, PAGE_SIZE, PROT_READ));
    BUG_ON(nce_copy_user(&count, counter_address, sizeof(count), 0));
    BUG_ON(nce_copy_user(&count, counter_address, sizeof(count), 1) != sizeof(count));
    BUG_ON(futex_atomic_cmpxchg_inatomic(&previous, (void *)counter_address, 0, 1) != -EFAULT);
    BUG_ON(sys_mprotect(counter_address, PAGE_SIZE, PROT_READ | PROT_WRITE));
    lkl_printf("{\"test\":\"linux_smp_mmu_access_boundaries\",\"passed\":true}\n");

    atomic_set(&arrived, 0);
    atomic_set(&racing, 0);
    atomic_set(&reader_ready, 0);
    atomic_set(&finished, 0);
    atomic_set(&generation, 0);
    atomic_set(&observed, 0);
    struct task_struct *tasks[2];
    for (unsigned i = 0; i < 2; ++i) {
        workers[i] = (struct mmu_worker){.index = i};
        init_completion(&workers[i].done);
        tasks[i] = kthread_create(exercise_memory, &workers[i], "nce-mmu-test%u", i);
        BUG_ON(IS_ERR(tasks[i]));
        kthread_bind(tasks[i], i);
    }
    /* A task waiting at the barrier cannot occupy kthreadd's CPU while its
     * peer still needs to be created. */
    for (unsigned i = 0; i < 2; ++i)
        wake_up_process(tasks[i]);
    for (unsigned i = 0; i < 2; ++i) {
        BUG_ON(!wait_for_completion_timeout(&workers[i].done, 30 * HZ));
        BUG_ON(workers[i].error);
    }
    BUG_ON(nce_copy_user(&count, counter_address, sizeof(count), 0));
    BUG_ON(count != 40000 || !workers[0].copies || workers[0].changes < 7);
    lkl_printf(
        "{\"test\":\"linux_smp_mmu_atomic_and_unmap\",\"passed\":true,"
        "\"count\":%u,\"copies\":%u,\"faults\":%u,\"generations\":%u}\n",
        count, workers[0].copies, workers[0].faults, workers[0].changes);
    nce_smp_window_test(test_mm, counter_address);
    BUG_ON(vm_munmap(counter_address, 3 * PAGE_SIZE));
    current->thread.nce_state = NULL;
    if (!borrowed)
        kthread_unuse_mm(test_mm);
    mmput(test_mm);
    rcu_barrier();
    return 0;
}

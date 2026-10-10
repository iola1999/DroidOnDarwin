/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/completion.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <asm/host_ops.h>
#include <asm/nce-layout.h>
#include <asm/nce-window.h>
#include <asm/ptrace.h>

extern int nce_page_fault(unsigned long address, int access);

static unsigned long test_address;
static atomic_t ready, computing;
static bool shared;
static struct window_worker {
    struct mm_struct *mm;
    struct completion done;
    unsigned index, reads, stops;
    u64 begin, end;
} workers[2];

static int exercise_window(void *argument)
{
    struct window_worker *worker = argument;
    struct pt_regs registers = {0};
    u64 expected = 0x123456789abcdef0ULL + worker->index;
    kthread_use_mm(worker->mm);
    current->thread.nce_state = &registers;
    if (!shared) {
        BUG_ON(vm_mmap(NULL, test_address, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0) != test_address);
        BUG_ON(nce_copy_user(&expected, test_address, sizeof(expected), 1));
    }
    BUG_ON(nce_page_fault(test_address, 1 | NCE_FAULT_DIRECT));
    atomic_inc(&ready);
    while (atomic_read(&ready) != 2)
        cond_resched();
    if (shared) {
        nce_begin_user();
        atomic_inc(&computing);
        while (atomic_read(&computing) != 2)
            barrier();
        worker->begin = lkl_ops->time();
        for (unsigned i = 0; i < 100000; ++i)
            __atomic_fetch_add((u64 *)test_address, 1, __ATOMIC_RELAXED);
        worker->end = lkl_ops->time();
        nce_window_leave();
    } else {
        for (unsigned iteration = 0; iteration < 1000; ++iteration) {
            nce_begin_user();
            for (unsigned i = 0; i < 1024; ++i) {
                BUG_ON(READ_ONCE(*(u64 *)test_address) != expected);
                ++worker->reads;
                if (nce_window_requested()) {
                    ++worker->stops;
                    break;
                }
            }
            nce_window_leave();
            cond_resched();
        }
        BUG_ON(vm_munmap(test_address, PAGE_SIZE));
    }
    current->thread.nce_state = NULL;
    kthread_unuse_mm(worker->mm);
    complete(&worker->done);
    return 0;
}

void nce_smp_window_test(struct mm_struct *mm, unsigned long address)
{
    u64 token = nce_window_token();
    nce_window_lock();
    nce_window_unlock();
    BUG_ON(nce_window_try_enter(token));
    test_address = address;
    u64 count = 0;
    BUG_ON(nce_copy_user(&count, address, sizeof(count), 1));
    for (unsigned round = 0; round < 2; ++round) {
        shared = round == 0;
        atomic_set(&ready, 0);
        atomic_set(&computing, 0);
        struct task_struct *tasks[2];
        for (unsigned i = 0; i < 2; ++i) {
            workers[i] = (struct window_worker){.mm = shared ? mm : mm_alloc(), .index = i};
            BUG_ON(!workers[i].mm);
            init_completion(&workers[i].done);
            tasks[i] = kthread_create(exercise_window, &workers[i], "nce-window%u", i);
            BUG_ON(IS_ERR(tasks[i]));
            kthread_bind(tasks[i], i);
        }
        for (unsigned i = 0; i < 2; ++i)
            wake_up_process(tasks[i]);
        for (unsigned i = 0; i < 2; ++i)
            BUG_ON(!wait_for_completion_timeout(&workers[i].done, 30 * HZ));
        if (shared) {
            BUG_ON(nce_copy_user(&count, address, sizeof(count), 0));
            u64 begin = max(workers[0].begin, workers[1].begin);
            u64 end = min(workers[0].end, workers[1].end);
            BUG_ON(count != 200000 || end <= begin);
            lkl_printf(
                "{\"test\":\"linux_smp_shared_window\",\"passed\":true,"
                "\"count\":%llu,\"overlap_ns\":%llu}\n",
                count, end - begin);
        } else {
            BUG_ON(!workers[0].reads || !workers[1].reads || !(workers[0].stops + workers[1].stops));
            lkl_printf(
                "{\"test\":\"linux_smp_distinct_windows\",\"passed\":true,"
                "\"reads\":%u,\"stops\":%u}\n",
                workers[0].reads + workers[1].reads, workers[0].stops + workers[1].stops);
            for (unsigned i = 0; i < 2; ++i)
                mmput(workers[i].mm);
        }
    }
}

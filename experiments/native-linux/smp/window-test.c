/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/completion.h>
#include <linux/irqflags.h>
#include <linux/kthread.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <asm/host_ops.h>
#include <asm/nce-layout.h>
#include <asm/nce-window.h>
#include <asm/ptrace.h>

extern int nce_page_fault(unsigned long address, int access);

static unsigned long test_address;
static atomic_t ready, computing;
static atomic_t mapped;
static atomic_t guard_held, guard_checked;
static bool shared, mapping, restoring, library;
static unsigned long mapping_address;
static struct window_worker {
    struct mm_struct *mm;
    struct completion done;
    unsigned index, reads, stops;
    u64 begin, end;
    const char *phase;
} workers[2];

static void wait_guard(atomic_t *flag, int expected)
{
    u64 deadline = lkl_ops->time() + 1000000000;
    while (atomic_read_acquire(flag) != expected) {
        if (lkl_ops->time() >= deadline) {
            lkl_printf("guard timeout phases=%s/%s held=%d checked=%d window=%llx\n",
                       READ_ONCE(workers[0].phase), READ_ONCE(workers[1].phase),
                       atomic_read(&guard_held), atomic_read(&guard_checked), nce_window_token());
            BUG();
        }
        /* This wait models guest instructions. LKL cpu_relax dispatches
         * Linux IRQs, which must wait until the guest slot is released. */
        asm volatile("yield" ::: "memory");
    }
}

static int exercise_window(void *argument)
{
    struct window_worker *worker = argument;
    struct pt_regs registers = {0};
    u64 expected = 0x123456789abcdef0ULL + worker->index;
    WRITE_ONCE(worker->phase, "setup");
    kthread_use_mm(worker->mm);
    current->thread.nce_state = &registers;
    if (!shared) {
        BUG_ON(vm_mmap(NULL, test_address, PAGE_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, 0) != test_address);
        BUG_ON(nce_copy_user(&expected, test_address, sizeof(expected), 1));
    }
    BUG_ON(nce_page_fault(test_address, 1 | NCE_FAULT_DIRECT));
    atomic_inc(&ready);
    WRITE_ONCE(worker->phase, "peer-ready");
    while (atomic_read(&ready) != 2)
        cond_resched();
    if (shared) {
        BUG_ON(nce_window_try_lock_guest());
        WRITE_ONCE(worker->phase, "entry");
        nce_begin_user();
        atomic_inc(&computing);
        WRITE_ONCE(worker->phase, "peer-entry");
        while (atomic_read(&computing) != 2) {
            /* These kernel workers have no NCE signal handler. Cooperate
             * with a pending group switch while the peer reaches entry. */
            /* Once both CPUs have arrived, neither may withdraw before the
             * lock-contention check. A peer can arrive during this decision. */
            if ((need_resched() || nce_window_requested()) &&
                atomic_cmpxchg(&computing, 1, 0) == 1) {
                nce_window_leave();
                cond_resched();
                nce_begin_user();
                atomic_inc(&computing);
            }
            if (atomic_read(&computing) != 2)
                cpu_relax();
        }
        if (restoring) {
            unsigned long flags = arch_local_save_flags();
            if (worker->index) {
                WRITE_ONCE(worker->phase, "guard-acquire");
                BUG_ON(!nce_window_try_lock_guest());
                atomic_set_release(&guard_held, 1);
                WRITE_ONCE(worker->phase, "guard-check");
                wait_guard(&guard_checked, 1);
                nce_window_unlock_guest();
                atomic_set_release(&guard_held, 0);
            } else {
                WRITE_ONCE(worker->phase, "guard-ready");
                wait_guard(&guard_held, 1);
                BUG_ON(nce_window_try_lock_guest());
                atomic_set_release(&guard_checked, 1);
                WRITE_ONCE(worker->phase, "guard-release");
                wait_guard(&guard_held, 0);
            }
            BUG_ON(arch_local_save_flags() != flags);
        }
        worker->begin = lkl_ops->time();
        WRITE_ONCE(worker->phase, "compute");
        if (mapping && !worker->index) {
            if (!restoring)
                nce_window_leave();
            if (restoring) {
                unsigned long readonly = mapping_address + 32 * PAGE_SIZE;
                unsigned long inaccessible = readonly + PAGE_SIZE;
                BUG_ON(nce_page_fault(readonly, 1 | NCE_FAULT_TRY_PRESENT));
                BUG_ON(!nce_page_fault(readonly, NCE_FAULT_TRY_PRESENT));
                BUG_ON(READ_ONCE(*(u64 *)readonly) != 0x5a);
                BUG_ON(nce_page_fault(inaccessible, NCE_FAULT_TRY_PRESENT));
                if (library)
                    BUG_ON(nce_page_fault(inaccessible + PAGE_SIZE, NCE_FAULT_TRY_PRESENT));
            }
            for (unsigned i = 0; i < 32; ++i) {
                unsigned long address = mapping_address + i * PAGE_SIZE;
                if (restoring) {
                    BUG_ON(!nce_page_fault(address, 1 | NCE_FAULT_TRY_PRESENT));
                    BUG_ON(READ_ONCE(*(u64 *)address) != 0x5a);
                    BUG_ON(nce_page_fault(address, 1 | NCE_FAULT_TRY_PRESENT));
                } else {
                    BUG_ON(nce_page_fault(address, 1 | NCE_FAULT_DIRECT));
                }
                WRITE_ONCE(*(u64 *)address, i + 1);
            }
            atomic_set_release(&mapped, 1);
        } else if (mapping) {
            do {
                for (unsigned i = 0; i < 4096; ++i) {
                    BUG_ON(READ_ONCE(*(u64 *)test_address) != 0);
                    ++worker->reads;
                }
            } while (!atomic_read_acquire(&mapped) && lkl_ops->time() - worker->begin < 1000000000);
            worker->stops = !atomic_read_acquire(&mapped);
        } else {
            for (unsigned i = 0; i < 100000; ++i)
                __atomic_fetch_add((u64 *)test_address, 1, __ATOMIC_RELAXED);
        }
        worker->end = lkl_ops->time();
        WRITE_ONCE(worker->phase, "leaving");
        if (!mapping || worker->index || restoring)
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
    WRITE_ONCE(worker->phase, "exit");
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
    for (unsigned round = 0; round < 5; ++round) {
        shared = round != 1;
        mapping = round >= 2;
        restoring = round >= 3;
        library = round == 4;
        unsigned long pages = library ? 35 : restoring ? 34
                                                       : 32;
        if (mapping) {
            count = 0;
            BUG_ON(nce_copy_user(&count, address, sizeof(count), 1));
            mapping_address = vm_mmap(NULL, library ? nce_user_base + PAGE_SIZE : 0,
                                      pages * PAGE_SIZE, PROT_READ | PROT_WRITE,
                                      MAP_PRIVATE | MAP_ANONYMOUS | (library ? MAP_FIXED_NOREPLACE : 0), 0);
            BUG_ON(IS_ERR_VALUE(mapping_address));
            if (restoring) {
                u64 initial = 0x5a;
                for (unsigned i = 0; i < pages; ++i)
                    BUG_ON(nce_copy_user(&initial, mapping_address + i * PAGE_SIZE, sizeof(initial), 1));
                BUG_ON(sys_mprotect(mapping_address + 32 * PAGE_SIZE, PAGE_SIZE, PROT_READ));
                BUG_ON(sys_mprotect(mapping_address + 33 * PAGE_SIZE, PAGE_SIZE, PROT_NONE));
                /* Keep the Linux PTEs, but remove host views to exercise
                 * restoration after the shared address window changes. */
                mmap_pages_for_ptes(mm, mapping_address, pages, __pte(0));
            }
        }
        atomic_set(&ready, 0);
        atomic_set(&computing, 0);
        atomic_set(&mapped, 0);
        atomic_set(&guard_held, 0);
        atomic_set(&guard_checked, 0);
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
        for (unsigned i = 0; i < 2; ++i) {
            if (!wait_for_completion_timeout(&workers[i].done, 30 * HZ)) {
                lkl_printf("window timeout round=%u phases=%s/%s ready=%d computing=%d held=%d checked=%d mapped=%d\n",
                           round, READ_ONCE(workers[0].phase), READ_ONCE(workers[1].phase),
                           atomic_read(&ready), atomic_read(&computing), atomic_read(&guard_held),
                           atomic_read(&guard_checked), atomic_read(&mapped));
                BUG();
            }
        }
        if (mapping) {
            BUG_ON(workers[1].stops || !workers[1].reads);
            for (unsigned i = 0; i < 32; ++i) {
                BUG_ON(nce_copy_user(&count, mapping_address + i * PAGE_SIZE, sizeof(count), 0));
                BUG_ON(count != i + 1);
            }
            BUG_ON(vm_munmap(mapping_address, pages * PAGE_SIZE));
            lkl_printf("{\"test\":\"%s\",\"passed\":true,\"pages\":32,\"reads\":%u}\n",
                       library ? "linux_smp_guest_library_restore" : restoring ? "linux_smp_guest_present_restore"
                                                                               : "linux_smp_data_mapping_without_stop",
                       workers[1].reads);
        } else if (shared) {
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

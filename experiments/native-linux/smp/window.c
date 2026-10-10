/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/atomic.h>
#include <linux/irqflags.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <asm/nce-window.h>
#include <asm/host_ops.h>

#define GUESTS ((1ULL << NR_CPUS) - 1)
#define WRITER (1ULL << NR_CPUS)
#define VERSION (WRITER << 1)

static atomic64_t state = ATOMIC64_INIT(0);
static DEFINE_RAW_SPINLOCK(writer_lock);
static struct {
    unsigned depth;
    unsigned long irq_flags;
} owners[NR_CPUS];

extern void lkl_restore_guest_irq_flags(unsigned long flags);

static void release_writer(void)
{
    BUG_ON(!(atomic64_read(&state) & WRITER));
    /* Preserve simultaneous guest exits while invalidating entry tokens. */
    atomic64_add_return_release(VERSION - WRITER, &state);
    raw_spin_unlock(&writer_lock);
}

void nce_window_lock(void)
{
    unsigned long flags;
    local_irq_save(flags);
    unsigned cpu = raw_smp_processor_id();
    if (owners[cpu].depth++)
        return;
    owners[cpu].irq_flags = flags;
    raw_spin_lock(&writer_lock);
    u64 previous = atomic64_fetch_or(WRITER, &state);
    BUG_ON(previous & (1ULL << cpu));
}

void nce_window_stop(void)
{
    BUG_ON(!owners[raw_smp_processor_id()].depth);
    u64 guests = atomic64_read_acquire(&state) & GUESTS;
    if (guests && lkl_ops->user_interrupt)
        lkl_ops->user_interrupt(guests);
    /* The host's preemption signal makes active guest threads leave without
     * entering Linux. IRQs stay disabled until table walks and copies finish. */
    while (atomic64_read_acquire(&state) & GUESTS)
        cpu_relax();
}

void nce_window_unlock(void)
{
    unsigned cpu = raw_smp_processor_id();
    BUG_ON(!owners[cpu].depth);
    if (--owners[cpu].depth)
        return;
    release_writer();
    local_irq_restore(owners[cpu].irq_flags);
}

bool nce_window_try_lock_guest(void)
{
    unsigned long flags;
    local_irq_save(flags);
    unsigned cpu = raw_smp_processor_id();
    if (owners[cpu].depth || !(atomic64_read(&state) & (1ULL << cpu)) ||
        !raw_spin_trylock(&writer_lock)) {
        lkl_restore_guest_irq_flags(flags);
        return false;
    }
    owners[cpu].depth = 1;
    owners[cpu].irq_flags = flags;
    atomic64_fetch_or(WRITER, &state);
    return true;
}

void nce_window_unlock_guest(void)
{
    unsigned cpu = raw_smp_processor_id();
    BUG_ON(owners[cpu].depth != 1 || !(atomic64_read(&state) & (1ULL << cpu)));
    owners[cpu].depth = 0;
    release_writer();
    /* Restoring through local_irq_restore can enter the scheduler on this
     * Darwin signal stack. The normal return path will deliver pending IRQs. */
    lkl_restore_guest_irq_flags(owners[cpu].irq_flags);
}

u64 nce_window_token(void)
{
    return atomic64_read_acquire(&state);
}

bool nce_window_try_enter(u64 token)
{
    if (token & WRITER)
        return false;
    u64 bit = 1ULL << raw_smp_processor_id();
    return atomic64_cmpxchg_acquire(&state, token, token | bit) == token;
}

bool nce_window_requested(void)
{
    return atomic64_read_acquire(&state) & WRITER;
}

void nce_window_leave(void)
{
    atomic64_fetch_andnot_release(1ULL << raw_smp_processor_id(), &state);
}

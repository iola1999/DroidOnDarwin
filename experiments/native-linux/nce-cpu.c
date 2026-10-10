/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce-cpu.h"
#include <signal.h>
#include <stdlib.h>

static pthread_mutex_t target_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_t targets[NCE_MAX_CPUS];

static void lock_targets(void)
{
    if (pthread_mutex_lock(&target_mutex))
        abort();
}

static void unlock_targets(void)
{
    if (pthread_mutex_unlock(&target_mutex))
        abort();
}

void nce_cpu_target(unsigned cpu, pthread_t thread)
{
    if (cpu >= NCE_MAX_CPUS)
        abort();
    lock_targets();
    targets[cpu] = thread;
    unlock_targets();
}

void nce_cpu_forget(pthread_t thread)
{
    lock_targets();
    for (unsigned cpu = 0; cpu < NCE_MAX_CPUS; ++cpu)
        if (targets[cpu] == thread)
            targets[cpu] = NULL;
    unlock_targets();
}

void nce_cpu_interrupt(unsigned mask, int signal)
{
    lock_targets();
    for (unsigned cpu = 0; cpu < NCE_MAX_CPUS; ++cpu)
        if ((mask & (1u << cpu)) && targets[cpu])
            pthread_kill(targets[cpu], signal);
    unlock_targets();
}

int nce_cpu_resume(NceState *state, NceCode arena, unsigned cpu, unsigned cpus)
{
    if (!cpus || cpus > NCE_MAX_CPUS || cpu >= cpus || !arena.rw || !arena.rx ||
        ((uintptr_t)arena.rw & 3) || ((uintptr_t)arena.rx & 3))
        return -1;
    size_t slots = arena.size / 16 / cpus;
    if (!slots)
        return -1;
    size_t offset = (cpu * slots + (state->regs.pc >> 2) % slots) * 16;
    NceCode entry = {arena.rw + offset, arena.rx + offset, 16};
    uint32_t branch;
    if (nce_write_branch(&branch, (uintptr_t)entry.rx + 4, state->regs.pc))
        return -1;
    int changed = ((const uint32_t *)entry.rw)[1] != branch;
    if (changed) {
        if (nce_write_entry(entry, state->regs.pc))
            return -1;
        nce_sync_code(entry);
    }
    state->resume = (uintptr_t)entry.rx;
    return changed;
}

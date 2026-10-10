/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce-cpu.h"
#include <assert.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

enum { ROUNDS = 4,
       ITERATIONS = 20000 };
typedef struct {
    NceState state;
    unsigned index, cpu, expected;
    atomic_uint signals;
    unsigned rewrites, hits;
    void *stack;
} Worker;
static _Thread_local Worker *self;
static Worker workers[2];
static NceCode code, resume;
static atomic_uint ready, finished;
static atomic_bool go;

static void signal_handler(int signal)
{
    (void)signal;
    if (!self)
        _exit(91);
    atomic_fetch_add_explicit(&self->signals, 1, memory_order_relaxed);
}

static void syscall_from_guest(NceState *state)
{
    assert(state == &self->state);
    assert(state->regs.x[8] == 93 && state->regs.x[0] == self->expected);
    assert(state->regs.x[16] == 0x161616 && state->regs.x[17] == 0x171717);
    state->done = 1;
}

static void *run_worker(void *opaque)
{
    self = opaque;
    nce_cpu_target(self->cpu, pthread_self());
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go))
        sched_yield();
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        /* Only one slot per CPU, with repeated hits and colliding targets.
         * A shared slot lets the peer replace the branch before entry. */
        unsigned target = (i / 4 + self->index) & 1;
        self->expected = target + 1;
        NceState *state = &self->state;
        state->done = 0;
        state->regs.pc = (uintptr_t)code.rx + target * 16;
        int changed = nce_cpu_resume(state, resume, self->cpu, 2);
        assert(changed >= 0);
        self->rewrites += changed;
        self->hits += !changed;
        assert(state->resume == (uintptr_t)resume.rx + self->cpu * 16);
        if (!(i % 64))
            sched_yield();
        nce_enter(state);
    }
    nce_cpu_forget(pthread_self());
    atomic_fetch_add(&finished, 1);
    return NULL;
}

static void boundaries(void)
{
    NceState state = {.resume = 0x1234};
    state.regs.pc = (uintptr_t)code.rx;
    assert(nce_cpu_resume(&state, resume, 0, 0) == -1);
    assert(nce_cpu_resume(&state, resume, 0, 3) == -1);
    assert(nce_cpu_resume(&state, resume, 2, 2) == -1);
    assert(nce_cpu_resume(&state, (NceCode){resume.rw, resume.rx, 31}, 0, 2) == -1);
    assert(nce_cpu_resume(&state, (NceCode){resume.rw + 1, resume.rx, 32}, 0, 2) == -1);
    assert(state.resume == 0x1234);
    state.regs.pc = (uintptr_t)code.rx + 1;
    assert(nce_cpu_resume(&state, resume, 0, 2) == -1);
    state.regs.pc = (uintptr_t)code.rx + (1UL << 28);
    assert(nce_cpu_resume(&state, resume, 0, 2) == -1);
    assert(state.resume == 0x1234);
    state.regs.pc = (uintptr_t)code.rx;
    assert(nce_cpu_resume(&state, resume, 0, 1) == 1);
    assert(nce_cpu_resume(&state, resume, 0, 1) == 0);
    puts("{\"test\":\"nce_cpu_resume_boundaries\",\"passed\":true}");
}

static void run(void *unused)
{
    (void)unused;
    const size_t size = 16384;
    void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(rw != MAP_FAILED);
    vm_address_t rx = 0;
    vm_prot_t current, maximum;
    assert(vm_remap(mach_task_self(), &rx, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                    (uintptr_t)rw, FALSE, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(mprotect((void *)rx, size, PROT_READ | PROT_EXEC) == 0);
    code = (NceCode){rw, (void *)rx, size};
    /* mov x0,#1/2; mov x8,#93; svc #0; nop. */
    const uint32_t guest[] = {0xd2800020, 0xd2800ba8, 0xd4000001, 0xd503201f,
                              0xd2800040, 0xd2800ba8, 0xd4000001, 0xd503201f};
    NceState initial = {0};
    assert(nce_prepare(code, &initial, guest, sizeof(guest), 0) == 0);
    resume = (NceCode){code.rw + 4096, code.rx + 4096, 32};
    boundaries();
    struct sigaction action = {.sa_handler = signal_handler}, old;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, &old) == 0);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        atomic_store(&ready, 0);
        atomic_store(&finished, 0);
        atomic_store(&go, false);
        pthread_t threads[2];
        for (unsigned i = 0; i < 2; ++i) {
            workers[i] = (Worker){.state = initial, .index = i, .cpu = i ^ (round & 1)};
            workers[i].stack = malloc(65536);
            assert(workers[i].stack);
            workers[i].state.syscall = syscall_from_guest;
            workers[i].state.regs.sp = (uintptr_t)workers[i].stack + 65536;
            workers[i].state.regs.x[16] = 0x161616;
            workers[i].state.regs.x[17] = 0x171717;
            assert(pthread_create(&threads[i], NULL, run_worker, &workers[i]) == 0);
        }
        while (atomic_load(&ready) != 2)
            sched_yield();
        nce_cpu_interrupt(SIGUSR1);
        atomic_store(&go, true);
        while (atomic_load(&finished) != 2) {
            nce_cpu_interrupt(SIGUSR1);
            usleep(100);
        }
        for (unsigned i = 0; i < 2; ++i) {
            assert(pthread_join(threads[i], NULL) == 0);
            assert(workers[i].signals && workers[i].hits && workers[i].rewrites);
            assert(workers[i].state.calls == ITERATIONS);
            free(workers[i].stack);
        }
        nce_cpu_interrupt(SIGUSR1);
        printf(
            "{\"test\":\"nce_cpu_shared_resume_parallel\",\"passed\":true,\"round\":%u,"
            "\"iterations_per_thread\":%u,\"cpu\":[%u,%u],\"signals\":[%u,%u]}\n",
            round, ITERATIONS, workers[0].cpu, workers[1].cpu, workers[0].signals, workers[1].signals);
    }
    assert(sigaction(SIGUSR1, &old, NULL) == 0);
    assert(munmap(rw, size) == 0 && munmap((void *)rx, size) == 0);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        dispatch_queue_t queue = dispatch_queue_create("nce-cpu-probe", DISPATCH_QUEUE_SERIAL);
        dispatch_sync_f(queue, NULL, run);
        dispatch_release(queue);
    } else {
        run(NULL);
    }
    puts("{\"result\":\"passed\"}");
    return 0;
}

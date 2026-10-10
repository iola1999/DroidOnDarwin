/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "nce-thread.h"
#include "thread-image.h"
#include <assert.h>
#include <dispatch/dispatch.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

enum { ITERATIONS = 1000000,
       ROUNDS = 4,
       STACK_SIZE = 65536 };
static unsigned state_key;
static atomic_ulong progress[2];
_Static_assert(sizeof(progress[0]) == 8, "guest progress counter layout");
static atomic_int finished;
static _Thread_local NceState *expected_state;
typedef struct {
    NceState state;
    pthread_t thread;
    unsigned index;
    unsigned long token;
    void *stack;
    volatile sig_atomic_t signals;
    uint64_t begin_ns, end_ns, cpu_ns;
} Worker;
static Worker workers[2];

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec value;
    assert(clock_gettime(clock, &value) == 0);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void signal_handler(int signal)
{
    (void)signal;
    uintptr_t base;
    __asm__("mrs %0,tpidrro_el0" : "=r"(base));
    NceState *state = ((NceState **)base)[state_key];
    if (state) {
        if (state != expected_state)
            _exit(90);
        Worker *worker = state->opaque;
        if (worker->index != 0)
            _exit(91);
        ++worker->signals;
    }
}

static void syscall_from_guest(NceState *state)
{
    Worker *worker = state->opaque;
    assert(state == expected_state && state == &worker->state);
    assert(pthread_equal(pthread_self(), worker->thread));
    assert(state->tls == worker->token);
    assert(state->regs.fpcr == 0x400000 && state->regs.fpsr == 1);
    assert(state->regs.x[19] == (uintptr_t)&progress[worker->index]);
    assert(state->regs.x[20] == (uintptr_t)&progress[worker->index ^ 1]);
    assert(state->regs.x[21] == worker->token && state->regs.x[22] == ITERATIONS);
    assert(state->regs.x[26] == worker->token + 26 && state->regs.x[30] == worker->token + 30);
    for (unsigned reg = 0; reg < 32; ++reg)
        for (unsigned lane = 0; lane < 2; ++lane) {
            uint64_t expected = reg == 31 ? 1 : worker->token + reg * 2 + lane;
            if (!reg)
                expected += state->regs.x[23];
            assert(state->regs.vector[reg][lane] == expected);
        }
    if (state->regs.x[8] == 93) {
        assert(state->regs.x[0] == 0 && state->regs.x[23] == ITERATIONS);
        assert(state->regs.x[18] == worker->token);
        state->done = 1;
    } else {
        assert(state->regs.x[8] == 172 && state->regs.nzcv == 0xa0000000);
        state->regs.x[0] = worker->token;
    }
}

static void *run_worker(void *opaque)
{
    Worker *worker = opaque;
    expected_state = &worker->state;
    stack_t alternate = {.ss_sp = malloc(STACK_SIZE), .ss_size = STACK_SIZE};
    assert(alternate.ss_sp && sigaltstack(&alternate, NULL) == 0);
    worker->begin_ns = clock_ns(CLOCK_MONOTONIC_RAW);
    uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    nce_enter(&worker->state);
    worker->cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
    worker->end_ns = clock_ns(CLOCK_MONOTONIC_RAW);
    assert(pthread_getspecific(state_key) == NULL);
    atomic_fetch_add(&finished, 1);
    alternate.ss_flags = SS_DISABLE;
    assert(sigaltstack(&alternate, NULL) == 0);
    free(alternate.ss_sp);
    return NULL;
}

static void run(void *unused)
{
    (void)unused;
    const size_t size = 65536;
    void *rw = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(rw != MAP_FAILED);
    vm_address_t rx = 0;
    vm_prot_t current, maximum;
    assert(vm_remap(mach_task_self(), &rx, size, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                    (uintptr_t)rw, FALSE, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    assert(mprotect((void *)rx, size, PROT_READ | PROT_EXEC) == 0);
    NceState initial = {0};
    assert(nce_prepare((NceCode){rw, (void *)rx, size}, &initial, thread_image, sizeof(thread_image), 0) == 0);
    assert(nce_thread_key(&state_key) == 0);
    struct sigaction action = {.sa_handler = signal_handler, .sa_flags = SA_ONSTACK};
    struct sigaction old;
    sigemptyset(&action.sa_mask);
    assert(sigaction(SIGUSR1, &action, &old) == 0);
    for (unsigned round = 0; round < ROUNDS; ++round) {
        atomic_store(&finished, 0);
        for (unsigned i = 0; i < 2; ++i) {
            atomic_store(&progress[i], 0);
            Worker *worker = &workers[i];
            *worker = (Worker){.index = i, .token = 0x123400 + round * 0x100 + i * 16, .state = initial};
            worker->stack = malloc(STACK_SIZE);
            assert(worker->stack);
            NceState *state = &worker->state;
            state->regs.sp = (uintptr_t)worker->stack + STACK_SIZE;
            state->syscall = syscall_from_guest;
            state->opaque = worker;
            state->tls = worker->token;
            state->regs.x[0] = (uintptr_t)&progress[i];
            state->regs.x[1] = (uintptr_t)&progress[i ^ 1];
            state->regs.x[2] = worker->token;
            state->regs.x[3] = ITERATIONS;
            state->regs.x[26] = worker->token + 26;
            state->regs.x[30] = worker->token + 30;
            state->regs.fpcr = 0x400000;
            state->regs.fpsr = 1;
            for (unsigned reg = 0; reg < 32; ++reg)
                for (unsigned lane = 0; lane < 2; ++lane)
                    state->regs.vector[reg][lane] = reg == 31 ? 1 : worker->token + reg * 2 + lane;
        }
        for (unsigned i = 0; i < 2; ++i)
            assert(pthread_create(&workers[i].thread, NULL, run_worker, &workers[i]) == 0);
        while (atomic_load(&finished) != 2) {
            /* Both pthread handles remain valid until joined below. */
            if (atomic_load(&progress[0]))
                pthread_kill(workers[0].thread, SIGUSR1);
            usleep(100);
        }
        for (unsigned i = 0; i < 2; ++i) {
            assert(pthread_join(workers[i].thread, NULL) == 0);
            assert(atomic_load(&progress[i]) == ITERATIONS + 1);
            assert(workers[i].state.calls == ITERATIONS / 256 + 1);
            free(workers[i].stack);
        }
        assert(workers[0].signals > 0 && workers[1].signals == 0);
        uint64_t begin = workers[0].begin_ns > workers[1].begin_ns ? workers[0].begin_ns : workers[1].begin_ns;
        uint64_t end = workers[0].end_ns < workers[1].end_ns ? workers[0].end_ns : workers[1].end_ns;
        assert(end > begin);
        uint64_t first = workers[0].begin_ns < workers[1].begin_ns ? workers[0].begin_ns : workers[1].begin_ns;
        uint64_t last = workers[0].end_ns > workers[1].end_ns ? workers[0].end_ns : workers[1].end_ns;
        printf(
            "{\"test\":\"nce_shared_code_parallel_threads\",\"passed\":true,\"round\":%u,"
            "\"iterations_per_thread\":%u,\"overlap_ns\":%llu,\"wall_ns\":%llu,\"thread_cpu_ns\":[%llu,%llu],\"signals\":%d}\n",
            round, ITERATIONS, (unsigned long long)(end - begin), (unsigned long long)(last - first),
            (unsigned long long)workers[0].cpu_ns, (unsigned long long)workers[1].cpu_ns, workers[0].signals);
    }
    assert(sigaction(SIGUSR1, &old, NULL) == 0);
    assert(munmap(rw, size) == 0 && munmap((void *)rx, size) == 0);
}

int main(int argc, char **argv)
{
    (void)argv;
    if (argc > 1) {
        dispatch_queue_t queue = dispatch_queue_create("nce-thread-probe", DISPATCH_QUEUE_SERIAL);
        dispatch_sync_f(queue, NULL, run);
        dispatch_release(queue);
    } else {
        run(NULL);
    }
    puts("{\"result\":\"passed\"}");
    return 0;
}

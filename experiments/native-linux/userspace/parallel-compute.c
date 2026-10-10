/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "parallel-compute:%d\n", __LINE__); \
            exit(1); \
        } \
    } while (0)

enum { ITERATIONS = 64000000,
       UNITS = 2 };
static _Thread_local volatile uint64_t tls_token;
static atomic_int ready, go;

struct compute_worker {
    uint64_t seed, result, cpu_ns;
};

static uint64_t clock_ns(clockid_t clock)
{
    struct timespec value;
    CHECK(clock_gettime(clock, &value) == 0);
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static void calculate(struct compute_worker *worker)
{
    tls_token = worker->seed;
    uint64_t cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t value = worker->seed;
    for (unsigned i = 0; i < ITERATIONS; ++i) {
        value = value * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
        value ^= value >> 33;
        if (!(i & 1048575))
            CHECK(tls_token == worker->seed);
    }
    CHECK(tls_token == worker->seed);
    worker->result = value;
    worker->cpu_ns = clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu;
    CHECK(worker->cpu_ns > 0);
}

static void *compute_thread(void *opaque)
{
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&go))
        sched_yield();
    calculate(opaque);
    return NULL;
}

static void measure(unsigned round, unsigned threads, uint64_t results[UNITS])
{
    struct compute_worker workers[UNITS] = {{.seed = 17}, {.seed = 83}};
    pthread_t ids[UNITS];
    uint64_t begin;
    if (threads == 2) {
        atomic_store(&ready, 0);
        atomic_store(&go, 0);
        for (unsigned i = 0; i < UNITS; ++i)
            CHECK(pthread_create(&ids[i], NULL, compute_thread, &workers[i]) == 0);
        while (atomic_load(&ready) != UNITS)
            usleep(1000);
        /* Creation and the initial rendezvous precede the timed interval.
         * Joining finished workers remains part of the measured wall time. */
        begin = clock_ns(CLOCK_MONOTONIC);
        atomic_store(&go, 1);
        for (unsigned i = 0; i < UNITS; ++i)
            CHECK(pthread_join(ids[i], NULL) == 0);
    } else {
        begin = clock_ns(CLOCK_MONOTONIC);
        for (unsigned i = 0; i < UNITS; ++i)
            calculate(&workers[i]);
    }
    uint64_t elapsed = clock_ns(CLOCK_MONOTONIC) - begin;
    uint64_t cpu = 0;
    for (unsigned i = 0; i < UNITS; ++i) {
        results[i] = workers[i].result;
        cpu += workers[i].cpu_ns;
    }
    printf("{\"bench\":\"linux_user_compute\",\"round\":%u,\"threads\":%u,\"online_cpus\":%ld,\"work_units\":%u,\"iterations_per_unit\":%u,\"wall_ns\":%llu,\"thread_cpu_ns\":%llu}\n",
           round, threads, sysconf(_SC_NPROCESSORS_ONLN), UNITS, ITERATIONS,
           (unsigned long long)elapsed, (unsigned long long)cpu);
}

void parallel_compute_benchmark(void)
{
    for (unsigned round = 0; round < 3; ++round) {
        uint64_t results[2][UNITS];
        unsigned first = round % 2;
        measure(round, first + 1, results[first]);
        measure(round, (first ^ 1) + 1, results[first ^ 1]);
        for (unsigned unit = 0; unit < UNITS; ++unit)
            CHECK(results[0][unit] == results[1][unit]);
    }
    puts("{\"test\":\"linux_user_parallel_compute_integrity\",\"passed\":true,\"rounds\":3}");
}

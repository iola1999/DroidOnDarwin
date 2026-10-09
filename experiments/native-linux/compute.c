/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "memory.h"
#include "guest-image.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define REQUIRE(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "compute failure: %s\n", #c); \
            abort(); \
        } \
    } while (0)
extern uint64_t native_work(uint64_t, uint64_t *, uint64_t);
static uint64_t now(void)
{
    struct timespec t;
    REQUIRE(clock_gettime(CLOCK_MONOTONIC_RAW, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void finish(NceState *s)
{
    REQUIRE(s->regs.x[8] == 93);
    s->done = 1;
}

/* Both paths execute the same arithmetic loop. Only NCE's exit uses a gate;
 * this measures compute overhead, excluding Linux syscalls and graphics. */
void nce_compute_benchmark(void)
{
    NceCode code = nce_memory_take(65536);
    void *stack = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(stack != MAP_FAILED);
    for (unsigned sample = 0; sample < 5; ++sample) {
        uint64_t native_data = 7, guest_data = 7;
        const uint64_t iterations = 4000000;
        NceState state = {.regs.sp = (uintptr_t)stack + 65536, .syscall = finish};
        REQUIRE(nce_prepare(code, &state, guest_image, sizeof(guest_image), GUEST_WORK) == 0);
        state.regs.x[0] = iterations;
        state.regs.x[1] = (uintptr_t)&guest_data;
        uint64_t begin = now();
        uint64_t expected = native_work(iterations, &native_data, 0);
        uint64_t direct_ns = now() - begin;
        begin = now();
        nce_enter(&state);
        uint64_t nce_ns = now() - begin;
        REQUIRE(state.done && state.calls == 1 && state.regs.x[0] == expected && native_data == guest_data);
        printf("{\"bench\":\"prepared_nce_compute\",\"sample\":%u,\"iterations\":%llu,\"direct_ns\":%llu,\"nce_ns\":%llu}\n",
               sample, (unsigned long long)iterations, (unsigned long long)direct_ns, (unsigned long long)nce_ns);
    }
    REQUIRE(munmap(stack, 65536) == 0);
}

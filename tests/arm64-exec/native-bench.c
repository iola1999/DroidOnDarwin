/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <time.h>
#include "benchmarks.h"

static unsigned long ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000UL + ts.tv_nsec;
}

int main(void)
{
    for (int sample = 0; sample < 3; sample++) {
        unsigned long start = ns();
        unsigned long sum = calculate(INTEGER_ITERATIONS);
        unsigned long integer_ns = ns() - start;
        volatile unsigned long value = 0;
        start = ns();
        memory_loop(&value, MEMORY_ITERATIONS);
        unsigned long memory_ns = ns() - start;
        if (sum != 5000000050000000UL || value != MEMORY_ITERATIONS) return 1;
        printf("{\"sample\":%d,\"integer_ns\":%lu,\"memory_ns\":%lu}\n",
               sample, integer_ns, memory_ns);
    }
}

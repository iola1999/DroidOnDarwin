/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

enum { INTEGER_ITERATIONS = 100000000, MEMORY_ITERATIONS = 100000000 };

__attribute__((noinline)) static unsigned long calculate(unsigned long n)
{
    unsigned long result;
    __asm__ volatile("mov %0, #0\n1: add %0, %0, %1\nsubs %1, %1, #1\nb.ne 1b"
                     : "=&r"(result), "+r"(n) : : "cc");
    return result;
}

__attribute__((noinline)) static void memory_loop(volatile unsigned long *p, unsigned long n)
{
    unsigned long value;
    __asm__ volatile("1: ldr %0, [%1]\nadd %0, %0, #1\nstr %0, [%1]\n"
                     "subs %2, %2, #1\nb.ne 1b"
                     : "=&r"(value), "+r"(p), "+r"(n) : : "memory", "cc");
}

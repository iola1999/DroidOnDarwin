/* SPDX-License-Identifier: GPL-2.0-or-later */
typedef unsigned long u64;
typedef long s64;

static s64 call(u64 number, u64 a, u64 b, u64 c, u64 d, u64 e, u64 f)
{
    register u64 x8 __asm__("x8") = number;
    register u64 x0 __asm__("x0") = a;
    register u64 x1 __asm__("x1") = b;
    register u64 x2 __asm__("x2") = c;
    register u64 x3 __asm__("x3") = d;
    register u64 x4 __asm__("x4") = e;
    register u64 x5 __asm__("x5") = f;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2),
                     "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}

static void print(const char *text)
{
    u64 size = 0;
    while (text[size]) size++;
    call(64, 1, (u64)text, size, 0, 0, 0);
}

static void number(u64 value)
{
    char buffer[24];
    u64 end = sizeof(buffer), pos = end;
    do {
        buffer[--pos] = '0' + value % 10;
        value /= 10;
    } while (value);
    call(64, 1, (u64)(buffer + pos), end - pos, 0, 0, 0);
}

__attribute__((noreturn)) static void finish(int success)
{
    print(success ? "HUSK-EL0-TEST PASS\n" : "HUSK-EL0-TEST FAIL\n");
    call(142, 0xfee1dead, 672274793, 0x4321fedc, 0, 0, 0);
    for (;;) call(124, 0, 0, 0, 0, 0, 0);
}

#define CHECK(value) do { if (!(value)) { print("failed line "); number(__LINE__); print("\n"); finish(0); } } while (0)

static u64 ns(void)
{
    s64 ts[2];
    CHECK(call(113, 1, (u64)ts, 0, 0, 0, 0) == 0);
    return ts[0] * 1000000000UL + ts[1];
}

#include "benchmarks.h"

extern u64 husk_vector_loop(u64 n);
extern u64 husk_float_loop(u64 n);
extern void husk_vector_memory(u64 *p, u64 n);
extern u64 husk_vector_checks(u64 n);
extern u64 husk_call_loop(u64 n);
extern u64 husk_indirect_call_loop(u64 n);
extern u64 husk_pauth_loop(u64 n);
extern u64 husk_pauth_invalid(u64 n);
extern u64 husk_atomic_loop(u64 *p, u64 n);

static void vector_benchmarks(void)
{
    const u64 n = 10000000;
    u64 start = ns(), value = husk_vector_loop(n);
    u64 elapsed = ns() - start;
    CHECK(value == n);
    print("BENCH vector_ns="); number(elapsed); print(" value="); number(value); print("\n");
    start = ns();
    value = husk_float_loop(n);
    elapsed = ns() - start;
    CHECK(value == n + 1);
    print("BENCH float_ns="); number(elapsed); print(" value="); number(value); print("\n");
    __attribute__((aligned(16))) u64 values[2] = {0, 3};
    start = ns();
    husk_vector_memory(values, n);
    elapsed = ns() - start;
    CHECK(values[0] == n && values[1] == n + 3);
    print("BENCH vector_memory_ns="); number(elapsed); print(" value="); number(values[0]); print("\n");
    CHECK(husk_vector_checks(n) == 0);
    start = ns();
    value = husk_call_loop(10000000);
    elapsed = ns() - start;
    CHECK(value == 10000000);
    print("BENCH calls_ns="); number(elapsed); print(" value="); number(value); print("\n");
    start = ns();
    value = husk_indirect_call_loop(1000000);
    elapsed = ns() - start;
    CHECK(value == 1000000);
    print("BENCH indirect_calls_ns="); number(elapsed); print(" value="); number(value); print("\n");
    start = ns();
    CHECK(husk_pauth_loop(100000) == 0);
    elapsed = ns() - start;
    print("BENCH pauth_ns="); number(elapsed); print(" count=100000\n");
    // PR_PAC_GET_ENABLED_KEYS distinguishes HINT-as-NOP CPUs from enabled PAC.
    if (call(167, 61, 0, 0, 0, 0, 0) > 0) {
        s64 child = call(220, 17, 0, 0, 0, 0, 0);
        CHECK(child >= 0);
        if (!child) {
            husk_pauth_invalid(10000);
            call(93, 0, 0, 0, 0, 0, 0);
            for (;;) {}
        }
        int status = 0;
        CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child);
        CHECK((status & 127) == 4 || (status & 127) == 11);
        print("PASS PAC authentication failure reaches Linux signal\n");
    } else {
        print("SKIP PAC keys unavailable\n");
    }
}

static void switching_benchmark(void)
{
    int request[2], response[2], status = 0;
    CHECK(call(59, (u64)request, 0, 0, 0, 0, 0) == 0);
    CHECK(call(59, (u64)response, 0, 0, 0, 0, 0) == 0);
    s64 child = call(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0);
    unsigned char token = 0;
    if (!child) {
        for (unsigned i = 0; i < 1000; ++i) {
            CHECK(call(63, request[0], (u64)&token, 1, 0, 0, 0) == 1);
            CHECK(token == (unsigned char)i);
            CHECK(call(64, response[1], (u64)&token, 1, 0, 0, 0) == 1);
        }
        call(93, 0, 0, 0, 0, 0, 0);
        for (;;) {}
    }
    u64 start = ns();
    for (unsigned i = 0; i < 1000; ++i) {
        token = i;
        CHECK(call(64, request[1], (u64)&token, 1, 0, 0, 0) == 1);
        CHECK(call(63, response[0], (u64)&token, 1, 0, 0, 0) == 1);
        CHECK(token == (unsigned char)i);
    }
    u64 elapsed = ns() - start;
    CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child && status == 0);
    print("BENCH switch_ns="); number(elapsed); print(" count=1000\n");
    for (unsigned i = 0; i < 2; ++i) {
        CHECK(call(57, request[i], 0, 0, 0, 0, 0) == 0);
        CHECK(call(57, response[i], 0, 0, 0, 0, 0) == 0);
    }
}

void guest_main(u64 *initial_stack)
{
    u64 *auxv = initial_stack + initial_stack[0] + 2;
    while (*auxv++) {}
    u64 hwcap = 0;
    for (; auxv[0]; auxv += 2) if (auxv[0] == 16) hwcap = auxv[1];
    print("HUSK-EL0-TEST START\n");
    CHECK(call(172, 0, 0, 0, 0, 0, 0) == 1);
    u64 start = ns();
    u64 result = calculate(INTEGER_ITERATIONS);
    u64 elapsed = ns() - start;
    CHECK(result == 5000000050000000UL);
    print("BENCH integer_ns="); number(elapsed); print(" sum="); number(result); print("\n");

    s64 mapping = call(222, 0, 8192, 3, 0x22, (u64)-1, 0);
    CHECK(mapping > 0);
    volatile u64 *p = (void *)mapping;
    p[0] = 0;
    start = ns();
    memory_loop(p, MEMORY_ITERATIONS);
    elapsed = ns() - start;
    CHECK(p[0] == MEMORY_ITERATIONS);
    print("BENCH memory_ns="); number(elapsed); print(" value="); number(p[0]); print("\n");
    volatile u64 *tagged = (void *)((u64)p | 0xb400000000000000UL);
    start = ns();
    memory_loop(tagged, MEMORY_ITERATIONS);
    elapsed = ns() - start;
    CHECK(p[0] == 2 * MEMORY_ITERATIONS && *tagged == p[0]);
    print("BENCH tagged_ns="); number(elapsed); print(" value="); number(p[0]); print("\n");
    print("PASS tagged data address through Linux MMU\n");
    start = ns();
    for (unsigned i = 0; i < 10000; ++i) CHECK(call(172, 0, 0, 0, 0, 0, 0) == 1);
    elapsed = ns() - start;
    print("BENCH syscall_ns="); number(elapsed); print(" count=10000\n");
    switching_benchmark();
    p[0] = 41;
    s64 child = call(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0);
    if (!child) {
        CHECK(p[0] == 41);
        p[0] = 73;
        call(93, p[0], 0, 0, 0, 0, 0);
        for (;;) {}
    }
    int status = 0;
    CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child);
    CHECK(status == (73 << 8) && p[0] == 41);
    print("PASS fork/COW and address spaces\n");

    CHECK(call(226, mapping, 4096, 1, 0, 0, 0) == 0);
    child = call(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0);
    if (!child) {
        *tagged = 0;
        call(93, 1, 0, 0, 0, 0, 0);
        for (;;) {}
    }
    CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child);
    CHECK((status & 127) == 11 && p[0] == 41);
    print("PASS write protection and Linux signal\n");

    CHECK(call(215, mapping + 4096, 4096, 0, 0, 0, 0) == 0);
    child = call(220, 17, 0, 0, 0, 0, 0);
    CHECK(child >= 0);
    if (!child) {
        volatile u64 value = *(volatile u64 *)(mapping + 4092);
        call(93, value, 0, 0, 0, 0, 0);
        for (;;) {}
    }
    CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child);
    CHECK((status & 127) == 11);
    print("PASS cross-page load fault\n");

    CHECK(call(226, mapping, 4096, 7, 0, 0, 0) == 0);
    volatile unsigned *code = (void *)mapping;
    code[0] = 0xd28000e0; // mov x0,7
    code[1] = 0xd65f03c0; // ret
    __asm__ volatile("dc cvau, %0\ndsb ish\nic ivau, %0\ndsb ish\nisb" : : "r"(mapping) : "memory");
    u64 (*function)(void) = (void *)mapping;
    CHECK(function() == 7);
    code[0] = 0xd2800120; // mov x0,9
    __asm__ volatile("dc cvau, %0\ndsb ish\nic ivau, %0\ndsb ish\nisb" : : "r"(mapping) : "memory");
    CHECK(function() == 9);
    print("PASS modified executable page\n");

    code[0] = 0xd503245f; // bti c
    code[1] = 0xd2800160; // mov x0,11
    code[2] = 0xd65f03c0; // ret
    __asm__ volatile("dc cvau, %0\ndsb ish\nic ivau, %0\ndsb ish\nisb" : : "r"(mapping) : "memory");
    // PROT_BTI requires both kernel and CPU support; cortex-a57 lacks BTI.
    if (call(226, mapping, 4096, 0x15, 0, 0, 0) == 0) {
        // Repeated calls admit both the caller and destination to Direct.
        for (unsigned i = 0; i < 10000; ++i) CHECK(function() == 11);
        child = call(220, 17, 0, 0, 0, 0, 0);
        CHECK(child >= 0);
        if (!child) {
            u64 (*invalid_landing)(void) = (void *)(mapping + 4);
            call(93, invalid_landing(), 0, 0, 0, 0, 0);
            for (;;) {}
        }
        CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child);
        CHECK((status & 127) == 4);
        print("PASS BTI landing protection and Linux SIGILL\n");
    } else {
        print("SKIP BTI unavailable\n");
    }
    vector_benchmarks();
    if (hwcap & (1UL << 8)) {
        u64 atomic_value = 0;
        start = ns();
        CHECK(husk_atomic_loop(&atomic_value, 10000000) == 0);
        elapsed = ns() - start;
        CHECK(atomic_value == 20000000);
        print("BENCH atomic_ns="); number(elapsed); print(" value="); number(atomic_value); print("\n");
        CHECK(call(226, mapping, 4096, 1, 0, 0, 0) == 0);
        child = call(220, 17, 0, 0, 0, 0, 0);
        CHECK(child >= 0);
        if (!child) {
            husk_atomic_loop((u64 *)mapping, 10000000);
            call(93, 0, 0, 0, 0, 0, 0);
            for (;;) {}
        }
        CHECK(call(260, child, (u64)&status, 0, 0, 0, 0) == child);
        CHECK((status & 127) == 11);
        print("PASS LSE write permission reaches Linux SIGSEGV\n");
    } else {
        print("SKIP LSE unavailable\n");
    }
    finish(1);
}

__attribute__((naked, noreturn)) void _start(void)
{
    __asm__ volatile("mov x0, sp\nbl guest_main\nb .");
}

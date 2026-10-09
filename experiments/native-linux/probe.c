/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "probe.h"
#include "guest-image.h"
#include <errno.h>
#include <inttypes.h>
#include <mach/mach.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "FAIL %s:%d: %s\n", \
                    __FILE__, __LINE__, #c); \
            abort(); \
        } \
    } while (0)

static size_t page;
static uint64_t now(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC_RAW, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static void *allocate(size_t size, int protection)
{
    void *p = mmap(NULL, size, protection, MAP_PRIVATE | MAP_ANON, -1, 0);
    CHECK(p != MAP_FAILED);
    return p;
}

static void alias(void *destination, void *source, size_t size)
{
    vm_address_t target = (uintptr_t)destination;
    vm_prot_t current, maximum;
    CHECK(vm_remap(mach_task_self(), &target, size, 0,
                   VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
                   (uintptr_t)source, FALSE, &current, &maximum, VM_INHERIT_NONE) == KERN_SUCCESS);
    CHECK(target == (uintptr_t)destination);
}

static void *copy_mapping(void *source, size_t size)
{
    vm_address_t target = 0;
    vm_prot_t current, maximum;
    CHECK(vm_remap(mach_task_self(), &target, size, 0, VM_FLAGS_ANYWHERE,
                   mach_task_self(), (uintptr_t)source, TRUE, &current, &maximum,
                   VM_INHERIT_NONE) == KERN_SUCCESS);
    return (void *)target;
}

static void probe_syscall(NceState *s)
{
    CHECK((s->instruction & 0x001fffe0) == 0);
    if (s->regs.x[8] == 93)
        s->done = 1;
    else {
        CHECK(s->regs.x[8] == 172);
        s->regs.x[0] = 11;
    }
}

static void prepare(NceCode code, NceState *s, size_t entry, void *stack)
{
    memset(s, 0, sizeof(*s));
    s->regs.sp = (uintptr_t)stack;
    s->syscall = probe_syscall;
    CHECK(nce_prepare(code, s, guest_image, sizeof(guest_image), entry) == 0);
}

static int operand_memory(NceState *state, uintptr_t address, void *buffer, size_t size, int write)
{
    (void)state;
    if (write)
        memcpy((void *)address, buffer, size);
    else
        memcpy(buffer, (void *)address, size);
    return 0;
}

static uint64_t pattern(unsigned index, unsigned stage)
{
    return UINT64_C(0x9876543200000000) + stage * 1000 + index;
}

static void register_syscall(NceState *s)
{
    unsigned stage = (s->instruction >> 5) & 0xffff;
    CHECK(stage == 1 || stage == 2);
    for (unsigned r = 0; r < 31; ++r)
        CHECK(s->regs.x[r] == pattern(r, stage - 1));
    for (unsigned r = 0; r < 64; ++r)
        CHECK(s->regs.vector[r / 2][r % 2] == pattern(r + 31, stage - 1));
    CHECK(s->regs.nzcv == (stage == 1 ? 0xa0000000u : 0x60000000u));
    CHECK(s->regs.fpcr == (stage == 1 ? 0x400000u : 0x800000u));
    CHECK(s->regs.fpsr == (stage == 1 ? 1u : 2u));
    if (stage == 2) {
        s->done = 1;
        return;
    }
    for (unsigned r = 0; r < 31; ++r)
        s->regs.x[r] = pattern(r, 1);
    for (unsigned r = 0; r < 64; ++r)
        s->regs.vector[r / 2][r % 2] = pattern(r + 31, 1);
    s->regs.nzcv = 0x60000000u;
    s->regs.fpcr = 0x800000u;
    s->regs.fpsr = 2;
}

static sigjmp_buf fault_return;
static volatile sig_atomic_t fault_armed, fault_seen;
static uintptr_t fault_address;
static void fault_handler(int signal_number, siginfo_t *info, void *context)
{
    (void)context;
    if (!fault_armed)
        _exit(128 + signal_number);
    fault_seen = signal_number;
    fault_address = (uintptr_t)info->si_addr;
    siglongjmp(fault_return, 1);
}

static void expect_fault(NceState *s, uintptr_t address)
{
    fault_seen = 0;
    fault_armed = 1;
    if (sigsetjmp(fault_return, 1) == 0)
        nce_enter(s);
    fault_armed = 0;
    CHECK(fault_seen == SIGBUS || fault_seen == SIGSEGV);
    CHECK(fault_address == address);
}

static void correctness(NceCode code, void *stack)
{
    NceState s;
    prepare(code, &s, GUEST_REGISTERS, stack);
    s.syscall = register_syscall;
    for (unsigned r = 0; r < 31; ++r)
        s.regs.x[r] = pattern(r, 0);
    for (unsigned r = 0; r < 64; ++r)
        s.regs.vector[r / 2][r % 2] = pattern(r + 31, 0);
    s.regs.nzcv = 0xa0000000u;
    s.regs.fpcr = 0x400000u;
    s.regs.fpsr = 1;
    nce_enter(&s);
    CHECK(s.calls == 2 && s.regs.sp == (uintptr_t)stack);
    puts("{\"test\":\"registers_simd_flags\",\"passed\":true}");

    prepare(code, &s, GUEST_TLS, stack);
    s.tls = 0x123;
    nce_enter(&s);
    CHECK(s.regs.x[5] == 0x123 && s.regs.x[6] == 0x678 && s.tls_accesses == 3);
    puts("{\"test\":\"tls_read_write\",\"passed\":true}");

    prepare(code, &s, GUEST_X18, stack);
    s.regs.x[18] = UINT64_C(0x123456789abcdef0);
    s.regs.x[2] = UINT64_C(0xfedcba9876543210);
    s.regs.x[3] = UINT64_C(0x9876543287654321);
    nce_enter(&s);
    CHECK(s.regs.x[9] == UINT64_C(0x123456789abcdef0) && s.regs.x[10] == 0x9abcdef0u);
    CHECK(s.regs.x[11] == s.regs.x[2] && s.regs.x[12] == 0x87654321u);
    CHECK(s.regs.x[13] == 0 && s.regs.x[18] == 0 && s.calls == 2);
    puts("{\"test\":\"guest_x18_moves_and_syscalls\",\"passed\":true}");
    uint64_t operands[20] = {0};
    prepare(code, &s, GUEST_X18_MEMORY, stack);
    s.memory = operand_memory;
    s.regs.x[0] = (uintptr_t)operands;
    s.regs.x[18] = UINT64_C(0x123456789abcdef0);
    s.regs.x[19] = UINT64_C(0xfedcba9876543210);
    nce_enter(&s);
    CHECK(operands[18] == UINT64_C(0x123456789abcdef0) && operands[19] == UINT64_C(0xfedcba9876543210));
    CHECK(s.regs.x[18] == operands[18] && s.regs.x[19] == operands[19]);
    CHECK(s.regs.x[4] == operands[18] && s.regs.x[5] == (operands[18] & 0x3fff));
    puts("{\"test\":\"guest_x18_pair_and_logical\",\"passed\":true}");

    prepare(code, &s, GUEST_UPDATE, stack);
    uint64_t entry = s.resume;
    nce_enter(&s);
    CHECK(s.regs.x[0] == 41);
    uint32_t replacement = 0xd2800540; /* mov x0, #42 */
    memcpy(code.rw + GUEST_UPDATE, &replacement, 4);
    nce_sync_code(code);
    s.resume = entry;
    s.done = 0;
    nce_enter(&s);
    CHECK(s.regs.x[0] == 42);
    puts("{\"test\":\"dual_alias_code_update\",\"passed\":true}");

    void *memory = allocate(page * 2, PROT_READ | PROT_WRITE);
    struct sigaction action = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO};
    struct sigaction old_bus, old_segv;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGBUS, &action, &old_bus) == 0);
    CHECK(sigaction(SIGSEGV, &action, &old_segv) == 0);
    CHECK(mprotect(memory, page, PROT_READ) == 0);
    prepare(code, &s, GUEST_FAULT_WRITE, stack);
    s.regs.x[0] = (uintptr_t)memory;
    s.regs.x[1] = 3;
    expect_fault(&s, s.regs.x[0]);
    CHECK(mprotect((char *)memory + page, page, PROT_NONE) == 0);
    prepare(code, &s, GUEST_FAULT_READ, stack);
    s.regs.x[0] = (uintptr_t)memory + page;
    expect_fault(&s, s.regs.x[0]);
    CHECK(sigaction(SIGBUS, &old_bus, NULL) == 0);
    CHECK(sigaction(SIGSEGV, &old_segv, NULL) == 0);
    puts("{\"test\":\"protection_faults\",\"passed\":true}");
    errno = 0;
    int result = mprotect((char *)memory + 4096, 4096, PROT_NONE);
    printf("{\"test\":\"4k_subpage_protect\",\"host_page\":%zu,\"return\":%d,\"errno\":%d}\n",
           page, result, errno);
    CHECK(munmap(memory, page * 2) == 0);
}

typedef struct {
    NceState saved[2];
    unsigned current, completed;
    void *window, *private[2];
    size_t size;
    uint64_t expected[2], switches;
} ProcessPair;

static void process_syscall(NceState *s)
{
    ProcessPair *p = s->opaque;
    unsigned id = p->current;
    unsigned immediate = (s->instruction >> 5) & 0xffff;
    if (immediate == 3) {
        CHECK(s->regs.x[5] == ++p->expected[id]);
        CHECK(s->regs.x[4] == 100 + id);
    } else {
        CHECK(immediate == 0 && s->regs.x[8] == 93 && s->regs.x[0] == 0);
        s->done = 1;
        ++p->completed;
    }
    p->saved[id] = *s;
    if (p->completed == 2)
        return;
    unsigned next = id ^ 1;
    if (p->saved[next].done)
        return;
    alias(p->window, p->private[next], p->size);
    uint64_t host_sp = s->host_sp, host_x18 = s->host_x18;
    *s = p->saved[next];
    s->host_sp = host_sp;
    s->host_x18 = host_x18;
    p->current = next;
    ++p->switches;
}

static void processes(NceCode code)
{
    const unsigned iterations = 2000;
    ProcessPair p = {.size = page * 4};
    p.window = allocate(p.size + page, PROT_NONE);
    p.private[0] = allocate(p.size, PROT_READ | PROT_WRITE);
    *(uint64_t *)p.private[0] = 500;
    p.private[1] = copy_mapping(p.private[0], p.size);
    *(uint64_t *)p.private[1] = 900;
    CHECK(*(uint64_t *)p.private[0] == 500);
    void *shared = allocate(page, PROT_READ | PROT_WRITE);
    alias((char *)p.window + p.size, shared, page);
    alias(p.window, p.private[0], p.size);
    NceState state;
    prepare(code, &state, GUEST_PROCESS, (char *)p.window + p.size);
    state.syscall = process_syscall;
    state.opaque = &p;
    state.regs.x[0] = iterations;
    state.regs.x[1] = (uintptr_t)p.window;
    state.regs.x[2] = (uintptr_t)p.window + p.size;
    p.saved[0] = p.saved[1] = state;
    p.saved[0].tls = state.tls = 100;
    p.saved[1].tls = 101;
    p.expected[0] = 500;
    p.expected[1] = 900;
    uint64_t start = now();
    nce_enter(&state);
    uint64_t elapsed = now() - start;
    CHECK(p.completed == 2);
    CHECK(*(uint64_t *)p.private[0] == 500 + iterations);
    CHECK(*(uint64_t *)p.private[1] == 900 + iterations);
    CHECK(*(uint64_t *)shared == iterations * 2);
    printf(
        "{\"test\":\"two_address_spaces_cow_shared_tls_stack\",\"passed\":true,"
        "\"switches\":%" PRIu64 ",\"ns_per_switch_including_guest\":%.2f}\n",
        p.switches, (double)elapsed / p.switches);
    CHECK(munmap(p.window, p.size + page) == 0);
    CHECK(munmap(p.private[0], p.size) == 0);
    CHECK(munmap(p.private[1], p.size) == 0);
    CHECK(munmap(shared, page) == 0);
}

extern uint64_t native_work(uint64_t, void *, uint64_t);

static void benchmarks(NceCode code, void *stack)
{
    const uint64_t iterations = 4000000;
    const unsigned intervals[] = {0, 4096, 256, 16, 1};
    for (unsigned k = 0; k < sizeof(intervals) / sizeof(intervals[0]); ++k) {
        NceState s;
        prepare(code, &s, GUEST_WORK, stack);
        uint64_t resume = s.resume;
        for (unsigned run = 0; run < 5; ++run) {
            uint64_t native_value = 7, guest_value = 7, native_ns, nce_ns;
            uint64_t native_result = 0, start;
            s.regs.x[0] = iterations;
            s.regs.x[1] = (uintptr_t)&guest_value;
            s.regs.x[2] = intervals[k];
            s.resume = resume;
            s.done = 0;
            s.calls = 0;
            // Alternate order to reduce bias from frequency and temperature.
            if (run & 1) {
                start = now();
                nce_enter(&s);
                nce_ns = now() - start;
                start = now();
                native_result = native_work(iterations, &native_value, intervals[k]);
                native_ns = now() - start;
            } else {
                start = now();
                native_result = native_work(iterations, &native_value, intervals[k]);
                native_ns = now() - start;
                start = now();
                nce_enter(&s);
                nce_ns = now() - start;
            }
            CHECK(s.regs.x[0] == native_result && native_value == guest_value);
            CHECK(s.calls == (intervals[k] ? iterations / intervals[k] : 0) + 1);
            printf(
                "{\"bench\":\"native_vs_nce\",\"interval\":%u,\"run\":%u,"
                "\"iterations\":%" PRIu64 ",\"native_ns\":%" PRIu64 ",\"nce_ns\":%" PRIu64
                ",\"calls\":%" PRIu64 "}\n",
                intervals[k], run, iterations, native_ns, nce_ns, s.calls);
        }
    }
    const size_t counts[] = {1, 16, 128};
    for (unsigned touch_all = 0; touch_all < 2; ++touch_all) {
        for (unsigned fragmented = 0; fragmented < 2; ++fragmented) {
            for (unsigned k = 0; k < sizeof(counts) / sizeof(counts[0]); ++k) {
                size_t count = counts[k], size = count * page;
                unsigned char *a = allocate(size * 2, PROT_READ | PROT_WRITE);
                unsigned char *window = allocate(size, PROT_NONE);
                for (size_t i = 0; i < count * 2; ++i)
                    *(uint64_t *)(a + i * page) = i;
                const unsigned switches = 1000;
                uint64_t start = now();
                for (unsigned i = 0; i < switches; ++i) {
                    unsigned char *source = a + (i & 1) * size;
                    if (fragmented) {
                        for (size_t p = 0; p < count; ++p)
                            alias(window + p * page, source + (count - p - 1) * page, page);
                    } else
                        alias(window, source, size);
                    CHECK(*(volatile uint64_t *)window == (i & 1) * count + (fragmented ? count - 1 : 0));
                    if (touch_all) {
                        for (size_t p = 0; p < count; ++p)
                            CHECK(*(volatile uint64_t *)(window + p * page) ==
                                  (i & 1) * count + (fragmented ? count - p - 1 : p));
                    }
                }
                uint64_t elapsed = now() - start;
                printf(
                    "{\"bench\":\"remap\",\"touch_all\":%s,\"fragmented\":%s,\"pages\":%zu,\"bytes\":%zu,"
                    "\"switches\":%u,\"ns_per_switch\":%.2f}\n",
                    touch_all ? "true" : "false", fragmented ? "true" : "false",
                    count, size, switches, (double)elapsed / switches);
                CHECK(munmap(window, size) == 0);
                CHECK(munmap(a, size * 2) == 0);
            }
        }
    }
}

int nce_probe_run(NceCode code)
{
    page = (size_t)getpagesize();
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("{\"prototype\":\"N0\",\"host_page\":%zu,\"linux_kernel_integrated\":false}\n", page);
    void *stack = allocate(page * 6, PROT_NONE);
    CHECK(mprotect((char *)stack + page, page * 4, PROT_READ | PROT_WRITE) == 0);
    void *top = (char *)stack + page * 5;
    correctness(code, top);
    processes(code);
    benchmarks(code, top);
    CHECK(munmap(stack, page * 6) == 0);
    puts("{\"result\":\"passed\"}");
    return 0;
}

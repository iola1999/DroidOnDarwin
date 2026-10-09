/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/auxv.h>
#include <asm/hwcap.h>
#include <unistd.h>
#include <ucontext.h>

#define CHECK(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "failed line %d\n", __LINE__); \
            return 1; \
        } \
    } while (0)
static uint64_t ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static uintptr_t fault_pc, fault_address;
static unsigned fault_btype;

static void landing_fault(int signal_number, siginfo_t *info, void *context)
{
    ucontext_t *saved = context;
    _exit(signal_number == SIGSEGV && (uintptr_t)info->si_addr == fault_address &&
                  saved->uc_mcontext.pc == fault_pc &&
                  ((saved->uc_mcontext.pstate >> 10) & 3) == fault_btype
              ? 77
              : 1);
}

static int check_landing(void)
{
    size_t page = sysconf(_SC_PAGESIZE);
    uint32_t *code = mmap(NULL, page, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t *data = mmap(NULL, page, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t *bad = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(code != MAP_FAILED && data != MAP_FAILED && bad != MAP_FAILED);
    for (unsigned position = 0; position < 2; ++position) {
        CHECK(mprotect(code, page, PROT_READ | PROT_WRITE) == 0);
        code[0] = 0x91000129;  // add x9,x9,#0
        code[position] = 0xf9400000;  // ldr x0,[x0]
        code[position + 1] = 0xd65f03c0;  // ret
        __builtin___clear_cache((char *)code, (char *)(code + 3));
        CHECK(mprotect(code, page, PROT_READ | PROT_EXEC) == 0);
        CHECK(mprotect(data, page, PROT_READ | PROT_WRITE) == 0);
        *data = 73;
        uint64_t (*function)(uint64_t *) = (void *)code;
        for (unsigned i = 0; i < 10000; ++i)
            CHECK(function(data) == 73);
        fault_pc = (uintptr_t)(code + position);
        fault_address = (uintptr_t)bad;
        fault_btype = !position && (getauxval(AT_HWCAP2) & HWCAP2_BTI) ? 2 : 0;
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            struct sigaction action = {.sa_sigaction = landing_fault, .sa_flags = SA_SIGINFO};
            sigemptyset(&action.sa_mask);
            if (sigaction(SIGSEGV, &action, NULL))
                _exit(1);
            for (unsigned i = 0; i < 10000; ++i)
                if (function(data) != 73)
                    _exit(1);
            function(bad);
            _exit(1);
        }
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
    }
    munmap(code, page);
    munmap(data, page);
    munmap(bad, page);
    puts("PASS branch landing fault state");
    return 0;
}

int main(void)
{
    int (*counters)(uint64_t *) = dlsym(RTLD_DEFAULT, "husk_bulk_counters");
    uint64_t counts[3] = {0};
    int features = counters ? counters(counts) : 0;
    printf("bulk_enabled=%d mops=%d\n", features,
           (getauxval(AT_HWCAP2) & HWCAP2_MOPS) != 0);
    CHECK(check_landing() == 0);
    const size_t maximum = 1048576, sizes[] = {64, 256, 1024, 4096, 65536, maximum};
    unsigned char *source = mmap(NULL, maximum * 2, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(source != MAP_FAILED);
    unsigned char *destination = source + maximum;
    for (size_t i = 0; i < maximum; ++i)
        source[i] = (i * 31 + i / 4096) & 255;
    uint64_t mixed = 7, mixed_start = ns();
    for (unsigned i = 0; i < 4096; ++i) {
        memcpy(destination, source, 64);
        mixed += destination[i & 63];
        unsigned remaining = 4096;
        __asm__ volatile(
            "1: add %0, %0, #17\n"
            "eor %0, %0, %0, ror #13\n"
            "subs %w1, %w1, #1\n"
            "b.ne 1b\n"
            : "+r"(mixed), "+r"(remaining) : : "cc");
    }
    printf("BENCH mixed_ns=%llu mixed_value=%llu\n",
           (unsigned long long)(ns() - mixed_start), (unsigned long long)mixed);
    size_t (*volatile string_length)(const char *) = strlen;
    memset(destination, 'a', 64);
    destination[63] = 0;
    uint64_t string_start = ns(), string_sum = 0;
    for (unsigned i = 0; i < 262144; ++i)
        string_sum += string_length((char *)destination);
    CHECK(string_sum == 262144UL * 63);
    printf("BENCH strlen_ns=%llu strlen_value=%llu\n",
           (unsigned long long)(ns() - string_start), (unsigned long long)string_sum);
    memset(destination, 0, maximum);
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); ++s) {
        size_t size = sizes[s], iterations = 16777216 / size;
        uint64_t start = ns();
        for (size_t i = 0; i < iterations; ++i)
            CHECK(memcpy(destination, source, size) == destination);
        uint64_t copy = ns() - start;
        CHECK(!memcmp(source, destination, size));
        start = ns();
        for (size_t i = 0; i < iterations; ++i)
            CHECK(memset(destination, i & 255, size) == destination);
        uint64_t set = ns() - start;
        for (size_t i = 0; i < size; ++i)
            CHECK(destination[i] == ((iterations - 1) & 255));
        printf("BENCH size=%zu bytes=%zu copy_ns=%llu set_ns=%llu\n", size, size * iterations,
               (unsigned long long)copy, (unsigned long long)set);
    }
    CHECK(memmove(source + 7, source, 65529) == source + 7);
    for (size_t i = 7; i < 65536; ++i)
        CHECK(source[i] == (((i - 7) * 31 + (i - 7) / 4096) & 255));
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        memset(destination, 0x23, maximum);
        _exit(destination[0] == 0x23 ? 73 : 1);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 73);
    CHECK(destination[0] == 15);
    CHECK(mprotect(destination + maximum - 4096, 4096, PROT_READ) == 0);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        /* This test checks the memory fault without Android's crash service. */
        signal(SIGSEGV, SIG_DFL);
        memset(destination, 0, maximum);
        _exit(1);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    if (counters)
        counters(counts);
    printf("completed=%llu refused=%llu transferred=%llu\nPASS Android libc bulk\n",
           (unsigned long long)counts[0], (unsigned long long)counts[1], (unsigned long long)counts[2]);
    munmap(source, maximum * 2);
    return 0;
}

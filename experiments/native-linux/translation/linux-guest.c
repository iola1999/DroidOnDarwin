/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "translation-guest:%d errno=%d\n", __LINE__, errno); \
            _exit(95); \
        } \
    } while (0)
#define PAGE 16384
static _Thread_local unsigned tls = 7;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned counter;
static atomic_uint ready;
static volatile sig_atomic_t delivered;
static sigjmp_buf fault_jump;

static void handler(int signal)
{
    delivered = signal;
    if (signal == SIGSEGV)
        siglongjmp(fault_jump, 1);
}

static void inaccessible(unsigned long address, int write_access)
{
    delivered = 0;
    if (!sigsetjmp(fault_jump, 1)) {
        if (write_access)
            *(volatile unsigned char *)address = 0x53;
        else
            (void)*(volatile unsigned char *)address;
        CHECK(0);
    }
    CHECK(delivered == SIGSEGV);
}

static void layout_tests(void)
{
    FILE *input = fopen("/probe-layout", "r");
    if (!input) {
        CHECK(errno == ENOENT);
        return;
    }
    char layout[8];
    unsigned long base, end, physical, ram, host_pointer;
    CHECK(fscanf(input, "%7s %lu %lu %lu %lu %lu", layout, &base, &end, &physical, &ram, &host_pointer) == 6);
    CHECK(!fclose(input));
    int low = !strcmp(layout, "low");
    CHECK(low || !strcmp(layout, "host"));
    CHECK(base && end > base && physical >= end);
    CHECK(!low || (base == 0x10000 && end == 0x100000000UL));
    unsigned long stack_address = (unsigned long)&input;
    CHECK((unsigned long)&layout_tests >= base && (unsigned long)&layout_tests < end);
    CHECK(stack_address >= base && stack_address < end && (unsigned long)&tls >= base && (unsigned long)&tls < end);
    struct sigaction action = {.sa_handler = handler}, previous;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGSEGV, &action, &previous));
    int fd = open("/host-pointer", O_RDWR);
    CHECK(fd >= 0);
    const unsigned long forbidden[] = {0, base - PAGE, end, end + 3 * PAGE, physical, ram, host_pointer, UINTPTR_MAX - 7};
    for (unsigned i = 0; i < sizeof(forbidden) / sizeof(*forbidden); ++i) {
        unsigned long address = forbidden[i];
        CHECK(address < base || address >= end);
        inaccessible(address, 0);
        inaccessible(address, 1);
        CHECK(syscall(SYS_write, fd, address, 1) == -1 && errno == EFAULT);
        CHECK(syscall(SYS_pread64, fd, address, 1, 0) == -1 && errno == EFAULT);
        CHECK(syscall(SYS_openat, AT_FDCWD, address, O_RDONLY, 0) == -1 && errno == EFAULT);
    }
    if (low) {
        unsigned char *mapped = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(mapped != MAP_FAILED && (unsigned long)mapped >= base && (unsigned long)mapped <= end - PAGE);
        const char path[] = "/host-pointer";
        memcpy(mapped, path, sizeof(path));
        const unsigned long high_bits[] = {1UL << 32, 1UL << 48};
        for (unsigned i = 0; i < sizeof(high_bits) / sizeof(*high_bits); ++i) {
            unsigned long address = (unsigned long)mapped + high_bits[i];
            CHECK(address >= end && (uint32_t)address == (unsigned long)mapped);
            inaccessible(address, 0);
            inaccessible(address, 1);
            CHECK(!memcmp(mapped, path, sizeof(path)));
            CHECK(syscall(SYS_write, fd, address, 1) == -1 && errno == EFAULT);
            CHECK(syscall(SYS_pread64, fd, address, 1, 0) == -1 && errno == EFAULT);
            CHECK(!memcmp(mapped, path, sizeof(path)));
            CHECK(syscall(SYS_openat, AT_FDCWD, address, O_RDONLY, 0) == -1 && errno == EFAULT);
            CHECK(!memcmp(mapped, path, sizeof(path)));
        }
        CHECK(!munmap(mapped, PAGE));
        void *last = mmap((void *)(end - PAGE), PAGE, 3, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        CHECK(last == (void *)(end - PAGE));
        *(volatile unsigned char *)(end - 1) = 0x4b;
        CHECK(*(volatile unsigned char *)(end - 1) == 0x4b);
        if (!sigsetjmp(fault_jump, 1)) {
            (void)*(volatile unsigned long *)(end - 4);
            CHECK(0);
        }
        CHECK(delivered == SIGSEGV);
        const unsigned long outside[] = {0, PAGE, base - PAGE, end, end + 3 * PAGE};
        const int fixed[] = {MAP_FIXED, MAP_FIXED_NOREPLACE};
        for (unsigned i = 0; i < sizeof(outside) / sizeof(*outside); ++i) {
            for (unsigned j = 0; j < sizeof(fixed) / sizeof(*fixed); ++j) {
                CHECK(mmap((void *)outside[i], PAGE, 3, MAP_PRIVATE | MAP_ANONYMOUS | fixed[j], -1, 0) == MAP_FAILED);
                CHECK(errno == EACCES);
            }
            CHECK(mprotect((void *)outside[i], PAGE, PROT_READ) == -1 && errno == EACCES);
            CHECK(mremap(last, PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, (void *)outside[i]) == MAP_FAILED);
            CHECK(errno == EACCES);
        }
        CHECK(mmap((void *)(end - PAGE), 2 * PAGE, 3, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED && errno == EACCES);
        CHECK(munmap(last, PAGE) == 0);
        void *hint = mmap((void *)PAGE, PAGE, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(hint != MAP_FAILED && (unsigned long)hint >= base && (unsigned long)hint <= end - PAGE);
        CHECK(munmap(hint, PAGE) == 0);
        void *growing = mmap((void *)(base + PAGE), PAGE, 3, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE | MAP_GROWSDOWN, -1, 0);
        CHECK(growing == (void *)(base + PAGE));
        *(volatile unsigned char *)base = 0x39;
        CHECK(*(volatile unsigned char *)base == 0x39);
        inaccessible(base - PAGE, 0);
        CHECK(munmap((void *)base, 2 * PAGE) == 0);
    }
    CHECK(!close(fd));
    CHECK(!sigaction(SIGSEGV, &previous, NULL));
    printf("{\"test\":\"translated_guest_layout_boundaries\",\"passed\":true,\"layout\":\"%s\"}\n", layout);
}

static void *thread(void *argument)
{
    CHECK(tls == 7);
    tls = (uintptr_t)argument;
    atomic_fetch_add(&ready, 1);
    while (atomic_load(&ready) != 2)
        sched_yield();
    for (unsigned i = 0; i < 1000; ++i) {
        CHECK(pthread_mutex_lock(&mutex) == 0);
        ++counter;
        CHECK(pthread_mutex_unlock(&mutex) == 0);
        if (i % 32 == 0)
            sched_yield();
        CHECK(tls == (uintptr_t)argument);
    }
    return argument;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "after-build")) {
        CHECK(getpid() > 1 && tls == 7);
        return 0;
    }
    CHECK(argc == 1 && !strcmp(argv[0], "/probe-guest") && getpid() == 1);
    CHECK(sysconf(_SC_PAGESIZE) == PAGE);
    CHECK(getenv("NCE_TEST") && !strcmp(getenv("NCE_TEST"), "translation"));
    setvbuf(stdout, NULL, _IOLBF, 0);
    extern void signal_tests(const char *libc), dynamic_code_tests(void), memory_exchange_benchmarks(void);
    extern void memory_tests(void), memory_capacity_test(void);
    extern void translation_timer_tests(int high_resolution);
#if NCE_TRANSLATION_CPUS == 2
    extern void translation_smp_guest_test(void);
    cpu_set_t available;
    CPU_ZERO(&available);
    CPU_SET(0, &available);
    CPU_SET(1, &available);
    CHECK(!sched_setaffinity(0, sizeof(available), &available));
    CPU_ZERO(&available);
    CHECK(!sched_getaffinity(0, sizeof(available), &available));
    CHECK(CPU_ISSET(0, &available) && (CPU_COUNT(&available) == 1 || CPU_COUNT(&available) == 2));
    printf("{\"test\":\"translated_smp_online_cpus\",\"passed\":true,\"online\":%d}\n", CPU_COUNT(&available));
    if (CPU_COUNT(&available) == 2) {
        CHECK(CPU_ISSET(1, &available));
        translation_smp_guest_test();
    }
#endif
    layout_tests();
#ifdef NCE_TRANSLATION_STANDARD_JIT
    extern void standard_jit_tests(void), simd_fallback_tests(void);
    standard_jit_tests();
    simd_fallback_tests();
#else
    dynamic_code_tests();
#endif
    translation_timer_tests(NCE_TRANSLATION_HIGH_RES_TIMERS);
    signal_tests("musl-translated");
    memory_tests();
    memory_capacity_test();
    memory_exchange_benchmarks();
    pthread_t threads[2];
    for (uintptr_t i = 0; i < 2; ++i)
        CHECK(pthread_create(&threads[i], NULL, thread, (void *)(i + 19)) == 0);
    for (uintptr_t i = 0; i < 2; ++i) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0 && result == (void *)(i + 19));
    }
    CHECK(counter == 2000 && tls == 7);
    puts("{\"test\":\"translated_pthread_tls_mutex\",\"passed\":true}");
    struct sigaction action = {.sa_handler = handler};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGUSR1, &action, NULL) == 0 && raise(SIGUSR1) == 0 && delivered == SIGUSR1);
    CHECK(sigaction(SIGSEGV, &action, NULL) == 0);
    volatile unsigned *page = mmap(NULL, PAGE, 3, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(page != MAP_FAILED);
    *page = 37;
    CHECK(mprotect((void *)page, PAGE, PROT_READ) == 0);
    if (!sigsetjmp(fault_jump, 1)) {
        *page = 99;
        CHECK(0);
    }
    CHECK(delivered == SIGSEGV && *page == 37);
    CHECK(mprotect((void *)page, PAGE, PROT_READ | PROT_WRITE) == 0);
    *page = 83;
    CHECK(*page == 83 && munmap((void *)page, PAGE) == 0);
    puts("{\"test\":\"translated_signals_protection_restore\",\"passed\":true}");
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        char *args[] = {"/probe-after-build", "after-build", NULL};
        execv(args[0], args);
        _exit(96);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    puts("{\"test\":\"translated_runtime_elf_exec\",\"passed\":true}");
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        char *args[] = {"/dynamic", NULL};
        execv(args[0], args);
        _exit(97);
    }
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    puts("{\"test\":\"translated_runtime_libraries\",\"passed\":true}");
    return 0;
}

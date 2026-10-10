/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
    dynamic_code_tests();
    signal_tests("musl-translated");
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

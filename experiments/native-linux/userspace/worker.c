/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/syscall.h>
#include <unistd.h>

#define CHECK(c) do { if (!(c)) { dprintf(2, "worker:%d errno=%d\n", __LINE__, errno); return 93; } } while (0)
static _Thread_local unsigned local = 123;
static volatile unsigned private_value = 17;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned total;
static atomic_int spinner_started, release_spinner, detached_tid;

static void *spin_work(void *unused)
{
    (void)unused;
    typedef unsigned vector4 __attribute__((vector_size(16)));
    vector4 value = {1, 2, 3, 4}, step = {1, 2, 3, 4};
    unsigned loops = 0;
    atomic_store(&spinner_started, 1);
    /* No syscall occurs in this loop; another runnable thread must preempt it. */
    while (!atomic_load(&release_spinner) || loops < 4000000) { value += step; ++loops; }
    for (unsigned i = 0; i < 4; ++i) if (value[i] != (loops + 1) * (i + 1)) return (void *)5;
    return NULL;
}

static void *release_work(void *unused)
{
    (void)unused;
    atomic_store(&release_spinner, 1);
    return NULL;
}

static void *detached_work(void *unused)
{
    (void)unused;
    atomic_store(&detached_tid, (int)syscall(SYS_gettid));
    return NULL;
}

static void *thread_work(void *argument)
{
    unsigned id = (unsigned)(uintptr_t)argument;
    if (local != 123) return (void *)1;
    local = 700 + id;
    for (unsigned i = 0; i < 1024; ++i) {
        if (pthread_mutex_lock(&mutex)) return (void *)2;
        ++total;
        if (pthread_mutex_unlock(&mutex)) return (void *)3;
        if (!(i % 64)) sched_yield();
        if (local != 700 + id) return (void *)4;
    }
    return NULL;
}

/* Mapping symbols mark these words as data even inside an executable section. */
__asm__(".pushsection .text\n.balign 4\n.global instruction_constants\ninstruction_constants:\n.word 0xd4000001, 0xd53bd040, 0xd51bd040\n.popsection");
extern const unsigned instruction_constants[3];
extern unsigned exercise_large_text(unsigned);

int main(int argc, char **argv)
{
    CHECK(argc == 2 && !strcmp(argv[1], "check"));
    CHECK(local == 123);
    unsigned expected = 7;
    for (unsigned i = 0; i < 4096; ++i) expected = (expected + i * 7919 + 17) ^ (expected >> 3);
    CHECK(exercise_large_text(7) == expected);
    volatile long double input = 1.25L;
    char formatted[32];
    CHECK(snprintf(formatted, sizeof(formatted), "%.3Lf", input / 2) == 5 && !strcmp(formatted, "0.625"));
    CHECK(instruction_constants[0] == 0xd4000001 && instruction_constants[1] == 0xd53bd040 && instruction_constants[2] == 0xd51bd040);
    pthread_t threads[4];
    for (uintptr_t i = 0; i < 4; ++i) CHECK(pthread_create(&threads[i], NULL, thread_work, (void *)i) == 0);
    for (unsigned i = 0; i < 4; ++i) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0 && result == NULL);
    }
    CHECK(total == 4096 && local == 123);
    CHECK(pthread_create(&threads[0], NULL, spin_work, NULL) == 0);
    while (!atomic_load(&spinner_started)) sched_yield();
    CHECK(pthread_create(&threads[1], NULL, release_work, NULL) == 0);
    for (unsigned i = 0; i < 2; ++i) {
        void *result;
        CHECK(pthread_join(threads[i], &result) == 0 && result == NULL);
    }
    pthread_attr_t attr;
    CHECK(pthread_attr_init(&attr) == 0);
    CHECK(pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED) == 0);
    CHECK(pthread_create(&threads[0], &attr, detached_work, NULL) == 0);
    CHECK(pthread_attr_destroy(&attr) == 0);
    while (!atomic_load(&detached_tid)) sched_yield();
    while (syscall(SYS_tgkill, getpid(), atomic_load(&detached_tid), 0) == 0) sched_yield();
    CHECK(errno == ESRCH);
    size_t size = 2 * 1024 * 1024;
    unsigned char *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(data != MAP_FAILED);
    for (size_t i = 0; i < size; i += 16384) data[i] = (unsigned char)(i / 16384);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        private_value = 29;
        local = 456;
        for (size_t i = 0; i < size; i += 16384) data[i] ^= 0xff;
        _exit(private_value == 29 && local == 456 ? 43 : 94);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 43);
    CHECK(private_value == 17 && local == 123);
    for (size_t i = 0; i < size; i += 16384) CHECK(data[i] == (unsigned char)(i / 16384));
    CHECK(munmap(data, size) == 0);
    CHECK(printf("worker passed pid=%ld tls=%u\n", (long)getpid(), local) > 0);
    return 0;
}

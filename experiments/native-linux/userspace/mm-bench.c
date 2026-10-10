/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include "../layout.h"
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PAGE 16384
#define ROUNDS 128
#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "mm-bench:%d errno=%d\n", __LINE__, errno); \
            _exit(95); \
        } \
    } while (0)

struct exchange {
    volatile unsigned char *private, *shared;
    size_t private_bytes, shared_bytes;
    int request[2], reply[2], touch_all, threads;
};

static uint64_t now(void)
{
    struct timespec t;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

static unsigned char pattern(size_t offset, unsigned side)
{
    return (unsigned char)(offset / PAGE * 13 + side + 1);
}

static void touch(struct exchange *x, unsigned side)
{
    size_t stride = x->touch_all ? PAGE : x->private_bytes;
    for (size_t i = 0; i < x->private_bytes; i += stride) {
        CHECK(x->private[i] == pattern(i, x->threads ? 0 : side));
        x->private[i] = pattern(i, x->threads ? 0 : side);
    }
    stride = x->touch_all ? PAGE : x->shared_bytes;
    for (size_t i = 0; i < x->shared_bytes; i += stride) {
        CHECK(x->shared[i] == pattern(i, 7));
        x->shared[i] = pattern(i, 7);
    }
}

static void *responder(void *arg)
{
    struct exchange *x = arg;
    if (!x->threads) {
        close(x->request[1]);
        close(x->reply[0]);
        /* Materialize private COW pages before timing the exchanges. */
        for (size_t i = 0; i < x->private_bytes; i += PAGE)
            x->private[i] = pattern(i, 1);
    }
    char value = 1;
    CHECK(write(x->reply[1], &value, 1) == 1);
    for (unsigned i = 0; i < ROUNDS; ++i) {
        CHECK(read(x->request[0], &value, 1) == 1 && value == 1);
        touch(x, 1);
        CHECK(write(x->reply[1], &value, 1) == 1);
    }
    return NULL;
}

static void benchmark(size_t bytes, unsigned shared_percent, int all, int threads)
{
    struct exchange x = {.touch_all = all, .threads = threads};
    x.shared_bytes = bytes * shared_percent / 100;
    x.private_bytes = bytes - x.shared_bytes;
    if (x.private_bytes) {
        x.private = mmap(NULL, x.private_bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(x.private != MAP_FAILED);
        for (size_t i = 0; i < x.private_bytes; i += PAGE)
            x.private[i] = pattern(i, 0);
    }
    if (x.shared_bytes) {
        x.shared = mmap(NULL, x.shared_bytes, PROT_READ | PROT_WRITE,
                        MAP_SHARED | MAP_ANONYMOUS, -1, 0);
        CHECK(x.shared != MAP_FAILED);
        for (size_t i = 0; i < x.shared_bytes; i += PAGE)
            x.shared[i] = pattern(i, 7);
    }
    CHECK(pipe(x.request) == 0 && pipe(x.reply) == 0);
    pthread_t thread;
    pid_t child = -1;
    if (threads)
        CHECK(pthread_create(&thread, NULL, responder, &x) == 0);
    else {
        child = fork();
        CHECK(child >= 0);
        if (!child) {
            responder(&x);
            _exit(0);
        }
        close(x.request[0]);
        close(x.reply[1]);
    }
    char value;
    CHECK(read(x.reply[0], &value, 1) == 1 && value == 1);
    uint64_t begin = now();
    for (unsigned i = 0; i < ROUNDS; ++i) {
        touch(&x, 0);
        CHECK(write(x.request[1], &value, 1) == 1);
        CHECK(read(x.reply[0], &value, 1) == 1 && value == 1);
    }
    uint64_t elapsed = now() - begin;
    if (threads)
        CHECK(pthread_join(thread, NULL) == 0);
    else {
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    }
    x.touch_all = 1;
    touch(&x, 0);
    if (threads) {
        CHECK(close(x.request[0]) == 0);
        CHECK(close(x.reply[1]) == 0);
    }
    CHECK(close(x.request[1]) == 0 && close(x.reply[0]) == 0);
    if (x.private_bytes)
        CHECK(munmap((void *)x.private, x.private_bytes) == 0);
    if (x.shared_bytes)
        CHECK(munmap((void *)x.shared, x.shared_bytes) == 0);
    printf("{\"bench\":\"linux_mm_exchange\",\"bytes\":%zu,\"shared_percent\":%u,\"touch_all\":%d,\"threads\":%d,\"rounds\":%u,\"ns_per_roundtrip\":%llu}\n",
           bytes, shared_percent, all, threads, ROUNDS, (unsigned long long)(elapsed / ROUNDS));
    CHECK(fflush(stdout) == 0);
}

void memory_capacity_test(void)
{
    const size_t bytes = NCE_RAM_SIZE * 5 / 8;
    volatile unsigned char *data = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(data != MAP_FAILED);
    for (size_t offset = 0; offset < bytes; offset += PAGE) {
        CHECK(data[offset] == 0);
        data[offset] = pattern(offset, 0);
    }
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        for (size_t offset = 0; offset < bytes; offset += PAGE)
            CHECK(data[offset] == pattern(offset, 0));
        data[0] ^= 0xff;
        data[bytes / 2] ^= 0xff;
        data[bytes - PAGE] ^= 0xff;
        _exit(0);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child && status == 0);
    for (size_t offset = 0; offset < bytes; offset += PAGE)
        CHECK(data[offset] == pattern(offset, 0));
    CHECK(munmap((void *)data, bytes) == 0);
    printf("{\"test\":\"linux_ram_capacity_cow\",\"passed\":true,\"bytes\":%zu}\n", bytes);
}

void memory_exchange_benchmarks(void)
{
    for (size_t bytes = 2 * 1024 * 1024; bytes <= 16 * 1024 * 1024; bytes *= 2) {
        for (unsigned shared = 0; shared <= 100; shared += 50)
            for (int all = 0; all <= 1; ++all)
                benchmark(bytes, shared, all, 0);
        benchmark(bytes, 0, 1, 1);
    }
    puts("{\"test\":\"linux_mm_exchange_integrity\",\"passed\":true,\"cases\":28}");
}

int main(void)
{
    extern void memory_tests(void), code_tests(void);
    extern void signal_tests(const char *libc);
    code_tests();
    signal_tests("musl");
    memory_tests();
    memory_capacity_test();
    memory_exchange_benchmarks();
    return 0;
}

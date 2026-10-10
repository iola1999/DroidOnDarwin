/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "fault-race-test:%d\n", __LINE__); \
            _exit(1); \
        } \
    } while (0)

enum { PAGE = 16384,
       SIZE = 64 * 1024 * 1024 };
static volatile unsigned char *private;
static atomic_int started, finished;

static void *discard(void *unused)
{
    (void)unused;
    atomic_store(&started, 1);
    for (unsigned pass = 0; pass < 32; ++pass) {
        CHECK(madvise((void *)private, SIZE, MADV_DONTNEED) == 0);
        usleep(1000);
    }
    atomic_store(&finished, 1);
    return NULL;
}

int main(void)
{
    alarm(30);
    int fd = memfd_create("nce-fault-race", 0);
    CHECK(fd >= 0 && ftruncate(fd, SIZE) == 0);
    unsigned char *shared = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    CHECK(shared != MAP_FAILED);
    for (unsigned i = 0; i < SIZE; i += PAGE)
        shared[i] = 0xa5;
    private = mmap(NULL, SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    CHECK(private != MAP_FAILED);
    pthread_t worker;
    CHECK(pthread_create(&worker, NULL, discard, NULL) == 0);
    while (!atomic_load(&started))
        usleep(1000);
    unsigned passes = 0;
    do {
        for (unsigned i = 0; i < SIZE; i += PAGE)
            private[i] = (unsigned char)passes;
        ++passes;
    } while (passes < 32 || !atomic_load(&finished));
    CHECK(pthread_join(worker, NULL) == 0);
    // Check content after the thread can no longer discard private writes.
    for (unsigned i = 0; i < SIZE; i += PAGE) {
        private[i] = 0x39;
        CHECK(private[i] == 0x39 && shared[i] == 0xa5);
    }
    CHECK(madvise((void *)private, SIZE, MADV_DONTNEED) == 0);
    for (unsigned i = 0; i < SIZE; i += PAGE)
        CHECK(private[i] == 0xa5);
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
        CHECK(ftruncate(fd, 0) == 0);
        _exit(private[0]);
    }
    int status;
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
    CHECK(munmap((void *)private, SIZE) == 0 && munmap(shared, SIZE) == 0);
    CHECK(close(fd) == 0);
    printf("{\"test\":\"linux_private_file_discard_cow\",\"passes\":%u,\"passed\":true}\n", passes);
    puts("{\"test\":\"linux_truncated_file_fault\",\"passed\":true}");
    return 0;
}

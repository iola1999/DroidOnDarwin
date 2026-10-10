/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <pthread.h>
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
            dprintf(2, "schedule-test:%d\n", __LINE__); \
            exit(1); \
        } \
    } while (0)

static struct shared {
    atomic_int started, stop;
} *state;
static int pipefd[2];

static uint64_t clock_ns(void)
{
    struct timespec time;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &time) == 0);
    return (uint64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

static void *busy(void *unused)
{
    (void)unused;
    atomic_store(&state->started, 1);
    while (!atomic_load(&state->stop)) {
        __asm__ volatile("" ::: "memory");
    }
    return NULL;
}

static void *blocked(void *unused)
{
    (void)unused;
    char byte;
    atomic_store(&state->started, 1);
    CHECK(read(pipefd[0], &byte, 1) == 1 && byte == 'x');
    return NULL;
}

int main(void)
{
    alarm(10);
    char mode[32];
    CHECK(fgets(mode, sizeof(mode), stdin));
    mode[strcspn(mode, "\n")] = 0;
    int is_blocked = !strcmp(mode, "blocked"), is_process = !strcmp(mode, "process");
    CHECK(is_blocked || is_process || !strcmp(mode, "busy"));
    state = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(state != MAP_FAILED);
    atomic_init(&state->started, 0);
    atomic_init(&state->stop, 0);
    CHECK(pipe(pipefd) == 0);
    pid_t child = -1;
    pthread_t thread;
    if (is_process) {
        child = fork();
        CHECK(child >= 0);
        if (!child) {
            busy(NULL);
            _exit(0);
        }
    } else {
        CHECK(pthread_create(&thread, NULL, is_blocked ? blocked : busy, NULL) == 0);
    }
    while (!atomic_load(&state->started))
        usleep(1000);
    // Give the reader time to enter its blocking call before sustained work.
    if (is_blocked)
        usleep(20000);
    uint64_t end = clock_ns() + 500000000;
    do {
        for (unsigned i = 0; i < 4096; ++i)
            __asm__ volatile("" ::: "memory");
    } while (clock_ns() < end);
    atomic_store(&state->stop, 1);
    if (is_blocked)
        CHECK(write(pipefd[1], "x", 1) == 1);
    if (is_process) {
        int status;
        CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    } else {
        CHECK(pthread_join(thread, NULL) == 0);
    }
    CHECK(close(pipefd[0]) == 0 && close(pipefd[1]) == 0);
    CHECK(munmap(state, 16384) == 0);
    printf("{\"test\":\"linux_runnable_workload\",\"mode\":\"%s\",\"passed\":true}\n", mode);
    return 0;
}

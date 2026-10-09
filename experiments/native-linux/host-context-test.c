/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lkl-host-ops.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>

extern struct lkl_host_operations nce_lkl_host;
#define CHECK(c) \
    do { \
        if (!(c)) { \
            fprintf(stderr, "host-context:%d\n", __LINE__); \
            exit(1); \
        } \
    } while (0)
static struct lkl_jmp_buf context;
static int jumped;
extern int nce_disk_open(unsigned index, const char *path);
extern void nce_disk_close(void);

static void jump_back(void)
{
    jumped = 1;
    nce_lkl_host.jmp_buf_longjmp(&context, 1);
    abort();
}

static void *worker(void *unused)
{
    (void)unused;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    CHECK(pthread_sigmask(SIG_BLOCK, &mask, NULL) == 0);
    nce_lkl_host.jmp_buf_set(&context, jump_back);
    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0);
    CHECK(sigismember(&mask, SIGUSR2) && jumped);
    return NULL;
}

static void disk_tests(void)
{
    char path[] = "/tmp/nce-storage-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0 && ftruncate(fd, 513) == 0);
    CHECK(nce_disk_open(0, path) == -1);
    CHECK(ftruncate(fd, 1024) == 0);
    CHECK(nce_disk_open(0, path) == 0);
    CHECK(nce_lkl_host.disk_size(0) == 1024 && nce_lkl_host.disk_size(2) == 0);
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == -1);
    unsigned char write[512], read[512];
    memset(write, 0xa5, sizeof(write));
    CHECK(nce_lkl_host.disk_io(0, 1, 512, write, sizeof(write)) == 0);
    CHECK(nce_lkl_host.disk_io(0, 0, 512, read, sizeof(read)) == 0);
    CHECK(memcmp(read, write, sizeof(read)) == 0);
    CHECK(nce_lkl_host.disk_io(0, 0, 513, read, sizeof(read)) == -1);
    CHECK(nce_lkl_host.disk_io(0, 1, UINT64_MAX, write, 1) == -1);
    CHECK(nce_lkl_host.disk_io(2, 0, 0, read, 1) == -1);
    CHECK(nce_lkl_host.disk_io(0, 3, 0, read, 1) == -1);
    CHECK(nce_lkl_host.disk_io(0, 2, 0, NULL, 0) == 0);
    nce_disk_close();
    CHECK(nce_disk_open(1, path) == 0);
    CHECK(nce_lkl_host.disk_io(1, 0, 512, read, sizeof(read)) == 0);
    CHECK(memcmp(read, write, sizeof(read)) == 0);
    CHECK(nce_lkl_host.disk_io(1, 1, 0, write, 1) == -1);
    nce_disk_close();
    CHECK(nce_lkl_host.disk_size(0) == 0 && nce_lkl_host.disk_size(1) == 0);
    CHECK(close(fd) == 0 && unlink(path) == 0);
    puts("{\"test\":\"host_disk_bounds_readonly_and_locking\",\"passed\":true}");
}

int main(void)
{
    disk_tests();
    sigset_t mask, original;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR2);
    CHECK(pthread_sigmask(SIG_UNBLOCK, &mask, &original) == 0);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_sigmask(SIG_BLOCK, NULL, &mask) == 0);
    CHECK(!sigismember(&mask, SIGUSR2));
    CHECK(pthread_sigmask(SIG_SETMASK, &original, NULL) == 0);
    puts("{\"test\":\"host_context_preserves_thread_masks\",\"passed\":true}");
    return 0;
}

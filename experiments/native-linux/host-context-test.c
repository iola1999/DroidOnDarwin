/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lkl-host-ops.h"
#include "layout.h"
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include <limits.h>
#include <errno.h>

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
extern int nce_disk_close(void);

#ifdef NCE_HOST_DISK_FAILURE_TEST
static int injected_sync_error, injected_close_error;
static unsigned sync_calls, close_calls;

int nce_host_test_fsync(int fd)
{
    ++sync_calls;
    if (injected_sync_error) {
        errno = injected_sync_error;
        injected_sync_error = 0;
        return -1;
    }
    return fsync(fd);
}

int nce_host_test_close(int fd)
{
    ++close_calls;
    int result = close(fd);
    /* Error injection follows an actual close; it does not model Darwin EINTR. */
    if (!result && injected_close_error) {
        errno = injected_close_error;
        injected_close_error = 0;
        return -1;
    }
    return result;
}
#endif

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
    const char *temporary = getenv("TMPDIR");
    if (!temporary || temporary[0] != '/')
        temporary = "/tmp";
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/nce-storage-XXXXXX", temporary) < (int)sizeof(path));
    int fd = mkstemp(path);
    CHECK(fd >= 0 && ftruncate(fd, 513) == 0);
    CHECK(nce_disk_open(0, path) == -1);
    CHECK(nce_disk_open(NCE_DISK_COUNT, path) == -1);
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
    CHECK(nce_lkl_host.disk_io(NCE_DISK_COUNT, 0, 0, read, 1) == -1);
    CHECK(nce_lkl_host.disk_io(0, 3, 0, read, 1) == -1);
    CHECK(nce_lkl_host.disk_io(0, 2, 0, NULL, 0) == 0);
    CHECK(nce_disk_close() == 0);
    for (unsigned index = 1; index < NCE_DISK_COUNT; ++index) {
        CHECK(nce_disk_open(index, path) == 0);
        CHECK(flock(fd, LOCK_EX | LOCK_NB) == -1);
        CHECK(nce_lkl_host.disk_size(index) == 1024);
        CHECK(nce_lkl_host.disk_io(index, 0, 512, read, sizeof(read)) == 0);
        CHECK(memcmp(read, write, sizeof(read)) == 0);
        CHECK(nce_lkl_host.disk_io(index, 1, 0, write, 1) == -1);
        CHECK(nce_lkl_host.disk_io(index, 0, 513, read, sizeof(read)) == -1);
        CHECK(nce_lkl_host.disk_io(index, 2, 0, NULL, 0) == 0);
    }
    CHECK(nce_disk_close() == 0);
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0 && flock(fd, LOCK_UN) == 0);
    for (unsigned index = 0; index < NCE_DISK_COUNT; ++index)
        CHECK(nce_lkl_host.disk_size(index) == 0 && nce_lkl_host.disk_io(index, 0, 0, read, 1) == -1);
    CHECK(close(fd) == 0 && unlink(path) == 0);
    puts("{\"test\":\"host_disk_bounds_readonly_and_locking\",\"passed\":true}");
}

#ifdef NCE_HOST_DISK_FAILURE_TEST
static void disk_failure_tests(void)
{
    const char *temporary = getenv("TMPDIR");
    CHECK(temporary && temporary[0] == '/');
    char paths[NCE_DISK_COUNT][PATH_MAX];
    int observers[NCE_DISK_COUNT];
    for (unsigned index = 0; index < NCE_DISK_COUNT; ++index) {
        CHECK(snprintf(paths[index], sizeof(paths[index]), "%s/nce-close-XXXXXX", temporary) < (int)sizeof(paths[index]));
        observers[index] = mkstemp(paths[index]);
        CHECK(observers[index] >= 0 && ftruncate(observers[index], 1024) == 0);
    }
    const char *cases[] = {"sync_error", "sync_and_close_error", "close_error"};
    for (unsigned test = 0; test < 3; ++test) {
        for (unsigned index = 0; index < NCE_DISK_COUNT; ++index) {
            CHECK(nce_disk_open(index, paths[index]) == 0);
            CHECK(flock(observers[index], LOCK_EX | LOCK_NB) == -1);
        }
        unsigned previous_sync = sync_calls, previous_close = close_calls;
        injected_sync_error = test < 2 ? EIO : 0;
        injected_close_error = test ? EINTR : 0;
        CHECK(nce_disk_close() == -(test < 2 ? EIO : EINTR));
        CHECK(sync_calls == previous_sync + 1 && close_calls == previous_close + NCE_DISK_COUNT);
        CHECK(!injected_sync_error && !injected_close_error);
        unsigned char byte;
        for (unsigned index = 0; index < NCE_DISK_COUNT; ++index) {
            CHECK(nce_lkl_host.disk_size(index) == 0 && nce_lkl_host.disk_io(index, 0, 0, &byte, 1) == -1);
            CHECK(flock(observers[index], LOCK_EX | LOCK_NB) == 0 && flock(observers[index], LOCK_UN) == 0);
        }
        CHECK(nce_disk_close() == 0);
        CHECK(sync_calls == previous_sync + 1 && close_calls == previous_close + NCE_DISK_COUNT);
        printf("{\"test\":\"host_disk_failure_closes_all_slots\",\"case\":\"%s\",\"passed\":true,\"locks_released\":%u,\"sync_calls\":1,\"close_calls\":%u}\n",
               cases[test], NCE_DISK_COUNT, NCE_DISK_COUNT);
    }
    for (unsigned index = 0; index < NCE_DISK_COUNT; ++index)
        CHECK(close(observers[index]) == 0 && unlink(paths[index]) == 0);
}
#endif

int main(void)
{
    disk_tests();
#ifdef NCE_HOST_DISK_FAILURE_TEST
    disk_failure_tests();
#endif
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

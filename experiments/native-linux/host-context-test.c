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
extern int nce_disk_open(unsigned index, const char *path, int writable);
extern int nce_disk_close(void);

#ifdef NCE_HOST_DISK_FAILURE_TEST
static int injected_sync_error, injected_close_error;
static unsigned sync_calls, close_calls, skipped_sync_calls;

int nce_host_test_fsync(int fd)
{
    ++sync_calls;
    if (injected_sync_error && skipped_sync_calls) {
        --skipped_sync_calls;
    } else if (injected_sync_error) {
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
    CHECK(nce_disk_open(0, path, 1) == -1);
    CHECK(nce_disk_open(NCE_DISK_COUNT, path, 1) == -1);
    CHECK(ftruncate(fd, 1024) == 0);
    CHECK(nce_disk_open(2, path, -1) == -1 && nce_disk_open(2, path, 2) == -1);
    CHECK(nce_disk_open(2, NULL, 2) == -1);
    CHECK(nce_lkl_host.disk_size(2) == 0 && nce_lkl_host.disk_writable(2) == 0);
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0 && flock(fd, LOCK_UN) == 0);
    CHECK(nce_disk_open(2, NULL, 1) == 0 && nce_lkl_host.disk_writable(2) == 0);
    CHECK(nce_disk_open(0, path, 1) == 0);
    CHECK(nce_lkl_host.disk_writable(0) == 1);
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
        CHECK(nce_disk_open(index, path, 0) == 0);
        CHECK(nce_lkl_host.disk_writable(index) == 0);
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
        CHECK(nce_lkl_host.disk_size(index) == 0 && nce_lkl_host.disk_writable(index) == 0 &&
              nce_lkl_host.disk_io(index, 0, 0, read, 1) == -1);
    CHECK(nce_lkl_host.disk_writable(NCE_DISK_COUNT) == 0);
    CHECK(close(fd) == 0 && unlink(path) == 0);
    puts("{\"test\":\"host_disk_bounds_readonly_and_locking\",\"passed\":true}");
}

static void disk_mode_tests(void)
{
    const char *temporary = getenv("TMPDIR");
    CHECK(temporary && temporary[0] == '/');
    char path[PATH_MAX];
    CHECK(snprintf(path, sizeof(path), "%s/nce-mode-XXXXXX", temporary) < (int)sizeof(path));
    int fd = mkstemp(path);
    CHECK(fd >= 0 && ftruncate(fd, 1024) == 0);
    CHECK(nce_disk_open(0, path, 0) == 0 && nce_disk_open(1, path, 0) == 0);
    CHECK(nce_lkl_host.disk_writable(0) == 0 && nce_lkl_host.disk_writable(1) == 0);
    CHECK(flock(fd, LOCK_SH | LOCK_NB) == 0 && flock(fd, LOCK_UN) == 0);
    CHECK(nce_disk_open(2, path, 1) == -1);
    CHECK(nce_lkl_host.disk_size(2) == 0 && nce_lkl_host.disk_writable(2) == 0);
    unsigned char written[512], actual[512];
    memset(written, 0x3e, sizeof(written));
    CHECK(nce_lkl_host.disk_io(0, 1, 0, written, sizeof(written)) == -1);
#ifdef NCE_HOST_DISK_FAILURE_TEST
    unsigned previous_sync = sync_calls;
#endif
    CHECK(nce_lkl_host.disk_io(0, 2, 0, NULL, 0) == 0);
    CHECK(nce_disk_close() == 0);
#ifdef NCE_HOST_DISK_FAILURE_TEST
    CHECK(sync_calls == previous_sync);
#endif
    memset(written, 0, sizeof(written));
    CHECK(pread(fd, actual, sizeof(actual), 0) == sizeof(actual) && !memcmp(actual, written, sizeof(actual)));
    CHECK(nce_disk_open(2, path, 1) == 0 && nce_lkl_host.disk_writable(2) == 1);
    CHECK(flock(fd, LOCK_SH | LOCK_NB) == -1 && flock(fd, LOCK_EX | LOCK_NB) == -1);
    CHECK(nce_disk_open(0, path, 0) == -1 && nce_disk_open(1, path, 1) == -1);
    CHECK(nce_lkl_host.disk_size(0) == 0 && nce_lkl_host.disk_size(1) == 0);
    memset(written, 0x3e, sizeof(written));
    CHECK(nce_lkl_host.disk_io(2, 1, 512, written, sizeof(written)) == 0);
    CHECK(nce_lkl_host.disk_io(2, 0, 512, actual, sizeof(actual)) == 0 && !memcmp(actual, written, sizeof(actual)));
    CHECK(nce_lkl_host.disk_io(2, 1, 1024, written, 1) == -1);
    CHECK(nce_lkl_host.disk_io(2, 1, UINT64_MAX, written, 1) == -1);
    CHECK(nce_lkl_host.disk_io(2, 0, 1024, actual, 1) == -1);
    CHECK(nce_lkl_host.disk_io(2, 2, 0, NULL, 0) == 0);
#ifdef NCE_HOST_DISK_FAILURE_TEST
    CHECK(sync_calls == previous_sync + 1);
    injected_sync_error = EIO;
    CHECK(nce_lkl_host.disk_io(2, 2, 0, NULL, 0) == -1 && !injected_sync_error);
#endif
    CHECK(nce_disk_close() == 0 && nce_lkl_host.disk_writable(2) == 0);
    CHECK(pread(fd, actual, sizeof(actual), 512) == sizeof(actual) && !memcmp(actual, written, sizeof(actual)));
    CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0 && flock(fd, LOCK_UN) == 0);
    CHECK(close(fd) == 0 && unlink(path) == 0);
    puts("{\"test\":\"host_disk_explicit_writable_modes\",\"passed\":true}");
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
    const struct {
        const char *name;
        unsigned writable_mask, skip_sync;
        int sync_error, close_error, result;
    } cases[] = {
        {"sync_error", 1, 0, EIO, 0, -EIO},
        {"sync_and_close_error", 1, 0, EIO, EINTR, -EIO},
        {"close_error", 1, 0, 0, EINTR, -EINTR},
        {"metadata_sync_error", 4, 0, EIO, 0, -EIO},
        {"second_writable_sync_error", 5, 1, EIO, 0, -EIO},
        {"readonly_close_then_metadata_sync_error", 4, 0, EIO, EINTR, -EINTR},
        {"multiple_writable_success", 5, 0, 0, 0, 0},
    };
    for (unsigned test = 0; test < sizeof(cases) / sizeof(*cases); ++test) {
        unsigned writable_count = 0;
        for (unsigned index = 0; index < NCE_DISK_COUNT; ++index) {
            int writable = !!(cases[test].writable_mask & (1U << index));
            CHECK(nce_disk_open(index, paths[index], writable) == 0);
            CHECK(nce_lkl_host.disk_writable(index) == writable);
            CHECK(flock(observers[index], LOCK_EX | LOCK_NB) == -1);
            writable_count += writable;
        }
        unsigned previous_sync = sync_calls, previous_close = close_calls;
        injected_sync_error = cases[test].sync_error;
        injected_close_error = cases[test].close_error;
        skipped_sync_calls = cases[test].skip_sync;
        CHECK(nce_disk_close() == cases[test].result);
        CHECK(sync_calls == previous_sync + writable_count && close_calls == previous_close + NCE_DISK_COUNT);
        CHECK(!injected_sync_error && !injected_close_error && !skipped_sync_calls);
        unsigned char byte;
        for (unsigned index = 0; index < NCE_DISK_COUNT; ++index) {
            CHECK(nce_lkl_host.disk_size(index) == 0 && nce_lkl_host.disk_writable(index) == 0 &&
                  nce_lkl_host.disk_io(index, 0, 0, &byte, 1) == -1);
            CHECK(flock(observers[index], LOCK_EX | LOCK_NB) == 0 && flock(observers[index], LOCK_UN) == 0);
        }
        CHECK(nce_disk_close() == 0);
        CHECK(sync_calls == previous_sync + writable_count && close_calls == previous_close + NCE_DISK_COUNT);
        if (test < 3)
            printf("{\"test\":\"host_disk_failure_closes_all_slots\",\"case\":\"%s\",\"passed\":true,\"locks_released\":%u,\"sync_calls\":1,\"close_calls\":%u}\n",
                   cases[test].name, NCE_DISK_COUNT, NCE_DISK_COUNT);
        else
            printf("{\"test\":\"host_disk_writable_modes_close\",\"case\":\"%s\",\"passed\":true,\"locks_released\":%u,\"writable_mask\":%u,\"sync_calls\":%u,\"close_calls\":%u}\n",
                   cases[test].name, NCE_DISK_COUNT, cases[test].writable_mask, writable_count, NCE_DISK_COUNT);
    }
    for (unsigned index = 0; index < NCE_DISK_COUNT; ++index)
        CHECK(close(observers[index]) == 0 && unlink(paths[index]) == 0);
}
#endif

int main(void)
{
    disk_tests();
    disk_mode_tests();
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

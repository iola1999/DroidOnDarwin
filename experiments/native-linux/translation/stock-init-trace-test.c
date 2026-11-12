/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static atomic_bool started;
static atomic_ulong copy_calls, getpid_calls;
static unsigned char payload[2304], label[300];
static const char security_path[] = "/system/etc/selinux/trace-concurrency-file";
static const char attribute[] = "security.selinux";
static const char source_path[] = "/dev/loop-trace-concurrency";
static const char target_path[] = "/data/trace-concurrency";
static const char filesystem[] = "ext4";
static long mount_args[6];

static int copy_user(void *destination, unsigned long address, unsigned long bytes, int write)
{
    assert(!write && address);
    memcpy(destination, (const void *)address, bytes);
    atomic_fetch_add_explicit(&copy_calls, 1, memory_order_relaxed);
    return 0;
}

static long run_syscall(long number, const long *args)
{
    assert(number == 172);
    for (unsigned i = 0; i < 6; ++i)
        assert(!args[i]);
    atomic_fetch_add_explicit(&getpid_calls, 1, memory_order_relaxed);
    return getpid();
}

#include STOCK_INIT_TRACE_HEADER

struct worker {
    unsigned role, iterations;
};

static void *emit_rows(void *opaque)
{
    const struct worker *worker = opaque;
    while (!atomic_load_explicit(&started, memory_order_acquire))
        sched_yield();
    unsigned count = worker->iterations * (worker->role == 3 ? 32 : 1);
    for (unsigned i = 0; i < count; ++i) {
        if (worker->role == 0) {
            long write[6] = {1000 + i, (long)payload, sizeof(payload)};
            stock_init_trace_write(64, write, sizeof(payload));
            unsigned long vectors[4] = {(unsigned long)payload + 37, 48,
                                        (unsigned long)payload + 111, 96};
            long writev[6] = {2000 + i, (long)vectors, 2};
            stock_init_trace_write(66, writev, 144);
        } else if (worker->role == 1) {
            long xattr[6] = {(long)security_path, (long)attribute, (long)label, sizeof(label)};
            stock_init_trace_security(5, xattr, 0, NULL, 0);
            xattr[0] = 3000 + i;
            stock_init_trace_security(7, xattr, -13, NULL, 0);
        } else if (worker->role == 2) {
            stock_init_trace_block(40, mount_args, i & 1 ? -16 : 0);
            stock_init_trace_result(63, mount_args, 1, 0x12340 + 4UL * i);
        } else {
            /* 单次 printf 与头文件的完整 JSON 记录共用 stdout。 */
            printf("{\"memory_sample\":true,\"iteration\":%u,\"rss_bytes\":%lu,\"guest_bytes\":%lu}\n",
                   i, 4096UL * i + 1, 16384UL * i + 7);
        }
        if (!(i & 15))
            sched_yield();
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    char *end;
    errno = 0;
    unsigned long count = strtoul(argv[1], &end, 10);
    if (errno || *end || !count || count > 4096)
        return 2;
    for (unsigned i = 0; i < sizeof(payload); ++i)
        payload[i] = (17 * i + 3) & 255;
    for (unsigned i = 0; i < sizeof(label); ++i)
        label[i] = (29 * i + 7) & 255;
    mount_args[0] = (long)source_path;
    mount_args[1] = (long)target_path;
    mount_args[2] = (long)filesystem;
    mount_args[3] = 4096;
    assert(setvbuf(stdout, NULL, _IOFBF, 65536) == 0);
    printf("{\"stock_init_trace_fixture\":true,\"iterations\":%lu,\"pid\":%ld,\"block_args\":[%lu,%lu,%lu,%lu,%lu,%lu]}\n",
           count, (long)getpid(), (unsigned long)mount_args[0], (unsigned long)mount_args[1],
           (unsigned long)mount_args[2], (unsigned long)mount_args[3],
           (unsigned long)mount_args[4], (unsigned long)mount_args[5]);
    pthread_t threads[4];
    struct worker workers[4];
    for (unsigned i = 0; i < 4; ++i) {
        workers[i] = (struct worker){i, (unsigned)count};
        assert(pthread_create(threads + i, NULL, emit_rows, workers + i) == 0);
    }
    atomic_store_explicit(&started, true, memory_order_release);
    for (unsigned i = 0; i < 4; ++i)
        assert(pthread_join(threads[i], NULL) == 0);
    assert(fflush(stdout) == 0);
    fprintf(stderr, "{\"stock_init_trace_calls\":true,\"copy_user\":%lu,\"getpid\":%lu,\"threads\":4}\n",
            atomic_load_explicit(&copy_calls, memory_order_relaxed),
            atomic_load_explicit(&getpid_calls, memory_order_relaxed));
    return 0;
}

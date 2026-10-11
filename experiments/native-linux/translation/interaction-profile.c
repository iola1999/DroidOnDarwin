/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "interaction-profile.h"
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <time.h>

#define THREAD_CAPACITY 4096
struct NceExecutionProfile {
    long pid, tid;
    uint64_t host_tid;
    uint64_t wall[NCE_STAGE_COUNT], cpu[NCE_STAGE_COUNT], calls[NCE_STAGE_COUNT];
};
static struct NceExecutionProfile threads[THREAD_CAPACITY];
static unsigned thread_count, overflow;
static atomic_bool enabled;
static atomic_uint clock_errors;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t began_ns;
static bool used;

static uint64_t now(clockid_t clock)
{
    struct timespec value;
    if (clock_gettime(clock, &value)) {
        atomic_fetch_add_explicit(&clock_errors, 1, memory_order_relaxed);
        return 0;
    }
    return (uint64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

bool nce_execution_profile_enabled(void)
{
    return atomic_load_explicit(&enabled, memory_order_relaxed);
}

NceExecutionProfile *nce_execution_profile_thread(long pid, long tid)
{
    uint64_t host_tid = 0;
    bool valid = pid > 0 && tid > 0 && !pthread_threadid_np(NULL, &host_tid);
    pthread_mutex_lock(&lock);
    static NceExecutionProfile discarded;
    NceExecutionProfile *result = &discarded;
    if (valid && thread_count < THREAD_CAPACITY) {
        result = &threads[thread_count++];
        *result = (NceExecutionProfile){.pid = pid, .tid = tid, .host_tid = host_tid};
    } else {
        ++overflow;
    }
    pthread_mutex_unlock(&lock);
    return result;
}

NceProfileStamp nce_execution_profile_begin(void)
{
    if (!nce_execution_profile_enabled())
        return (NceProfileStamp){0};
    return (NceProfileStamp){now(CLOCK_MONOTONIC), now(CLOCK_THREAD_CPUTIME_ID)};
}

void nce_execution_profile_end(NceExecutionProfile *thread, unsigned stage, NceProfileStamp start)
{
    if (!start.wall || !start.cpu || !thread || stage >= NCE_STAGE_COUNT)
        return;
    uint64_t cpu = now(CLOCK_THREAD_CPUTIME_ID), wall = now(CLOCK_MONOTONIC);
    pthread_mutex_lock(&lock);
    // Only intervals entirely inside the window are accumulated. The end
    // snapshot holds this lock, so unfinished calls cannot mutate its result.
    if (nce_execution_profile_enabled() && start.wall >= began_ns && wall >= start.wall && cpu >= start.cpu) {
        thread->wall[stage] += wall - start.wall;
        thread->cpu[stage] += cpu - start.cpu;
        ++thread->calls[stage];
    }
    pthread_mutex_unlock(&lock);
}

int nce_execution_profile_control(bool active, const char *directory)
{
    pthread_mutex_lock(&lock);
    if (active) {
        if (used) {
            pthread_mutex_unlock(&lock);
            return -EBUSY;
        }
        used = true;
        began_ns = now(CLOCK_MONOTONIC);
        atomic_store_explicit(&enabled, true, memory_order_relaxed);
        pthread_mutex_unlock(&lock);
        return 0;
    }
    atomic_store_explicit(&enabled, false, memory_order_relaxed);
    uint64_t ended_ns = now(CLOCK_MONOTONIC);
    char path[1024];
    int result = 0;
    if (snprintf(path, sizeof(path), "%s/execution.json", directory) >= (int)sizeof(path)) {
        result = -ENAMETOOLONG;
    } else {
        FILE *out = fopen(path, "w");
        if (!out) {
            result = -errno;
        } else {
            fprintf(out, "{\"host_begin_ns\":%llu,\"host_end_ns\":%llu,\"overflow\":%u,\"clock_errors\":%u,\"scope\":\"completed intervals only; excludes intervals crossing window boundaries and kernel/syscall waits; diagnostic overhead unqualified\",\"stages\":[\"kernel_prepare\",\"direct_including_compile\",\"unicorn_prepare_and_execute\"],\"threads\":[",
                    (unsigned long long)began_ns, (unsigned long long)ended_ns, overflow, atomic_load_explicit(&clock_errors, memory_order_relaxed));
            for (unsigned i = 0; i < thread_count; ++i) {
                NceExecutionProfile *thread = &threads[i];
                fprintf(out, "%s{\"android_pid\":%ld,\"android_tid\":%ld,\"host_tid\":%llu,\"stages\":[", i ? "," : "", thread->pid, thread->tid, (unsigned long long)thread->host_tid);
                for (unsigned stage = 0; stage < NCE_STAGE_COUNT; ++stage)
                    fprintf(out, "%s{\"wall_ns\":%llu,\"cpu_ns\":%llu,\"calls\":%llu}", stage ? "," : "", (unsigned long long)thread->wall[stage], (unsigned long long)thread->cpu[stage], (unsigned long long)thread->calls[stage]);
                fputs("]}", out);
            }
            fputs("]}\n", out);
            if (ferror(out))
                result = -EIO;
            if (fclose(out) && !result)
                result = -EIO;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}

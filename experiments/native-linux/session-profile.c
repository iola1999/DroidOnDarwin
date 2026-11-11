/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "session.h"
#include "session-internal.h"
#include <pthread.h>
#ifdef NCE_DIRECT_EXECUTION
#include "translation/interaction-profile.h"
#endif
/* Profiling uses a separate lock so a stop can interrupt a pending request. */
#include "interaction-profile.h"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t profile_mutex = PTHREAD_MUTEX_INITIALIZER;
static int profile_scheduler;
static int profile_first_open;

static uint64_t monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}

static int export_profile(const char *directory, const char *name, int optional)
{
    char path[1024];
    if (snprintf(path, sizeof(path), NCE_PROFILE_OUTPUT "%s", name) >= (int)sizeof(path))
        return -EINVAL;
    long fd = nce_session_call(56, -100, (long)path, 0x80000, 0);
    if (fd < 0)
        return optional && fd == -2 ? 0 : (int)fd;
    int result = 0;
    FILE *out = NULL;
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path)) {
        result = -ENAMETOOLONG;
        goto done;
    }
    out = fopen(path, "wb");
    if (!out) {
        result = -errno;
        goto done;
    }
    char buffer[16384];
    size_t total = 0;
    for (;;) {
        long count = nce_session_call(63, fd, (long)buffer, sizeof(buffer), 0);
        if (count == -4)
            continue;
        if (count <= 0) {
            result = (int)count;
            break;
        }
        total += count;
        if (total > 32 * 1024 * 1024) {
            result = -EFBIG;
            break;
        }
        if (fwrite(buffer, 1, count, out) != (size_t)count) {
            result = -EIO;
            break;
        }
    }
done:
    nce_session_call(57, fd, 0, 0, 0);
    if (out && fclose(out) && !result)
        result = -EIO;
    return result;
}

/* /proc in a host task uses the initial PID namespace; Android remounts its
 * own procfs. NSpid/NStgid preserve the IDs needed to join logs with ftrace. */
static int proc_ids(const char *path, int *ids, size_t capacity)
{
    /* AArch64 O_DIRECTORY is 040000; 0200000 selects O_DIRECT. */
    long fd = nce_session_call(56, -100, (long)path, 0x84000, 0);
    if (fd < 0)
        return (int)fd;
    char buffer[8192];
    int count = 0;
    for (;;) {
        long bytes = nce_session_call(61, fd, (long)buffer, sizeof(buffer), 0);
        if (bytes <= 0) {
            if (bytes < 0)
                count = (int)bytes;
            break;
        }
        for (size_t offset = 0; offset < (size_t)bytes;) {
            uint16_t length;
            if (offset + 20 > (size_t)bytes) {
                count = -EIO;
                goto done;
            }
            memcpy(&length, buffer + offset + 16, sizeof(length));
            if (length < 20 || offset + length > (size_t)bytes || !memchr(buffer + offset + 19, 0, length - 19)) {
                count = -EIO;
                goto done;
            }
            char *end;
            long id = strtol(buffer + offset + 19, &end, 10);
            if (id > 0 && id <= INT32_MAX && !*end) {
                if ((size_t)count == capacity) {
                    count = -EOVERFLOW;
                    goto done;
                }
                ids[count++] = (int)id;
            }
            offset += length;
        }
    }
done:
    nce_session_call(57, fd, 0, 0, 0);
    return count;
}

static int profile_threads(const char *directory, const char *name)
{
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path))
        return -ENAMETOOLONG;
    FILE *out = fopen(path, "w");
    if (!out)
        return -errno;
    fprintf(out, "host_before_ns=%llu\n", (unsigned long long)monotonic_ns());
    int pids[4096], tids[4096];
    int processes = proc_ids("/proc", pids, 4096);
    unsigned captured = 0;
    int result = processes < 0 ? processes : 0;
    for (int i = 0; i < processes && !result; ++i) {
        snprintf(path, sizeof(path), "/proc/%d/task", pids[i]);
        int threads = proc_ids(path, tids, 4096);
        if (threads < 0 && threads != -2) {
            result = threads;
            break;
        }
        for (int j = 0; j < threads; ++j) {
            if (++captured > 4096) {
                result = -EOVERFLOW;
                break;
            }
            snprintf(path, sizeof(path), "/proc/%d/task/%d/status", pids[i], tids[j]);
            long fd = nce_session_call(56, -100, (long)path, 0x80000, 0);
            if (fd == -2)
                continue;
            if (fd < 0) {
                result = (int)fd;
                break;
            }
            fprintf(out, "[thread global_pid=%d global_tid=%d]\n", pids[i], tids[j]);
            char buffer[8192];
            long count = nce_session_call(63, fd, (long)buffer, sizeof(buffer), 0);
            nce_session_call(57, fd, 0, 0, 0);
            if (count <= 0 || count == sizeof(buffer)) {
                result = count < 0 ? (int)count : -EIO;
                break;
            }
            fwrite(buffer, 1, count, out);
        }
    }
    fprintf(out, "host_after_ns=%llu\n", (unsigned long long)monotonic_ns());
    if (ferror(out))
        result = -EIO;
    if (fclose(out) && !result)
        result = -EIO;
    return result;
}

int nce_session_profile(int command, const char *directory)
{
    if (command < NCE_PROFILE_SETTINGS || command > NCE_PROFILE_SETTINGS_PREPARE || !directory)
        return -EINVAL;
    if (pthread_mutex_trylock(&profile_mutex))
        return -EBUSY;
    if (command == NCE_PROFILE_FRAMES || command == NCE_PROFILE_SCHEDULER) {
        int status = profile_threads(directory, "threads-before.txt");
        if (status) {
            pthread_mutex_unlock(&profile_mutex);
            return status;
        }
    }
#ifdef NCE_DIRECT_EXECUTION
    if (command == NCE_PROFILE_END && profile_scheduler) {
        int status = nce_execution_profile_control(false, directory);
        if (status) {
            pthread_mutex_unlock(&profile_mutex);
            return status;
        }
    }
#endif
    nce_session_call(35, -100, (long)NCE_PROFILE_RESPONSE, 0, 0);
    long fd = nce_session_call(56, -100, (long)NCE_PROFILE_REQUEST ".tmp", 0x80241, 0600);
    int result = fd < 0 ? (int)fd : 0;
    if (fd >= 0) {
        long count = nce_session_call(64, fd, (long)&command, sizeof(command), 0);
        nce_session_call(57, fd, 0, 0, 0);
        result = count == sizeof(command) ? 0 : count < 0 ? (int)count
                                                          : -EIO;
    }
    if (!result)
        result = (int)nce_session_call(38, -100, (long)NCE_PROFILE_REQUEST ".tmp", -100, (long)NCE_PROFILE_REQUEST);
    // First-open failure can spend 45 seconds in its helper and 25 more
    // disabling atrace before replying with the original failure.
    uint64_t limit = command == NCE_PROFILE_SETTINGS && profile_first_open ? UINT64_C(90000000000) : UINT64_C(60000000000);
    uint64_t deadline = monotonic_ns() + limit;
    while (!result) {
        fd = nce_session_call(56, -100, (long)"/data/nce/session-stop", 0x80000, 0);
        if (fd >= 0) {
            nce_session_call(57, fd, 0, 0, 0);
            result = -ECANCELED;
            break;
        }
        fd = nce_session_call(56, -100, (long)NCE_PROFILE_RESPONSE, 0x80000, 0);
        if (fd >= 0) {
            long count = nce_session_call(63, fd, (long)&result, sizeof(result), 0);
            nce_session_call(57, fd, 0, 0, 0);
            if (count != sizeof(result))
                result = -EIO;
            break;
        }
        if (fd != -2 || monotonic_ns() >= deadline) {
            result = fd == -2 ? -ETIMEDOUT : (int)fd;
            break;
        }
        usleep(20000);
    }
    if (!result) {
        char path[1024];
        if (snprintf(path, sizeof(path), "%s/clock-pairs.jsonl", directory) >= (int)sizeof(path)) {
            result = -ENAMETOOLONG;
        } else {
            FILE *out = fopen(path, "a");
            if (!out) {
                result = -errno;
            } else {
                struct timespec host_resolution = {0};
                int64_t guest_resolution[2] = {0};
                int resolution_status = clock_getres(CLOCK_MONOTONIC, &host_resolution);
                long guest_resolution_status = nce_session_call(114, 1, (long)guest_resolution, 0, 0);
                for (int i = 0; i < 3; ++i) {
                    int64_t guest[2] = {0};
                    uint64_t before = monotonic_ns();
                    long status = nce_session_call(113, 1, (long)guest, 0, 0);
                    uint64_t after = monotonic_ns();
                    fprintf(out, "{\"command\":%d,\"host_before_ns\":%llu,\"guest_monotonic_ns\":%llu,\"host_after_ns\":%llu,\"status\":%ld,\"host_resolution_ns\":%llu,\"guest_resolution_ns\":%llu,\"resolution_status\":%ld}\n",
                            command, (unsigned long long)before,
                            (unsigned long long)(guest[0] * INT64_C(1000000000) + guest[1]),
                            (unsigned long long)after, status,
                            (unsigned long long)((uint64_t)host_resolution.tv_sec * 1000000000 + host_resolution.tv_nsec),
                            (unsigned long long)(guest_resolution[0] * INT64_C(1000000000) + guest_resolution[1]),
                            resolution_status ? (long)resolution_status : guest_resolution_status);
                    if (status)
                        result = (int)status;
                }
                if (fclose(out) && !result)
                    result = -EIO;
            }
        }
    }
#ifdef NCE_DIRECT_EXECUTION
    if (!result && command == NCE_PROFILE_SCHEDULER)
        result = nce_execution_profile_control(true, directory);
    if (result && profile_scheduler)
        nce_execution_profile_control(false, directory);
#endif
    if (!result && (command == NCE_PROFILE_FRAMES || command == NCE_PROFILE_SCHEDULER))
        profile_scheduler = command == NCE_PROFILE_SCHEDULER;
    if (!result && command == NCE_PROFILE_SETTINGS_PREPARE)
        profile_first_open = 1;
    if (command == NCE_PROFILE_SETTINGS_PREPARE) {
        int status = export_profile(directory, "launch.jsonl", result != 0);
        if (!result)
            result = status;
    }
    int failed_open = result && (profile_first_open || command == NCE_PROFILE_SETTINGS_PREPARE);
    if (command == NCE_PROFILE_END || failed_open) {
        const char *files[] = {"frames.txt", "kernel.ftrace", "cpu0.txt", "cpu1.txt", "clock.txt"};
        for (unsigned i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
            if (i && !profile_scheduler)
                continue;
            int status = export_profile(directory, files[i], failed_open || i == 3);
            if (status)
                fprintf(stderr, "interaction export %s failed: %d\n", files[i], status);
            if (!result)
                result = status;
        }
        for (unsigned i = 0; i < 2; ++i) {
            int status = export_profile(directory, i ? "launch-frames.txt" : "launch.jsonl", 1);
            if (status)
                fprintf(stderr, "interaction launch export failed: %d\n", status);
            if (!result)
                result = status;
        }
    }
    if (command == NCE_PROFILE_END) {
        int status = profile_threads(directory, "threads-after.txt");
        if (!result)
            result = status;
    }
    if (command == NCE_PROFILE_END || failed_open)
        profile_first_open = 0;
    pthread_mutex_unlock(&profile_mutex);
    return result;
}

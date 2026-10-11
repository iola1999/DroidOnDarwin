/* SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "proc-exit:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(119); \
        } \
    } while (0)

struct proc_state {
    _Atomic int pid, done;
    unsigned reads;
};

static void *short_thread(void *unused)
{
    usleep(3000);
    return unused;
}

void proc_exit_tests(void)
{
    // Numeric proc paths must use this test's PID namespace.
    CHECK(unshare(CLONE_NEWNS) == 0);
    CHECK(mount(NULL, "/", NULL, MS_PRIVATE | MS_REC, NULL) == 0);
    CHECK(mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL) == 0);
    struct proc_state *shared = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(shared != MAP_FAILED);
    pid_t observer = fork();
    CHECK(observer >= 0);
    if (!observer) {
        alarm(20);
        while (!atomic_load(&shared->done)) {
            int pid = atomic_load(&shared->pid);
            if (!pid) {
                usleep(100);
                continue;
            }
            char path[128];
            snprintf(path, sizeof(path), "/proc/%d/task", pid);
            DIR *tasks = opendir(path);
            if (!tasks) {
                // proc_pid_permission returns ESRCH if exit races path lookup.
                CHECK(errno == ENOENT || errno == ESRCH);
                continue;
            }
            struct dirent *entry;
            while ((entry = readdir(tasks))) {
                if (entry->d_name[0] == '.')
                    continue;
                long tid = strtol(entry->d_name, NULL, 10);
                CHECK(tid > 0);
                snprintf(path, sizeof(path), "/proc/%d/task/%ld/stat", pid, tid);
                int fd = open(path, O_RDONLY | O_CLOEXEC);
                if (fd < 0) {
                    CHECK(errno == ENOENT || errno == ESRCH);
                    continue;
                }
                char text[2048];
                ssize_t count = read(fd, text, sizeof(text));
                CHECK(count > 0 || (count < 0 && errno == ESRCH));
                shared->reads += count > 0;
                CHECK(close(fd) == 0);
            }
            CHECK(closedir(tasks) == 0);
        }
        _exit(0);
    }
    for (unsigned round = 0; round < 32; ++round) {
        pid_t child = fork();
        CHECK(child >= 0);
        if (!child) {
            pthread_t threads[8];
            for (unsigned i = 0; i < 8; ++i)
                CHECK(pthread_create(&threads[i], NULL, short_thread, NULL) == 0);
            for (unsigned i = 0; i < 8; ++i)
                CHECK(pthread_join(threads[i], NULL) == 0);
            _exit(0);
        }
        atomic_store(&shared->pid, child);
        int status;
        CHECK(waitpid(child, &status, 0) == child && status == 0);
    }
    atomic_store(&shared->done, 1);
    int status;
    pid_t waited = waitpid(observer, &status, 0);
    if (waited != observer || status || !shared->reads)
        dprintf(2, "proc-exit observer=%d waited=%d status=%d stat_reads=%u\n", observer, waited, status, shared->reads);
    CHECK(waited == observer && status == 0 && shared->reads > 0);
    printf("{\"test\":\"linux_proc_concurrent_exit\",\"passed\":true,\"processes\":32,\"threads\":256,\"stat_reads\":%u}\n", shared->reads);
    CHECK(munmap(shared, 16384) == 0);
    CHECK(umount("/proc") == 0);
}

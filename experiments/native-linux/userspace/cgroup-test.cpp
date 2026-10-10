/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <errno.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECK(c) \
    do { \
        if (!(c)) { \
            dprintf(2, "cgroup-test:%d errno=%d: %s\n", __LINE__, errno, #c); \
            _exit(109); \
        } \
    } while (0)

static void scheduling_capability(bool enabled)
{
    __user_cap_header_struct header = {_LINUX_CAPABILITY_VERSION_3, 0};
    __user_cap_data_struct data[2] = {};
    data[CAP_SYS_NICE / 32].permitted = 1U << (CAP_SYS_NICE % 32);
    if (enabled)
        data[CAP_SYS_NICE / 32].effective = data[CAP_SYS_NICE / 32].permitted;
    CHECK(syscall(__NR_capset, &header, data) == 0);
}

void nce_cgroup_permission_test()
{
    int ready[2], stop[2];
    CHECK(pipe2(ready, O_CLOEXEC) == 0 && pipe2(stop, O_CLOEXEC) == 0);
    pid_t app = fork();
    CHECK(app >= 0);
    if (!app) {
        alarm(10);
        CHECK(close(ready[0]) == 0 && close(stop[1]) == 0);
        CHECK(setgid(20001) == 0 && setuid(20001) == 0);
        char value = 1;
        CHECK(write(ready[1], &value, 1) == 1);
        CHECK(read(stop[0], &value, 1) == 1);
        _exit(0);
    }
    CHECK(close(ready[1]) == 0 && close(stop[0]) == 0);
    char value;
    CHECK(read(ready[0], &value, 1) == 1 && close(ready[0]) == 0);
    pid_t manager = fork();
    CHECK(manager >= 0);
    if (!manager) {
        alarm(5);
        CHECK(prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0) == 0);
        CHECK(setgid(1000) == 0 && setuid(1000) == 0);
        scheduling_capability(false);
        int fd = open("/dev/cpuctl/foreground/tasks", O_WRONLY | O_CLOEXEC);
        CHECK(fd >= 0);
        char pid[32];
        int length = snprintf(pid, sizeof(pid), "%d", app);
        CHECK(write(fd, pid, length) == -1 && errno == EACCES);
        scheduling_capability(true);
        CHECK(write(fd, pid, length) == length);
        scheduling_capability(false);
        CHECK(write(fd, pid, length) == -1 && errno == EACCES);
        CHECK(close(fd) == 0);
        _exit(0);
    }
    int status;
    CHECK(waitpid(manager, &status, 0) == manager && status == 0);
    char path[64], membership[1024] = {};
    snprintf(path, sizeof(path), "/proc/%d/cgroup", app);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    ssize_t count = read(fd, membership, sizeof(membership) - 1);
    CHECK(count > 0 && close(fd) == 0);
    CHECK(strstr(membership, ":cpu:/foreground\n"));
    CHECK(write(stop[1], &value, 1) == 1 && close(stop[1]) == 0);
    CHECK(waitpid(app, &status, 0) == app && status == 0);
    puts("{\"test\":\"android_cgroup_cross_uid_permissions\",\"passed\":true,\"unprivileged_denied\":true}");
}

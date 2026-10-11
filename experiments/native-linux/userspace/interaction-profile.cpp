/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../interaction-profile.h"
#include "desktop-session.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool tracing;
static bool capturing;

static int put(const char *path, const char *value)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return -errno;
    ssize_t count = write(fd, value, strlen(value));
    int result = count == static_cast<ssize_t>(strlen(value)) ? 0 : -(count < 0 ? errno : EIO);
    close(fd);
    return result;
}

static int copy(const char *source, const char *name)
{
    char path[160];
    snprintf(path, sizeof(path), NCE_PROFILE_OUTPUT "%s", name);
    int in = open(source, O_RDONLY | O_CLOEXEC);
    if (in < 0)
        return -errno;
    int out = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (out < 0) {
        int error = -errno;
        close(in);
        return error;
    }
    char buffer[16384];
    int result = 0;
    size_t total = 0;
    for (;;) {
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            result = n == 0 ? 0 : -errno;
            break;
        }
        total += n;
        if (total > 32 * 1024 * 1024) {
            result = -EFBIG;
            break;
        }
        if (write(out, buffer, n) != n) {
            result = -EIO;
            break;
        }
    }
    close(in);
    close(out);
    return result;
}

static int frames(bool reset)
{
    pid_t child = fork();
    if (child < 0)
        return -errno;
    if (!child) {
        alarm(25);
        int fd = open(NCE_PROFILE_OUTPUT "frames.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0)
            _exit(120);
        close(fd);
        execl("/system/bin/dumpsys", "dumpsys", "-t", "20", "gfxinfo", "com.android.settings", reset ? "reset" : "framestats", nullptr);
        _exit(121);
    }
    int status = nce_wait_desktop_child(child, true);
    return status == 0 ? 0 : -EIO;
}

static int start_trace()
{
    mkdir("/sys/kernel/tracing", 0700);
    if (mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, nullptr) < 0 && errno != EBUSY)
        return -errno;
    const char *settings[][2] = {
        {"tracing_on", "0"},
        {"events/enable", "0"},
        {"buffer_size_kb", "4096"},
        {"trace_clock", "mono"},
        {"trace", "\n"},
        {"events/sched/sched_switch/enable", "1"},
        {"events/sched/sched_waking/enable", "1"},
        {"events/sched/sched_process_fork/enable", "1"},
        {"events/sched/sched_process_exit/enable", "1"},
        {"events/binder/binder_transaction/enable", "1"},
        {"events/binder/binder_transaction_received/enable", "1"},
        {"tracing_on", "1"},
    };
    for (const auto &setting : settings) {
        char path[192];
        snprintf(path, sizeof(path), "/sys/kernel/tracing/%s", setting[0]);
        int result = put(path, setting[1]);
        if (result) {
            put("/sys/kernel/tracing/tracing_on", "0");
            return result;
        }
    }
    return 0;
}

static int perform(int command)
{
    if (command == NCE_PROFILE_SETTINGS) {
        if (capturing)
            return -EBUSY;
        // Existing command helpers abort on failure; isolate them from Android init.
        pid_t child = fork();
        if (child < 0)
            return -errno;
        if (!child) {
            if (setpgid(0, 0) < 0)
                _exit(123);
            alarm(45);
            nce_android_command({"activity", "start-activity", "-a", "android.settings.SETTINGS"}, "Starting:");
            _exit(nce_surface_check("com.android.settings", "--ready", true) ? 0 : 122);
        }
        if (setpgid(child, child) < 0 && errno != EACCES && errno != ESRCH)
            dprintf(2, "interaction helper setpgid: %s\n", strerror(errno));
        int status = nce_wait_desktop_child(child, true);
        // Reap helper descendants after cancellation or an alarm. Android init
        // adopts them when their parent exits; they must not retain Binder work.
        kill(-child, SIGKILL);
        while (waitpid(-child, nullptr, 0) > 0 || errno == EINTR) {
        }
        return status == 0 ? 0 : -EIO;
    }
    if (command == NCE_PROFILE_FRAMES || command == NCE_PROFILE_SCHEDULER) {
        if (capturing)
            return -EBUSY;
        int result = frames(true);
        if (!result && command == NCE_PROFILE_SCHEDULER)
            result = start_trace();
        if (!result) {
            capturing = true;
            tracing = command == NCE_PROFILE_SCHEDULER;
        }
        return result;
    }
    if (command != NCE_PROFILE_END || !capturing)
        return -EINVAL;
    int result = 0;
    if (tracing) {
        result = put("/sys/kernel/tracing/tracing_on", "0");
        if (!result)
            result = copy("/sys/kernel/tracing/trace", "kernel.ftrace");
        for (int cpu = 0; cpu < 2; ++cpu) {
            char path[128], name[32];
            snprintf(path, sizeof(path), "/sys/kernel/tracing/per_cpu/cpu%d/stats", cpu);
            snprintf(name, sizeof(name), "cpu%d.txt", cpu);
            int status = copy(path, name);
            if (status && !(cpu == 1 && status == -ENOENT) && !result)
                result = status;
        }
        int status = copy("/sys/kernel/tracing/trace_clock", "clock.txt");
        if (!result)
            result = status;
    }
    int status = frames(false);
    capturing = tracing = false;
    return result ? result : status;
}

void nce_interaction_profile_poll()
{
    int fd = open(NCE_PROFILE_REQUEST, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    int command = 0;
    ssize_t count = read(fd, &command, sizeof(command));
    close(fd);
    unlink(NCE_PROFILE_REQUEST);
    int status = count == sizeof(command) ? perform(command) : -EINVAL;
    fd = open(NCE_PROFILE_RESPONSE ".tmp", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        if (write(fd, &status, sizeof(status)) == sizeof(status))
            rename(NCE_PROFILE_RESPONSE ".tmp", NCE_PROFILE_RESPONSE);
        close(fd);
    }
    dprintf(2, "{\"interaction_profile\":%d,\"status\":%d}\n", command, status);
}

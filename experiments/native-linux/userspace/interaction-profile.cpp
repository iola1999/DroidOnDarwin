/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../interaction-profile.h"
#include "desktop-session.h"
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static bool tracing;
static bool capturing;
static bool first_open;
static bool settings_opened;
static int trace_preparation = -EAGAIN;

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

static int frames(bool reset, const char *name = "frames.txt")
{
    pid_t child = fork();
    if (child < 0)
        return -errno;
    if (!child) {
        alarm(25);
        char path[160];
        snprintf(path, sizeof(path), NCE_PROFILE_OUTPUT "%s", name);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0)
            _exit(120);
        close(fd);
        execl("/system/bin/dumpsys", "dumpsys", "-t", "20", "gfxinfo", "com.android.settings", reset ? "reset" : "framestats", nullptr);
        _exit(121);
    }
    int status = nce_wait_desktop_child(child, true);
    return status == 0 ? 0 : -EIO;
}

static uint64_t monotonic_ns()
{
    timespec now;
    return clock_gettime(CLOCK_MONOTONIC, &now) ? 0 : uint64_t(now.tv_sec) * 1000000000 + now.tv_nsec;
}

static int settings_process(pid_t *pid = nullptr, uint64_t *starttime = nullptr)
{
    DIR *directory = opendir("/proc");
    if (!directory)
        return -errno;
    int result = 0;
    bool found = false;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(directory);
        if (!entry) {
            result = -errno;
            break;
        }
        char *end;
        long number = strtol(entry->d_name, &end, 10);
        if (*end || number <= 0 || number > INT32_MAX)
            continue;
        char path[128], name[128] = {};
        snprintf(path, sizeof(path), "/proc/%ld/cmdline", number);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT)
                continue;
            result = -errno;
            break;
        }
        ssize_t count = read(fd, name, sizeof(name) - 1);
        int error = errno;
        close(fd);
        if (count < 0) {
            result = -error;
            break;
        }
        constexpr char package[] = "com.android.settings";
        if (strncmp(name, package, sizeof(package) - 1) || (name[sizeof(package) - 1] && name[sizeof(package) - 1] != ':'))
            continue;
        if (!pid) {
            result = -EBUSY;
            break;
        }
        if (name[sizeof(package) - 1] == ':')
            continue;
        if (found) {
            result = -EEXIST;
            break;
        }
        snprintf(path, sizeof(path), "/proc/%ld/stat", number);
        FILE *file = fopen(path, "re");
        char stat[4096];
        if (!file) {
            result = -errno;
            break;
        }
        bool complete = fgets(stat, sizeof(stat), file) && strchr(stat, '\n');
        fclose(file);
        // comm can contain spaces and parentheses; field 22 follows its last ')'.
        char *field = complete ? strrchr(stat, ')') : nullptr;
        char *save = nullptr;
        for (int index = 3; index <= 22 && field; ++index)
            field = strtok_r(index == 3 ? field + 1 : nullptr, " \n", &save);
        errno = 0;
        unsigned long long ticks = field ? strtoull(field, &end, 10) : 0;
        if (!ticks || errno || *end) {
            result = -EIO;
            break;
        }
        *pid = static_cast<pid_t>(number);
        *starttime = ticks;
        found = true;
    }
    closedir(directory);
    return result ? result : pid && !found ? -ESRCH
                                           : 0;
}

static int launch_record(const char *phase, uint64_t instant, pid_t pid = 0, uint64_t starttime = 0, uint64_t snapshot_end = 0)
{
    if (!instant)
        return -EIO;
    FILE *file = fopen(NCE_PROFILE_OUTPUT "launch.jsonl", "ae");
    if (!file)
        return -errno;
    int count = fprintf(file, "{\"phase\":\"%s\",\"mode\":\"force_stopped_process\",\"guest_ns\":%llu,\"pid\":%d,\"starttime_ticks\":%llu,\"snapshot_end_ns\":%llu}\n",
                        phase, static_cast<unsigned long long>(instant), pid,
                        static_cast<unsigned long long>(starttime), static_cast<unsigned long long>(snapshot_end));
    int closed = fclose(file);
    return count < 0 || closed ? -EIO : 0;
}

static int settings_command(bool prepare)
{
    // Existing command helpers abort on failure; isolate them from Android init.
    pid_t child = fork();
    if (child < 0)
        return -errno;
    if (!child) {
        if (setpgid(0, 0) < 0)
            _exit(123);
        alarm(45);
        if (prepare) {
            nce_android_command({"activity", "force-stop", "com.android.settings"});
            _exit(!settings_process() && !launch_record("prepared", monotonic_ns()) ? 0 : 122);
        }
        if (first_open && (settings_process() || launch_record("begin", monotonic_ns())))
            _exit(122);
        nce_android_command({"activity", "start-activity", "-a", "android.settings.SETTINGS"}, "Starting:");
        if (!nce_surface_check("com.android.settings", "--ready", true))
            _exit(122);
        if (first_open) {
            uint64_t ready = monotonic_ns(), starttime, after_starttime;
            pid_t pid, after_pid;
            if (settings_process(&pid, &starttime) || frames(false, "launch-frames.txt") ||
                settings_process(&after_pid, &after_starttime) || pid != after_pid || starttime != after_starttime ||
                launch_record("ready", ready, pid, starttime, monotonic_ns()))
                _exit(122);
        }
        _exit(0);
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

static long trace_property(const char *name)
{
    char value[PROP_VALUE_MAX] = {};
    if (!__system_property_get(name, value) || !value[0])
        return 0;
    char *end = nullptr;
    errno = 0;
    long number = strtol(value, &end, 0);
    return errno || !end || *end || number < 0 ? -EINVAL : number;
}

static int userspace_trace(bool enabled)
{
    pid_t child = fork();
    if (child < 0)
        return -errno;
    if (!child) {
        alarm(25);
        if (enabled)
            execl("/system/bin/atrace", "atrace", "--only_userspace", "--async_start",
                  "-a", "com.android.settings", "input", "gfx", "view", nullptr);
        else
            execl("/system/bin/atrace", "atrace", "--only_userspace", "--async_stop", nullptr);
        _exit(121);
    }
    int status = nce_wait_desktop_child(child, enabled);
    long tags = trace_property("debug.atrace.tags.enableflags");
    long apps = trace_property("debug.atrace.app_number");
    // atrace can exit successfully after a property error. Verify the state
    // that existing Android trace producers actually read (gfx/input/view).
    int result = status == 0 && tags == (enabled ? 14 : 0) && apps == (enabled ? 1 : 0) ? 0 : -EIO;
    dprintf(2, "{\"interaction_userspace_trace\":%s,\"tags\":%ld,\"apps\":%ld,\"wait_status\":%d,\"status\":%d}\n",
            enabled ? "true" : "false", tags, apps, status, result);
    return result;
}

int nce_interaction_profile_check()
{
    if (trace_preparation)
        return trace_preparation;
    void *library = dlopen("/system/lib64/libcutils.so", RTLD_NOW | RTLD_LOCAL);
    if (!library)
        return -ENOENT;
    auto tags = reinterpret_cast<uint64_t (*)()>(dlsym(library, "atrace_get_enabled_tags"));
    auto begin = reinterpret_cast<void (*)(const char *)>(dlsym(library, "atrace_begin_body"));
    auto end = reinterpret_cast<void (*)()>(dlsym(library, "atrace_end_body"));
    // Query before atrace starts so a missing initial property cannot be
    // hidden by the diagnostic itself creating it before Android services.
    int result = tags && begin && end && !(tags() & 14) ? 0 : -EIO;
    if (!result)
        result = put("/sys/kernel/tracing/trace", "\n");
    if (!result)
        result = userspace_trace(true);
    if (!result && (tags() & 14) != 14)
        result = -EIO;
    if (!result)
        result = put("/sys/kernel/tracing/tracing_on", "1");
    if (!result) {
        begin("nce_atrace_control_check");
        end();
    }
    int stopped = put("/sys/kernel/tracing/tracing_on", "0");
    if (!result)
        result = stopped;
    if (!result) {
        FILE *file = fopen("/sys/kernel/tracing/trace", "re");
        bool found = false;
        char line[1024];
        if (file) {
            while (fgets(line, sizeof(line), file))
                found |= strstr(line, "|nce_atrace_control_check") != nullptr;
            fclose(file);
        }
        if (!found)
            result = -EIO;
    }
    int cleanup = userspace_trace(false);
    if (!result && (tags() & 14))
        result = -EIO;
    dlclose(library);
    return result ? result : cleanup;
}

static int prepare_tracefs()
{
    mkdir("/sys/kernel/tracing", 0700);
    if (mount("tracefs", "/sys/kernel/tracing", "tracefs", 0, nullptr) < 0 && errno != EBUSY)
        return -errno;
    if (chmod("/sys/kernel/tracing", 0755) < 0 || chmod("/sys/kernel/tracing/trace_marker", 0222) < 0)
        return -errno;
    return put("/sys/kernel/tracing/tracing_on", "0");
}

void nce_interaction_profile_init()
{
    // libcutils opens trace_marker once per process. Prepare it before Android
    // services start; enabling tags after a failed open cannot repair that fd.
    trace_preparation = prepare_tracefs();
    dprintf(2, "{\"interaction_trace_prepare\":%d}\n", trace_preparation);
}

static int start_trace()
{
    if (trace_preparation)
        return trace_preparation;
    if (trace_property("debug.atrace.tags.enableflags") != 0 || trace_property("debug.atrace.app_number") != 0)
        return -EBUSY;
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
    int result = userspace_trace(true);
    if (!result)
        result = put("/sys/kernel/tracing/tracing_on", "1");
    if (result) {
        put("/sys/kernel/tracing/tracing_on", "0");
        userspace_trace(false);
    }
    return result;
}

static int finish_capture(bool collect_frames)
{
    int result = 0;
    if (tracing) {
        result = put("/sys/kernel/tracing/tracing_on", "0");
        int cleanup = userspace_trace(false);
        if (!result)
            result = cleanup;
        int exported = copy("/sys/kernel/tracing/trace", "kernel.ftrace");
        if (!result)
            result = exported;
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
    int status = collect_frames ? frames(false) : 0;
    if (!status && first_open && collect_frames) {
        pid_t pid;
        uint64_t starttime;
        status = settings_opened ? settings_process(&pid, &starttime) : -EINVAL;
        if (!status)
            status = launch_record("end", monotonic_ns(), pid, starttime);
    }
    if (first_open && !collect_frames) {
        int recorded = launch_record("failed", monotonic_ns());
        if (!status)
            status = recorded;
    }
    capturing = tracing = first_open = settings_opened = false;
    return result ? result : status;
}

static int perform(int command)
{
    if (command == NCE_PROFILE_SETTINGS_PREPARE) {
        if (capturing || first_open)
            return -EBUSY;
        for (const char *name : {"launch.jsonl", "launch-frames.txt", "frames.txt", "kernel.ftrace", "cpu0.txt", "cpu1.txt", "clock.txt"}) {
            char path[160];
            snprintf(path, sizeof(path), NCE_PROFILE_OUTPUT "%s", name);
            if (unlink(path) && errno != ENOENT)
                return -errno;
        }
        int result = settings_command(true);
        if (result)
            launch_record("prepare_failed", monotonic_ns());
        first_open = !result;
        settings_opened = false;
        return result;
    }
    if (command == NCE_PROFILE_SETTINGS) {
        if ((capturing && (!first_open || settings_opened)) || (!capturing && first_open))
            return -EBUSY;
        settings_opened = capturing;
        int result = settings_command(false);
        if (result && capturing) {
            int cleanup = finish_capture(false);
            dprintf(2, "{\"interaction_failed_capture_cleanup\":%d}\n", cleanup);
        }
        return result;
    }
    if (command == NCE_PROFILE_FRAMES || command == NCE_PROFILE_SCHEDULER) {
        if (capturing)
            return -EBUSY;
        if (!first_open) {
            unlink(NCE_PROFILE_OUTPUT "launch.jsonl");
            unlink(NCE_PROFILE_OUTPUT "launch-frames.txt");
        }
        int result = first_open ? settings_process() : frames(true);
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
    return finish_capture(true);
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
